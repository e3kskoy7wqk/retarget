"""Bounded, causal prefetch windows for homogeneous per-layer KV allocations.

The serving-layout adapter uses completed-token counters to estimate bounded
prefetch ranges. It does not track slot reuse and stops at the aligned capacity
limit. Other accesses remain under demand UVM.
"""
from __future__ import annotations

import dataclasses
import math
import time
from typing import Mapping

from experiments.control import ControlError, UvmController

INITIAL = (1 << 64) - 1


@dataclasses.dataclass(frozen=True)
class CacheWindowSpec:
    allocation_bytes: int
    allocation_count: int
    bytes_per_token: int
    width_tokens: int = 32768
    lookahead_tokens: int = 16384
    quantum_tokens: int = 8192
    min_update_seconds: float = 1.0
    idle_only: bool = True
    update_mode: str = "incremental"

    def __post_init__(self):
        for name in ("allocation_bytes", "allocation_count", "bytes_per_token",
                     "width_tokens", "lookahead_tokens", "quantum_tokens"):
            value = getattr(self, name)
            if type(value) is not int or value <= 0:
                raise ValueError(f"cache_window.{name} must be a positive integer")
        if self.lookahead_tokens >= self.width_tokens:
            raise ValueError("cache window must retain a trailing region")
        if self.quantum_tokens * self.bytes_per_token % 4096:
            raise ValueError("cache window quantum must be page aligned")
        if self.width_tokens % self.quantum_tokens or self.lookahead_tokens % self.quantum_tokens:
            raise ValueError("cache window dimensions must be multiples of the quantum")
        if self.width_tokens * self.bytes_per_token > self.allocation_bytes:
            raise ValueError("cache window exceeds each allocation")
        if not math.isfinite(self.min_update_seconds) or self.min_update_seconds < 0:
            raise ValueError("invalid cache window update interval")
        if type(self.idle_only) is not bool:
            raise ValueError("cache_window.idle_only must be boolean")
        if self.update_mode not in {"incremental", "initial_only", "full_cache"}:
            raise ValueError("invalid cache_window.update_mode")

    def window(self, completed_tokens: float) -> tuple[int, int]:
        if not math.isfinite(completed_tokens) or completed_tokens < 0:
            raise ValueError("invalid completed-token count")
        # Recycled allocator addresses cannot be inferred from the counters.
        # Keep the final window bounded; all other accesses use normal UVM.
        q = self.quantum_tokens
        desired_end = max(self.width_tokens,
                          math.ceil((completed_tokens + self.lookahead_tokens) / q) * q)
        max_end = self.allocation_bytes // (q * self.bytes_per_token) * q
        end = min(desired_end, max_end)
        return ((end - self.width_tokens) * self.bytes_per_token,
                end * self.bytes_per_token)


def interval_difference(a: tuple[int, int], b: tuple[int, int]) -> list[tuple[int, int]]:
    return [(lo, hi) for lo, hi in ((a[0], min(a[1], b[0])),
                                   (max(a[0], b[1]), a[1])) if hi > lo]


class CacheWindowController:
    def __init__(self, spec: CacheWindowSpec):
        self.spec = spec
        self.current: tuple[int, int] | None = None
        self.signature: tuple | None = None
        self.last_update = -math.inf
        self.completed_highwater = 0.0

    def _counters(self, metrics: Mapping[str, float]) -> float:
        names = ("sglang:prompt_tokens_total", "sglang:generation_tokens_total")
        if any(name not in metrics for name in names):
            raise ControlError("cache-window policy requires SGLang completed-token counters")
        values = [metrics[name] for name in names]
        if any(not math.isfinite(v) or v < 0 for v in values):
            raise ControlError("invalid SGLang completed-token counters")
        self.completed_highwater = max(self.completed_highwater, sum(values))
        return self.completed_highwater

    def update(self, uvm: UvmController, metrics: Mapping[str, float], now: float,
               *, initial: bool = False) -> dict | None:
        completed = self._counters(metrics)
        window = self.spec.window(completed)
        if not initial:
            if self.spec.update_mode == "initial_only":
                return None
            if window == self.current or now - self.last_update < self.spec.min_update_seconds:
                return None
            if self.spec.idle_only and (metrics["sglang:num_running_reqs"] > 0 or
                                       metrics["sglang:num_queue_reqs"] > 0):
                return None
        uvm.wait_idle(timeout=600)
        generation, allocations = uvm._allocation_snapshot()
        cache = [a for a in allocations if a["bytes"] == self.spec.allocation_bytes]
        if len(cache) != self.spec.allocation_count:
            raise ControlError(f"cache layout mismatch: expected {self.spec.allocation_count} "
                               f"blocks of {self.spec.allocation_bytes}, found {len(cache)}")
        signature = tuple((a["id"], a["bytes"], a["device"]) for a in cache)
        full = initial or signature != self.signature or self.current is None
        old = (INITIAL, INITIAL) if full else self.current
        refresh = not full and self.spec.update_mode == "full_cache"
        command = "windowfullv" if refresh else "windowv"
        reply = uvm.command(f"{command} {generation} {self.spec.allocation_bytes} "
                            f"{self.spec.allocation_count} {old[0]} {old[1]} {window[0]} {window[1]}")
        if not reply.get("ok"):
            raise ControlError(f"cache window rejected: {reply}")
        try:
            status = uvm._wait_job(int(reply["job_id"]), timeout=600)
        except Exception:
            self.signature = None
            self.current = None
            raise
        if full:
            gpu_bytes = sum(a["bytes"] for a in allocations if a["bytes"] != self.spec.allocation_bytes)
            gpu_bytes += len(cache) * (window[1] - window[0])
            cpu_bytes = len(cache) * (self.spec.allocation_bytes - window[1] + window[0])
        elif refresh:
            gpu_bytes = len(cache) * (window[1] - window[0])
            cpu_bytes = len(cache) * (self.spec.allocation_bytes - window[1] + window[0])
        else:
            gpu_bytes = len(cache) * sum(hi - lo for lo, hi in interval_difference(window, old))
            cpu_bytes = len(cache) * sum(hi - lo for lo, hi in interval_difference(old, window))
        self.current, self.signature, self.last_update = window, signature, now
        return {"audit_plan_started_monotonic_s": now,
                "audit_plan_committed_monotonic_s": time.monotonic(),
                "initial": full, "update_mode": self.spec.update_mode,
                "full_cache_refresh": refresh, "allocation_generation": generation,
                "cache_allocation_count": len(cache), "cache_allocation_bytes": self.spec.allocation_bytes,
                "old_window": None if full else list(old), "new_window": list(window),
                "completed_token_highwater": completed, "requested_gpu_bytes": gpu_bytes,
                "requested_cpu_bytes": cpu_bytes, "status": dataclasses.asdict(status)}

    def due(self, metrics: Mapping[str, float], now: float) -> bool:
        if self.spec.update_mode == "initial_only":
            return False
        window = self.spec.window(self._counters(metrics))
        return (window != self.current and now - self.last_update >= self.spec.min_update_seconds
                and (not self.spec.idle_only or (metrics["sglang:num_running_reqs"] == 0
                                                and metrics["sglang:num_queue_reqs"] == 0)))
