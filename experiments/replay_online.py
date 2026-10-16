#!/usr/bin/env python3
"""Replay a prepared trace against SGLang's OpenAI-compatible completions API."""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import time
from pathlib import Path
from typing import Any, AsyncIterator, Awaitable, Callable, Iterable, Mapping, Sequence

DEFAULT_TOKENIZER = Path("models/Qwen3-14B")
Clock = Callable[[], float]
Sleep = Callable[[float], Awaitable[None]]


def _positive_float(value: str) -> float:
    parsed = float(value)
    if not math.isfinite(parsed) or parsed <= 0:
        raise argparse.ArgumentTypeError("must be a finite number greater than zero")
    return parsed


def _manifest_int(value: Any, field: str, line_number: int) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field} must be an integer on line {line_number}")
    return value


def load_manifest(path: Path) -> list[dict[str, Any]]:
    requests: list[dict[str, Any]] = []
    seen_ids: set[str] = set()
    with path.open("r", encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
                if not isinstance(item, dict):
                    raise ValueError("record must be a JSON object")
                request_id = item["request_id"]
                if not isinstance(request_id, str) or not request_id.strip():
                    raise ValueError("request_id must be a non-empty string")
                arrival_value = item["arrival_time_s"]
                if isinstance(arrival_value, bool):
                    raise ValueError("arrival_time_s must be numeric")
                arrival = float(arrival_value)
                output_tokens = _manifest_int(
                    item["requested_output_tokens"], "requested_output_tokens", line_number
                )
                input_tokens = _manifest_int(
                    item["requested_input_tokens"], "requested_input_tokens", line_number
                )
                prompt = item["prompt"]
            except (KeyError, TypeError, ValueError, json.JSONDecodeError) as exc:
                raise ValueError(f"invalid manifest record on line {line_number}") from exc
            if request_id in seen_ids:
                raise ValueError(f"duplicate request_id {request_id!r} on line {line_number}")
            if not math.isfinite(arrival) or arrival < 0:
                raise ValueError(f"invalid arrival_time_s on line {line_number}")
            if output_tokens <= 0 or input_tokens < 0 or not isinstance(prompt, str):
                raise ValueError(f"invalid token counts or prompt on line {line_number}")
            seen_ids.add(request_id)
            normalized = dict(item)
            normalized.update(
                {
                    "request_id": request_id,
                    "arrival_time_s": arrival,
                    "requested_input_tokens": input_tokens,
                    "requested_output_tokens": output_tokens,
                    "prompt": prompt,
                }
            )
            requests.append(normalized)
    return requests


def load_tokenizer(path: Path) -> Any:
    from transformers import AutoTokenizer

    return AutoTokenizer.from_pretrained(str(path), local_files_only=True)


def count_tokens(tokenizer: Any, text: str) -> int:
    return len(tokenizer.encode(text, add_special_tokens=False))


def _safe_count_tokens(tokenizer: Any, text: str) -> int | None:
    try:
        return count_tokens(tokenizer, text)
    except Exception:
        return None


def _usage_completion_tokens(usage: Mapping[str, Any] | None) -> int | None:
    if usage is None:
        return None
    value = usage.get("completion_tokens")
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        return None
    return value


async def iter_sse_data(byte_chunks: AsyncIterator[bytes]) -> AsyncIterator[str]:
    """Parse SSE data fields from arbitrarily split byte chunks."""
    buffer = b""
    async for chunk in byte_chunks:
        buffer += chunk
        buffer = buffer.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        while b"\n\n" in buffer:
            raw_event, buffer = buffer.split(b"\n\n", 1)
            data_lines = []
            for line in raw_event.split(b"\n"):
                if line.startswith(b"data:"):
                    data_lines.append(line[5:].lstrip().decode("utf-8"))
            if data_lines:
                yield "\n".join(data_lines)
    if buffer.strip():
        data_lines = []
        for line in buffer.split(b"\n"):
            if line.startswith(b"data:"):
                data_lines.append(line[5:].lstrip().decode("utf-8"))
        if data_lines:
            yield "\n".join(data_lines)


async def sleep_until(deadline: float, clock: Clock, sleep: Sleep) -> None:
    while True:
        delay = deadline - clock()
        if delay <= 0:
            return
        await sleep(delay)


def _base_result(item: Mapping[str, Any], scheduled: float) -> dict[str, Any]:
    return {
        "request_id": item["request_id"],
        "scheduled_time_s": scheduled,
        "submitted_time_s": None,
        "first_token_time_s": None,
        "completed_time_s": None,
        "queue_delay_s": None,
        "ttft_s": None,
        "e2e_s": None,
        "total_latency_s": None,
        "itls_s": [],
        "stream_event_count": 0,
        "itl_measurement": "nonempty_sse_emission_interval",
        "requested_input_tokens": int(item["requested_input_tokens"]),
        "requested_output_tokens": int(item["requested_output_tokens"]),
        "actual_input_tokens": None,
        "actual_output_tokens": 0,
        "retry_count": 0,
        "status": "error",
        "http_status": None,
        "error": None,
    }


async def replay_one(
    item: Mapping[str, Any],
    *,
    session: Any,
    replay_start: float,
    endpoint: str,
    model: str,
    tokenizer: Any,
    api_key: str | None,
    ignore_eos: bool,
    timeout_s: float = 600.0,
    clock: Clock = time.monotonic,
    sleep: Sleep = asyncio.sleep,
) -> dict[str, Any]:
    """Schedule and stream one request, returning a complete result record."""
    scheduled = float(item["arrival_time_s"])
    result = _base_result(item, scheduled)
    result["replay_start_monotonic_s"] = replay_start
    emission_times: list[float] = []
    output_text = ""
    server_usage: Mapping[str, Any] | None = None
    stream_done = False

    await sleep_until(replay_start + scheduled, clock, sleep)
    try:
        remaining = replay_start + scheduled + timeout_s - clock()
        if remaining <= 0:
            raise TimeoutError("request deadline expired before submission")
        async with asyncio.timeout(remaining):
            result["actual_input_tokens"] = (
                int(item["_actual_input_tokens"])
                if "_actual_input_tokens" in item
                else count_tokens(tokenizer, str(item["prompt"]))
            )
            headers = {"Content-Type": "application/json"}
            if api_key:
                headers["Authorization"] = f"Bearer {api_key}"
            payload = {
                "model": model,
                "prompt": item["prompt"],
                "max_tokens": int(item["requested_output_tokens"]),
                "temperature": 0.0,
                "top_p": 1.0,
                "stream": True,
                "stream_options": {"include_usage": True},
                "ignore_eos": ignore_eos,
                "rid": str(item["request_id"]),
            }
            import aiohttp

            transient_errors = (
                aiohttp.ServerDisconnectedError,
                aiohttp.ClientConnectionError,
                aiohttp.ServerTimeoutError,
            )
            submitted = clock() - replay_start
            result["submitted_time_s"] = submitted
            result["queue_delay_s"] = max(0.0, submitted - scheduled)
            for attempt in range(2):
                try:
                    async with session.post(endpoint, json=payload, headers=headers) as response:
                        result["http_status"] = int(response.status)
                        if response.status != 200:
                            body = await response.text()
                            raise RuntimeError(f"HTTP {response.status}: {body[:1000]}")
                        async for data in iter_sse_data(response.content.iter_any()):
                            if data == "[DONE]":
                                stream_done = True
                                break
                            try:
                                event = json.loads(data)
                            except json.JSONDecodeError as exc:
                                raise RuntimeError(f"invalid SSE JSON: {data[:200]}") from exc
                            if event.get("error"):
                                raise RuntimeError(f"server stream error: {event['error']}")
                            if event.get("usage") is not None:
                                if not isinstance(event["usage"], Mapping):
                                    raise RuntimeError("server returned malformed usage data")
                                server_usage = event["usage"]
                            choices = event.get("choices") or []
                            if not choices:
                                continue
                            delta = choices[0].get("text") or ""
                            if not delta:
                                continue
                            emission_times.append(clock() - replay_start)
                            output_text += delta
                    break
                except transient_errors:
                    if attempt == 0 and result["http_status"] is None and not emission_times:
                        result["retry_count"] = 1
                        continue
                    raise

            if not stream_done:
                raise RuntimeError("SSE stream closed before [DONE]")
            completed = clock() - replay_start
            usage_output = _usage_completion_tokens(server_usage)
            actual_output = (
                usage_output
                if usage_output is not None
                else count_tokens(tokenizer, output_text)
            )
            result["completed_time_s"] = completed
            result["e2e_s"] = completed - submitted
            result["total_latency_s"] = completed - scheduled
            result["actual_output_tokens"] = actual_output
            result["stream_event_count"] = len(emission_times)
            if emission_times:
                result["first_token_time_s"] = emission_times[0]
                result["ttft_s"] = emission_times[0] - submitted
                result["itls_s"] = [
                    emission_times[index] - emission_times[index - 1]
                    for index in range(1, len(emission_times))
                ]
            result["status"] = "ok"
            if server_usage is not None:
                result["server_usage"] = dict(server_usage)
    except asyncio.CancelledError:
        raise
    except Exception as exc:
        completed = clock() - replay_start
        result["completed_time_s"] = completed
        result["total_latency_s"] = completed - scheduled
        if result["submitted_time_s"] is not None:
            result["e2e_s"] = completed - result["submitted_time_s"]
        elif result["queue_delay_s"] is None:
            result["queue_delay_s"] = max(0.0, completed - scheduled)
        usage_output = _usage_completion_tokens(server_usage)
        safe_output_count = _safe_count_tokens(tokenizer, output_text)
        if usage_output is not None:
            result["actual_output_tokens"] = usage_output
        elif safe_output_count is not None:
            result["actual_output_tokens"] = safe_output_count
        result["stream_event_count"] = len(emission_times)
        if emission_times:
            result["first_token_time_s"] = emission_times[0]
            result["ttft_s"] = emission_times[0] - result["submitted_time_s"]
            result["itls_s"] = [
                emission_times[index] - emission_times[index - 1]
                for index in range(1, len(emission_times))
            ]
        result["status"] = "error"
        result["error"] = f"{type(exc).__name__}: {exc}"
    return result


async def replay_all(
    requests: Sequence[Mapping[str, Any]],
    *,
    output: Path,
    endpoint: str,
    model: str,
    tokenizer: Any,
    timeout_s: float,
    api_key: str | None,
    ignore_eos: bool,
    session_factory: Callable[..., Any] | None = None,
    clock: Clock = time.monotonic,
    sleep: Sleep = asyncio.sleep,
) -> list[dict[str, Any]]:
    """Replay all requests and write each result as soon as it completes."""
    import aiohttp

    output.parent.mkdir(parents=True, exist_ok=True)
    timeout = aiohttp.ClientTimeout(total=timeout_s)
    if session_factory is None:
        session_context = aiohttp.ClientSession(
            timeout=timeout, connector=aiohttp.TCPConnector(limit=0)
        )
    else:
        session_context = session_factory(timeout=timeout)
    writer_lock = asyncio.Lock()
    results: list[dict[str, Any]] = []

    async with session_context as session:
        with output.open("w", encoding="utf-8") as handle:
            prepared_requests = []
            for request in requests:
                prepared = dict(request)
                prepared["_actual_input_tokens"] = count_tokens(
                    tokenizer, str(request["prompt"])
                )
                prepared_requests.append(prepared)
            replay_start = clock()

            async def run_and_write(item: Mapping[str, Any]) -> None:
                result = await replay_one(
                    item,
                    session=session,
                    replay_start=replay_start,
                    endpoint=endpoint,
                    model=model,
                    tokenizer=tokenizer,
                    api_key=api_key,
                    ignore_eos=ignore_eos,
                    timeout_s=timeout_s,
                    clock=clock,
                    sleep=sleep,
                )
                async with writer_lock:
                    handle.write(json.dumps(result, ensure_ascii=False, separators=(",", ":")))
                    handle.write("\n")
                    handle.flush()
                    results.append(result)

            await asyncio.gather(*(run_and_write(item) for item in prepared_requests))
    return results


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Asynchronously replay a prepared JSONL trace against SGLang /v1/completions."
    )
    parser.add_argument("--manifest", type=Path, required=True, help="prepared input JSONL")
    parser.add_argument("--output", type=Path, required=True, help="per-request result JSONL")
    parser.add_argument(
        "--base-url", default="http://127.0.0.1:30000",
        help="SGLang server base URL (default: %(default)s)",
    )
    parser.add_argument("--model", default="default", help="OpenAI-compatible model field")
    parser.add_argument("--tokenizer", type=Path, default=DEFAULT_TOKENIZER)
    parser.add_argument("--timeout", type=_positive_float, default=600.0, help="per-request total timeout seconds")
    parser.add_argument("--api-key", default=None, help="optional bearer token")
    parser.add_argument(
        "--respect-eos", action="store_true",
        help="allow EOS to stop generation before the requested output length",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    requests = load_manifest(args.manifest)
    tokenizer = load_tokenizer(args.tokenizer)
    endpoint = args.base_url.rstrip("/") + "/v1/completions"
    asyncio.run(
        replay_all(
            requests,
            output=args.output,
            endpoint=endpoint,
            model=args.model,
            tokenizer=tokenizer,
            timeout_s=args.timeout,
            api_key=args.api_key,
            ignore_eos=not args.respect_eos,
        )
    )
    print(f"wrote {len(requests)} results to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
