#!/usr/bin/env python3
"""Aggregate raw colocation runs and derive post-hoc SLA thresholds.

SLA thresholds are anchored only to isolated requests from Infer-Only and use
the paper's 4x standalone-latency convention. They are not tuned against OURS.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import Counter
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence

DISPLAY_NAMES = {
    "ours": "OURS",
    "task_switch": "TaskSwitch",
    "sp50": "SP-50",
    "sp75": "SP-75",
    "um_mps": "UM+MPS (MPS off)",
    "infer_only": "Infer-Only",
}


def read_jsonl(path: Path) -> list[dict[str, Any]]:
    result = []
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, start=1):
            if not line.strip():
                continue
            try:
                item = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"invalid JSON in {path}:{line_number}") from exc
            if not isinstance(item, dict):
                raise ValueError(f"non-object JSON in {path}:{line_number}")
            result.append(item)
    return result


def quantile(values: Sequence[float], probability: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    position = probability * (len(ordered) - 1)
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return ordered[lower]
    fraction = position - lower
    return ordered[lower] * (1 - fraction) + ordered[upper] * fraction


def mean(values: Sequence[float]) -> float | None:
    return statistics.fmean(values) if values else None


def safe_rate(numerator: float, denominator: float) -> float | None:
    return numerator / denominator if denominator > 0 else None


def discover_runs(root: Path) -> list[Path]:
    return sorted(path.parent for path in root.glob("*/replicate-*/metadata.json"))


def successful_requests(run_dir: Path) -> list[dict[str, Any]]:
    return [
        item
        for item in read_jsonl(run_dir / "online_requests.jsonl")
        if item.get("status") == "ok"
    ]


def isolated_infer_only_samples(run_dirs: Sequence[Path]) -> tuple[list[float], list[float]]:
    ttfts: list[float] = []
    itls: list[float] = []
    for run_dir in run_dirs:
        metadata = json.loads((run_dir / "metadata.json").read_text())
        if metadata["baseline"]["name"] != "infer_only":
            continue
        requests = sorted(
            (
                item
                for item in read_jsonl(run_dir / "online_requests.jsonl")
                if item.get("submitted_time_s") is not None
                and item.get("completed_time_s") is not None
            ),
            key=lambda item: item["submitted_time_s"],
        )
        previous_completion = -math.inf
        for index, item in enumerate(requests):
            submitted = float(item["submitted_time_s"])
            completed = float(item["completed_time_s"])
            next_submission = (
                float(requests[index + 1]["submitted_time_s"])
                if index + 1 < len(requests)
                else math.inf
            )
            isolated = submitted >= previous_completion and next_submission >= completed
            previous_completion = max(previous_completion, completed)
            if not isolated or item.get("status") != "ok":
                continue
            if item.get("ttft_s") is not None:
                ttfts.append(float(item["ttft_s"]))
            itls.extend(float(value) for value in item.get("itls_s", []))
    if not ttfts or not itls:
        raise ValueError(
            "Infer-Only did not provide enough isolated TTFT and ITL samples for SLA calibration"
        )
    return ttfts, itls


def validate_matrix(run_dirs: Sequence[Path], *, allow_incomplete: bool) -> None:
    cells: dict[tuple[str, int], tuple[Path, dict[str, Any]]] = {}
    for run_dir in run_dirs:
        metadata = json.loads((run_dir / "metadata.json").read_text())
        baseline = str(metadata["baseline"]["name"])
        replicate = int(metadata["replicate"])
        key = (baseline, replicate)
        if key in cells:
            raise ValueError(f"duplicate experiment cell {key}: {cells[key][0]} and {run_dir}")
        cells[key] = (run_dir, metadata)

    expected = set(DISPLAY_NAMES)
    baselines = {baseline for baseline, _ in cells}
    if not allow_incomplete and baselines != expected:
        raise ValueError(
            f"incomplete baseline set: expected {sorted(expected)}, got {sorted(baselines)}"
        )
    replicate_sets = {
        baseline: {replicate for candidate, replicate in cells if candidate == baseline}
        for baseline in baselines
    }
    if not allow_incomplete and len({frozenset(value) for value in replicate_sets.values()}) != 1:
        raise ValueError(f"unbalanced replicate matrix: {replicate_sets}")

    common_argument_names = (
        "context_length",
        "max_running_requests",
        "chunked_prefill_size",
        "disable_cuda_graph",
        "request_timeout_seconds",
    )
    training_argument_names = (
        "train_sequence_length",
        "train_batch_size",
        "gradient_accumulation",
        "training_mode",
        "trainable_layers",
    )
    for replicate in sorted({replicate for _, replicate in cells}):
        cohort = [metadata for (baseline, rep), (_, metadata) in cells.items() if rep == replicate]
        if len(cohort) <= 1:
            continue
        for field in ("seed", "manifest_sha256", "preload_sha256", "harness_sha256"):
            values = {metadata.get(field) for metadata in cohort}
            if len(values) != 1:
                raise ValueError(f"replicate {replicate} has mismatched {field}: {values}")
        for name in common_argument_names:
            values = {metadata.get("arguments", {}).get(name) for metadata in cohort}
            if len(values) != 1:
                raise ValueError(
                    f"replicate {replicate} has mismatched argument {name}: {values}"
                )
        training_cohort = [metadata for metadata in cohort if metadata["baseline"]["training"]]
        for name in training_argument_names:
            values = {
                metadata.get("arguments", {}).get(name) for metadata in training_cohort
            }
            if len(values) > 1:
                raise ValueError(
                    f"replicate {replicate} has mismatched training argument {name}: {values}"
                )
        if any(not isinstance(metadata.get("mps_enabled"), bool) for metadata in cohort):
            raise ValueError(f"replicate {replicate} contains an unknown MPS state")


def derive_sla(run_dirs: Sequence[Path]) -> dict[str, Any]:
    ttfts, itls = isolated_infer_only_samples(run_dirs)
    standalone_ttft = statistics.fmean(ttfts)
    standalone_itl = statistics.fmean(itls)
    return {
        "method": (
            "4x mean latency of isolated, non-overlapping Infer-Only requests; "
            "TTFT is request-level and the streaming interval is measured between "
            "non-empty SSE emissions (SGLang stream_interval=1), not reconstructed BPE tokens; "
            "thresholds are not tuned to OURS"
        ),
        "multiplier": 4.0,
        "isolated_ttft_samples": len(ttfts),
        "isolated_stream_interval_samples": len(itls),
        "standalone_mean_ttft_s": standalone_ttft,
        "standalone_mean_stream_interval_s": standalone_itl,
        "ttft_sla_s": 4.0 * standalone_ttft,
        "stream_interval_sla_s": 4.0 * standalone_itl,
    }


def _origin(records: Sequence[Mapping[str, Any]]) -> float | None:
    for record in records:
        if record.get("event") == "config" and record.get("monotonic_origin_seconds") is not None:
            return float(record["monotonic_origin_seconds"])
    return None


def training_window_metrics(
    run_dir: Path,
    requests: Sequence[Mapping[str, Any]],
) -> dict[str, Any]:
    path = run_dir / "offline_train.jsonl"
    if not path.is_file():
        return {
            "offline_processed_tokens": None,
            "offline_supervised_tokens": None,
            "offline_examples": None,
            "offline_processed_tokens_per_s": None,
            "offline_supervised_tokens_per_s": None,
            "offline_examples_per_s": None,
            "offline_steps": None,
        }
    records = read_jsonl(path)
    origin = _origin(records)
    if origin is None or not requests:
        return {
            "offline_processed_tokens": None,
            "offline_supervised_tokens": None,
            "offline_examples": None,
            "offline_processed_tokens_per_s": None,
            "offline_supervised_tokens_per_s": None,
            "offline_examples_per_s": None,
            "offline_steps": None,
        }
    replay_origin = float(requests[0]["replay_start_monotonic_s"])
    window_start = replay_origin
    window_end = replay_origin + max(float(item["completed_time_s"]) for item in requests)
    steps = []
    for record in records:
        if record.get("event") != "optimizer_step":
            continue
        step_start = origin + float(
            record.get(
                "step_start_elapsed_seconds",
                float(record["elapsed_seconds"]) - float(record["step_seconds"]),
            )
        )
        step_end = origin + float(
            record.get("step_end_elapsed_seconds", record["elapsed_seconds"])
        )
        if step_start >= window_start and step_end <= window_end:
            steps.append(record)
    processed_tokens = sum(int(record["tokens"]) for record in steps)
    supervised_tokens = sum(int(record["train_tokens"]) for record in steps)
    examples = sum(int(record["examples"]) for record in steps)
    duration = max(0.0, window_end - window_start)
    return {
        "offline_processed_tokens": processed_tokens,
        "offline_supervised_tokens": supervised_tokens,
        "offline_examples": examples,
        "offline_processed_tokens_per_s": safe_rate(processed_tokens, duration),
        "offline_supervised_tokens_per_s": safe_rate(supervised_tokens, duration),
        "offline_examples_per_s": safe_rate(examples, duration),
        "offline_steps": len(steps),
    }


def _replay_window(requests: Sequence[Mapping[str, Any]]) -> tuple[float, float] | None:
    if not requests:
        return None
    origin = float(requests[0]["replay_start_monotonic_s"])
    return origin, origin + max(float(item["completed_time_s"]) for item in requests)


def scheduler_metrics(
    run_dir: Path, requests: Sequence[Mapping[str, Any]]
) -> dict[str, Any]:
    path = run_dir / "scheduler.jsonl"
    if not path.is_file():
        return {}
    records = read_jsonl(path)
    origin = _origin(records)
    window = _replay_window(requests)
    samples = [item for item in records if item.get("event") == "sample"]
    if origin is not None and window is not None:
        start, end = window
        samples = [
            item
            for item in samples
            if start <= origin + float(item["elapsed_seconds"]) <= end
        ]
    states = Counter(item.get("effective_state") for item in samples)
    controls = [item["control"] for item in samples if "control" in item]
    return {
        "scheduler_errors": sum(item.get("event") == "error" for item in records),
        "scheduler_control_count": len(controls),
        "scheduler_control_seconds": sum(
            float(item.get("control_seconds", 0.0)) for item in controls
        ),
        "state_idle_samples": states.get("idle", 0),
        "state_low_samples": states.get("low", 0),
        "state_high_samples": states.get("high", 0),
        "state_overload_samples": states.get("overload", 0),
        "offline_paused_transitions": sum(
            bool(item.get("offline_paused")) for item in controls
        ),
    }


def gpu_metrics(
    run_dir: Path, requests: Sequence[Mapping[str, Any]]
) -> dict[str, Any]:
    path = run_dir / "gpu.jsonl"
    if not path.is_file():
        return {}
    records = read_jsonl(path)
    origin = _origin(records)
    samples = [item for item in records if item.get("event") == "sample"]
    window = _replay_window(requests)
    if origin is not None and window is not None:
        start, end = window
        samples = [
            item
            for item in samples
            if start <= origin + float(item["elapsed_seconds"]) <= end
        ]
    if not samples:
        return {}
    memory = [float(item["memory_used_bytes"]) for item in samples]
    utilization = [float(item["gpu_utilization_percent"]) for item in samples]
    pcie_rx = [float(item.get("pcie_rx_kib_s") or 0) for item in samples]
    pcie_tx = [float(item.get("pcie_tx_kib_s") or 0) for item in samples]
    return {
        "gpu_memory_peak_gib": max(memory) / (1024 ** 3),
        "gpu_memory_mean_gib": statistics.fmean(memory) / (1024 ** 3),
        "gpu_utilization_mean_percent": statistics.fmean(utilization),
        "pcie_rx_peak_gib_s": max(pcie_rx) / (1024 ** 2),
        "pcie_tx_peak_gib_s": max(pcie_tx) / (1024 ** 2),
    }


def token_tbt_metrics(requests: Sequence[Mapping[str, Any]], threshold_s: float) -> dict[str, Any]:
    """Token-interval violations for native replay; never relabel legacy SSE gaps."""
    if any(item.get("tbt_measurement") != "client_output_token_arrival_interval"
           for item in requests):
        raise ValueError("TBT requires explicit token-arrival measurements for every request")
    intervals = []
    for item in requests:
        if item.get("status") != "ok":
            continue
        times = item["token_arrival_times_s"]
        if len(times) != item["actual_output_tokens"] or len(item["output_token_ids"]) != len(times):
            raise ValueError("TBT token count mismatch")
        gaps = [float(b) - float(a) for a, b in zip(times, times[1:])]
        if any(not math.isfinite(x) or x < 0 for x in gaps):
            raise ValueError("invalid TBT interval")
        if gaps != item["tbt_s"]:
            raise ValueError("TBT intervals disagree with token arrival timestamps")
        intervals.extend(gaps)
    violations = sum(x > threshold_s for x in intervals)
    return {
        "tbt_measurement": "client_output_token_arrival_interval",
        "tbt_sla_s": threshold_s,
        "tbt_intervals": len(intervals),
        "tbt_violations": violations,
        "tbt_violation_rate": safe_rate(violations, len(intervals)),
        "tbt_p99_s": quantile(intervals, 0.99),
        "tbt_mean_s": mean(intervals),
        "tbt_single_token_requests": sum(x.get("status") == "ok" and x["actual_output_tokens"] == 1 for x in requests),
        "tbt_coalesced_events": sum(x.get("coalesced_token_events", 0) for x in requests),
        "tbt_coalesced_internal_intervals": sum(x.get("coalesced_internal_intervals", 0) for x in requests),
    }


def summarize_run(run_dir: Path, sla: Mapping[str, Any]) -> dict[str, Any]:
    metadata = json.loads((run_dir / "metadata.json").read_text())
    baseline = metadata["baseline"]["name"]
    requests = read_jsonl(run_dir / "online_requests.jsonl")
    ok = [item for item in requests if item.get("status") == "ok"]
    errors = len(requests) - len(ok)
    ttfts = [float(item["ttft_s"]) for item in ok if item.get("ttft_s") is not None]
    e2es = [float(item["e2e_s"]) for item in ok if item.get("e2e_s") is not None]
    total_latencies = [
        float(item["total_latency_s"])
        for item in ok
        if item.get("total_latency_s") is not None
    ]
    itls = [float(value) for item in ok for value in item.get("itls_s", [])]
    ttft_sla = float(sla["ttft_sla_s"])
    stream_interval_sla = float(sla["stream_interval_sla_s"])
    ttft_compliant = sum(
        item.get("status") == "ok"
        and item.get("ttft_s") is not None
        and float(item["ttft_s"]) <= ttft_sla
        for item in requests
    )
    stream_interval_compliant = sum(value <= stream_interval_sla for value in itls)
    stream_interval_request_compliant = 0
    joint_compliant = 0
    derived_e2e_compliant = 0
    for item in requests:
        if item.get("status") != "ok" or item.get("ttft_s") is None:
            continue
        request_itls = [float(value) for value in item.get("itls_s", [])]
        request_stream_interval_pass = all(value <= stream_interval_sla for value in request_itls)
        if request_stream_interval_pass:
            stream_interval_request_compliant += 1
        if float(item["ttft_s"]) <= ttft_sla and request_stream_interval_pass:
            joint_compliant += 1
        stream_events = int(item.get("stream_event_count", 0))
        e2e_limit = ttft_sla + max(0, stream_events - 1) * stream_interval_sla
        if item.get("e2e_s") is not None and float(item["e2e_s"]) <= e2e_limit:
            derived_e2e_compliant += 1

    summary = {
        "baseline": baseline,
        "display_name": DISPLAY_NAMES.get(baseline, baseline),
        "replicate": metadata["replicate"],
        "status": metadata.get("status"),
        "requests": len(requests),
        "successful_requests": len(ok),
        "request_errors": errors,
        "retry_count": sum(int(item.get("retry_count", 0)) for item in requests),
        "ttft_mean_s": mean(ttfts),
        "ttft_p50_s": quantile(ttfts, 0.50),
        "ttft_p95_s": quantile(ttfts, 0.95),
        "ttft_p99_s": quantile(ttfts, 0.99),
        "ttft_max_s": max(ttfts) if ttfts else None,
        "e2e_mean_s": mean(e2es),
        "e2e_p50_s": quantile(e2es, 0.50),
        "e2e_p95_s": quantile(e2es, 0.95),
        "e2e_p99_s": quantile(e2es, 0.99),
        "total_latency_p99_s": quantile(total_latencies, 0.99),
        "stream_interval_mean_s": mean(itls),
        "stream_interval_p95_s": quantile(itls, 0.95),
        "stream_interval_p99_s": quantile(itls, 0.99),
        "ttft_slo_compliance": safe_rate(ttft_compliant, len(requests)),
        "stream_interval_request_slo_compliance": safe_rate(stream_interval_request_compliant, len(requests)),
        "stream_interval_sample_slo_compliance": safe_rate(stream_interval_compliant, len(itls)),
        "joint_request_slo_compliance": safe_rate(joint_compliant, len(requests)),
        "derived_e2e_slo_compliance": safe_rate(derived_e2e_compliant, len(requests)),
        "managed_peak_candidate_gib": metadata.get("managed_peak_candidate_gib"),
        "max_total_num_tokens": metadata.get("server_info", {}).get(
            "max_total_num_tokens"
        ),
        "mps_enabled": metadata.get("mps_enabled"),
        "wall_seconds": metadata.get("wall_seconds"),
    }
    summary.update(training_window_metrics(run_dir, requests))
    summary.update(scheduler_metrics(run_dir, requests))
    summary.update(gpu_metrics(run_dir, requests))
    if "tbt_sla_s" in sla:
        summary.update(token_tbt_metrics(requests, float(sla["tbt_sla_s"])))
    return summary


def aggregate_runs(rows: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    groups: dict[str, list[Mapping[str, Any]]] = {}
    for row in rows:
        groups.setdefault(str(row["baseline"]), []).append(row)
    result = []
    preferred = [
        "ours", "task_switch", "sp50", "sp75", "um_mps", "infer_only"
    ]
    for baseline in preferred:
        group = groups.get(baseline, [])
        if not group:
            continue
        aggregate: dict[str, Any] = {
            "baseline": baseline,
            "display_name": DISPLAY_NAMES[baseline],
            "replicates": len(group),
        }
        keys = sorted(set().union(*(row.keys() for row in group)))
        for key in keys:
            values = [
                float(row[key])
                for row in group
                if isinstance(row.get(key), (int, float))
                and not isinstance(row.get(key), bool)
                and math.isfinite(float(row[key]))
            ]
            if not values:
                continue
            aggregate[f"{key}_mean"] = statistics.fmean(values)
            aggregate[f"{key}_std"] = statistics.stdev(values) if len(values) > 1 else 0.0
        result.append(aggregate)
    return result


def write_csv(path: Path, rows: Sequence[Mapping[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = sorted(set().union(*(row.keys() for row in rows))) if rows else []
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def validate_managed_footprint(metadata: Mapping[str, Any]) -> None:
    if metadata["baseline"]["name"] == "infer_only":
        return
    contract = metadata.get("managed_allocation_contract", {})
    try:
        target = float(contract["target_gib"])
        minimum = float(contract["minimum_gib"])
        maximum = float(contract["maximum_gib"])
        footprint = float(metadata["managed_peak_candidate_gib"])
    except (KeyError, TypeError, ValueError) as exc:
        raise ValueError("missing or invalid managed-allocation measurement/contract") from exc
    if not all(math.isfinite(value) for value in (minimum, target, maximum, footprint)) or not (
        0 < minimum <= target <= maximum and minimum <= footprint <= maximum
    ):
        raise ValueError(
            f"{metadata['baseline']['name']} replicate {metadata.get('replicate')} "
            f"lacks a valid {minimum:g}-{maximum:g} GiB managed-allocation measurement"
        )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--allow-incomplete",
        action="store_true",
        help="analyze an incomplete matrix for debugging; never use for paper comparisons",
    )
    args = parser.parse_args(argv)

    run_dirs = discover_runs(args.input_root)
    if not run_dirs:
        raise SystemExit(f"no completed run metadata found below {args.input_root}")
    completed_runs = []
    for run_dir in run_dirs:
        metadata = json.loads((run_dir / "metadata.json").read_text())
        if metadata.get("status") == "completed" and (run_dir / "online_requests.jsonl").is_file():
            validate_managed_footprint(metadata)
            completed_runs.append(run_dir)
    validate_matrix(completed_runs, allow_incomplete=args.allow_incomplete)
    sla = derive_sla(completed_runs)
    rows = [summarize_run(run_dir, sla) for run_dir in completed_runs]
    aggregates = aggregate_runs(rows)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    (args.output_dir / "sla.json").write_text(
        json.dumps(sla, indent=2, sort_keys=True) + "\n"
    )
    (args.output_dir / "summary.json").write_text(
        json.dumps({"sla": sla, "runs": rows, "aggregates": aggregates}, indent=2, sort_keys=True)
        + "\n"
    )
    write_csv(args.output_dir / "runs.csv", rows)
    write_csv(args.output_dir / "aggregates.csv", aggregates)
    print(f"analyzed {len(rows)} runs into {args.output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
