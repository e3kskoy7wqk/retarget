"""Bounded GPU-only prefetch using real slot reuse and attention feedback.

The committed set is a bounded record of prefetch intent, not page residency.
Runtime CPU eviction is deliberately absent: sampled batch metadata cannot
prove that every outstanding inference operation has stopped using a page.
"""
import dataclasses
import json
import math
import os
from pathlib import Path
import time

from experiments.cache_window import CacheWindowController
from experiments.control import ControlError


@dataclasses.dataclass(frozen=True)
class SlotPlacementSpec:
    block_tokens: int = 256
    lookahead_tokens: int = 4096
    min_update_seconds: float = .25
    max_blocks_per_update: int = 4
    active_seconds: float = 2.0
    refresh_seconds: float = 10.0
    max_feedback_lag_seconds: float = 1.0

    def __post_init__(self):
        for key in ['block_tokens','lookahead_tokens','max_blocks_per_update']:
            if type(getattr(self,key)) is not int or getattr(self,key)<=0:
                raise ValueError(key)
        for key in ['min_update_seconds','active_seconds','refresh_seconds','max_feedback_lag_seconds']:
            if not math.isfinite(getattr(self,key)) or getattr(self,key)<=0:
                raise ValueError(key)


def target_blocks(snapshot, spec, budget, now):
    """Real recent reads/writes first; actual free-list head second; LRU last."""
    reads=snapshot['recent_reads'];writes=snapshot['recent_writes']
    active_reads=[i for i,t in reads if now-t <= spec.active_seconds]
    active_writes=[i for i,t in writes if now-t <= spec.active_seconds]
    history=[i for i,t in sorted(reads+writes,key=lambda p:-p[1])]
    ranked=list(dict.fromkeys(active_reads+active_writes+snapshot['free_head_blocks']+history))
    return ranked[:budget]


def encode_mask(blocks):
    mask=0
    for block in blocks:
        mask |= 1 << block
    return format(mask,'x')


class SlotPlacementController(CacheWindowController):
    def __init__(self, cache_spec, slot_spec, pid):
        super().__init__(cache_spec)
        self.slot_spec=slot_spec
        self.pid=pid
        self.feedback_path=Path(os.environ['RETARGET_SLOT_FEEDBACK_DIR'])/f'{pid}.json'
        self.budget=cache_spec.width_tokens//slot_spec.block_tokens
        if not self.budget or cache_spec.width_tokens % slot_spec.block_tokens:
            raise ValueError('KV budget must be divisible by block_tokens')
        if slot_spec.block_tokens*cache_spec.bytes_per_token % 4096:
            raise ValueError('slot blocks must be page aligned')
        self.committed={}
        self.epoch=None
        self.last_attempt=-math.inf

    def snapshot(self, now):
        try:
            data=json.loads(self.feedback_path.read_text())
        except FileNotFoundError:
            return None
        if data['pid'] != self.pid or data['block_tokens'] != self.slot_spec.block_tokens:
            raise ControlError('slot feedback identity or geometry mismatch')
        lag=data['published_monotonic_s']-data['observed_monotonic_s']
        if lag > self.slot_spec.max_feedback_lag_seconds:
            return None
        # If no new allocator/attention event exists, an old idle snapshot is
        # still useful only for the unchanged free-list head, not recent reads.
        return data

    def choose(self, snapshot, now):
        desired=target_blocks(snapshot,self.slot_spec,self.budget,now)
        versions=snapshot['block_versions']
        todo=[]
        for block in desired:
            old=self.committed.get(block)
            if (old is None or old[0] != versions[block]
                    or (now-old[1] >= self.slot_spec.refresh_seconds
                        and any(i==block and now-t<=self.slot_spec.active_seconds
                                for i,t in snapshot['recent_reads']+snapshot['recent_writes']))):
                todo.append(block)
        return desired,todo[:self.slot_spec.max_blocks_per_update]

    def due(self, metrics, now):
        if now-self.last_attempt < self.slot_spec.min_update_seconds:
            return False
        snapshot=self.snapshot(now)
        if snapshot is None:
            return False
        if self.epoch != snapshot['epoch']:
            return True
        return bool(self.choose(snapshot,now)[1])

    def update(self, uvm, metrics, now, *, initial=False):
        if initial:
            result=super().update(uvm,metrics,now,initial=True)
            snapshot=self.snapshot(time.monotonic())
            if snapshot is None:
                raise ControlError('slot feedback unavailable after model warmup')
            self.epoch=snapshot['epoch']
            unit=self.slot_spec.block_tokens*self.spec.bytes_per_token
            self.committed={i:(snapshot['block_versions'][i],time.monotonic())
                            for i in range(self.current[0]//unit,self.current[1]//unit)}
            result.update(placement_kind='slot_feedback',committed_blocks=sorted(self.committed),
                          block_tokens=self.slot_spec.block_tokens,feedback_epoch=self.epoch)
            return result
        self.last_attempt=now
        snapshot=self.snapshot(now)
        if snapshot is None:
            return None
        if self.epoch != snapshot['epoch']:
            self.committed.clear();self.epoch=snapshot['epoch']
        desired,selected=self.choose(snapshot,now)
        if not selected:
            return None
        generation,allocations=uvm._allocation_snapshot()
        cache=[a for a in allocations if a['bytes']==self.spec.allocation_bytes]
        signature=tuple((a['id'],a['bytes'],a['device']) for a in cache)
        if len(cache)!=self.spec.allocation_count:
            raise ControlError('slot-placement cache geometry changed')
        if signature != self.signature:
            self.committed.clear();self.signature=signature
            desired,selected=self.choose(snapshot,now)
        block_bytes=self.slot_spec.block_tokens*self.spec.bytes_per_token
        command=f'blocksv {generation} {self.spec.allocation_bytes} {self.spec.allocation_count} {block_bytes} {encode_mask(selected)}'
        uvm.wait_idle(timeout=600)
        reply=uvm.command(command)
        if not reply.get('ok'):
            self.committed.clear()
            raise ControlError(f'slot prefetch rejected: {reply}')
        try:
            status=uvm._wait_job(int(reply['job_id']),timeout=600)
        except Exception:
            self.committed.clear()
            raise
        commit=time.monotonic()
        retained={b:v for b,v in self.committed.items() if b in desired}
        for block in selected:
            retained[block]=(snapshot['block_versions'][block],commit)
        self.committed=retained
        self.last_update=commit
        covered=sum(min(block_bytes,self.spec.allocation_bytes-b*block_bytes) for b in selected)*len(cache)
        return dict(initial=False,placement_kind='slot_feedback',update_mode='slot_feedback',
                    allocation_generation=generation,feedback_epoch=self.epoch,
                    feedback_sequence=snapshot['sequence'],feedback_observed_monotonic_s=snapshot['observed_monotonic_s'],
                    audit_plan_started_monotonic_s=now,audit_plan_committed_monotonic_s=commit,
                    block_tokens=self.slot_spec.block_tokens,desired_blocks=desired,
                    gpu_blocks=selected,committed_blocks=sorted(self.committed),
                    requested_gpu_bytes=covered,requested_cpu_bytes=0,
                    status=dataclasses.asdict(status))
