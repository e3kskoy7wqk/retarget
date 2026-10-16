"""Llama3-14B/Qwen3-14B experiment with explicit oversubscription validation."""
import hashlib
import json
from pathlib import Path

from experiments import run_experiment as launcher
from experiments.control import UvmController

original_scheduler_command=launcher.scheduler_command


def scheduler_command(*args,**kwargs):
    return original_scheduler_command(*args,**kwargs)+['--metrics-timeout','120']


def record_oversubscription(metadata,args,managed_bytes):
    physical=args.physical_mib*2**20
    baseline=launcher.configured_baseline(args)
    config=metadata['training_config']['cuda_padding_reservation']
    padding=config['allocated_bytes']
    unpadded=managed_bytes-padding
    metadata.update(managed_peak_candidate_bytes=managed_bytes,managed_peak_candidate_gib=managed_bytes/2**30,
        managed_allocation_contract=launcher.allocation_contract(args),managed_allocation_contract_enforced=False,
        managed_allocation_in_bounds=args.target_gib-args.tolerance_gib<=managed_bytes/2**30<=args.target_gib+args.upper_tolerance_gib,
        physical_memory_bytes=physical,padding_reservation_bytes=padding,managed_excluding_padding_gib=unpadded/2**30,
        oversubscription_verified=managed_bytes>physical,nonpadding_oversubscription_verified=unpadded>physical,
        model_substitution_wrapper_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        observation_timeout_seconds=120)
    if managed_bytes<=physical:
        raise RuntimeError(f'Combined managed allocations {managed_bytes/2**30:.3f} GiB do not exceed physical capacity; refusing replay.')
    if baseline.name=='ours':
        if padding or unpadded<=physical:raise RuntimeError('RETARGET must oversubscribe without padding')
        spec=json.loads(args.policy.read_text())['cache_window']
        generation,allocations=UvmController(metadata['online_gpu_pid'])._allocation_snapshot()
        matches=[a for a in allocations if a['bytes']==spec['allocation_bytes']]
        metadata['model_cache_geometry_verification']={'generation':generation,'expected':spec,'matched_count':len(matches)}
        if len(matches)!=spec['allocation_count']:raise RuntimeError(f"Expected {spec['allocation_count']} KV buffers; found {len(matches)}")


if __name__=='__main__':
    launcher.record_managed_footprint=record_oversubscription
    launcher.scheduler_command=scheduler_command
    raise SystemExit(launcher.main())
