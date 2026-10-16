"""Open-loop replay with explicit native SGLang output-token arrival times."""
from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from typing import Any

from experiments.replay_online import (
    _base_result, build_parser, count_tokens, iter_sse_data, load_manifest,
    load_tokenizer, sleep_until,
)


class TokenArrivals:
    """Validate cumulative token IDs and record when each becomes client-visible."""

    def __init__(self):
        self.ids: list[int] = []
        self.times: list[float] = []
        self.events: list[dict[str, Any]] = []
        self.meta: dict[str, Any] = {}

    def observe(self, event: dict[str, Any], timestamp: float) -> None:
        if event.get("error"):
            raise ValueError(f"server stream error: {event['error']}")
        ids = event.get("output_ids")
        if not isinstance(ids, list) or any(type(x) is not int for x in ids):
            raise ValueError("native stream must supply cumulative integer output_ids")
        if ids[:len(self.ids)] != self.ids:
            raise ValueError("native stream rewrote or truncated previous token IDs")
        if self.times and timestamp < self.times[-1]:
            raise ValueError("token arrival clock moved backwards")
        new_count = len(ids) - len(self.ids)
        if new_count:
            self.events.append({"time_s": timestamp, "new_tokens": new_count})
            self.times.extend([timestamp] * new_count)
            self.ids = list(ids)
        meta = event.get("meta_info")
        if not isinstance(meta, dict):
            raise ValueError("native stream is missing meta_info")
        self.meta = meta

    def finish(self, requested: int, ignore_eos: bool) -> None:
        count = self.meta.get("completion_tokens")
        if not self.ids or count != len(self.ids):
            raise ValueError(f"token accounting mismatch: usage={count}, ids={len(self.ids)}")
        if ignore_eos and count != requested:
            raise ValueError(f"generated {count} of {requested} requested tokens with ignore_eos")
        if self.meta.get("finish_reason") is None:
            raise ValueError("native stream ended without a finish reason")


async def replay_one(item, *, session, replay_start, endpoint, tokenizer,
                     ignore_eos=True, timeout_s=600.0, api_key=None,
                     clock=time.monotonic, sleep=asyncio.sleep):
    scheduled = float(item["arrival_time_s"])
    result = _base_result(item, scheduled)
    result.update(replay_start_monotonic_s=replay_start,
                  itl_measurement="native_output_token_arrival_interval",
                  tbt_measurement="client_output_token_arrival_interval")
    arrivals = TokenArrivals()
    await sleep_until(replay_start + scheduled, clock, sleep)
    try:
        remaining = replay_start + scheduled + timeout_s - clock()
        if remaining <= 0:
            raise TimeoutError("request deadline expired before submission")
        async with asyncio.timeout(remaining):
            result["actual_input_tokens"] = item.get("_actual_input_tokens")
            if result["actual_input_tokens"] is None:
                result["actual_input_tokens"] = count_tokens(tokenizer, item["prompt"])
            headers = {"Content-Type": "application/json"}
            if api_key:
                headers["Authorization"] = f"Bearer {api_key}"
            payload = {
                "text": item["prompt"], "rid": item["request_id"], "stream": True,
                "sampling_params": {
                    "max_new_tokens": item["requested_output_tokens"],
                    "temperature": 0.0, "top_p": 1.0, "ignore_eos": ignore_eos,
                },
            }
            submitted = clock() - replay_start
            result.update(submitted_time_s=submitted,
                          queue_delay_s=max(0.0, submitted - scheduled))
            done = False
            async with session.post(endpoint, json=payload, headers=headers) as response:
                result["http_status"] = int(response.status)
                if response.status != 200:
                    raise RuntimeError(f"HTTP {response.status}: {(await response.text())[:1000]}")
                async for data in iter_sse_data(response.content.iter_any()):
                    timestamp = clock() - replay_start
                    if data == "[DONE]":
                        done = True
                        break
                    arrivals.observe(json.loads(data), timestamp)
            if not done:
                raise ValueError("native stream closed before [DONE]")
            arrivals.finish(item["requested_output_tokens"], ignore_eos)
            result["status"] = "ok"
    except asyncio.CancelledError:
        raise
    except Exception as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    completed = clock() - replay_start
    result.update(completed_time_s=completed,
                  total_latency_s=completed - scheduled,
                  actual_output_tokens=len(arrivals.ids),
                  output_token_ids=arrivals.ids,
                  token_arrival_times_s=arrivals.times,
                  token_events=arrivals.events,
                  stream_event_count=len(arrivals.events),
                  coalesced_token_events=sum(x["new_tokens"] > 1 for x in arrivals.events),
                  coalesced_internal_intervals=sum(max(0, x["new_tokens"] - 1) for x in arrivals.events),
                  server_usage=arrivals.meta)
    if result["submitted_time_s"] is not None:
        result["e2e_s"] = completed - result["submitted_time_s"]
    if arrivals.times:
        result["first_token_time_s"] = arrivals.times[0]
        result["ttft_s"] = arrivals.times[0] - result["submitted_time_s"]
    intervals = [b - a for a, b in zip(arrivals.times, arrivals.times[1:])]
    result["itls_s"] = intervals
    result["tbt_s"] = intervals
    return result


async def replay(args):
    import aiohttp

    requests = load_manifest(args.manifest)
    tokenizer = load_tokenizer(args.tokenizer)
    for item in requests:
        item["_actual_input_tokens"] = count_tokens(tokenizer, item["prompt"])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    connector = aiohttp.TCPConnector(limit=0)
    async with aiohttp.ClientSession(connector=connector,
                                   timeout=aiohttp.ClientTimeout(total=None)) as session:
        start = time.monotonic()
        with args.output.open("w") as handle:
            async def run(item):
                result = await replay_one(
                    item, session=session, replay_start=start,
                    endpoint=args.base_url.rstrip("/") + "/generate",
                    tokenizer=tokenizer, ignore_eos=not args.respect_eos,
                    timeout_s=args.timeout, api_key=args.api_key)
                handle.write(json.dumps(result, ensure_ascii=False, separators=(",", ":")) + "\n")
                handle.flush()
            await asyncio.gather(*(run(item) for item in requests))
    print(f"wrote {len(requests)} token-timed results to {args.output}")


def main():
    parser = build_parser()
    parser.description = __doc__
    asyncio.run(replay(parser.parse_args()))


if __name__ == "__main__":
    main()
