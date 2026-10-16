#!/usr/bin/env python3
"""Control client for processes instrumented by select.cpp.

This module intentionally mirrors the ABI in select.cpp/taskman.c instead of
linking taskman.c.  taskman.c and schedule1S.cpp remain read-only references.
"""

from __future__ import annotations

import ctypes
import dataclasses
import errno
import json
import os
import pathlib
import socket
import time
from typing import Any, Iterable

BUF_SIZE = 1024 * 1024
MAX_CONTROL_SLOTS = 1024
IPC_KEY_BASE = 2647
MQ_MESSAGE_SIZE = 16
# Driver-default quantum used by the paper baselines; model-loading defaults
# inside the preloaded worker are independent of this controller setting.
DEFAULT_TIMESLICE_US = 2048


class ControlError(RuntimeError):
    """Raised when an instrumented process cannot be controlled safely."""


class _Slot(ctypes.Structure):
    _fields_ = [
        ("pid", ctypes.c_long),
        ("override_x_dim", ctypes.c_int),
        ("n_slices", ctypes.c_int),
        ("mask", ctypes.c_uint32 * 4),
        ("timeslice_us", ctypes.c_uint64),
        ("memory_limit", ctypes.c_uint64),
    ]


@dataclasses.dataclass(frozen=True)
class SlotSnapshot:
    pid: int
    worker_threads: int
    n_slices: int
    timeslice_us: int
    memory_limit: int


@dataclasses.dataclass(frozen=True)
class UvmStatus:
    pid: int
    managed_bytes: int
    allocation_count: int
    allocation_generation: int
    busy: bool
    job_id: int
    completed_job: int
    last_error: int
    completed_cpu_bytes: int
    completed_gpu_bytes: int
    job_completed_bytes: int

    @classmethod
    def from_reply(cls, reply: dict[str, Any]) -> "UvmStatus":
        if not reply.get("ok"):
            raise ControlError(f"UVM status failed: {reply}")
        return cls(
            pid=int(reply["pid"]),
            managed_bytes=int(reply["managed_bytes"]),
            allocation_count=int(reply["allocation_count"]),
            allocation_generation=int(reply.get("allocation_generation", 0)),
            busy=bool(reply["busy"]),
            job_id=int(reply["job_id"]),
            completed_job=int(reply["completed_job"]),
            last_error=int(reply["last_error"]),
            completed_cpu_bytes=int(reply["completed_cpu_bytes"]),
            completed_gpu_bytes=int(reply["completed_gpu_bytes"]),
            job_completed_bytes=int(reply["job_completed_bytes"]),
        )


@dataclasses.dataclass(frozen=True)
class UvmTargetResult:
    requested_bytes: int
    clamped_bytes: int
    submitted_bytes: int
    status: UvmStatus


@dataclasses.dataclass(frozen=True)
class UvmRetargetResult:
    previous_requested_bytes: int
    previous_submitted_bytes: int
    requested_bytes: int
    clamped_bytes: int
    submitted_bytes: int
    status: UvmStatus


def registry_key(hostname: str | None = None) -> int:
    raw = (hostname or socket.gethostname()).encode()
    return IPC_KEY_BASE + sum(byte + 1 for byte in raw)


def process_start_time(pid: int) -> int:
    try:
        text = pathlib.Path(f"/proc/{pid}/stat").read_text()
    except OSError as exc:
        raise ControlError(f"cannot read process {pid}: {exc}") from exc
    close_paren = text.rfind(")")
    if close_paren < 0:
        raise ControlError(f"malformed /proc/{pid}/stat")
    fields_after_comm = text[close_paren + 2 :].split()
    try:
        return int(fields_after_comm[19])  # Field 22 overall: starttime.
    except (IndexError, ValueError) as exc:
        raise ControlError(f"malformed /proc/{pid}/stat starttime") from exc


def process_uid(pid: int) -> int:
    try:
        return pathlib.Path(f"/proc/{pid}").stat().st_uid
    except OSError as exc:
        raise ControlError(f"cannot stat process {pid}: {exc}") from exc


class SharedRegistry:
    """Attach to select.cpp's SysV process registry without creating it."""

    def __init__(self, key: int | None = None, slot_count: int = MAX_CONTROL_SLOTS):
        self.key = registry_key() if key is None else key
        self.slot_count = slot_count
        self._libc = ctypes.CDLL(None, use_errno=True)
        self._libc.shmget.argtypes = [ctypes.c_int, ctypes.c_size_t, ctypes.c_int]
        self._libc.shmget.restype = ctypes.c_int
        self._libc.shmat.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_int]
        self._libc.shmat.restype = ctypes.c_void_p
        self._libc.shmdt.argtypes = [ctypes.c_void_p]
        self._libc.shmdt.restype = ctypes.c_int
        self._address: int | None = None
        self._slots: Any = None

    def attach(self) -> "SharedRegistry":
        if self._address is not None:
            return self
        shmid = self._libc.shmget(self.key, BUF_SIZE, 0o600)
        if shmid < 0:
            err = ctypes.get_errno()
            raise ControlError(
                f"select.cpp registry key {self.key} is unavailable: {os.strerror(err)}"
            )
        address = self._libc.shmat(shmid, None, 0)
        if address == ctypes.c_void_p(-1).value:
            err = ctypes.get_errno()
            raise ControlError(f"cannot attach shared registry: {os.strerror(err)}")
        array_type = _Slot * self.slot_count
        self._address = int(address)
        self._slots = array_type.from_address(self._address)
        return self

    def close(self) -> None:
        if self._address is None:
            return
        if self._libc.shmdt(ctypes.c_void_p(self._address)) != 0:
            err = ctypes.get_errno()
            raise ControlError(f"cannot detach shared registry: {os.strerror(err)}")
        self._address = None
        self._slots = None

    def __enter__(self) -> "SharedRegistry":
        return self.attach()

    def __exit__(self, *_: object) -> None:
        self.close()

    def _require_slots(self) -> Any:
        if self._slots is None:
            self.attach()
        return self._slots

    def find_index(self, pid: int) -> int:
        for index, slot in enumerate(self._require_slots()):
            if int(slot.pid) == pid:
                return index
        raise ControlError(f"PID {pid} is not registered by select.cpp")

    def snapshot(self, pid: int) -> SlotSnapshot:
        slot = self._require_slots()[self.find_index(pid)]
        return SlotSnapshot(
            pid=int(slot.pid),
            worker_threads=int(slot.override_x_dim),
            n_slices=int(slot.n_slices),
            timeslice_us=int(slot.timeslice_us),
            memory_limit=int(slot.memory_limit),
        )

    def snapshots(self) -> list[SlotSnapshot]:
        result = []
        for slot in self._require_slots():
            pid = int(slot.pid)
            if pid <= 0:
                continue
            result.append(
                SlotSnapshot(
                    pid=pid,
                    worker_threads=int(slot.override_x_dim),
                    n_slices=int(slot.n_slices),
                    timeslice_us=int(slot.timeslice_us),
                    memory_limit=int(slot.memory_limit),
                )
            )
        return result

    def prune_dead_slots(self) -> list[int]:
        """Clear slots whose PID no longer exists.

        This is intended for launcher preflight before new instrumented processes
        are started. A live PID is always retained, even if it is unrelated and
        merely reused a stale slot's numeric PID.
        """
        cleared: list[int] = []
        for slot in self._require_slots():
            pid = int(slot.pid)
            if pid <= 0:
                continue
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                slot.pid = 0
                cleared.append(pid)
            except PermissionError:
                continue
        return cleared

    def update(
        self,
        pid: int,
        *,
        worker_threads: int | None = None,
        timeslice_us: int | None = None,
    ) -> SlotSnapshot:
        slot = self._require_slots()[self.find_index(pid)]
        if worker_threads is not None:
            if worker_threads < 0:
                raise ValueError("worker_threads must be non-negative")
            slot.override_x_dim = worker_threads
        if timeslice_us is not None:
            if timeslice_us < 1:
                raise ValueError("timeslice_us must be positive")
            slot.timeslice_us = timeslice_us
        return self.snapshot(pid)


class ProcessController:
    """Safely mutate one process slot and ring its POSIX-MQ doorbell."""

    def __init__(self, pid: int, registry: SharedRegistry | None = None):
        if pid <= 0:
            raise ValueError("pid must be positive")
        if process_uid(pid) != os.getuid():
            raise ControlError(f"PID {pid} is owned by another user")
        self.pid = pid
        self._start_time = process_start_time(pid)
        self.registry = registry or SharedRegistry()
        self._owns_registry = registry is None
        self._resume_threads: int | None = None
        try:
            snapshot = self.registry.snapshot(pid)
        except Exception:
            if self._owns_registry:
                self.registry.close()
            raise
        if snapshot.worker_threads > 0:
            self._resume_threads = snapshot.worker_threads

    @property
    def queue_name(self) -> bytes:
        return f"/PropertyA_DA_{self.pid:x}".encode()

    def close(self) -> None:
        if self._owns_registry:
            self.registry.close()

    def __enter__(self) -> "ProcessController":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _validate_identity(self) -> None:
        if process_start_time(self.pid) != self._start_time:
            raise ControlError(f"PID {self.pid} was replaced; refusing stale control")
        if process_uid(self.pid) != os.getuid():
            raise ControlError(f"PID {self.pid} ownership changed")

    def _notify(self) -> None:
        self._validate_identity()
        librt = ctypes.CDLL("librt.so.1", use_errno=True)
        librt.mq_open.argtypes = [ctypes.c_char_p, ctypes.c_int]
        librt.mq_open.restype = ctypes.c_int
        librt.mq_send.argtypes = [
            ctypes.c_int,
            ctypes.c_char_p,
            ctypes.c_size_t,
            ctypes.c_uint,
        ]
        librt.mq_send.restype = ctypes.c_int
        librt.mq_close.argtypes = [ctypes.c_int]
        librt.mq_close.restype = ctypes.c_int

        descriptor = librt.mq_open(self.queue_name, os.O_WRONLY)
        if descriptor < 0:
            err = ctypes.get_errno()
            raise ControlError(
                f"cannot open queue {self.queue_name.decode()}: {os.strerror(err)}"
            )
        message = b"done\0".ljust(MQ_MESSAGE_SIZE, b"\0")
        try:
            if librt.mq_send(descriptor, message, len(message), 0) != 0:
                err = ctypes.get_errno()
                raise ControlError(f"cannot notify PID {self.pid}: {os.strerror(err)}")
        finally:
            librt.mq_close(descriptor)

    def snapshot(self) -> SlotSnapshot:
        self._validate_identity()
        return self.registry.snapshot(self.pid)

    def set_controls(
        self,
        *,
        worker_threads: int | None = None,
        timeslice_us: int | None = None,
        settle_seconds: float = 0.05,
    ) -> SlotSnapshot:
        self._validate_identity()
        if worker_threads is not None and worker_threads > 0:
            self._resume_threads = worker_threads
        requested = self.registry.update(
            self.pid,
            worker_threads=worker_threads,
            timeslice_us=timeslice_us,
        )
        self._notify()
        if settle_seconds > 0:
            time.sleep(settle_seconds)
        observed = self.snapshot()
        if timeslice_us is not None and observed.timeslice_us != timeslice_us:
            raise ControlError(
                f"PID {self.pid} rejected timeslice {timeslice_us}; "
                f"slot now reports {observed.timeslice_us}"
            )
        if worker_threads is not None and observed.worker_threads != worker_threads:
            raise ControlError(
                f"PID {self.pid} did not retain worker setting {worker_threads}"
            )
        return requested

    def pause(self) -> SlotSnapshot:
        current = self.snapshot()
        if current.worker_threads > 0:
            self._resume_threads = current.worker_threads
        return self.set_controls(worker_threads=0)

    def resume(self, worker_threads: int | None = None) -> SlotSnapshot:
        target = worker_threads or self._resume_threads
        if target is None or target <= 0:
            raise ControlError(f"no positive resume worker count recorded for PID {self.pid}")
        return self.set_controls(worker_threads=target)

    def set_timeslice(self, timeslice_us: int) -> SlotSnapshot:
        return self.set_controls(timeslice_us=timeslice_us)


def aligned_uvm_target(
    allocation_sizes: Iterable[int], desired_bytes: int, page_size: int
) -> int:
    """Round a global residency target at the current allocation boundary.

    select.cpp requires only a partial allocation's offset to be page aligned;
    preceding allocation sizes do not themselves have to be page multiples.
    """
    if desired_bytes < 0 or page_size <= 0:
        raise ValueError("invalid target or page size")
    cumulative = 0
    for size in allocation_sizes:
        if size < 0:
            raise ValueError("allocation sizes must be non-negative")
        end = cumulative + size
        if desired_bytes >= end:
            cumulative = end
            continue
        offset = desired_bytes - cumulative
        offset -= offset % page_size
        return cumulative + offset
    return cumulative


class UvmController:
    """One-command-per-connection client for select.cpp's UVM socket."""

    def __init__(self, pid: int, timeout: float = 2.0):
        if pid <= 0:
            raise ValueError("pid must be positive")
        if process_uid(pid) != os.getuid():
            raise ControlError(f"PID {pid} is owned by another user")
        self.pid = pid
        self._start_time = process_start_time(pid)
        self.timeout = timeout
        self.path = f"/tmp/select-uvm-{os.getuid()}-{pid}.sock"
        self._target_signature: tuple[tuple[int, int, int], ...] | None = None
        self._target_generation: int | None = None
        self._submitted_target: int | None = None
        self._pending_job: int | None = None
        self._pending_signature: tuple[tuple[int, int, int], ...] | None = None
        self._pending_generation: int | None = None
        self._pending_target: int | None = None

    def _validate_identity(self) -> None:
        if process_start_time(self.pid) != self._start_time:
            raise ControlError(f"PID {self.pid} was replaced; refusing stale UVM control")

    def command(self, command: str) -> dict[str, Any]:
        self._validate_identity()
        if "\0" in command or "\n" in command or "\r" in command:
            raise ValueError("UVM command must be one line without terminators")
        wire = (command + "\n").encode("ascii")
        if len(wire) >= 512:
            raise ValueError("UVM command is too long")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as client:
            client.settimeout(self.timeout)
            try:
                client.connect(self.path)
                client.sendall(wire)
                chunks: list[bytes] = []
                total = 0
                while True:
                    chunk = client.recv(4096)
                    if not chunk:
                        break
                    chunks.append(chunk)
                    total += len(chunk)
                    if total > 1024 * 1024:
                        raise ControlError("oversized UVM response")
                    if b"\n" in chunk:
                        break
            except OSError as exc:
                raise ControlError(f"UVM command to PID {self.pid} failed: {exc}") from exc
        raw = b"".join(chunks).split(b"\n", 1)[0]
        try:
            reply = json.loads(raw)
        except json.JSONDecodeError as exc:
            raise ControlError(f"invalid UVM response from PID {self.pid}: {raw!r}") from exc
        if not isinstance(reply, dict):
            raise ControlError(f"unexpected UVM response from PID {self.pid}: {reply!r}")
        return reply

    def status(self) -> UvmStatus:
        status = UvmStatus.from_reply(self.command("status"))
        if status.pid != self.pid:
            raise ControlError(
                f"UVM socket identity mismatch: expected {self.pid}, got {status.pid}"
            )
        return status

    def _allocation_snapshot(self) -> tuple[int, list[dict[str, int]]]:
        reply = self.command("list")
        if not reply.get("ok"):
            raise ControlError(f"UVM list failed: {reply}")
        result = []
        for allocation in reply.get("allocations", []):
            result.append(
                {
                    "id": int(allocation["id"]),
                    "bytes": int(allocation["bytes"]),
                    "device": int(allocation["device"]),
                }
            )
        return int(reply.get("allocation_generation", 0)), result

    def allocations(self) -> list[dict[str, int]]:
        return self._allocation_snapshot()[1]

    def _reconcile_pending(self, status: UvmStatus) -> None:
        if self._pending_job is None:
            return
        job_id = self._pending_job
        if status.completed_job < job_id:
            raise ControlError(
                f"UVM worker for PID {self.pid} became idle before job {job_id} completed"
            )
        signature = self._pending_signature
        generation = self._pending_generation
        target = self._pending_target
        self._pending_job = None
        self._pending_signature = None
        self._pending_generation = None
        self._pending_target = None
        if status.last_error != 0:
            self._target_signature = None
            self._target_generation = None
            self._submitted_target = None
            raise ControlError(
                f"UVM job {job_id} for PID {self.pid} failed with CUDA error "
                f"{status.last_error}"
            )
        self._target_signature = signature
        self._target_generation = generation
        self._submitted_target = target

    def wait_idle(self, timeout: float = 120.0, poll_seconds: float = 0.05) -> UvmStatus:
        deadline = time.monotonic() + timeout
        while True:
            status = self.status()
            if not status.busy:
                self._reconcile_pending(status)
                return status
            if time.monotonic() >= deadline:
                raise ControlError(f"UVM worker for PID {self.pid} remained busy")
            time.sleep(poll_seconds)

    def _wait_job(
        self,
        job_id: int,
        timeout: float = 120.0,
        poll_seconds: float = 0.05,
    ) -> UvmStatus:
        deadline = time.monotonic() + timeout
        while True:
            status = self.status()
            if not status.busy and status.completed_job >= job_id:
                if status.last_error != 0:
                    raise ControlError(
                        f"UVM job {job_id} for PID {self.pid} failed with CUDA error "
                        f"{status.last_error}"
                    )
                return status
            if time.monotonic() >= deadline:
                raise ControlError(f"timed out waiting for UVM job {job_id}")
            time.sleep(poll_seconds)

    def set_target(
        self,
        gpu_bytes: int,
        *,
        timeout: float = 120.0,
        wait: bool = True,
    ) -> UvmTargetResult | int:
        if gpu_bytes < 0:
            raise ValueError("gpu_bytes must be non-negative")
        status = self.wait_idle(timeout=timeout)
        desired = min(gpu_bytes, status.managed_bytes)
        generation, allocations = self._allocation_snapshot()
        signature = tuple(
            (allocation["id"], allocation["bytes"], allocation["device"])
            for allocation in allocations
        )
        target = aligned_uvm_target(
            (allocation["bytes"] for allocation in allocations),
            desired,
            os.sysconf("SC_PAGE_SIZE"),
        )
        reply = self.command(f"targetv {generation} {target}")
        if not reply.get("ok"):
            raise ControlError(f"UVM target {target} for PID {self.pid} failed: {reply}")
        job_id = int(reply["job_id"])
        if not wait:
            self._pending_job = job_id
            self._pending_signature = signature
            self._pending_generation = generation
            self._pending_target = target
            return job_id
        try:
            completed = self._wait_job(job_id, timeout=timeout)
        except Exception:
            self._target_signature = None
            self._target_generation = None
            self._submitted_target = None
            raise
        self._target_signature = signature
        self._target_generation = generation
        self._submitted_target = target
        return UvmTargetResult(
            requested_bytes=gpu_bytes,
            clamped_bytes=desired,
            submitted_bytes=target,
            status=completed,
        )

    def retarget(
        self,
        previous_gpu_bytes: int,
        gpu_bytes: int,
        *,
        timeout: float = 120.0,
        wait: bool = True,
    ) -> UvmRetargetResult | UvmTargetResult | int:
        if previous_gpu_bytes < 0 or gpu_bytes < 0:
            raise ValueError("GPU targets must be non-negative")
        status = self.wait_idle(timeout=timeout)
        generation, allocations = self._allocation_snapshot()
        signature = tuple(
            (allocation["id"], allocation["bytes"], allocation["device"])
            for allocation in allocations
        )
        sizes = [allocation["bytes"] for allocation in allocations]
        page_size = os.sysconf("SC_PAGE_SIZE")
        previous_desired = min(previous_gpu_bytes, status.managed_bytes)
        desired = min(gpu_bytes, status.managed_bytes)
        previous_target = aligned_uvm_target(sizes, previous_desired, page_size)
        target = aligned_uvm_target(sizes, desired, page_size)
        if (
            self._target_signature != signature
            or self._target_generation != generation
            or self._submitted_target != previous_target
        ):
            return self.set_target(gpu_bytes, timeout=timeout, wait=wait)
        reply = self.command(
            f"retarget {generation} {previous_target} {target}"
        )
        if not reply.get("ok"):
            raise ControlError(
                f"UVM retarget {previous_target}->{target} for PID {self.pid} "
                f"failed: {reply}"
            )
        job_id = int(reply["job_id"])
        if not wait:
            self._pending_job = job_id
            self._pending_signature = signature
            self._pending_generation = generation
            self._pending_target = target
            return job_id
        try:
            completed = self._wait_job(job_id, timeout=timeout)
        except Exception:
            self._target_signature = None
            self._target_generation = None
            self._submitted_target = None
            raise
        self._target_signature = signature
        self._target_generation = generation
        self._submitted_target = target
        return UvmRetargetResult(
            previous_requested_bytes=previous_gpu_bytes,
            previous_submitted_bytes=previous_target,
            requested_bytes=gpu_bytes,
            clamped_bytes=desired,
            submitted_bytes=target,
            status=completed,
        )

    def prefetch(
        self,
        allocation_id: int,
        offset: int,
        length: int,
        destination: str,
        *,
        timeout: float = 120.0,
    ) -> UvmStatus:
        if destination not in {"cpu", "gpu"}:
            raise ValueError("destination must be 'cpu' or 'gpu'")
        self.wait_idle(timeout=timeout)
        self._target_signature = None
        self._target_generation = None
        self._submitted_target = None
        reply = self.command(
            f"prefetch {allocation_id} {offset} {length} {destination}"
        )
        if not reply.get("ok"):
            raise ControlError(f"UVM prefetch failed: {reply}")
        return self._wait_job(int(reply["job_id"]), timeout=timeout)


def wait_for_instrumented_process(
    pid: int,
    *,
    require_uvm: bool = True,
    timeout: float = 120.0,
    poll_seconds: float = 0.1,
) -> tuple[ProcessController, UvmController | None]:
    deadline = time.monotonic() + timeout
    last_error: Exception | None = None
    while time.monotonic() < deadline:
        process_controller: ProcessController | None = None
        try:
            process_controller = ProcessController(pid)
            uvm = UvmController(pid) if require_uvm else None
            if uvm is not None:
                uvm.status()
            return process_controller, uvm
        except (ControlError, OSError) as exc:
            last_error = exc
            if process_controller is not None:
                process_controller.close()
            time.sleep(poll_seconds)
    raise ControlError(
        f"PID {pid} did not expose the select.cpp control ABI within {timeout}s: {last_error}"
    )


def close_all(controllers: Iterable[ProcessController]) -> None:
    errors = []
    for controller in controllers:
        try:
            controller.close()
        except Exception as exc:  # Best-effort cleanup for orchestration shutdown.
            errors.append(exc)
    if errors:
        raise ControlError(f"failed to close {len(errors)} controllers: {errors}")
