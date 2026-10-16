#!/usr/bin/env python3
"""Load-aware scheduler for colocated SGLang inference and offline fine-tuning.

The paper configurations keep both context timeslices fixed during replay.
Serving metrics determine placement and pause transitions, without observing
latency or SLO violations. The constant timeslice functions in the policy files
retain the control path used by the recorded experiments. Scheduler settings
take effect after model warmup and before replay.
"""

from __future__ import annotations

import argparse
import dataclasses
import json
import math
import signal
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Mapping, Sequence

from experiments.control import (
    DEFAULT_TIMESLICE_US,
    ControlError,
    ProcessController,
    UvmController,
    wait_for_instrumented_process,
)
from experiments.cache_window import CacheWindowController, CacheWindowSpec

MIB = 1024 * 1024
STATE_ORDER = {"idle": 0, "low": 1, "high": 2, "overload": 3}


@dataclasses.dataclass(frozen=True)
class StateSpec:
    online_target_mib: int
    offline_target_mib: int
    offline_timeslice_us: int
    pause_offline: bool = False
    refresh_on_entry: bool = True
    manage_online_target: bool = True
    manage_offline_target: bool = True
    wait_for_migration: bool = True
    incremental_migration: bool = False
    entry_pause_seconds: float = 0.0


@dataclasses.dataclass(frozen=True)
class LoadThresholds:
    high_running: int = 4
    overload_queue: int = 16
    overload_running: int = 32
    arrival_guard_seconds: float = 0.0


@dataclasses.dataclass(frozen=True)
class TimesliceFunction:
    base_us: int = 2048
    running_weight: float = 1.0
    queue_weight: float = 4.0
    min_us: int = 2048
    max_us: int = 2048
    quantum_us: int = 128
    min_update_seconds: float = 0.25
    relative_change: float = 0.1
    recovery_hold_seconds: float = 1.0
    skip_recovery_hold: bool = False

    def __post_init__(self) -> None:
        for name in ("base_us", "min_us", "max_us", "quantum_us"):
            value = getattr(self, name)
            if type(value) is not int or value <= 0:
                raise ValueError(f"timeslice_function.{name} must be a positive integer")
        if self.min_us > self.max_us:
            raise ValueError("timeslice_function.min_us must not exceed max_us")
        if self.min_us % self.quantum_us or self.max_us % self.quantum_us:
            raise ValueError("timeslice_function bounds must be multiples of quantum_us")
        for name in (
            "running_weight", "queue_weight", "min_update_seconds",
            "relative_change", "recovery_hold_seconds",
        ):
            value = getattr(self, name)
            if (
                isinstance(value, bool)
                or not isinstance(value, (int, float))
                or not math.isfinite(value)
                or value < 0
            ):
                raise ValueError(f"timeslice_function.{name} must be finite and non-negative")
        if self.relative_change > 1:
            raise ValueError("timeslice_function.relative_change must not exceed one")
        if type(self.skip_recovery_hold) is not bool:
            raise ValueError("timeslice_function.skip_recovery_hold must be boolean")

    def compute(self, running: float, queued: float) -> int:
        """Return the target from this sample alone; update limits are separate."""
        validate_load(running, queued)
        raw_us = self.base_us / (
            1 + self.running_weight * running + self.queue_weight * queued
        )
        rounded_us = math.floor(raw_us / self.quantum_us) * self.quantum_us
        return max(self.min_us, min(self.max_us, rounded_us))

    def should_update(self, requested_us: int, applied_us: int, elapsed: float) -> bool:
        return (
            requested_us != applied_us
            and elapsed >= self.min_update_seconds
            and abs(requested_us - applied_us) / applied_us >= self.relative_change
        )


class TimesliceRecovery:
    """Gate actuation, leaving f(r, q) pure.

    With a positive hold, decreases bypass both time gates but retain the relative
    change threshold. Growth uses the minimum continuously eligible increase;
    reset when a request fails the growth threshold or after a successful write.
    A zero hold keeps the historical update and state-transition behavior.
    """

    def __init__(self, function: TimesliceFunction):
        self.function = function
        self.reset()

    def reset(self) -> None:
        self.since: float | None = None
        self.minimum_us: int | None = None

    def observe(
        self, requested_us: int, applied_us: int, now: float, elapsed: float,
    ) -> tuple[int, str | None, float | None]:
        recovery_elapsed = None
        eligible_us = requested_us
        if requested_us <= applied_us or (
            (requested_us - applied_us) / applied_us < self.function.relative_change
        ):
            self.reset()
        else:
            if self.since is None:
                self.since = now
                self.minimum_us = requested_us
            assert self.minimum_us is not None
            self.minimum_us = min(self.minimum_us, requested_us)
            eligible_us = self.minimum_us
            recovery_elapsed = now - self.since
        if eligible_us == applied_us:
            reason = "unchanged"
        elif abs(eligible_us - applied_us) / applied_us < self.function.relative_change:
            reason = "relative_change"
        elif (eligible_us > applied_us and not self.function.skip_recovery_hold
              and recovery_elapsed < self.function.recovery_hold_seconds):
            reason = "recovery_hold"
        elif eligible_us > applied_us and elapsed < self.function.min_update_seconds:
            reason = "min_update"
        else:
            reason = None
        return eligible_us, reason, recovery_elapsed


DEFAULT_STATES = {
    state: StateSpec(34_816, 47_104, 2048, pause_offline=state == "overload",
                     refresh_on_entry=False, incremental_migration=True)
    for state in STATE_ORDER
}


class JsonlLog:
    def __init__(self, path: Path):
        self.path = path
        self.handle: Any = None

    def __enter__(self) -> "JsonlLog":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.handle = self.path.open("w", encoding="utf-8", buffering=1)
        return self

    def write(self, event: Mapping[str, Any]) -> None:
        self.handle.write(json.dumps(dict(event), sort_keys=True, allow_nan=False) + "\n")
        self.handle.flush()

    def __exit__(self, *_: object) -> None:
        if self.handle is not None:
            self.handle.close()


def parse_prometheus(text: str) -> dict[str, float]:
    totals: dict[str, float] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.rsplit(None, 1)
        if len(parts) != 2:
            continue
        metric_and_labels, raw_value = parts
        name = metric_and_labels.split("{", 1)[0]
        try:
            value = float(raw_value)
        except ValueError:
            continue
        if math.isfinite(value):
            totals[name] = totals.get(name, 0.0) + value
    return totals


def fetch_metrics(url: str, timeout: float) -> dict[str, float]:
    request = urllib.request.Request(url, headers={"Accept": "text/plain"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        payload = response.read(8 * 1024 * 1024 + 1)
    if len(payload) > 8 * 1024 * 1024:
        raise RuntimeError("metrics response exceeds 8 MiB")
    return parse_prometheus(payload.decode("utf-8", errors="replace"))


def load_state_specs(path: Path | None) -> dict[str, StateSpec]:
    if path is None:
        return dict(DEFAULT_STATES)
    raw = json.loads(path.read_text())
    result = {}
    for state in STATE_ORDER:
        item = raw[state]
        if type(item["offline_timeslice_us"]) is not int or item["offline_timeslice_us"] <= 0:
            raise ValueError(f"{state}.offline_timeslice_us must be a positive integer")
        result[state] = StateSpec(
            online_target_mib=int(item["online_target_mib"]),
            offline_target_mib=int(item["offline_target_mib"]),
            offline_timeslice_us=int(item["offline_timeslice_us"]),
            pause_offline=bool(item.get("pause_offline", False)),
            refresh_on_entry=bool(item.get("refresh_on_entry", True)),
            manage_online_target=bool(item.get("manage_online_target", True)),
            manage_offline_target=bool(item.get("manage_offline_target", True)),
            wait_for_migration=bool(item.get("wait_for_migration", True)),
            incremental_migration=bool(item.get("incremental_migration", False)),
            entry_pause_seconds=float(item.get("entry_pause_seconds", 0.0)),
        )
    return result


def load_timeslice_function(path: Path | None) -> TimesliceFunction | None:
    if path is None:
        return None
    raw = json.loads(path.read_text())
    if "timeslice_function" not in raw:
        return None
    item = raw["timeslice_function"]
    if not isinstance(item, dict):
        raise ValueError("timeslice_function must be an object")
    try:
        return TimesliceFunction(**item)
    except TypeError as exc:
        raise ValueError(f"invalid timeslice_function: {exc}") from exc


def offline_timeslice_max_us(
    specs: Mapping[str, StateSpec],
    function: TimesliceFunction | None,
    initial_offline_timeslice_us: int = DEFAULT_TIMESLICE_US,
) -> int:
    """Include initialization; function state quanta are only placeholders."""
    if type(initial_offline_timeslice_us) is not int or initial_offline_timeslice_us <= 0:
        raise ValueError("initial_offline_timeslice_us must be a positive integer")
    if function is not None:
        policy_max = function.max_us
    else:
        if set(specs) != set(STATE_ORDER):
            raise ValueError("matching requires a known offline maximum for all states")
        values = [spec.offline_timeslice_us for spec in specs.values()]
        if any(type(value) is not int or value <= 0 for value in values):
            raise ValueError("offline timeslices must be positive integers")
        policy_max = max(values)
    return max(initial_offline_timeslice_us, policy_max)


def resolve_online_timeslice_us(
    requested_us: int,
    match_online_offline_max: bool,
    specs: Mapping[str, StateSpec],
    function: TimesliceFunction | None,
    initial_offline_timeslice_us: int = DEFAULT_TIMESLICE_US,
) -> int:
    if type(requested_us) is not int or requested_us <= 0:
        raise ValueError("online_timeslice_us must be a positive integer")
    if type(match_online_offline_max) is not bool:
        raise ValueError("match_online_offline_max must be boolean")
    if match_online_offline_max:
        return offline_timeslice_max_us(specs, function, initial_offline_timeslice_us)
    return requested_us


def load_thresholds(path: Path | None) -> LoadThresholds:
    if path is None:
        return LoadThresholds()
    raw = json.loads(path.read_text()).get("thresholds", {})
    return LoadThresholds(
        high_running=int(raw.get("high_running", 2)),
        overload_queue=int(raw.get("overload_queue", 8)),
        overload_running=int(raw.get("overload_running", 16)),
        arrival_guard_seconds=float(raw.get("arrival_guard_seconds", 0.0)),
    )


def validate_thresholds(thresholds: LoadThresholds) -> None:
    if thresholds.high_running < 2:
        raise ValueError("high_running must be at least two")
    if thresholds.overload_queue < 1:
        raise ValueError("overload_queue must be positive")
    if thresholds.overload_running < thresholds.high_running:
        raise ValueError("overload_running must be at least high_running")
    if (
        not math.isfinite(thresholds.arrival_guard_seconds)
        or thresholds.arrival_guard_seconds < 0
    ):
        raise ValueError("arrival_guard_seconds must be finite and non-negative")


def validate_state_specs(specs: Mapping[str, StateSpec], physical_mib: int) -> None:
    if set(specs) != set(STATE_ORDER):
        raise ValueError(f"state policy must define exactly {sorted(STATE_ORDER)}")
    for name, spec in specs.items():
        if spec.online_target_mib < 0 or spec.offline_target_mib < 0:
            raise ValueError(f"negative residency target in {name}")
        if spec.online_target_mib + spec.offline_target_mib != physical_mib:
            raise ValueError(
                f"{name} residency targets must sum to {physical_mib} MiB"
            )
        if spec.offline_timeslice_us <= 0:
            raise ValueError(f"invalid offline timeslice in {name}")
        if not math.isfinite(spec.entry_pause_seconds) or spec.entry_pause_seconds < 0:
            raise ValueError(f"invalid entry pause in {name}")


def validate_load(running: float, queued: float) -> None:
    if not all(math.isfinite(value) and value >= 0 for value in (running, queued)):
        raise ValueError(
            f"SGLang load metrics must be finite and non-negative: "
            f"running={running}, queued={queued}"
        )


def classify_load(
    running: float,
    queued: float,
    thresholds: LoadThresholds = LoadThresholds(),
) -> str:
    validate_load(running, queued)
    if queued >= thresholds.overload_queue or (
        queued >= 1 and running >= thresholds.overload_running
    ):
        return "overload"
    if queued >= 1 or running >= thresholds.high_running:
        return "high"
    if running >= 1:
        return "low"
    return "idle"


class StableState:
    """Escalate immediately and require stability before de-escalating."""

    def __init__(self, initial: str, deescalate_seconds: float):
        self.current = initial
        self.candidate = initial
        self.candidate_since = time.monotonic()
        self.deescalate_seconds = deescalate_seconds

    def observe(self, desired: str, now: float) -> str:
        if STATE_ORDER[desired] > STATE_ORDER[self.current]:
            self.current = desired
            self.candidate = desired
            self.candidate_since = now
            return self.current
        if STATE_ORDER[desired] == STATE_ORDER[self.current]:
            self.candidate = desired
            self.candidate_since = now
            return self.current
        if desired != self.candidate:
            self.candidate = desired
            self.candidate_since = now
            return self.current
        if now - self.candidate_since >= self.deescalate_seconds:
            self.current = desired
            self.candidate_since = now
        return self.current


def _status_dict(controller: UvmController | None) -> dict[str, Any] | None:
    if controller is None:
        return None
    status = controller.status()
    return dataclasses.asdict(status)


def _target_dict(result: Any) -> dict[str, Any]:
    if isinstance(result, dict):
        return {**result, "waited": True}
    if isinstance(result, int):
        return {"submitted_job_id": result, "waited": False}
    if hasattr(result, "status"):
        data = dataclasses.asdict(result)
        data["waited"] = True
        return data
    return {"status": dataclasses.asdict(result), "waited": True}


def _migrate_target(
    controller: UvmController,
    previous_mib: int | None,
    target_mib: int,
    spec: StateSpec,
    migration_timeout: float,
) -> Any:
    if previous_mib is not None and spec.incremental_migration:
        return controller.retarget(
            previous_mib * MIB,
            target_mib * MIB,
            timeout=migration_timeout,
            wait=spec.wait_for_migration,
        )
    return controller.set_target(
        target_mib * MIB,
        timeout=migration_timeout,
        wait=spec.wait_for_migration,
    )


def apply_ours_state(
    state: str,
    previous_state: str | None,
    specs: Mapping[str, StateSpec],
    offline_process: ProcessController,
    online_uvm: UvmController,
    offline_uvm: UvmController,
    offline_workers: int,
    migration_timeout: float,
    *,
    write_timeslice: bool = True,
    cache_window: CacheWindowController | None = None,
    metrics: Mapping[str, float] | None = None,
) -> dict[str, Any]:
    spec = specs[state]
    previous = specs[previous_state] if previous_state is not None else None
    escalating_transition = (
        previous_state is not None and STATE_ORDER[state] > STATE_ORDER[previous_state]
    )
    refresh = escalating_transition and spec.refresh_on_entry
    migrate_online = spec.manage_online_target and (
        previous is None
        or refresh
        or previous.online_target_mib != spec.online_target_mib
    )
    migrate_offline = spec.manage_offline_target and (
        previous is None
        or refresh
        or previous.offline_target_mib != spec.offline_target_mib
    )
    migration_needed = migrate_online or migrate_offline
    started = time.monotonic()

    entry_guard = spec.entry_pause_seconds > 0 and not spec.pause_offline
    if migration_needed or entry_guard:
        offline_process.pause()
    if write_timeslice:
        offline_process.set_timeslice(spec.offline_timeslice_us)

    escalating = previous_state is None or STATE_ORDER[state] >= STATE_ORDER[previous_state]
    if escalating:
        offline_status = (
            _migrate_target(
                offline_uvm,
                previous.offline_target_mib if previous is not None else None,
                spec.offline_target_mib,
                spec,
                migration_timeout,
            )
            if migrate_offline
            else offline_uvm.status()
        )
        online_status = (
            cache_window.update(online_uvm, metrics, time.monotonic(), initial=True)
            if migrate_online and cache_window is not None else
            _migrate_target(
                online_uvm,
                previous.online_target_mib if previous is not None else None,
                spec.online_target_mib,
                spec,
                migration_timeout,
            )
            if migrate_online
            else online_uvm.status()
        )
    else:
        online_status = (
            _migrate_target(
                online_uvm,
                previous.online_target_mib if previous is not None else None,
                spec.online_target_mib,
                spec,
                migration_timeout,
            )
            if migrate_online
            else online_uvm.status()
        )
        offline_status = (
            _migrate_target(
                offline_uvm,
                previous.offline_target_mib if previous is not None else None,
                spec.offline_target_mib,
                spec,
                migration_timeout,
            )
            if migrate_offline
            else offline_uvm.status()
        )

    if spec.pause_offline:
        if not migration_needed:
            offline_process.pause()
    else:
        if entry_guard:
            remaining = spec.entry_pause_seconds - (time.monotonic() - started)
            if remaining > 0:
                time.sleep(remaining)
        if migration_needed or entry_guard or (previous is not None and previous.pause_offline):
            offline_process.resume(offline_workers)
    return {
        "control_seconds": time.monotonic() - started,
        "offline_paused": spec.pause_offline,
        "entry_pause_seconds": spec.entry_pause_seconds,
        "online_migrated": migrate_online,
        "offline_migrated": migrate_offline,
        "online_target_mib": spec.online_target_mib,
        "offline_target_mib": spec.offline_target_mib,
        "offline_timeslice_us": spec.offline_timeslice_us,
        "online_uvm": _target_dict(online_status),
        "offline_uvm": _target_dict(offline_status),
    }


def apply_task_switch(
    active: bool,
    offline_process: ProcessController,
    online_uvm: UvmController,
    offline_uvm: UvmController,
    offline_workers: int,
    physical_mib: int,
    migration_timeout: float,
) -> dict[str, Any]:
    started = time.monotonic()
    if active:
        offline_process.pause()
        offline_status = offline_uvm.set_target(0, timeout=migration_timeout)
        online_status = online_uvm.set_target(
            physical_mib * MIB, timeout=migration_timeout
        )
    else:
        online_status = online_uvm.set_target(0, timeout=migration_timeout)
        offline_status = offline_uvm.set_target(
            physical_mib * MIB, timeout=migration_timeout
        )
        offline_process.resume(offline_workers)
    return {
        "control_seconds": time.monotonic() - started,
        "offline_paused": active,
        "online_target_mib": physical_mib if active else 0,
        "offline_target_mib": 0 if active else physical_mib,
        "online_uvm": _target_dict(online_status),
        "offline_uvm": _target_dict(offline_status),
    }


def run(args: argparse.Namespace) -> int:
    specs = load_state_specs(args.policy)
    thresholds = load_thresholds(args.policy)
    timeslice_function = load_timeslice_function(args.policy)
    policy_data = json.loads(args.policy.read_text()) if args.policy else {}
    cache_window = (CacheWindowController(CacheWindowSpec(**policy_data["cache_window"]))
                    if "cache_window" in policy_data else None)
    if "slot_placement" in policy_data:
        from experiments.slot_placement import SlotPlacementController, SlotPlacementSpec
        cache_window = SlotPlacementController(CacheWindowSpec(**policy_data["cache_window"]),
                                                SlotPlacementSpec(**policy_data["slot_placement"]),
                                                args.online_pid)
    if cache_window is not None:
        # This policy manages cache ranges itself; prefix targets are retained
        # only for the trainer and must not request competing online changes.
        if len({spec.online_target_mib for spec in specs.values()}) != 1 or any(
            spec.refresh_on_entry for spec in specs.values()
        ):
            raise ValueError("cache windows require constant legacy targets and no entry refresh")
    validate_state_specs(specs, args.physical_mib)
    validate_thresholds(thresholds)
    online_timeslice_us = resolve_online_timeslice_us(
        args.online_timeslice_us, args.match_online_offline_max,
        specs, timeslice_function, args.initial_offline_timeslice_us,
    )
    if args.skip_timeslice_control and (
        args.match_online_offline_max or args.online_timeslice_us != DEFAULT_TIMESLICE_US
    ):
        raise ValueError("online timeslice configuration requires timeslice control")
    if timeslice_function is not None and args.mode == "ours" and args.skip_timeslice_control:
        raise ValueError("timeslice_function requires timeslice control")
    stop = False

    def request_stop(_signum: int, _frame: Any) -> None:
        nonlocal stop
        stop = True

    previous_handlers = {
        signum: signal.signal(signum, request_stop)
        for signum in (signal.SIGINT, signal.SIGTERM)
    }

    online_process: ProcessController | None = None
    offline_process: ProcessController | None = None
    online_uvm: UvmController | None = None
    offline_uvm: UvmController | None = None
    try:
        online_process, online_uvm = wait_for_instrumented_process(
            args.online_pid,
            require_uvm=args.mode in {"ours", "task_switch"},
            timeout=args.control_ready_timeout,
        )
        if not args.skip_timeslice_control:
            online_process.set_timeslice(online_timeslice_us)

        if args.offline_pid is not None:
            offline_process, offline_uvm = wait_for_instrumented_process(
                args.offline_pid,
                require_uvm=args.mode in {"ours", "task_switch"},
                timeout=args.control_ready_timeout,
            )
            if args.skip_timeslice_control:
                offline_process.set_controls(worker_threads=args.offline_workers)
            else:
                offline_process.set_controls(
                    worker_threads=args.offline_workers,
                    timeslice_us=args.initial_offline_timeslice_us,
                )
        elif args.mode != "observe":
            raise ValueError(f"mode {args.mode} requires --offline-pid")

        started = time.monotonic()
        last_uvm_sample = -math.inf
        last_applied_state: str | None = None
        applied_timeslice_us = args.initial_offline_timeslice_us
        last_timeslice_update = -math.inf
        recovery = (
            TimesliceRecovery(timeslice_function)
            if timeslice_function is not None and timeslice_function.recovery_hold_seconds > 0
            else None
        )
        task_switch_active: bool | None = None
        previous_pressure = 0.0
        stable = StableState("idle", args.deescalate_seconds)

        with JsonlLog(args.output) as log:
            log.write(
                {
                    "event": "config",
                    "monotonic_origin_seconds": started,
                    "mode": args.mode,
                    "online_pid": args.online_pid,
                    "offline_pid": args.offline_pid,
                    "online_timeslice_us": online_timeslice_us,
                    "online_timeslice_us_requested": args.online_timeslice_us,
                    "match_online_offline_max": args.match_online_offline_max,
                    "online_timeslice_from_offline_max": args.match_online_offline_max,
                    "offline_timeslice_max_us": (
                        online_timeslice_us if args.match_online_offline_max else None
                    ),
                    "timeslice_control_enabled": not args.skip_timeslice_control,
                    "initial_offline_timeslice_us": args.initial_offline_timeslice_us,
                    "offline_workers": args.offline_workers,
                    "physical_mib": args.physical_mib,
                    "poll_seconds": args.poll_seconds,
                    "deescalate_seconds": args.deescalate_seconds,
                    "thresholds": dataclasses.asdict(thresholds),
                    "timeslice_function": (
                        dataclasses.asdict(timeslice_function)
                        if timeslice_function is not None else None
                    ),
                    "cache_window": dataclasses.asdict(cache_window.spec) if cache_window else None,
                    "slot_placement": policy_data.get("slot_placement"),
                    "states": {
                        name: dataclasses.asdict(spec) for name, spec in specs.items()
                    },
                }
            )
            while not stop:
                loop_started = time.monotonic()
                elapsed = loop_started - started
                if args.duration_seconds is not None and elapsed >= args.duration_seconds:
                    break
                if args.stop_file and args.stop_file.exists():
                    break

                try:
                    metrics = fetch_metrics(args.metrics_url, args.metrics_timeout)
                    required_metrics = (
                        "sglang:num_running_reqs",
                        "sglang:num_queue_reqs",
                    )
                    missing = [name for name in required_metrics if name not in metrics]
                    if missing:
                        raise RuntimeError(f"required SGLang metrics are missing: {missing}")
                    running = metrics["sglang:num_running_reqs"]
                    queued = metrics["sglang:num_queue_reqs"]
                    desired = classify_load(running, queued, thresholds)
                    state = stable.observe(desired, loop_started)
                    event: dict[str, Any] = {
                        "event": "sample",
                        "elapsed_seconds": elapsed,
                        "running_requests": running,
                        "queued_requests": queued,
                        "desired_state": desired,
                        "effective_state": state,
                    }

                    if args.mode == "ours":
                        assert offline_process is not None
                        assert online_uvm is not None and offline_uvm is not None
                        requested_us = (
                            timeslice_function.compute(running, queued)
                            if timeslice_function is not None
                            else specs[state].offline_timeslice_us
                        )
                        transition = state != last_applied_state
                        update_elapsed = loop_started - last_timeslice_update
                        eligible_us = requested_us
                        held_reason = None
                        recovery_elapsed = None
                        if recovery is not None:
                            eligible_us, held_reason, recovery_elapsed = recovery.observe(
                                requested_us, applied_timeslice_us, loop_started, update_elapsed,
                            )
                            write_timeslice = held_reason is None
                        else:
                            # Zero hold preserves historical transition/update gates.
                            write_timeslice = transition or (
                                timeslice_function is not None and timeslice_function.should_update(
                                    requested_us, applied_timeslice_us, update_elapsed,
                                )
                            )
                            if not write_timeslice and timeslice_function is not None:
                                held_reason = (
                                    "unchanged" if requested_us == applied_timeslice_us
                                    else "min_update" if update_elapsed < timeslice_function.min_update_seconds
                                    else "relative_change"
                                )
                        target_us = eligible_us if write_timeslice else applied_timeslice_us
                        update_reason = "held"
                        if transition:
                            previous = specs[last_applied_state] if last_applied_state is not None else None
                            pause_change = specs[state].pause_offline != (
                                previous.pause_offline if previous is not None else False
                            )
                            spec = specs[state]
                            refresh = (
                                last_applied_state is not None
                                and STATE_ORDER[state] > STATE_ORDER[last_applied_state]
                                and spec.refresh_on_entry
                            )
                            migration_needed = previous is None or (
                                spec.manage_online_target
                                and (refresh or spec.online_target_mib != previous.online_target_mib)
                            ) or (
                                spec.manage_offline_target
                                and (refresh or spec.offline_target_mib != previous.offline_target_mib)
                            )
                            if (
                                recovery is None or write_timeslice or pause_change
                                or migration_needed or spec.entry_pause_seconds > 0
                            ):
                                # UVM and pause transitions use the gated quantum during recovery.
                                effective_specs = dict(specs)
                                effective_specs[state] = dataclasses.replace(
                                    specs[state], offline_timeslice_us=target_us
                                )
                                event["control"] = apply_ours_state(
                                    state,
                                    last_applied_state,
                                    effective_specs,
                                    offline_process,
                                    online_uvm,
                                    offline_uvm,
                                    args.offline_workers,
                                    args.migration_timeout,
                                    write_timeslice=write_timeslice,
                                    cache_window=cache_window,
                                    metrics=metrics,
                                )
                                update_reason = "state_transition"
                            last_applied_state = state
                        elif write_timeslice:
                            control_started = time.monotonic()
                            offline_process.set_timeslice(target_us)
                            event["control"] = {
                                "control_seconds": time.monotonic() - control_started,
                                "offline_timeslice_us": target_us,
                            }
                            update_reason = "function_update"
                        if write_timeslice:
                            applied_timeslice_us = target_us
                            last_timeslice_update = time.monotonic()
                            if recovery is not None:
                                recovery.reset()
                        if timeslice_function is not None:
                            if "control" in event:
                                event["control"].update({
                                    "requested_offline_timeslice_us": requested_us,
                                    "eligible_offline_timeslice_us": eligible_us,
                                    "timeslice_updated": write_timeslice,
                                })
                            event["timeslice_function"] = {
                                "computed_offline_timeslice_us": requested_us,
                                "requested_offline_timeslice_us": requested_us,
                                "eligible_offline_timeslice_us": eligible_us,
                                "applied_offline_timeslice_us": applied_timeslice_us,
                                "updated": write_timeslice,
                                "update_reason": update_reason,
                                "held_reason": held_reason,
                                "recovery_elapsed_seconds": recovery_elapsed,
                                "since_update_seconds": update_elapsed if math.isfinite(update_elapsed) else None,
                                "control_seconds": event.get("control", {}).get("control_seconds", 0.0),
                                "offline_paused": specs[state].pause_offline,
                            }
                        if (cache_window is not None and specs[state].manage_online_target
                                and not specs[state].pause_offline and cache_window.current is not None
                                and cache_window.due(metrics, time.monotonic())):
                            control_started = time.monotonic()
                            offline_process.pause()
                            try:
                                action = cache_window.update(online_uvm, metrics, control_started)
                            finally:
                                offline_process.resume(args.offline_workers)
                            if action is not None:
                                event["cache_window"] = {
                                    **action, "control_seconds": time.monotonic() - control_started,
                                    "running_requests": running, "queued_requests": queued,
                                }
                    elif args.mode == "task_switch":
                        assert offline_process is not None
                        assert online_uvm is not None and offline_uvm is not None
                        active = state != "idle"
                        if active != task_switch_active:
                            event["control"] = apply_task_switch(
                                active,
                                offline_process,
                                online_uvm,
                                offline_uvm,
                                args.offline_workers,
                                args.physical_mib,
                                args.migration_timeout,
                            )
                            task_switch_active = active

                    pressure = running + queued
                    arrival_guard = (
                        args.mode == "ours"
                        and thresholds.arrival_guard_seconds > 0
                        and pressure > previous_pressure
                        and not specs[state].pause_offline
                    )
                    if arrival_guard:
                        assert offline_process is not None
                        guard_started = time.monotonic()
                        offline_process.pause()
                        remaining = thresholds.arrival_guard_seconds - (
                            time.monotonic() - guard_started
                        )
                        if remaining > 0:
                            time.sleep(remaining)
                        offline_process.resume(args.offline_workers)
                        event["arrival_guard"] = {
                            "pressure_before": previous_pressure,
                            "pressure_after": pressure,
                            "requested_seconds": thresholds.arrival_guard_seconds,
                            "actual_seconds": time.monotonic() - guard_started,
                        }
                    previous_pressure = pressure

                    if loop_started - last_uvm_sample >= args.uvm_sample_seconds:
                        event["online_uvm"] = _status_dict(online_uvm)
                        event["offline_uvm"] = _status_dict(offline_uvm)
                        last_uvm_sample = loop_started
                    log.write(event)
                except Exception as exc:
                    log.write(
                        {
                            "event": "error",
                            "elapsed_seconds": elapsed,
                            "error": f"{type(exc).__name__}: {exc}",
                        }
                    )
                    if args.fail_fast:
                        raise

                sleep_for = args.poll_seconds - (time.monotonic() - loop_started)
                if sleep_for > 0:
                    time.sleep(sleep_for)

            log.write(
                {
                    "event": "shutdown",
                    "elapsed_seconds": time.monotonic() - started,
                    "signal_or_stop": stop,
                }
            )
        return 0
    finally:
        # Never leave training paused merely because the scheduler exits.
        if offline_process is not None:
            try:
                offline_process.resume(args.offline_workers)
            except Exception:
                pass
        if online_process is not None:
            online_process.close()
        if offline_process is not None:
            offline_process.close()
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("ours", "task_switch", "observe"), required=True)
    parser.add_argument("--online-pid", type=int, required=True)
    parser.add_argument("--offline-pid", type=int)
    parser.add_argument("--metrics-url", default="http://127.0.0.1:30000/metrics")
    parser.add_argument("--metrics-timeout", type=float, default=2.0)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--policy", type=Path)
    parser.add_argument("--physical-mib", type=int, default=81_920)
    parser.add_argument("--offline-workers", type=int, default=108)
    parser.add_argument("--online-timeslice-us", type=int, default=DEFAULT_TIMESLICE_US,
                        help="fixed online timeslice during sampling/replay, after model warmup")
    parser.add_argument("--match-online-offline-max", action=argparse.BooleanOptionalAction,
                        default=False, help="derive fixed online timeslice from the offline maximum")
    parser.add_argument("--initial-offline-timeslice-us", type=int, default=DEFAULT_TIMESLICE_US)
    parser.add_argument("--skip-timeslice-control", action="store_true")
    parser.add_argument("--poll-seconds", type=float, default=0.01)
    parser.add_argument("--uvm-sample-seconds", type=float, default=1.0)
    parser.add_argument("--deescalate-seconds", type=float, default=0.1)
    parser.add_argument("--migration-timeout", type=float, default=600.0)
    parser.add_argument("--control-ready-timeout", type=float, default=180.0)
    parser.add_argument("--duration-seconds", type=float)
    parser.add_argument("--stop-file", type=Path)
    parser.add_argument("--fail-fast", action=argparse.BooleanOptionalAction, default=True)
    return parser


def validate_args(args: argparse.Namespace) -> None:
    # Type checks also cover programmatic callers, beyond argparse's CLI types.
    resolve_online_timeslice_us(
        args.online_timeslice_us, args.match_online_offline_max, DEFAULT_STATES, None,
    )
    if type(args.initial_offline_timeslice_us) is not int or args.initial_offline_timeslice_us <= 0:
        raise ValueError("initial_offline_timeslice_us must be a positive integer")
    positive = {
        "online_pid": args.online_pid,
        "physical_mib": args.physical_mib,
        "offline_workers": args.offline_workers,
        "initial_offline_timeslice_us": args.initial_offline_timeslice_us,
        "poll_seconds": args.poll_seconds,
        "uvm_sample_seconds": args.uvm_sample_seconds,
        "deescalate_seconds": args.deescalate_seconds,
        "migration_timeout": args.migration_timeout,
        "control_ready_timeout": args.control_ready_timeout,
        "metrics_timeout": args.metrics_timeout,
    }
    for name, value in positive.items():
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"{name} must be finite and positive")
    if args.offline_pid is not None and args.offline_pid <= 0:
        raise ValueError("offline_pid must be positive")
    if args.duration_seconds is not None and (
        not math.isfinite(args.duration_seconds) or args.duration_seconds <= 0
    ):
        raise ValueError("duration_seconds must be finite and positive")


def main(argv: Sequence[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    validate_args(args)
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
