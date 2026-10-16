#!/usr/bin/env python3
"""Offline causal-LM fine-tuning workload over local SQuAD parquet files.

The timed region starts after model, optimizer, data iterator, and optional CUDA
reservation setup. Metrics are flushed as JSONL after every completed optimizer
step. No checkpoints are written.
"""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import importlib.util
import json
import math
import os
import random
import signal
import sys
import time
from collections.abc import Iterable, Iterator, Sequence
from pathlib import Path
from typing import Any, TextIO

DEFAULT_MODEL = "models/Qwen3-14B"
DEFAULT_PARQUET = (
    "data/squad/train-00000-of-00001.parquet"
)


@dataclasses.dataclass
class StopState:
    requested: bool = False
    signal_number: int | None = None

    def request(self, signum: int | None = None) -> None:
        self.requested = True
        self.signal_number = signum


@dataclasses.dataclass(frozen=True)
class PackedSequence:
    input_ids: list[int]
    labels: list[int]
    examples: int


class JsonlWriter:
    def __init__(self, path: str):
        self.path = path
        self._stream: TextIO | None = None
        self._owns_stream = False

    def __enter__(self) -> "JsonlWriter":
        if self.path == "-":
            self._stream = sys.stdout
        else:
            output = Path(self.path)
            output.parent.mkdir(parents=True, exist_ok=True)
            self._stream = output.open("a", encoding="utf-8", buffering=1)
            self._owns_stream = True
        return self

    def write(self, record: dict[str, Any]) -> None:
        assert self._stream is not None
        self._stream.write(json.dumps(record, sort_keys=True, allow_nan=False) + "\n")
        self._stream.flush()

    def __exit__(self, *_: object) -> None:
        if self._owns_stream and self._stream is not None:
            self._stream.close()


def seed_everything(seed: int, *, include_cuda: bool) -> None:
    random.seed(seed)
    os.environ["PYTHONHASHSEED"] = str(seed)
    try:
        import numpy as np

        np.random.seed(seed)
    except ImportError:
        pass
    import torch

    torch.manual_seed(seed)
    if include_cuda:
        torch.cuda.manual_seed_all(seed)
    if hasattr(torch, "use_deterministic_algorithms"):
        torch.use_deterministic_algorithms(True, warn_only=True)


def iter_squad_records(
    parquet_paths: Sequence[str], *, max_records: int | None = None
) -> Iterator[dict[str, Any]]:
    """Yield SQuAD records in stable file and row order without loading all rows."""
    import pyarrow.parquet as pq

    emitted = 0
    for raw_path in parquet_paths:
        parquet = pq.ParquetFile(raw_path)
        for batch in parquet.iter_batches(
            batch_size=1024, columns=["context", "question", "answers"]
        ):
            for row in batch.to_pylist():
                yield row
                emitted += 1
                if max_records is not None and emitted >= max_records:
                    return


def cycle_squad_records(
    parquet_paths: Sequence[str], *, max_records: int | None = None
) -> Iterator[dict[str, Any]]:
    """Repeat a deterministic parquet traversal, rejecting an empty data source."""
    while True:
        seen = False
        for row in iter_squad_records(parquet_paths, max_records=max_records):
            seen = True
            yield row
        if not seen:
            raise ValueError("the configured parquet files contain no SQuAD records")


def _first_answer(record: dict[str, Any]) -> str:
    answers = record.get("answers") or {}
    texts = answers.get("text") or []
    return str(texts[0]) if texts else ""


def encode_squad_record(tokenizer: Any, record: dict[str, Any]) -> tuple[list[int], list[int]]:
    """Encode one example and mask user/prompt tokens from the language-model loss."""
    context = str(record.get("context", ""))
    question = str(record.get("question", ""))
    answer = _first_answer(record)
    user_text = (
        "Answer the question using only the supplied context.\n\n"
        f"Context:\n{context}\n\nQuestion:\n{question}"
    )

    apply_template = getattr(tokenizer, "apply_chat_template", None)
    if callable(apply_template):
        user_messages = [{"role": "user", "content": user_text}]
        full_messages = user_messages + [{"role": "assistant", "content": answer}]
        try:
            prompt_ids = list(
                apply_template(
                    user_messages, tokenize=True, add_generation_prompt=True
                )
            )
            full_ids = list(
                apply_template(
                    full_messages, tokenize=True, add_generation_prompt=False
                )
            )
            if full_ids[: len(prompt_ids)] == prompt_ids and len(full_ids) > len(prompt_ids):
                labels = [-100] * len(prompt_ids) + full_ids[len(prompt_ids) :]
                return full_ids, labels
        except (TypeError, ValueError, AttributeError):
            pass

    prompt = f"{user_text}\n\nAnswer:\n"
    prompt_ids = list(tokenizer.encode(prompt, add_special_tokens=False))
    answer_ids = list(tokenizer.encode(answer, add_special_tokens=False))
    eos_id = getattr(tokenizer, "eos_token_id", None)
    if eos_id is not None:
        answer_ids.append(int(eos_id))
    input_ids = prompt_ids + answer_ids
    labels = [-100] * len(prompt_ids) + answer_ids
    return input_ids, labels


def iter_tokenized_examples(
    tokenizer: Any,
    records: Iterable[dict[str, Any]],
    *,
    max_length: int | None = None,
) -> Iterator[tuple[list[int], list[int]]]:
    for record in records:
        input_ids, labels = encode_squad_record(tokenizer, record)
        # Keep the question and answer at the end when one example exceeds the
        # context window. This also prevents prompt-only packed blocks with an
        # all--100 target, for which causal-LM cross entropy would be undefined.
        if max_length is not None and len(input_ids) > max_length:
            input_ids = input_ids[-max_length:]
            labels = labels[-max_length:]
        if input_ids and any(label != -100 for label in labels):
            yield input_ids, labels


def pack_tokenized_examples(
    examples: Iterable[tuple[list[int], list[int]]], sequence_length: int
) -> Iterator[PackedSequence]:
    """Concatenate examples into fixed-length blocks, retaining loss masks.

    The examples count is assigned to the block containing each example's first
    token, so cumulative counts are exact across emitted blocks.
    """
    if sequence_length <= 0:
        raise ValueError("sequence_length must be positive")
    ids_buffer: list[int] = []
    labels_buffer: list[int] = []
    starts_buffer: list[int] = []
    for input_ids, labels in examples:
        if len(input_ids) != len(labels):
            raise ValueError("input_ids and labels must have equal lengths")
        if not input_ids:
            continue
        ids_buffer.extend(int(value) for value in input_ids)
        labels_buffer.extend(int(value) for value in labels)
        starts_buffer.extend([1] + [0] * (len(input_ids) - 1))
        while len(ids_buffer) >= sequence_length:
            yield PackedSequence(
                input_ids=ids_buffer[:sequence_length],
                labels=labels_buffer[:sequence_length],
                examples=sum(starts_buffer[:sequence_length]),
            )
            del ids_buffer[:sequence_length]
            del labels_buffer[:sequence_length]
            del starts_buffer[:sequence_length]


def iter_training_batches(
    packed: Iterable[PackedSequence], batch_size: int
) -> Iterator[dict[str, Any]]:
    if batch_size <= 0:
        raise ValueError("batch_size must be positive")
    import torch

    iterator = iter(packed)
    while True:
        rows = [next(iterator) for _ in range(batch_size)]
        input_ids = torch.tensor([row.input_ids for row in rows], dtype=torch.long)
        labels = torch.tensor([row.labels for row in rows], dtype=torch.long)
        yield {
            "input_ids": input_ids,
            "labels": labels,
            "attention_mask": torch.ones_like(input_ids),
            "examples": sum(row.examples for row in rows),
            "tokens": input_ids.numel(),
            "train_tokens": int(labels.ne(-100).sum().item()),
        }


def should_stop(
    state: StopState,
    *,
    stop_file: str | None,
    started_at: float,
    duration_seconds: float | None,
    completed_steps: int,
    max_steps: int | None,
    now: float | None = None,
) -> str | None:
    if state.requested:
        return "signal"
    if stop_file and Path(stop_file).exists():
        return "stop_file"
    current = time.monotonic() if now is None else now
    if duration_seconds is not None and current - started_at >= duration_seconds:
        return "duration"
    if max_steps is not None and completed_steps >= max_steps:
        return "max_steps"
    return None


def _model_layers(model: Any) -> Sequence[Any]:
    candidates = (
        ("model", "layers"),
        ("transformer", "h"),
        ("gpt_neox", "layers"),
    )
    for path in candidates:
        value = model
        for name in path:
            value = getattr(value, name, None)
            if value is None:
                break
        if value is not None and len(value) > 0:
            return value
    raise ValueError("cannot locate transformer layers for trainable-subset mode")


def configure_trainable_parameters(model: Any, args: argparse.Namespace) -> dict[str, Any]:
    """Configure exactly the explicitly requested parameter-efficient mode."""
    requested_mode = args.training_mode
    if requested_mode == "lora":
        try:
            from peft import LoraConfig, TaskType, get_peft_model
        except ImportError as exc:
            raise RuntimeError(
                "--training-mode lora requires PEFT, but the 'peft' package is not installed"
            ) from exc
        targets = [item.strip() for item in args.lora_targets.split(",") if item.strip()]
        config = LoraConfig(
            task_type=TaskType.CAUSAL_LM,
            r=args.lora_rank,
            lora_alpha=args.lora_alpha,
            lora_dropout=args.lora_dropout,
            target_modules=targets,
            bias="none",
        )
        model = get_peft_model(model, config)
        effective_mode = "lora"
        details: dict[str, Any] = {
            "rank": args.lora_rank,
            "alpha": args.lora_alpha,
            "dropout": args.lora_dropout,
            "targets": targets,
        }
    elif requested_mode == "last-layers":
        for parameter in model.parameters():
            parameter.requires_grad_(False)
        layers = _model_layers(model)
        count = min(args.trainable_layers, len(layers))
        for layer in layers[-count:]:
            for parameter in layer.parameters():
                parameter.requires_grad_(True)
        final_norm = getattr(getattr(model, "model", None), "norm", None)
        if final_norm is not None:
            for parameter in final_norm.parameters():
                parameter.requires_grad_(True)
        effective_mode = "last-layers"
        details = {"layers": count, "final_norm": final_norm is not None}
    else:
        raise ValueError(f"unsupported training mode: {requested_mode}")

    trainable = sum(parameter.numel() for parameter in model.parameters() if parameter.requires_grad)
    total = sum(parameter.numel() for parameter in model.parameters())
    if trainable == 0:
        raise ValueError("parameter-efficient configuration selected no trainable parameters")
    details.update(
        {
            "requested_mode": requested_mode,
            "effective_mode": effective_mode,
            "trainable_parameters": trainable,
            "total_parameters": total,
            "trainable_fraction": trainable / total,
        }
    )
    return {"model": model, "details": details}


def make_data_iterator(args: argparse.Namespace, tokenizer: Any) -> Iterator[dict[str, Any]]:
    records = cycle_squad_records(args.parquet, max_records=args.max_records_per_epoch)
    examples = iter_tokenized_examples(
        tokenizer, records, max_length=args.sequence_length
    )
    packed = pack_tokenized_examples(examples, args.sequence_length)
    return iter_training_batches(packed, args.batch_size)


def _cuda_metrics(torch: Any, device: Any, reservation_bytes: int) -> dict[str, int]:
    allocated = int(torch.cuda.memory_allocated(device))
    return {
        "allocated_bytes": allocated,
        "reserved_bytes": int(torch.cuda.memory_reserved(device)),
        "peak_allocated_bytes": int(torch.cuda.max_memory_allocated(device)),
        "peak_reserved_bytes": int(torch.cuda.max_memory_reserved(device)),
        "padding_reservation_bytes": reservation_bytes,
        "allocated_excluding_padding_bytes": max(0, allocated - reservation_bytes),
    }


def cuda_reservation_metadata(requested_bytes: int, allocated_bytes: int) -> dict[str, Any]:
    """Describe padding without claiming knowledge hidden by allocator interposition."""
    conversion_setting = os.environ.get("SELECT_UVM_CONVERT_ALLOC")
    if conversion_setting == "1":
        managed_status = "possible_select_uvm_conversion_requested"
    else:
        managed_status = "no_select_uvm_conversion_requested_not_independently_verified"
    return {
        "requested_bytes": requested_bytes,
        "allocated_bytes": allocated_bytes,
        "allocation_request": "ordinary_torch_cuda_tensor",
        "select_uvm_convert_alloc": conversion_setting,
        "managed_memory_status": managed_status,
        "note": (
            "torch requests an ordinary CUDA allocation; when "
            "SELECT_UVM_CONVERT_ALLOC=1, project interposition may convert the "
            "underlying cuMemAlloc_v2 allocation to managed memory"
        ),
    }


def run_dry_run(args: argparse.Namespace, stop_state: StopState) -> int:
    from transformers import AutoTokenizer

    seed_everything(args.seed, include_cuda=False)
    tokenizer = AutoTokenizer.from_pretrained(
        args.model, local_files_only=True, trust_remote_code=False
    )
    batches = make_data_iterator(args, tokenizer)
    started = time.monotonic()
    cumulative_examples = 0
    cumulative_tokens = 0
    with JsonlWriter(args.output) as writer:
        writer.write(
            {
                "event": "config",
                "dry_run": True,
                "model": args.model,
                "parquet": args.parquet,
                "sequence_length": args.sequence_length,
                "batch_size": args.batch_size,
                "seed": args.seed,
                "training_mode_requested": args.training_mode,
                "training_mode_effective": None,
                "trainable_parameters": None,
            }
        )
        reason = None
        completed_batches = 0
        for _ in range(args.dry_run_steps):
            reason = should_stop(
                stop_state,
                stop_file=args.stop_file,
                started_at=started,
                duration_seconds=args.duration_seconds,
                completed_steps=completed_batches,
                max_steps=args.max_steps,
            )
            if reason:
                break
            batch = next(batches)
            completed_batches += 1
            cumulative_examples += batch["examples"]
            cumulative_tokens += batch["tokens"]
            writer.write(
                {
                    "event": "dry_run_batch",
                    "batch": completed_batches,
                    "examples": batch["examples"],
                    "tokens": batch["tokens"],
                    "train_tokens": batch["train_tokens"],
                    "cumulative_examples": cumulative_examples,
                    "cumulative_tokens": cumulative_tokens,
                    "shape": list(batch["input_ids"].shape),
                }
            )
        writer.write(
            {
                "event": "shutdown",
                "dry_run": True,
                "reason": reason or "dry_run_complete",
                "batches": completed_batches,
            }
        )
    return 0


def run_training(args: argparse.Namespace, stop_state: StopState) -> int:
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer

    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is required unless --dry-run is used")
    if not torch.cuda.is_bf16_supported():
        raise RuntimeError("CUDA device 0 does not support BF16")
    device = torch.device("cuda:0")
    torch.cuda.set_device(device)
    seed_everything(args.seed, include_cuda=True)

    tokenizer = AutoTokenizer.from_pretrained(
        args.model, local_files_only=True, trust_remote_code=False
    )
    model = AutoModelForCausalLM.from_pretrained(
        args.model,
        local_files_only=True,
        trust_remote_code=False,
        dtype=torch.bfloat16,
    )
    model.config.use_cache = False
    if hasattr(model, "gradient_checkpointing_disable"):
        model.gradient_checkpointing_disable()
    configured = configure_trainable_parameters(model, args)
    model = configured["model"].to(device)
    model.train()

    trainable_parameters = [p for p in model.parameters() if p.requires_grad]
    optimizer = torch.optim.AdamW(
        trainable_parameters,
        lr=args.learning_rate,
        betas=(args.adam_beta1, args.adam_beta2),
        eps=args.adam_epsilon,
        weight_decay=args.weight_decay,
        fused=args.fused_adam,
    )
    batches = make_data_iterator(args, tokenizer)

    reservation = None
    reservation_bytes = 0
    if args.cuda_reserve_mib:
        requested = args.cuda_reserve_mib * 1024 * 1024
        reservation = torch.empty(requested, dtype=torch.uint8, device=device)
        reservation.fill_(0)
        reservation_bytes = reservation.numel() * reservation.element_size()

    torch.cuda.reset_peak_memory_stats(device)
    started = time.monotonic()
    completed_steps = 0
    cumulative_examples = 0
    cumulative_tokens = 0
    cumulative_train_tokens = 0
    reason = None

    with JsonlWriter(args.output) as writer:
        writer.write(
            {
                "event": "config",
                "monotonic_origin_seconds": started,
                "dry_run": False,
                "model": args.model,
                "parquet": args.parquet,
                "device": str(device),
                "dtype": "bfloat16",
                "sequence_length": args.sequence_length,
                "batch_size": args.batch_size,
                "gradient_accumulation": args.gradient_accumulation,
                "seed": args.seed,
                "training_mode_requested": args.training_mode,
                "training_mode_effective": configured["details"]["effective_mode"],
                "trainable_parameters": configured["details"]["trainable_parameters"],
                "parameter_efficiency": configured["details"],
                "checkpointing": "disabled",
                "cuda_padding_reservation": cuda_reservation_metadata(
                    args.cuda_reserve_mib * 1024 * 1024, reservation_bytes
                ),
            }
        )
        optimizer.zero_grad(set_to_none=True)
        while True:
            reason = should_stop(
                stop_state,
                stop_file=args.stop_file,
                started_at=started,
                duration_seconds=args.duration_seconds,
                completed_steps=completed_steps,
                max_steps=args.max_steps,
            )
            if reason:
                break

            step_started = time.monotonic()
            examples_this_step = 0
            tokens_this_step = 0
            train_tokens_this_step = 0
            loss_sum = 0.0
            data_seconds = 0.0
            compute_seconds = 0.0
            interrupted = False

            for _ in range(args.gradient_accumulation):
                data_started = time.monotonic()
                batch = next(batches)
                input_ids = batch["input_ids"].to(device, non_blocking=False)
                labels = batch["labels"].to(device, non_blocking=False)
                attention_mask = batch["attention_mask"].to(device, non_blocking=False)
                data_seconds += time.monotonic() - data_started

                compute_started = time.monotonic()
                with torch.autocast(device_type="cuda", dtype=torch.bfloat16):
                    output = model(
                        input_ids=input_ids,
                        labels=labels,
                        attention_mask=attention_mask,
                        use_cache=False,
                    )
                    raw_loss = output.loss
                    loss = raw_loss / args.gradient_accumulation
                loss.backward()
                torch.cuda.synchronize(device)
                compute_seconds += time.monotonic() - compute_started

                loss_sum += float(raw_loss.detach().float().item())
                examples_this_step += batch["examples"]
                tokens_this_step += batch["tokens"]
                train_tokens_this_step += batch["train_tokens"]
                if stop_state.requested:
                    interrupted = True
                    break

            if interrupted:
                optimizer.zero_grad(set_to_none=True)
                reason = "signal"
                break

            optimizer_started = time.monotonic()
            if args.max_grad_norm > 0:
                torch.nn.utils.clip_grad_norm_(trainable_parameters, args.max_grad_norm)
            optimizer.step()
            optimizer.zero_grad(set_to_none=True)
            torch.cuda.synchronize(device)
            optimizer_seconds = time.monotonic() - optimizer_started
            step_end = time.monotonic()
            completed_steps += 1
            cumulative_examples += examples_this_step
            cumulative_tokens += tokens_this_step
            cumulative_train_tokens += train_tokens_this_step
            elapsed = step_end - started
            step_seconds = step_end - step_started

            writer.write(
                {
                    "event": "optimizer_step",
                    "step": completed_steps,
                    "elapsed_seconds": elapsed,
                    "step_start_elapsed_seconds": step_started - started,
                    "step_end_elapsed_seconds": elapsed,
                    "step_seconds": step_seconds,
                    "data_seconds": data_seconds,
                    "forward_backward_seconds": compute_seconds,
                    "optimizer_seconds": optimizer_seconds,
                    "examples": examples_this_step,
                    "tokens": tokens_this_step,
                    "train_tokens": train_tokens_this_step,
                    "loss": loss_sum / args.gradient_accumulation,
                    "cumulative_examples": cumulative_examples,
                    "cumulative_tokens": cumulative_tokens,
                    "cumulative_train_tokens": cumulative_train_tokens,
                    "step_examples_per_second": examples_this_step / step_seconds,
                    "step_tokens_per_second": tokens_this_step / step_seconds,
                    "cumulative_examples_per_second": cumulative_examples / elapsed,
                    "cumulative_tokens_per_second": cumulative_tokens / elapsed,
                    "cuda": _cuda_metrics(torch, device, reservation_bytes),
                }
            )

        writer.write(
            {
                "event": "shutdown",
                "reason": reason or "unknown",
                "signal": stop_state.signal_number,
                "completed_steps": completed_steps,
                "elapsed_seconds": time.monotonic() - started,
                "cumulative_examples": cumulative_examples,
                "cumulative_tokens": cumulative_tokens,
                "cuda": _cuda_metrics(torch, device, reservation_bytes),
            }
        )

    del reservation
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--parquet", action="append", default=None)
    parser.add_argument("--output", default="offline_train_metrics.jsonl")
    parser.add_argument("--stop-file")
    parser.add_argument("--duration-seconds", type=float, default=3600.0)
    parser.add_argument("--max-steps", type=int)
    parser.add_argument("--seed", type=int, default=2025)
    parser.add_argument("--sequence-length", type=int, default=1024)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--gradient-accumulation", type=int, default=8)
    parser.add_argument("--max-records-per-epoch", type=int)
    parser.add_argument("--learning-rate", type=float, default=2e-5)
    parser.add_argument("--weight-decay", type=float, default=0.01)
    parser.add_argument("--adam-beta1", type=float, default=0.9)
    parser.add_argument("--adam-beta2", type=float, default=0.95)
    parser.add_argument("--adam-epsilon", type=float, default=1e-8)
    parser.add_argument("--max-grad-norm", type=float, default=1.0)
    parser.add_argument("--fused-adam", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument(
        "--training-mode",
        required=True,
        choices=("last-layers", "lora"),
        help="Explicitly select final-layer subset training or PEFT LoRA.",
    )
    parser.add_argument("--lora-rank", type=int, default=16)
    parser.add_argument("--lora-alpha", type=int, default=32)
    parser.add_argument("--lora-dropout", type=float, default=0.0)
    parser.add_argument(
        "--lora-targets", default="q_proj,k_proj,v_proj,o_proj,gate_proj,up_proj,down_proj"
    )
    parser.add_argument("--trainable-layers", type=int, default=1)
    parser.add_argument(
        "--cuda-reserve-mib",
        type=int,
        default=0,
        help=(
            "Reserve this many MiB by requesting an ordinary CUDA tensor. Project "
            "interposition may convert the underlying allocation to managed memory "
            "when SELECT_UVM_CONVERT_ALLOC=1; metadata reports this separately."
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--dry-run-steps", type=int, default=2)
    return parser


def validate_args(args: argparse.Namespace) -> None:
    if args.parquet is None:
        args.parquet = [DEFAULT_PARQUET]
    for path in [args.model, *args.parquet]:
        if not Path(path).exists():
            raise ValueError(f"local input does not exist: {path}")

    finite_values = {
        "duration_seconds": args.duration_seconds,
        "learning_rate": args.learning_rate,
        "weight_decay": args.weight_decay,
        "adam_beta1": args.adam_beta1,
        "adam_beta2": args.adam_beta2,
        "adam_epsilon": args.adam_epsilon,
        "max_grad_norm": args.max_grad_norm,
        "lora_dropout": args.lora_dropout,
    }
    for name, value in finite_values.items():
        if value is not None and not math.isfinite(value):
            raise ValueError(f"{name} must be finite")

    positive = {
        "sequence_length": args.sequence_length,
        "batch_size": args.batch_size,
        "gradient_accumulation": args.gradient_accumulation,
        "learning_rate": args.learning_rate,
        "adam_epsilon": args.adam_epsilon,
        "lora_rank": args.lora_rank,
        "lora_alpha": args.lora_alpha,
        "trainable_layers": args.trainable_layers,
    }
    for name, value in positive.items():
        if value <= 0:
            raise ValueError(f"{name} must be positive")
    if args.duration_seconds is not None and args.duration_seconds < 0:
        raise ValueError("duration_seconds must be non-negative")
    if args.weight_decay < 0 or args.max_grad_norm < 0:
        raise ValueError("weight_decay and max_grad_norm must be non-negative")
    if not 0 <= args.adam_beta1 < 1 or not 0 <= args.adam_beta2 < 1:
        raise ValueError("Adam beta values must be in [0, 1)")
    if not 0 <= args.lora_dropout < 1:
        raise ValueError("lora_dropout must be in [0, 1)")
    if args.max_steps is not None and args.max_steps < 0:
        raise ValueError("max_steps must be non-negative")
    if args.max_records_per_epoch is not None and args.max_records_per_epoch <= 0:
        raise ValueError("max_records_per_epoch must be positive")
    if args.cuda_reserve_mib < 0 or args.dry_run_steps < 0:
        raise ValueError("reservation and dry-run step counts must be non-negative")
    if args.training_mode == "lora" and importlib.util.find_spec("peft") is None:
        raise RuntimeError(
            "--training-mode lora requires PEFT, but the 'peft' package is not installed"
        )


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    validate_args(args)
    stop_state = StopState()

    def handle_signal(signum: int, _frame: Any) -> None:
        stop_state.request(signum)

    previous_handlers: dict[int, Any] = {}
    for signum in (signal.SIGTERM, signal.SIGINT):
        previous_handlers[signum] = signal.signal(signum, handle_signal)
    try:
        if args.dry_run:
            return run_dry_run(args, stop_state)
        return run_training(args, stop_state)
    finally:
        for signum, handler in previous_handlers.items():
            with contextlib.suppress(ValueError):
                signal.signal(signum, handler)


if __name__ == "__main__":
    raise SystemExit(main())
