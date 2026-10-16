#!/usr/bin/env python3
"""Run one paper configuration with explicitly supplied local dependencies."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from experiments import run_experiment as launcher


def definitions():
    return json.loads((ROOT / "configs/experiments.json").read_text())


def validate_manifest(path, kind):
    expected = json.loads((ROOT / "configs/workloads.json").read_text())[kind]
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    if digest != expected["manifest_sha256"]:
        raise ValueError(f"manifest does not match {kind}; regenerate it with tools/prepare_workload.py")


def prepare(selected, case):
    """Resolve paths without starting processes or accessing a GPU."""
    parser = launcher.build_parser()
    args = parser.parse_args(["--baseline", case["runtime"]["baseline"]])
    for name, value in case["runtime"].items():
        if not hasattr(args, name):
            raise ValueError(f"unknown experiment parameter: {name}")
        setattr(args, name, value)
    args.online_python = Path(selected.online_python).expanduser().absolute()
    args.client_python = args.online_python
    args.offline_python = Path(selected.offline_python).expanduser().absolute()
    args.online_model = Path(selected.online_model).expanduser().resolve()
    args.offline_model = Path(selected.offline_model).expanduser().resolve()
    args.squad = Path(selected.squad).expanduser().resolve()
    args.manifest = Path(selected.manifest).expanduser().resolve()
    args.output_root = Path(selected.output_root).expanduser().resolve() / selected.case
    args.policy = ROOT / "configs" / case["policy"]
    args.gpu, args.port = selected.gpu, selected.port
    args.replicate = selected.replicate
    args.profile = None
    args.baseline_overrides = case["baseline_overrides"]
    args.profile_allowed_gpus = [selected.gpu]
    args.profile_calibration_required = False
    args.paper_case = selected.case
    return args


def main(argv=None):
    cases = definitions()
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--list", action="store_true", help="list configurations; no GPU access")
    p.add_argument("--case", choices=sorted(cases))
    p.add_argument("--online-python", default=sys.executable)
    p.add_argument("--offline-python", default=sys.executable)
    p.add_argument("--online-model")
    p.add_argument("--offline-model")
    p.add_argument("--squad")
    p.add_argument("--manifest")
    p.add_argument("--output-root")
    p.add_argument("--gpu", type=int, default=0)
    p.add_argument("--port", type=int, default=30000)
    p.add_argument("--replicate", type=int, default=1)
    p.add_argument("--dry-run", action="store_true", help="print resolved configuration without execution")
    selected = p.parse_args(argv)
    if selected.list:
        print("\n".join(cases))
        return 0
    for field in ("case", "online_model", "offline_model", "squad", "manifest", "output_root"):
        if getattr(selected, field) is None:
            p.error("--" + field.replace("_", "-") + " is required")
    if selected.gpu < 0 or not 1 <= selected.port <= 65535 or selected.replicate < 1:
        p.error("GPU must be nonnegative, port in 1..65535, and replicate positive")
    case = cases[selected.case]
    args = prepare(selected, case)
    if selected.dry_run:
        print(json.dumps({"case": selected.case, "arguments": vars(args),
                          "paper": case["paper"], "manifest_kind": case["manifest_kind"],
                          "environment": case["environment"],
                          "scheduler_metrics_timeout_seconds": case["scheduler_metrics_timeout_seconds"]},
                         default=str, indent=2))
        return 0
    validate_manifest(args.manifest, case["manifest_kind"])
    launcher.validate_args(args)
    os.environ.update(case["environment"])
    if case["model"] == "llama":
        from experiments.llama_adapter import record_oversubscription, scheduler_command
        launcher.record_managed_footprint = record_oversubscription
        launcher.scheduler_command = scheduler_command
    print(launcher.run_experiment(args))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
