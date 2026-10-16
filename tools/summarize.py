#!/usr/bin/env python3
"""Summarize newly generated runs using the paper's fixed SLO thresholds."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments.analyze_results import read_jsonl, summarize_run
from experiments.iteration_metrics import training_iterations

SLO = {"ttft_sla_s": 1.0, "tbt_sla_s": 0.05, "stream_interval_sla_s": 0.05}


def memory_cost(run, requests):
    path = run / "scheduler.jsonl"
    if not path.is_file() or not requests:
        return {}
    records = read_jsonl(path)
    config = next(r for r in records if r.get("event") == "config")
    origin = config["monotonic_origin_seconds"]
    start = requests[0]["replay_start_monotonic_s"]
    end = start + max(r["completed_time_s"] for r in requests)
    samples = [r for r in records if r.get("event") == "sample"]
    updates = []
    for row in samples:
        action = row.get("cache_window")
        if not isinstance(action, dict) or action.get("initial", False):
            continue
        began = action.get("audit_plan_started_monotonic_s", origin + row["elapsed_seconds"])
        committed = action.get("audit_plan_committed_monotonic_s", began)
        if began < start <= committed or began <= end < committed:
            raise ValueError("placement update crosses the measurement boundary")
        if start <= began <= committed <= end:
            updates.append(action)
    initial = [r["control"] for r in samples if r.get("control", {}).get("online_migrated")]
    coverage = sum(r["requested_cpu_bytes"] + r["requested_gpu_bytes"] for r in updates)
    return {"runtime_window_updates": len(updates),
            "runtime_prefetch_coverage_gib": coverage / 2**30,
            "runtime_control_seconds": sum(r["control_seconds"] for r in updates),
            "initial_placement": initial}


def summarize(run):
    meta = json.loads((run / "metadata.json").read_text())
    instrumented = bool(meta.get("arguments", {}).get("kv_audit") or (run / "kv_audit").is_dir())
    row = summarize_run(run, SLO)
    row.update(training_iterations(run))
    requests = read_jsonl(run / "online_requests.jsonl")
    row.update(memory_cost(run, requests))
    row.update(run=str(run), paper_case=meta.get("arguments", {}).get("paper_case"),
               measurement_group="placement" if instrumented else "system",
               ttft_violation_percent=100 * (1 - row["ttft_slo_compliance"]),
               tbt_violation_percent=100 * row["tbt_violation_rate"],
               tbt_attainment_percent=100 * (1 - row["tbt_violation_rate"]))
    return row


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("input_root", type=Path)
    p.add_argument("--output", type=Path, help="optional new JSON summary")
    args = p.parse_args(argv)
    files = sorted(args.input_root.rglob("metadata.json"))
    if not files:
        p.error("no metadata.json files found")
    rows, excluded = [], []
    for path in files:
        meta = json.loads(path.read_text())
        if meta.get("status") != "completed" or meta.get("arguments", {}).get("replay_api") != "generate":
            excluded.append({"run": str(path.parent), "status": meta.get("status")})
            print(f"Not summarized: {path.parent} ({meta.get('status')})", file=sys.stderr)
            continue
        row = summarize(path.parent)
        rows.append(row)
        throughput = row["offline_iterations_per_s"]
        training = "n/a" if throughput is None else f"{throughput:.4f}"
        print(f"[{row['measurement_group']}] {path.parent}: TTFT={row['ttft_violation_percent']:.2f}% "
              f"TBT={row['tbt_violation_percent']:.2f}% "
              f"P99 TTFT={row['ttft_p99_s']:.6f}s training={training} iterations/s "
              f"coverage={row.get('runtime_prefetch_coverage_gib', 0):.3f} GiB "
              f"control={row.get('runtime_control_seconds', 0):.3f}s "
              f"errors={row['request_errors']}")
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps({"slo": SLO, "runs": rows, "excluded": excluded}, indent=2) + "\n")
    return 0 if rows else 1


if __name__ == "__main__":
    raise SystemExit(main())
