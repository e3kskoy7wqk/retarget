#!/usr/bin/env python3
"""Launch one complete online/offline GPU colocation experiment."""

from __future__ import annotations

import argparse
import contextlib
import dataclasses
import hashlib
import json
import math
import os
import re
import signal
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any, Mapping, Sequence

from experiments.control import DEFAULT_TIMESLICE_US, SharedRegistry, UvmController
from experiments.scheduler import (
    DEFAULT_STATES,
    load_state_specs,
    load_thresholds,
    load_timeslice_function,
    resolve_online_timeslice_us,
    validate_state_specs,
    validate_thresholds,
)

ROOT = Path(__file__).resolve().parents[1]
PYTHON = Path(sys.executable)
ONLINE_MODEL = ROOT / "models" / "Qwen3-14B"
OFFLINE_MODEL = ROOT / "models" / "Qwen3-14B"
SQUAD = ROOT / "data" / "squad" / "train-00000-of-00001.parquet"
PRELOAD = ROOT / "a.so"
SERVED_MODEL = "qwen3-14b"


@dataclasses.dataclass(frozen=True)
class Baseline:
    name: str
    scheduler_mode: str
    training: bool
    convert_alloc: bool
    mem_fraction_static: float
    max_total_tokens: int | None
    description: str
    mps_enabled: bool = False
    timeslice_control: bool = True


BASELINE_RESERVE_MIB = {
    "ours": 0,
    "task_switch": 0,
    "sp50": 12596,
    "sp75": 1854,
    "um_mps": 0,
}

BASELINES = {
    "ours": Baseline(
        "ours", "ours", True, True, 0.90, 218_112,
        "Fixed context timeslices and proactive UVM placement; MPS disabled.",
    ),
    "task_switch": Baseline(
        "task_switch", "task_switch", True, True, 0.90, 218_112,
        "Temporal switching with offline launch pause and UVM migration; MPS disabled.",
    ),
    "sp50": Baseline(
        "sp50", "observe", True, True, 0.90, 137_500,
        "Static SGLang KV-cache capacity approximating 50% of Infer-Only KV; MPS disabled.",
    ),
    "sp75": Baseline(
        "sp75", "observe", True, True, 0.90, 206_250,
        "Static SGLang KV-cache capacity approximating 75% of Infer-Only KV; MPS disabled.",
    ),
    "um_mps": Baseline(
        "um_mps", "observe", True, True, 0.90, 218_112,
        "Reactive Unified Memory colocation; legacy UM+MPS label, MPS explicitly disabled.",
    ),
    "infer_only": Baseline(
        "infer_only", "observe", False, False, 0.90, 275_000,
        "Inference-only control; MPS disabled.",
    ),
}


class ManagedProcess:
    def __init__(
        self,
        name: str,
        command: Sequence[str],
        *,
        env: Mapping[str, str],
        log_path: Path,
        kv_audit: bool = False,
    ):
        self.name = name
        self.command = list(command)
        self.log_path = log_path
        log_path.parent.mkdir(parents=True, exist_ok=True)
        self._log = log_path.open("w", encoding="utf-8", buffering=1)
        env = dict(env)
        env.pop("RETARGET_KV_AUDIT_DIR", None)
        env.pop("RETARGET_SLOT_FEEDBACK_ENABLE", None)
        env.pop("RETARGET_SLOT_FEEDBACK_DIR", None)
        if kv_audit:
            if name not in ("sglang", "scheduler"):
                raise ValueError("Placement instrumentation is only supported for inference and its scheduler")
            env["RETARGET_SLOT_FEEDBACK_DIR"] = str(log_path.parent / "slot_feedback")
            if name == "sglang":
                env["RETARGET_KV_AUDIT_DIR"] = str(log_path.parent / "kv_audit")
                env["RETARGET_SLOT_FEEDBACK_ENABLE"] = "1"
                env["PYTHONPATH"] = os.pathsep.join(
                    [str(ROOT / "experiments" / "kv_audit"), str(ROOT)]
                )
        self.process = subprocess.Popen(
            self.command,
            cwd=ROOT,
            env=dict(env),
            stdin=subprocess.DEVNULL,
            stdout=self._log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            text=True,
        )

    @property
    def pid(self) -> int:
        return self.process.pid

    def poll(self) -> int | None:
        return self.process.poll()

    def require_running(self) -> None:
        return_code = self.poll()
        if return_code is not None:
            raise RuntimeError(
                f"{self.name} exited with status {return_code}; log tail:\n{self.tail()}"
            )

    def tail(self, characters: int = 8000) -> str:
        with contextlib.suppress(OSError):
            text = self.log_path.read_text(errors="replace")
            return text[-characters:]
        return "<unavailable>"

    def wait(self, timeout: float) -> bool:
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            return False
        self._log.close()
        return True

    def _group_alive(self) -> bool:
        try:
            os.killpg(self.pid, 0)
        except ProcessLookupError:
            return False
        except PermissionError:
            return True
        return True

    def stop(self, grace_seconds: float = 20.0) -> None:
        with contextlib.suppress(ProcessLookupError):
            os.killpg(self.pid, signal.SIGTERM)
        deadline = time.monotonic() + grace_seconds
        while self._group_alive() and time.monotonic() < deadline:
            time.sleep(0.1)
        if self._group_alive():
            with contextlib.suppress(ProcessLookupError):
                os.killpg(self.pid, signal.SIGKILL)
        if self.poll() is None:
            self.process.wait(timeout=10)
        self._log.close()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def harness_sha256() -> str:
    digest = hashlib.sha256()
    files = sorted((ROOT / "experiments").rglob("*.py"))
    files += sorted((ROOT / "tools").glob("*.py"))
    files += sorted((ROOT / "configs").glob("*.json"))
    files += [ROOT / "select.cpp"]
    for path in files:
        relative = path.relative_to(ROOT).as_posix().encode()
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(bytes.fromhex(sha256(path)))
    return digest.hexdigest()


def _without_toolkit_library_paths(value: str) -> str:
    blocked = ("/usr/local/cuda/lib64/stubs", "/usr/local/cuda-12.2/lib64/stubs")
    return ":".join(
        item for item in value.split(":") if item and not item.startswith(blocked)
    )


def base_environment(
    *, convert_alloc: bool, gpu: int, mps_enabled: bool = False,
    workers: int = 108, cuda_path: Path | None = None,
) -> dict[str, str]:
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = _without_toolkit_library_paths(
        env.get("LD_LIBRARY_PATH", "")
    )
    env.update(
        {
            # An isolated MPS daemon exposes its selected physical GPU as
            # logical device 0 to clients, irrespective of the host index.
            "CUDA_VISIBLE_DEVICES": "0" if mps_enabled else str(gpu),
            "CUDA_DEVICE_ORDER": "PCI_BUS_ID",
            "PYTHONNOUSERSITE": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONPATH": str(ROOT),
            "TOKENIZERS_PARALLELISM": "false",
            "LD_PRELOAD": str(PRELOAD),
            "SELECT_UVM_ENABLE": "1",
            "SELECT_UVM_CONVERT_ALLOC": "1" if convert_alloc else "0",
            "SELECT_UVM_CHUNK_MB": "256",
            "SELECT_TIMESLICE_LOG": "1",
            "GCN_NUM_THREADS": str(workers),
        }
    )
    if cuda_path is not None:
        env["CUDA_PATH"] = str(cuda_path)
        env["CUDA_HOME"] = str(cuda_path)
    for key in ("CUDA_CORE_SOFT_LIMIT", "PARTITION_GLOBAL", "PARTITION_STREAM"):
        env.pop(key, None)
    for key in list(env):
        if key.startswith("CUDA_MPS_") and not mps_enabled:
            env.pop(key)
    if mps_enabled and not env.get("CUDA_MPS_PIPE_DIRECTORY"):
        raise RuntimeError("MPS baseline requires CUDA_MPS_PIPE_DIRECTORY")
    if mps_enabled:
        env["SELECT_SKIP_TIMESLICE"] = "1"
    else:
        env.pop("SELECT_SKIP_TIMESLICE", None)
    return env


def client_environment(gpu: int = 0) -> dict[str, str]:
    env = dict(os.environ)
    env.update(
        {
            "CUDA_VISIBLE_DEVICES": str(gpu),
            "PYTHONNOUSERSITE": "1",
            "PYTHONDONTWRITEBYTECODE": "1",
            "PYTHONPATH": str(ROOT),
            "TOKENIZERS_PARALLELISM": "false",
        }
    )
    env.pop("LD_PRELOAD", None)
    for key in list(env):
        if key.startswith("SELECT_UVM_") or key.startswith("CUDA_MPS_"):
            env.pop(key)
    return env


def process_parent(pid: int) -> int | None:
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
    except OSError:
        return None
    close = text.rfind(")")
    if close < 0:
        return None
    fields = text[close + 2 :].split()
    try:
        return int(fields[1])  # Field 4 overall: ppid.
    except (IndexError, ValueError):
        return None


def descendants(root_pid: int) -> set[int]:
    parents: dict[int, int] = {}
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        pid = int(entry.name)
        parent = process_parent(pid)
        if parent is not None:
            parents[pid] = parent
    result = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, parent in parents.items():
            if parent in result and pid not in result:
                result.add(pid)
                changed = True
    return result


def require_compatible_compute_mode(gpu: int, *, mps_enabled: bool) -> int:
    import pynvml

    pynvml.nvmlInit()
    try:
        handle = pynvml.nvmlDeviceGetHandleByIndex(gpu)
        mode = int(pynvml.nvmlDeviceGetComputeMode(handle))
        if not mps_enabled and mode != pynvml.NVML_COMPUTEMODE_DEFAULT:
            raise RuntimeError(
                f"GPU {gpu} compute mode is {mode}; non-MPS SGLang/colocation requires "
                "Default compute mode. Ask the GPU owner to change the mode before "
                "running this experiment. No GPU configuration was changed."
            )
        return mode
    finally:
        pynvml.nvmlShutdown()


def nvml_residency(gpu: int = 0) -> dict[int, int]:
    import pynvml

    pynvml.nvmlInit()
    try:
        handle = pynvml.nvmlDeviceGetHandleByIndex(gpu)
        result = {}
        for process in pynvml.nvmlDeviceGetComputeRunningProcesses(handle):
            result[int(process.pid)] = int(getattr(process, "usedGpuMemory", 0) or 0)
        return result
    finally:
        pynvml.nvmlShutdown()


def wait_for_gpu_target(
    root_pid: int,
    process: ManagedProcess,
    *,
    gpu: int,
    timeout: float,
) -> tuple[int, dict[str, Any]]:
    deadline = time.monotonic() + timeout
    last_candidates: list[int] = []
    while time.monotonic() < deadline:
        process.require_running()
        tree = descendants(root_pid)
        residency = nvml_residency(gpu)
        candidates = []
        with SharedRegistry() as registry:
            for snapshot in registry.snapshots():
                if snapshot.pid not in tree:
                    continue
                socket_path = Path(f"/tmp/select-uvm-{os.getuid()}-{snapshot.pid}.sock")
                if not socket_path.exists():
                    continue
                try:
                    status = UvmController(snapshot.pid).status()
                except Exception:
                    continue
                candidates.append(
                    (
                        residency.get(snapshot.pid, 0),
                        status.managed_bytes,
                        snapshot.worker_threads,
                        snapshot.pid,
                        dataclasses.asdict(status),
                    )
                )
        if candidates:
            candidates.sort(reverse=True)
            _resident, _managed, workers, pid, status = candidates[0]
            if workers > 0:
                return pid, status
            last_candidates = [item[3] for item in candidates]
        time.sleep(0.2)
    raise RuntimeError(
        f"could not identify an initialized GPU process below PID {root_pid}; "
        f"last candidates={last_candidates}; log tail:\n{process.tail()}"
    )


def http_json(url: str, *, timeout: float = 5.0) -> Any:
    with urllib.request.urlopen(url, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def wait_http_ready(process: ManagedProcess, base_url: str, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        process.require_running()
        try:
            with urllib.request.urlopen(base_url + "/health", timeout=2.0) as response:
                if response.status == 200:
                    return
        except Exception as exc:
            last_error = exc
        time.sleep(1.0)
    raise RuntimeError(
        f"SGLang did not become healthy within {timeout}s: {last_error}; "
        f"log tail:\n{process.tail()}"
    )


def warmup(base_url: str, served_model: str = SERVED_MODEL) -> None:
    payload = json.dumps(
        {
            "model": served_model,
            "prompt": "Warm up the language model.",
            "max_tokens": 8,
            "temperature": 0.0,
            "ignore_eos": True,
        }
    ).encode()
    request = urllib.request.Request(
        base_url + "/v1/completions",
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=180) as response:
        if response.status != 200:
            raise RuntimeError(f"warmup returned HTTP {response.status}")
        response.read()


def wait_for_jsonl_event(
    path: Path,
    event_name: str,
    process: ManagedProcess,
    *,
    timeout: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        process.require_running()
        if path.exists():
            with path.open(encoding="utf-8") as handle:
                for line in handle:
                    with contextlib.suppress(json.JSONDecodeError):
                        item = json.loads(line)
                        if item.get("event") == event_name:
                            return item
        time.sleep(0.5)
    raise RuntimeError(
        f"did not observe {event_name!r} in {path} within {timeout}s; "
        f"process log tail:\n{process.tail()}"
    )


def has_jsonl_event(path: Path, event_name: str) -> bool:
    if not path.is_file():
        return False
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            with contextlib.suppress(json.JSONDecodeError):
                if json.loads(line).get("event") == event_name:
                    return True
    return False


def ensure_preflight(
    args: argparse.Namespace, *, allow_mps: bool, allow_unrelated_mps: bool
) -> dict[str, Any]:
    gpu = args.gpu
    required = [
        args.online_python, args.offline_python, args.client_python,
        args.online_model, args.offline_model, args.squad, PRELOAD,
    ]
    missing = [str(path) for path in required if not path.exists()]
    if missing:
        raise RuntimeError(f"missing required inputs: {missing}")

    process_names = subprocess.run(
        ["ps", "-eo", "comm="], text=True, capture_output=True, check=True
    ).stdout.splitlines()
    active_mps = [name.strip() for name in process_names if "nvidia-cuda-mps" in name]
    if active_mps and not (allow_mps or allow_unrelated_mps):
        raise RuntimeError(f"MPS is active but this experiment disables it: {active_mps}")
    if allow_mps and not active_mps:
        raise RuntimeError("MPS baseline requested but no MPS control daemon is active")

    compute_mode = require_compatible_compute_mode(gpu, mps_enabled=allow_mps)
    residency = nvml_residency(gpu)
    if residency:
        unexpected = {}
        for pid, used in residency.items():
            try:
                command = Path(f"/proc/{pid}/comm").read_text().strip()
            except OSError:
                command = ""
            if not (allow_mps and "nvidia-cuda-mps" in command):
                unexpected[pid] = used
        if unexpected:
            raise RuntimeError(
                f"GPU {gpu} is already used by unexpected compute processes: {unexpected}"
            )

    # Cleanup is intentionally not performed here: two validation runs may share
    # the host concurrently, and one preflight must never touch the other's IPC.
    cleanup = subprocess.CompletedProcess([], 0, "skipped for concurrent-safe preflight", "")
    return {
        "cleanup": cleanup.stdout.strip(),
        "mps_active": bool(active_mps),
        "mps_allowed": allow_mps,
        "gpu": gpu,
        "compute_mode": compute_mode,
    }


def server_command(baseline: Baseline, args: argparse.Namespace) -> list[str]:
    command = [
        str(args.online_python),
        "-s",
        "-m",
        "sglang.launch_server",
        "--model-path",
        str(args.online_model),
        "--served-model-name",
        args.served_model,
        "--host",
        "127.0.0.1",
        "--port",
        str(args.port),
        "--dtype",
        "bfloat16",
        "--context-length",
        str(args.context_length),
        "--mem-fraction-static",
        str(baseline.mem_fraction_static),
        "--max-running-requests",
        str(args.max_running_requests),
        "--chunked-prefill-size",
        str(args.chunked_prefill_size),
        "--random-seed",
        str(args.seed),
        "--enable-metrics",
    ]
    max_total_tokens = (
        baseline.max_total_tokens
        if args.max_total_tokens is None
        else args.max_total_tokens
    )
    if max_total_tokens is not None:
        command += ["--max-total-tokens", str(max_total_tokens)]
    if args.disable_cuda_graph:
        command.append("--disable-cuda-graph")
    if args.attention_backend is not None:
        command += ["--attention-backend", args.attention_backend]
    if args.sampling_backend is not None:
        command += ["--sampling-backend", args.sampling_backend]
    if args.enable_mixed_chunk:
        command.append("--enable-mixed-chunk")
    if args.replay_api == "generate":
        command += ["--stream-interval", "1"]
    return command


def trainer_command(
    baseline: Baseline,
    args: argparse.Namespace,
    metrics: Path,
    stop_file: Path,
) -> list[str]:
    overrides = getattr(args, "baseline_overrides", {}).get(baseline.name, {})
    reserve_mib = (
        overrides.get("cuda_reserve_mib", BASELINE_RESERVE_MIB.get(baseline.name, 0))
        if args.cuda_reserve_mib is None
        else args.cuda_reserve_mib
    )
    return [
        str(args.offline_python),
        "-s",
        "-m",
        "experiments.offline_train",
        "--model",
        str(args.offline_model),
        "--parquet",
        str(args.squad),
        "--output",
        str(metrics),
        "--stop-file",
        str(stop_file),
        "--sequence-length",
        str(args.train_sequence_length),
        "--batch-size",
        str(args.train_batch_size),
        "--gradient-accumulation",
        str(args.gradient_accumulation),
        "--training-mode",
        args.training_mode,
        "--trainable-layers",
        str(args.trainable_layers),
        "--cuda-reserve-mib",
        str(reserve_mib),
        "--seed",
        str(args.seed),
    ]


def scheduler_command(
    baseline: Baseline,
    args: argparse.Namespace,
    online_pid: int,
    offline_pid: int | None,
    output: Path,
    stop_file: Path,
) -> list[str]:
    command = [
        str(args.client_python),
        "-s",
        "-m",
        "experiments.scheduler",
        "--mode",
        baseline.scheduler_mode,
        "--online-pid",
        str(online_pid),
        "--metrics-url",
        f"http://127.0.0.1:{args.port}/metrics",
        "--output",
        str(output),
        "--stop-file",
        str(stop_file),
        "--offline-workers",
        str(args.offline_workers),
        "--online-timeslice-us",
        str(args.online_timeslice_us),
        "--initial-offline-timeslice-us",
        str(args.initial_offline_timeslice_us),
        "--physical-mib",
        str(args.physical_mib),
        "--poll-seconds",
        str(args.scheduler_poll_seconds),
        "--deescalate-seconds",
        str(args.deescalate_seconds),
        "--migration-timeout",
        str(args.migration_timeout),
    ]
    if offline_pid is not None:
        command += ["--offline-pid", str(offline_pid)]
    if args.policy is not None:
        command += ["--policy", str(args.policy)]
    if args.match_online_offline_max:
        command.append("--match-online-offline-max")
    if not baseline.timeslice_control:
        command.append("--skip-timeslice-control")
    return command


def monitor_command(
    args: argparse.Namespace,
    online_pid: int,
    offline_pid: int | None,
    output: Path,
    stop_file: Path,
) -> list[str]:
    command = [
        str(args.client_python),
        "-s",
        "-m",
        "experiments.monitor_gpu",
        "--gpu",
        str(args.gpu),
        "--output",
        str(output),
        "--stop-file",
        str(stop_file),
        "--poll-seconds",
        str(args.monitor_poll_seconds),
        "--pid",
        f"online={online_pid}",
    ]
    if offline_pid is not None:
        command += ["--pid", f"offline={offline_pid}"]
    return command


def replay_command(args: argparse.Namespace, output: Path) -> list[str]:
    return [
        str(args.client_python),
        "-s",
        "-m",
        "experiments.replay_tokens" if args.replay_api == "generate" else "experiments.replay_online",
        "--manifest",
        str(args.manifest),
        "--output",
        str(output),
        "--base-url",
        f"http://127.0.0.1:{args.port}",
        "--model",
        args.served_model,
        "--tokenizer",
        str(args.online_model),
        "--timeout",
        str(args.request_timeout_seconds),
    ]


def configured_baseline(args: argparse.Namespace) -> Baseline:
    overrides = getattr(args, "baseline_overrides", {}).get(args.baseline, {})
    return dataclasses.replace(
        BASELINES[args.baseline],
        **{key: overrides[key] for key in ("mem_fraction_static", "max_total_tokens")
           if key in overrides},
    )


def allocation_contract(args: argparse.Namespace) -> dict[str, float]:
    return {
        "target_gib": args.target_gib,
        "minimum_gib": args.target_gib - args.tolerance_gib,
        "maximum_gib": args.target_gib + args.upper_tolerance_gib,
    }


def record_managed_footprint(
    metadata: dict[str, Any], args: argparse.Namespace, managed_bytes: int
) -> None:
    contract = allocation_contract(args)
    managed_gib = managed_bytes / (1024 ** 3)
    valid = contract["minimum_gib"] <= managed_gib <= contract["maximum_gib"]
    metadata.update({
        "managed_peak_candidate_bytes": managed_bytes,
        "managed_peak_candidate_gib": managed_gib,
        "managed_allocation_contract": contract,
        "managed_allocation_in_bounds": valid,
    })
    # Calibration may be undersized so KV/padding can be measured and adjusted.
    # The upper bound remains mandatory, including during calibration.
    if managed_gib > contract["maximum_gib"] or (not args.calibration_only and not valid):
        raise RuntimeError(
            f"combined managed allocation {managed_gib:.3f} GiB is outside "
            f"the declared {contract['minimum_gib']:g}-{contract['maximum_gib']:g} GiB "
            "allocation bounds"
        )


def effective_online_timeslice_us(args: argparse.Namespace) -> int:
    """Validate the launch contract without changing the requested CLI value."""
    requested = resolve_online_timeslice_us(
        args.online_timeslice_us, args.match_online_offline_max, DEFAULT_STATES, None,
    )
    baseline = configured_baseline(args)
    if not baseline.timeslice_control and (
        args.match_online_offline_max or args.online_timeslice_us != DEFAULT_TIMESLICE_US
    ):
        raise ValueError("online timeslice configuration requires timeslice control")
    if not args.match_online_offline_max:
        return requested
    try:
        specs = load_state_specs(args.policy)
        function = load_timeslice_function(args.policy)
        validate_state_specs(specs, args.physical_mib)
        validate_thresholds(load_thresholds(args.policy))
        effective = resolve_online_timeslice_us(
            args.online_timeslice_us, True, specs, function,
            args.initial_offline_timeslice_us,
        )
    except (OSError, KeyError, TypeError, ValueError, AttributeError) as exc:
        raise ValueError(f"matching requires a valid policy with a known offline maximum: {exc}") from exc
    if args.online_timeslice_us not in (DEFAULT_TIMESLICE_US, effective):
        raise ValueError(
            f"online_timeslice_us={args.online_timeslice_us} conflicts with "
            f"chosen policy offline maximum {effective}"
        )
    return effective


def wait_for_online_timeslice_acceptance(
    log_path: Path,
    online_pid: int,
    effective_us: int,
    server: ManagedProcess,
    scheduler: ManagedProcess,
    *,
    timeout: float = 5.0,
) -> dict[str, Any]:
    """Require the existing driver diagnostic to confirm actuation before replay."""
    diagnostic = re.compile(
        r"SELECT_TIMESLICE pid=(\d+) requested_us=(\d+) ioctl_ret=(-?\d+) "
        r"status=(\d+) readback_us=(\d+)"
    )
    deadline = time.monotonic() + timeout
    while True:
        server.require_running()
        scheduler.require_running()
        latest = None
        if log_path.is_file():
            # Inspect the latest online write: a later failed/reverted write must
            # not be hidden by an earlier successful acceptance.
            for match in diagnostic.finditer(log_path.read_text(errors="replace")):
                pid, requested_us, ioctl_ret, status, readback_us = map(int, match.groups())
                if pid == online_pid:
                    latest = {
                        "pid": pid, "requested_us": requested_us, "ioctl_ret": ioctl_ret,
                        "status": status, "readback_us": readback_us,
                    }
        if latest is not None and (
            latest["requested_us"] == effective_us
            and latest["ioctl_ret"] == 0 and latest["status"] == 0
            and latest["readback_us"] == effective_us
        ):
            return {"verified": True, **latest}
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"online timeslice {effective_us} us was not accepted by PID {online_pid} "
                f"before replay; latest diagnostic={latest}"
            )
        time.sleep(0.1)


def run_experiment(args: argparse.Namespace) -> Path:
    online_timeslice_us = effective_online_timeslice_us(args)
    baseline = configured_baseline(args)
    preflight = ensure_preflight(
        args,
        allow_mps=baseline.mps_enabled,
        allow_unrelated_mps=args.allow_unrelated_mps,
    )
    run_dir = args.output_root / baseline.name / f"replicate-{args.replicate}"
    if run_dir.exists() and any(run_dir.iterdir()):
        raise RuntimeError(f"refusing to overwrite non-empty run directory {run_dir}")
    run_dir.mkdir(parents=True, exist_ok=True)
    stop_file = run_dir / "STOP"
    base_url = f"http://127.0.0.1:{args.port}"
    processes: list[ManagedProcess] = []
    started_wall = time.time()
    metadata: dict[str, Any] = {
        "baseline": dataclasses.asdict(baseline),
        "replicate": args.replicate,
        "seed": args.seed,
        "manifest": str(args.manifest) if args.manifest is not None else None,
        "manifest_sha256": sha256(args.manifest) if args.manifest is not None else None,
        "profile_sha256": sha256(args.profile) if args.profile is not None else None,
        "calibration_only": args.calibration_only,
        "managed_allocation_contract": allocation_contract(args),
        "preload_sha256": sha256(PRELOAD),
        "harness_sha256": harness_sha256(),
        "policy_sha256": sha256(args.policy) if args.policy is not None else None,
        "preflight": preflight,
        "mps_enabled": baseline.mps_enabled,
        "online_timeslice_us": online_timeslice_us,
        "online_timeslice_us_requested": args.online_timeslice_us,
        "match_online_offline_max": args.match_online_offline_max,
        "online_timeslice_from_offline_max": args.match_online_offline_max,
        "started_unix_seconds": started_wall,
        "arguments": vars(args),
    }
    # pathlib values are not JSON serializable in vars(args).
    metadata["arguments"] = {
        key: str(value) if isinstance(value, Path) else value
        for key, value in vars(args).items()
    }

    server: ManagedProcess | None = None
    trainer: ManagedProcess | None = None
    scheduler: ManagedProcess | None = None
    monitor: ManagedProcess | None = None
    replay: ManagedProcess | None = None
    failure_info: tuple[Any, Any, Any] | None = None
    replay_succeeded = False
    calibration_succeeded = False
    finalization_errors: list[str] = []

    def interrupt(signum: int, _frame: Any) -> None:
        raise InterruptedError(f"received signal {signum}")

    previous_handlers = {
        signum: signal.signal(signum, interrupt)
        for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP)
    }
    try:
        server = ManagedProcess(
            "sglang",
            server_command(baseline, args),
            env=base_environment(
                convert_alloc=baseline.convert_alloc,
                gpu=args.gpu,
                mps_enabled=baseline.mps_enabled,
                workers=args.offline_workers,
                cuda_path=args.cuda_path,
            ),
            log_path=run_dir / "server.log",
            kv_audit=args.kv_audit,
        )
        processes.append(server)
        metadata["server_command"] = server.command
        wait_http_ready(server, base_url, args.server_start_timeout)
        warmup(base_url, args.served_model)
        online_pid, online_uvm = wait_for_gpu_target(
            server.pid, server, gpu=args.gpu, timeout=args.control_ready_timeout
        )
        metadata["server_root_pid"] = server.pid
        metadata["online_gpu_pid"] = online_pid
        metadata["online_uvm_after_warmup"] = online_uvm
        metadata["server_info"] = http_json(base_url + "/server_info", timeout=10)
        expected_tokens = (
            baseline.max_total_tokens
            if args.max_total_tokens is None
            else args.max_total_tokens
        )
        reported_tokens = int(metadata["server_info"]["max_total_num_tokens"])
        if expected_tokens is not None and reported_tokens != expected_tokens:
            raise RuntimeError(
                f"{baseline.name} requested {expected_tokens} KV tokens but SGLang "
                f"reported {reported_tokens}"
            )

        offline_pid: int | None = None
        trainer_metrics = run_dir / "offline_train.jsonl"
        if baseline.training:
            trainer = ManagedProcess(
                "offline_train",
                trainer_command(baseline, args, trainer_metrics, stop_file),
                env=base_environment(
                    convert_alloc=baseline.convert_alloc,
                    gpu=args.gpu,
                    mps_enabled=baseline.mps_enabled,
                    workers=args.offline_workers,
                    cuda_path=args.cuda_path,
                ),
                log_path=run_dir / "offline_train.log",
            )
            processes.append(trainer)
            metadata["trainer_command"] = trainer.command
            offline_pid, offline_uvm = wait_for_gpu_target(
                trainer.pid, trainer, gpu=args.gpu, timeout=args.control_ready_timeout
            )
            metadata["trainer_root_pid"] = trainer.pid
            metadata["offline_gpu_pid"] = offline_pid
            metadata["offline_uvm_after_load"] = offline_uvm
            training_config = wait_for_jsonl_event(
                trainer_metrics,
                "optimizer_step",
                trainer,
                timeout=args.training_warmup_timeout,
            )
            config_records = [
                item
                for item in (json.loads(line) for line in trainer_metrics.read_text().splitlines())
                if item.get("event") == "config"
            ]
            if not config_records:
                raise RuntimeError("offline trainer did not emit its configuration")
            training_config = config_records[0]
            metadata["training_config"] = training_config
            if training_config.get("training_mode_effective") != args.training_mode:
                raise RuntimeError(
                    f"trainer mode mismatch: requested {args.training_mode}, got "
                    f"{training_config.get('training_mode_effective')}"
                )
            online_after_training = dataclasses.asdict(UvmController(online_pid).status())
            offline_after_training = dataclasses.asdict(UvmController(offline_pid).status())
            metadata["online_uvm_after_training_load"] = online_after_training
            metadata["offline_uvm_after_training_load"] = offline_after_training
            record_managed_footprint(
                metadata, args,
                int(online_after_training["managed_bytes"])
                + int(offline_after_training["managed_bytes"]),
            )

        if args.calibration_only:
            calibration_succeeded = True
        else:
            scheduler_output = run_dir / "scheduler.jsonl"
            scheduler = ManagedProcess(
                "scheduler",
                scheduler_command(
                    baseline, args, online_pid, offline_pid, scheduler_output, stop_file
                ),
                env=client_environment(args.gpu),
                log_path=run_dir / "scheduler.log",
                kv_audit=args.kv_audit,
            )
            processes.append(scheduler)
            metadata["scheduler_command"] = scheduler.command
            wait_for_jsonl_event(
                scheduler_output,
                "sample",
                scheduler,
                timeout=args.scheduler_ready_timeout,
            )

            monitor_output = run_dir / "gpu.jsonl"
            monitor = ManagedProcess(
                "monitor",
                monitor_command(args, online_pid, offline_pid, monitor_output, stop_file),
                env=client_environment(args.gpu),
                log_path=run_dir / "monitor.log",
            )
            processes.append(monitor)
            metadata["monitor_command"] = monitor.command
            time.sleep(args.pre_replay_settle_seconds)

            if args.match_online_offline_max or args.verify_fixed_timeslices:
                metadata["online_timeslice_verification"] = {"verified": False}
                metadata["online_timeslice_verification"] = wait_for_online_timeslice_acceptance(
                    run_dir / "server.log", online_pid, online_timeslice_us, server, scheduler,
                )
            if args.verify_fixed_timeslices and trainer is not None:
                metadata["offline_timeslice_verification"] = {"verified": False}
                metadata["offline_timeslice_verification"] = wait_for_online_timeslice_acceptance(
                    run_dir / "offline_train.log", offline_pid,
                    args.initial_offline_timeslice_us, trainer, scheduler,
                )

            online_output = run_dir / "online_requests.jsonl"
            replay = ManagedProcess(
                "replay",
                replay_command(args, online_output),
                env=client_environment(args.gpu),
                log_path=run_dir / "replay.log",
            )
            processes.append(replay)
            metadata["replay_command"] = replay.command
            replay_deadline = time.monotonic() + args.max_experiment_seconds
            while replay.poll() is None:
                server.require_running()
                scheduler.require_running()
                monitor.require_running()
                if trainer is not None:
                    trainer.require_running()
                if time.monotonic() >= replay_deadline:
                    raise RuntimeError("trace replay exceeded max_experiment_seconds")
                time.sleep(0.5)
            metadata["replay_returncode"] = replay.poll()
            if replay.poll() != 0:
                raise RuntimeError(
                    f"trace replay failed with status {replay.poll()}:\n{replay.tail()}"
                )
            request_results = [
                json.loads(line) for line in online_output.read_text().splitlines() if line.strip()
            ]
            manifest_count = sum(1 for line in args.manifest.read_text().splitlines() if line.strip())
            successful = sum(item.get("status") == "ok" for item in request_results)
            error_rate = 1.0 - (successful / len(request_results) if request_results else 0.0)
            metadata["request_result_count"] = len(request_results)
            metadata["successful_request_count"] = successful
            metadata["request_error_rate"] = error_rate
            if len(request_results) != manifest_count:
                raise RuntimeError(
                    f"replay emitted {len(request_results)} results for {manifest_count} requests"
                )
            if successful == 0 or error_rate > args.max_request_error_rate:
                raise RuntimeError(
                    f"request error rate {error_rate:.6f} exceeds "
                    f"limit {args.max_request_error_rate:.6f}"
                )
            replay_succeeded = True
    except BaseException:
        failure_info = sys.exc_info()
        exc = failure_info[1]
        metadata["error"] = f"{type(exc).__name__}: {exc}"
    finally:
        for signum, handler in previous_handlers.items():
            signal.signal(signum, handler)
        stop_file.touch()

        if replay_succeeded or calibration_succeeded:
            graceful = [
                (scheduler, run_dir / "scheduler.jsonl", "scheduler"),
                (monitor, run_dir / "gpu.jsonl", "monitor"),
                (trainer, run_dir / "offline_train.jsonl", "offline trainer"),
            ]
            for process, output_path, label in graceful:
                if process is None:
                    continue
                if not process.wait(args.shutdown_timeout_seconds):
                    finalization_errors.append(f"{label} did not stop after STOP")
                elif process.poll() != 0:
                    finalization_errors.append(
                        f"{label} exited with status {process.poll()}: {process.tail()}"
                    )
                if not has_jsonl_event(output_path, "shutdown"):
                    finalization_errors.append(f"{label} did not emit a shutdown event")

        for process in reversed(processes):
            try:
                process.stop()
            except Exception as exc:
                finalization_errors.append(
                    f"failed to terminate {process.name} process group: {exc}"
                )
        metadata["process_returncodes"] = {
            process.name: process.poll() for process in processes
        }
        if (replay_succeeded or calibration_succeeded) and not finalization_errors and failure_info is None:
            metadata["status"] = "calibrated" if calibration_succeeded else "completed"
        else:
            metadata["status"] = "failed"
            if finalization_errors:
                metadata["finalization_errors"] = finalization_errors
        metadata["finished_unix_seconds"] = time.time()
        metadata["wall_seconds"] = metadata["finished_unix_seconds"] - started_wall

        # Each instrumented process cleans its own slot, queue, and socket on a
        # normal exit. Global stale-state cleanup is deferred until no validation
        # run is active, so parallel GPU runs cannot interfere with each other.
        metadata["post_run_cleanup"] = {"status": "deferred_concurrent_safe"}
        (run_dir / "metadata.json").write_text(
            json.dumps(metadata, indent=2, sort_keys=True, allow_nan=False) + "\n"
        )

    if failure_info is not None:
        raise failure_info[1].with_traceback(failure_info[2])
    if finalization_errors:
        raise RuntimeError("; ".join(finalization_errors))
    return run_dir


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", type=Path, help="JSON runtime defaults and per-baseline overrides")
    parser.add_argument("--calibration-only", action="store_true",
                        help="record warmed allocations and stop before scheduler/replay")
    parser.add_argument("--online-python", type=Path, default=PYTHON)
    parser.add_argument("--offline-python", type=Path, default=PYTHON)
    parser.add_argument("--client-python", type=Path, default=PYTHON)
    parser.add_argument("--online-model", type=Path, default=ONLINE_MODEL)
    parser.add_argument("--offline-model", type=Path, default=OFFLINE_MODEL)
    parser.add_argument("--squad", type=Path, default=SQUAD)
    parser.add_argument("--served-model", default=SERVED_MODEL)
    parser.add_argument("--cuda-path", type=Path, default=None)
    parser.add_argument("--physical-mib", type=int, default=81_920)
    parser.add_argument("--target-gib", type=float, default=92.0)
    parser.add_argument("--tolerance-gib", type=float, default=1.0,
                        help="allowed allocation shortfall below target")
    parser.add_argument("--upper-tolerance-gib", type=float, default=0.0,
                        help="allowed excess above target; zero enforces a hard cap")
    parser.add_argument("--attention-backend")
    parser.add_argument("--sampling-backend")
    parser.add_argument("--baseline", choices=tuple(BASELINES), required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output-root", type=Path, default=ROOT / "results" / "raw")
    parser.add_argument("--replicate", type=int, default=1)
    parser.add_argument("--seed", type=int, default=2027)
    parser.add_argument("--port", type=int, default=30000)
    parser.add_argument("--gpu", type=int, default=0)
    parser.add_argument("--allow-unrelated-mps", action="store_true")
    parser.add_argument("--policy", type=Path)
    parser.add_argument("--context-length", type=int, default=4096)
    parser.add_argument("--max-total-tokens", type=int)
    parser.add_argument("--max-running-requests", type=int, default=64)
    parser.add_argument("--chunked-prefill-size", type=int, default=256)
    parser.add_argument("--enable-mixed-chunk", action="store_true")
    parser.add_argument("--replay-api", choices=("completions", "generate"), default="generate")
    parser.add_argument("--disable-cuda-graph", action="store_true")
    parser.add_argument("--kv-audit", action="store_true",
                        help="enable common inference-slot instrumentation for the Table III policies")
    parser.add_argument("--train-sequence-length", type=int, default=128)
    parser.add_argument("--train-batch-size", type=int, default=1)
    parser.add_argument("--gradient-accumulation", type=int, default=4)
    parser.add_argument(
        "--training-mode", choices=("last-layers", "lora"), default="lora"
    )
    parser.add_argument("--trainable-layers", type=int, default=1)
    parser.add_argument(
        "--cuda-reserve-mib",
        type=int,
        default=None,
        help="override baseline-specific padding used to meet the managed-allocation target",
    )
    parser.add_argument("--offline-workers", type=int, default=108)
    parser.add_argument("--online-timeslice-us", type=int, default=DEFAULT_TIMESLICE_US,
                        help="fixed online timeslice during sampling/replay; model warmup uses 8192 us")
    parser.add_argument("--initial-offline-timeslice-us", type=int, default=DEFAULT_TIMESLICE_US,
                        help="training timeslice set at scheduler initialization; retained by fixed baselines")
    parser.add_argument("--verify-fixed-timeslices", action="store_true",
                        help="require online/offline driver readback before replay for fixed baselines")
    parser.add_argument("--match-online-offline-max", action=argparse.BooleanOptionalAction,
                        default=False, help="match fixed online timeslice to the chosen offline maximum")
    parser.add_argument("--request-timeout-seconds", type=float, default=900.0)
    parser.add_argument("--max-request-error-rate", type=float, default=0.0)
    parser.add_argument("--shutdown-timeout-seconds", type=float, default=120.0)
    parser.add_argument("--scheduler-poll-seconds", type=float, default=0.01)
    parser.add_argument("--deescalate-seconds", type=float, default=0.1)
    parser.add_argument("--monitor-poll-seconds", type=float, default=0.2)
    parser.add_argument("--pre-replay-settle-seconds", type=float, default=3.0)
    parser.add_argument("--server-start-timeout", type=float, default=900.0)
    parser.add_argument("--control-ready-timeout", type=float, default=180.0)
    parser.add_argument("--training-warmup-timeout", type=float, default=900.0)
    parser.add_argument("--scheduler-ready-timeout", type=float, default=1500.0)
    parser.add_argument("--migration-timeout", type=float, default=600.0)
    parser.add_argument("--max-experiment-seconds", type=float, default=2400.0)
    return parser


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    profile_parser = argparse.ArgumentParser(add_help=False)
    profile_parser.add_argument("--profile", type=Path)
    selected, _ = profile_parser.parse_known_args(argv)
    parser = build_parser()
    overrides: dict[str, Any] = {}
    allowed_gpus = list(range(8))
    calibration_required = False
    if selected.profile is not None:
        profile_path = selected.profile.expanduser().resolve()
        profile = json.loads(profile_path.read_text())
        if not isinstance(profile, dict):
            raise ValueError("profile must be a JSON object")
        unknown = set(profile) - {"runtime", "baselines", "allowed_gpus", "calibration_required"}
        if unknown:
            raise ValueError(f"unknown profile fields: {sorted(unknown)}")
        runtime = profile.get("runtime", {})
        if not isinstance(runtime, dict):
            raise ValueError("profile runtime must be an object")
        actions = {action.dest: action for action in parser._actions}
        defaults = {}
        for name, value in runtime.items():
            if name not in actions or name in {"help", "profile", "baseline", "calibration_only"}:
                raise ValueError(f"unsupported profile runtime argument: {name}")
            action = actions[name]
            if isinstance(action, (
                argparse._StoreTrueAction, argparse._StoreFalseAction, argparse.BooleanOptionalAction,
            )):
                if not isinstance(value, bool):
                    raise ValueError(f"profile {name} must be boolean")
            elif value is not None and action.type is not None:
                if action.type in (int, float) and (
                    isinstance(value, bool) or not isinstance(value, (int, float))
                    or (action.type is int and not isinstance(value, int))
                ):
                    raise ValueError(f"profile {name} has an invalid numeric value")
                value = action.type(value)
                if action.type is Path:
                    value = value.expanduser()
                    if not value.is_absolute():
                        value = profile_path.parent / value
            if action.choices is not None and value not in action.choices:
                raise ValueError(f"invalid profile value for {name}: {value}")
            if value is None and action.default is not None and name != "cuda_path":
                raise ValueError(f"profile {name} cannot be null")
            defaults[name] = value
        parser.set_defaults(**defaults)
        allowed_gpus = profile.get("allowed_gpus", allowed_gpus)
        if not isinstance(allowed_gpus, list) or not allowed_gpus or any(
            isinstance(gpu, bool) or not isinstance(gpu, int) or gpu < 0 for gpu in allowed_gpus
        ):
            raise ValueError("profile allowed_gpus must be a nonempty list of GPU indices")
        calibration_required = profile.get("calibration_required", False)
        if not isinstance(calibration_required, bool):
            raise ValueError("profile calibration_required must be boolean")
        overrides = profile.get("baselines", {})
        if not isinstance(overrides, dict) or set(overrides) - set(BASELINES):
            raise ValueError("profile contains invalid baseline names")
        for name, values in overrides.items():
            if not isinstance(values, dict) or set(values) - {
                "mem_fraction_static", "max_total_tokens", "cuda_reserve_mib", "calibration_required"
            }:
                raise ValueError(f"invalid baseline overrides for {name}")
            for field in ("max_total_tokens", "cuda_reserve_mib"):
                if field not in values or (field == "max_total_tokens" and values[field] is None):
                    continue
                value = values[field]
                if isinstance(value, bool) or not isinstance(value, int) or value < (1 if field == "max_total_tokens" else 0):
                    raise ValueError(f"invalid {field} for {name}")
            if "mem_fraction_static" in values:
                value = values["mem_fraction_static"]
                if isinstance(value, bool) or not isinstance(value, (int, float)) or not 0 < value <= 1:
                    raise ValueError(f"invalid mem_fraction_static for {name}")
            if "calibration_required" in values and not isinstance(values["calibration_required"], bool):
                raise ValueError(f"invalid calibration_required for {name}")
    args = parser.parse_args(argv)
    args.baseline_overrides = overrides
    args.profile_allowed_gpus = allowed_gpus
    args.profile_calibration_required = calibration_required
    for name, value in vars(args).items():
        if isinstance(value, Path):
            expanded = value.expanduser()
            # Keep interpreter symlinks: resolving a venv's python can select its base environment.
            setattr(args, name, expanded.absolute() if name.endswith("_python") else expanded.resolve())
    return args


def validate_args(args: argparse.Namespace) -> None:
    baseline = configured_baseline(args)
    if type(args.initial_offline_timeslice_us) is not int or args.initial_offline_timeslice_us <= 0:
        raise ValueError("initial_offline_timeslice_us must be a positive integer")
    if args.verify_fixed_timeslices and (not baseline.timeslice_control or baseline.scheduler_mode == "ours"):
        raise ValueError("fixed timeslice verification requires a fixed baseline with timeslice control")
    overrides = getattr(args, "baseline_overrides", {}).get(args.baseline, {})
    if not args.calibration_only and (
        getattr(args, "profile_calibration_required", False)
        or overrides.get("calibration_required", False)
    ):
        raise ValueError("profile requires calibration; freeze measured capacities and clear calibration_required before replay")
    if (
        not args.calibration_only
        and baseline.max_total_tokens is not None
        and args.max_total_tokens is not None
        and args.max_total_tokens != baseline.max_total_tokens
    ):
        raise ValueError(
            f"{baseline.name} requires max_total_tokens={baseline.max_total_tokens}; "
            f"override {args.max_total_tokens} would change the baseline identity"
        )
    allowed_gpus = getattr(args, "profile_allowed_gpus", [0, 1])
    if args.gpu not in allowed_gpus:
        raise ValueError(f"gpu must be one of {allowed_gpus} for this profile")
    if args.manifest is None and not args.calibration_only:
        raise ValueError("--manifest is required unless --calibration-only is used")
    if args.manifest is not None and not args.manifest.is_file():
        raise ValueError(f"manifest does not exist: {args.manifest}")
    if args.max_total_tokens is not None and args.max_total_tokens <= 0:
        raise ValueError("max_total_tokens must be positive")
    if not math.isfinite(args.target_gib) or args.target_gib <= 0:
        raise ValueError("target_gib must be finite and positive")
    for name in ("tolerance_gib", "upper_tolerance_gib"):
        value = getattr(args, name)
        if not math.isfinite(value) or value < 0:
            raise ValueError(f"{name} must be finite and non-negative")
    if args.tolerance_gib >= args.target_gib:
        raise ValueError("tolerance_gib must be below target_gib")
    if args.policy is not None and not args.policy.is_file():
        raise ValueError(f"policy does not exist: {args.policy}")
    effective_online_timeslice_us(args)
    integer_positive = [
        "replicate",
        "port",
        "context_length",
        "max_running_requests",
        "chunked_prefill_size",
        "train_sequence_length",
        "train_batch_size",
        "gradient_accumulation",
        "trainable_layers",
        "offline_workers",
        "physical_mib",
    ]
    for name in integer_positive:
        if getattr(args, name) <= 0:
            raise ValueError(f"{name} must be positive")
    if args.cuda_reserve_mib is not None and args.cuda_reserve_mib < 0:
        raise ValueError("cuda_reserve_mib must be non-negative")
    for name in [
        "request_timeout_seconds",
        "shutdown_timeout_seconds",
        "scheduler_poll_seconds",
        "deescalate_seconds",
        "monitor_poll_seconds",
        "pre_replay_settle_seconds",
        "server_start_timeout",
        "control_ready_timeout",
        "training_warmup_timeout",
        "scheduler_ready_timeout",
        "migration_timeout",
        "max_experiment_seconds",
    ]:
        value = getattr(args, name)
        if not math.isfinite(value) or value <= 0:
            raise ValueError(f"{name} must be finite and positive")
    if not math.isfinite(args.max_request_error_rate) or not 0 <= args.max_request_error_rate <= 1:
        raise ValueError("max_request_error_rate must be finite and between zero and one")


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    validate_args(args)
    run_dir = run_experiment(args)
    print(run_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
