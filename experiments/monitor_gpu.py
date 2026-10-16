#!/usr/bin/env python3
"""Sample NVML GPU and per-process residency metrics to JSONL."""

from __future__ import annotations

import argparse
import json
import math
import signal
import time
from pathlib import Path
from typing import Any, Sequence


def parse_named_pid(value: str) -> tuple[str, int]:
    if "=" not in value:
        raise argparse.ArgumentTypeError("expected NAME=PID")
    name, raw_pid = value.split("=", 1)
    if not name:
        raise argparse.ArgumentTypeError("process name must not be empty")
    try:
        pid = int(raw_pid)
    except ValueError as exc:
        raise argparse.ArgumentTypeError("PID must be an integer") from exc
    if pid <= 0:
        raise argparse.ArgumentTypeError("PID must be positive")
    return name, pid


def run(args: argparse.Namespace) -> int:
    import pynvml

    pynvml.nvmlInit()
    stop = False

    def request_stop(_signum: int, _frame: Any) -> None:
        nonlocal stop
        stop = True

    previous = {
        signum: signal.signal(signum, request_stop)
        for signum in (signal.SIGINT, signal.SIGTERM)
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    handle = pynvml.nvmlDeviceGetHandleByIndex(args.gpu)
    started = time.monotonic()
    try:
        with args.output.open("w", encoding="utf-8", buffering=1) as output:
            output.write(
                json.dumps(
                    {
                        "event": "config",
                        "monotonic_origin_seconds": started,
                        "gpu": args.gpu,
                        "gpu_name": pynvml.nvmlDeviceGetName(handle),
                        "poll_seconds": args.poll_seconds,
                        "pids": dict(args.pid),
                    },
                    sort_keys=True,
                )
                + "\n"
            )
            while not stop:
                loop_started = time.monotonic()
                elapsed = loop_started - started
                if args.duration_seconds is not None and elapsed >= args.duration_seconds:
                    break
                if args.stop_file and args.stop_file.exists():
                    break

                memory = pynvml.nvmlDeviceGetMemoryInfo(handle)
                utilization = pynvml.nvmlDeviceGetUtilizationRates(handle)
                try:
                    power_mw = int(pynvml.nvmlDeviceGetPowerUsage(handle))
                except pynvml.NVMLError:
                    power_mw = None
                try:
                    temperature_c = int(
                        pynvml.nvmlDeviceGetTemperature(
                            handle, pynvml.NVML_TEMPERATURE_GPU
                        )
                    )
                except pynvml.NVMLError:
                    temperature_c = None
                try:
                    pcie_tx_kib_s = int(
                        pynvml.nvmlDeviceGetPcieThroughput(
                            handle, pynvml.NVML_PCIE_UTIL_TX_BYTES
                        )
                    )
                    pcie_rx_kib_s = int(
                        pynvml.nvmlDeviceGetPcieThroughput(
                            handle, pynvml.NVML_PCIE_UTIL_RX_BYTES
                        )
                    )
                except pynvml.NVMLError:
                    pcie_tx_kib_s = pcie_rx_kib_s = None

                resident_by_pid: dict[int, int] = {}
                try:
                    for process in pynvml.nvmlDeviceGetComputeRunningProcesses(handle):
                        used = getattr(process, "usedGpuMemory", 0)
                        resident_by_pid[int(process.pid)] = int(used or 0)
                except pynvml.NVMLError:
                    pass

                record = {
                    "event": "sample",
                    "elapsed_seconds": elapsed,
                    "memory_used_bytes": int(memory.used),
                    "memory_free_bytes": int(memory.free),
                    "gpu_utilization_percent": int(utilization.gpu),
                    "memory_utilization_percent": int(utilization.memory),
                    "power_mw": power_mw,
                    "temperature_c": temperature_c,
                    "pcie_tx_kib_s": pcie_tx_kib_s,
                    "pcie_rx_kib_s": pcie_rx_kib_s,
                    "process_resident_bytes": {
                        name: resident_by_pid.get(pid, 0) for name, pid in args.pid
                    },
                    "all_compute_processes": {
                        str(pid): used for pid, used in resident_by_pid.items()
                    },
                }
                output.write(json.dumps(record, sort_keys=True, allow_nan=False) + "\n")
                output.flush()
                sleep_for = args.poll_seconds - (time.monotonic() - loop_started)
                if sleep_for > 0:
                    time.sleep(sleep_for)
            output.write(
                json.dumps(
                    {"event": "shutdown", "elapsed_seconds": time.monotonic() - started},
                    sort_keys=True,
                )
                + "\n"
            )
        return 0
    finally:
        pynvml.nvmlShutdown()
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pid", action="append", type=parse_named_pid, default=[])
    parser.add_argument("--poll-seconds", type=float, default=0.2)
    parser.add_argument("--duration-seconds", type=float)
    parser.add_argument("--stop-file", type=Path)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if args.gpu < 0:
        raise ValueError("gpu must be non-negative")
    for name in ("poll_seconds", "duration_seconds"):
        value = getattr(args, name)
        if value is not None and (not math.isfinite(value) or value <= 0):
            raise ValueError(f"{name} must be finite and positive")
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
