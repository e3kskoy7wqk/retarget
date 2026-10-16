#!/usr/bin/env python3
"""Generate and verify the paper workload from external source datasets."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments import prepare_trace


def verify(path, spec):
    rows = [json.loads(line) for line in path.read_text().splitlines()]
    if len(rows) != spec["requests"]:
        raise ValueError(f"expected {spec['requests']} requests, found {len(rows)}")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != spec["manifest_sha256"]:
        raise ValueError("generated manifest differs from the paper; check input datasets and tokenizer versions")
    return rows


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--kind", choices=("main",), default="main")
    p.add_argument("--trace", type=Path, required=True)
    p.add_argument("--squad", type=Path, required=True)
    p.add_argument("--tokenizer", type=Path, required=True, help="Qwen3-14B tokenizer for both models")
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args(argv)
    specs = json.loads((ROOT / "configs/workloads.json").read_text())
    if args.output.exists():
        p.error(f"refusing to overwrite {args.output}")
    spec = specs[args.kind]
    prepare_trace.main([
        "--trace", str(args.trace), "--squad", str(args.squad),
        "--tokenizer", str(args.tokenizer), "--output", str(args.output),
        "--start-time", str(spec["source_window_start_seconds"]),
        "--duration", str(spec["source_window_duration_seconds"]),
        "--thin-rate", str(spec["thin_rate"]), "--thin-method", spec["thin_method"],
        "--seed", str(spec["seed"]), "--time-compression", str(spec["time_compression"]),
        "--max-input-tokens", str(spec["max_input_tokens"]),
        "--max-output-tokens", str(spec["max_output_tokens"]),
    ])
    verify(args.output, specs[args.kind])
    print(f"Verified {args.kind}: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
