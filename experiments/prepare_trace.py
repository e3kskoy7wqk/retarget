#!/usr/bin/env python3
"""Prepare a deterministic BurstGPT replay manifest from local datasets."""

from __future__ import annotations

import argparse
import csv
import hashlib
import itertools
import json
import math
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator, Mapping, Sequence

DEFAULT_TRACE = Path(__file__).resolve().parents[1] / "third_party" / "BurstGPT_1.csv"
DEFAULT_SQUAD = Path("data/squad/train-00000-of-00001.parquet")
DEFAULT_TOKENIZER = Path("models/Qwen3-14B")


@dataclass(frozen=True)
class TraceRequest:
    source_row: int
    timestamp: float
    model: str
    input_tokens: int
    output_tokens: int
    log_type: str


def _nonnegative_int(value: str) -> int:
    parsed = int(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must be non-negative")
    return parsed


def _positive_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return parsed


def _rate(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or not 0.0 <= parsed <= 1.0:
        raise argparse.ArgumentTypeError("must be between zero and one")
    return parsed


def read_trace(path: Path) -> Iterator[TraceRequest]:
    """Yield validated BurstGPT rows in source order using constant memory."""
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        required = {
            "Timestamp",
            "Model",
            "Request tokens",
            "Response tokens",
            "Log Type",
        }
        missing = required.difference(reader.fieldnames or ())
        if missing:
            raise ValueError(f"trace is missing required columns: {sorted(missing)}")
        previous_timestamp = -math.inf
        for source_row, row in enumerate(reader, start=1):
            try:
                timestamp = float(row["Timestamp"])
                input_tokens = int(row["Request tokens"])
                output_tokens = int(row["Response tokens"])
            except (TypeError, ValueError) as exc:
                raise ValueError(f"invalid trace values on data row {source_row}") from exc
            if not math.isfinite(timestamp) or timestamp < previous_timestamp:
                raise ValueError(f"timestamps are not monotonic on data row {source_row}")
            if input_tokens < 0 or output_tokens < 0:
                raise ValueError(f"negative token count on data row {source_row}")
            previous_timestamp = timestamp
            yield TraceRequest(
                source_row=source_row,
                timestamp=timestamp,
                model=row["Model"],
                input_tokens=input_tokens,
                output_tokens=output_tokens,
                log_type=row["Log Type"],
            )


def _hash_keep(request: TraceRequest, rate: float, seed: int) -> bool:
    key = (
        f"{seed}\0{request.source_row}\0{request.timestamp:.9f}\0"
        f"{request.input_tokens}\0{request.output_tokens}"
    ).encode("utf-8")
    value = int.from_bytes(hashlib.sha256(key).digest()[:8], "big")
    return value < int(rate * (1 << 64))


def select_requests(
    requests: Iterable[TraceRequest],
    *,
    start_time: float | None,
    end_time: float | None,
    duration: float | None,
    thin_rate: float,
    thin_method: str,
    seed: int,
) -> tuple[float, list[TraceRequest]]:
    """Select a half-open time window and deterministically thin eligible rows."""
    if end_time is not None and duration is not None:
        raise ValueError("end_time and duration are mutually exclusive")
    if thin_method not in {"hash", "random"}:
        raise ValueError("thin_method must be 'hash' or 'random'")
    if not 0.0 <= thin_rate <= 1.0:
        raise ValueError("thin_rate must be between zero and one")

    iterator = iter(requests)
    try:
        first = next(iterator)
    except StopIteration:
        raise ValueError("trace contains no requests") from None

    window_start = first.timestamp if start_time is None else start_time
    if not math.isfinite(window_start):
        raise ValueError("start_time must be finite")
    if end_time is not None and not math.isfinite(end_time):
        raise ValueError("end_time must be finite")
    if duration is not None and (not math.isfinite(duration) or duration <= 0):
        raise ValueError("duration must be finite and greater than zero")
    window_end = end_time if end_time is not None else (
        window_start + duration if duration is not None else math.inf
    )
    if window_end <= window_start:
        raise ValueError("the time window must have positive duration")

    rng = random.Random(seed)
    selected: list[TraceRequest] = []
    for request in itertools.chain((first,), iterator):
        if request.timestamp < window_start:
            continue
        if request.timestamp >= window_end:
            # Keep consuming so read_trace still validates the complete CSV.
            continue
        if request.output_tokens == 0:
            continue
        if thin_method == "hash":
            keep = _hash_keep(request, thin_rate, seed)
        else:
            keep = rng.random() < thin_rate
        if keep:
            selected.append(request)
    return window_start, selected


def load_squad_records(path: Path) -> list[dict[str, str]]:
    """Load only the textual SQuAD columns from a local parquet file."""
    import pyarrow.parquet as pq

    table = pq.read_table(path, columns=["id", "title", "context", "question"])
    records = []
    for row in table.to_pylist():
        records.append({key: str(row[key]) for key in ("id", "title", "context", "question")})
    if not records:
        raise ValueError("SQuAD parquet contains no records")
    return records


def load_tokenizer(path: Path) -> Any:
    from transformers import AutoTokenizer

    return AutoTokenizer.from_pretrained(str(path), local_files_only=True)


def _encode(tokenizer: Any, text: str) -> list[int]:
    return list(tokenizer.encode(text, add_special_tokens=False))


def _decode(tokenizer: Any, token_ids: Sequence[int]) -> str:
    return tokenizer.decode(
        list(token_ids), skip_special_tokens=True, clean_up_tokenization_spaces=False
    )


def build_prompt(
    target_tokens: int,
    request_key: str,
    records: Sequence[Mapping[str, str]],
    tokenizer: Any,
    seed: int,
) -> tuple[str, list[str]]:
    """Build a deterministic SQuAD-derived prompt of exactly target_tokens tokens."""
    if target_tokens == 0:
        return "", []
    if not records:
        raise ValueError("at least one SQuAD record is required")

    digest = hashlib.sha256(f"{seed}\0{request_key}".encode("utf-8")).digest()
    index = int.from_bytes(digest[:8], "big") % len(records)
    pieces: list[str] = []
    source_ids: list[str] = []
    token_ids: list[int] = []
    while len(token_ids) < target_tokens:
        record = records[index]
        source_ids.append(record["id"])
        pieces.append(
            "Title: {title}\nContext: {context}\nQuestion: {question}\nAnswer:".format(
                title=record["title"],
                context=record["context"],
                question=record["question"],
            )
        )
        token_ids = _encode(tokenizer, "\n\n".join(pieces))
        index = (index + 1) % len(records)

    prompt = _decode(tokenizer, token_ids[:target_tokens])
    actual = _encode(tokenizer, prompt)
    if len(actual) != target_tokens:
        # Byte-level tokenizers normally round-trip. Fail rather than silently
        # misrepresenting a requested prompt length for a different tokenizer.
        raise ValueError(
            f"tokenizer decode/encode did not preserve the requested length "
            f"({target_tokens} became {len(actual)})"
        )
    return prompt, source_ids


def prepare_manifest(
    requests: Sequence[TraceRequest],
    *,
    window_start: float,
    time_compression: float,
    max_input_tokens: int,
    max_output_tokens: int,
    records: Sequence[Mapping[str, str]],
    tokenizer: Any,
    seed: int,
) -> Iterator[dict[str, Any]]:
    """Convert selected trace rows to replay manifest objects."""
    if not math.isfinite(time_compression) or time_compression <= 0:
        raise ValueError("time_compression must be finite and greater than zero")
    for request in requests:
        requested_input = min(request.input_tokens, max_input_tokens)
        requested_output = min(request.output_tokens, max_output_tokens)
        if requested_output <= 0:
            continue
        request_id = f"burstgpt-{request.source_row:07d}"
        prompt, squad_ids = build_prompt(
            requested_input, request_id, records, tokenizer, seed
        )
        arrival_time = (request.timestamp - window_start) / time_compression
        if not math.isfinite(arrival_time):
            raise ValueError("time compression produced a non-finite arrival time")
        yield {
            "request_id": request_id,
            "arrival_time_s": arrival_time,
            "requested_input_tokens": requested_input,
            "requested_output_tokens": requested_output,
            "prompt": prompt,
            "source": {
                "trace": {
                    "file": "BurstGPT_1.csv",
                    "row": request.source_row,
                    "timestamp_s": request.timestamp,
                    "model": request.model,
                    "log_type": request.log_type,
                    "original_input_tokens": request.input_tokens,
                    "original_output_tokens": request.output_tokens,
                },
                "prompt": {
                    "dataset": "SQuAD",
                    "record_ids": squad_ids,
                },
            },
        }


def write_jsonl(path: Path, rows: Iterable[Mapping[str, Any]]) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with path.open("w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")))
            handle.write("\n")
            count += 1
    return count


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Prepare a deterministic, SQuAD-backed BurstGPT JSONL replay manifest."
    )
    parser.add_argument("--trace", type=Path, default=DEFAULT_TRACE, help="local BurstGPT CSV")
    parser.add_argument("--squad", type=Path, default=DEFAULT_SQUAD, help="local SQuAD parquet")
    parser.add_argument(
        "--tokenizer", type=Path, default=DEFAULT_TOKENIZER, help="local Qwen tokenizer directory"
    )
    parser.add_argument("--output", type=Path, required=True, help="output JSONL manifest")
    parser.add_argument("--start-time", type=float, default=None, help="inclusive source timestamp; default: first row")
    window = parser.add_mutually_exclusive_group()
    window.add_argument("--end-time", type=float, default=None, help="exclusive source timestamp")
    window.add_argument("--duration", type=_positive_float, default=None, help="window length in source seconds (default: 3600)")
    parser.add_argument("--thin-rate", type=_rate, default=1.0, help="fraction to retain (default: 1)")
    parser.add_argument("--thin-method", choices=("hash", "random"), default="hash")
    parser.add_argument("--seed", type=int, default=0, help="deterministic thinning/prompt seed")
    parser.add_argument(
        "--time-compression", type=_positive_float, default=1.0,
        help="divide source arrival times by this factor (default: 1)",
    )
    parser.add_argument("--max-input-tokens", type=_nonnegative_int, default=4096)
    parser.add_argument("--max-output-tokens", type=_nonnegative_int, default=1024)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.max_output_tokens == 0:
        raise SystemExit("--max-output-tokens must be greater than zero")
    window_start, selected = select_requests(
        read_trace(args.trace),
        start_time=args.start_time,
        end_time=args.end_time,
        duration=(3600.0 if args.end_time is None and args.duration is None else args.duration),
        thin_rate=args.thin_rate,
        thin_method=args.thin_method,
        seed=args.seed,
    )
    records = load_squad_records(args.squad)
    tokenizer = load_tokenizer(args.tokenizer)
    count = write_jsonl(
        args.output,
        prepare_manifest(
            selected,
            window_start=window_start,
            time_compression=args.time_compression,
            max_input_tokens=args.max_input_tokens,
            max_output_tokens=args.max_output_tokens,
            records=records,
            tokenizer=tokenizer,
            seed=args.seed,
        ),
    )
    print(f"wrote {count} requests to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
