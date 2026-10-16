"""Microbatch throughput, using the existing complete-optimizer-step window."""
import hashlib
import json
from pathlib import Path
from experiments.analyze_results import training_window_metrics


def training_iterations(run_dir, requests=None):
    run_dir = Path(run_dir)
    path = run_dir / "offline_train.jsonl"
    if not path.is_file():
        return {"offline_microbatch_iterations": None, "offline_iterations_per_s": None,
                "offline_optimizer_steps_per_s": None, "gradient_accumulation": None,
                "offline_training_log_sha256": None}
    records = [json.loads(line) for line in path.read_text().splitlines()]
    config = next(r for r in records if r.get("event") == "config")
    accumulation = config["gradient_accumulation"]
    if type(accumulation) is not int or accumulation <= 0:
        raise ValueError(f"invalid gradient accumulation in {path}")
    if requests is None:
        requests = [json.loads(line) for line in
                    (run_dir / "online_requests.jsonl").read_text().splitlines()]
    if not requests:
        raise ValueError(f"no replay requests in {run_dir}")
    duration = max(float(r["completed_time_s"]) for r in requests)
    if duration <= 0:
        raise ValueError(f"empty replay window in {run_dir}")
    counters = training_window_metrics(run_dir, requests)
    steps = counters["offline_steps"]
    if steps is None:
        raise ValueError(f"missing training clock in {run_dir}")
    iterations = steps * accumulation
    expected_tokens = iterations * config["batch_size"] * config["sequence_length"]
    if counters["offline_processed_tokens"] != expected_tokens:
        raise ValueError(f"inconsistent full-step token count in {run_dir}")
    return {**counters, "offline_microbatch_iterations": iterations,
            "offline_iterations_per_s": iterations / duration,
            "offline_optimizer_steps_per_s": steps / duration,
            "offline_window_seconds": duration, "gradient_accumulation": accumulation,
            "offline_training_log_sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
