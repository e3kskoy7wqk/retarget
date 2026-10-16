"""CPU-only state derived from observed slot metadata, never token counters."""
from collections import deque
import json
import os
from pathlib import Path
import time

import numpy as np


class SlotFeedback:
    def __init__(self, directory, pid=None, block_tokens=256, publish_seconds=.05):
        self.directory = Path(directory)
        self.directory.mkdir(parents=True, exist_ok=True)
        self.pid = os.getpid() if pid is None else pid
        self.block_tokens = block_tokens
        self.publish_seconds = publish_seconds
        self.epoch = 0
        self.sequence = -1
        self.last_publish = -float('inf')
        self.capacity = 0
        self.free_head = deque()
        self.last_snapshot = None

    def reset(self, capacity):
        self.capacity = capacity
        self.generations = np.zeros(capacity+1, dtype=np.int32)
        count = (capacity+1+self.block_tokens-1)//self.block_tokens
        self.versions = np.zeros(count, dtype=np.int32)
        self.read_seen = np.full(count, -float('inf'))
        self.write_seen = np.full(count, -float('inf'))
        self.free_head.clear()
        self.epoch += 1

    def blocks(self, ranges):
        result = []
        seen = set()
        for begin, end in ranges:
            begin = max(1, begin)
            if begin >= end:
                continue
            assert end <= self.capacity+1
            for block in range(begin//self.block_tokens, (end-1)//self.block_tokens+1):
                if block not in seen:
                    result.append(block);seen.add(block)
        return result

    def consume(self, row):
        kind, at = row['event'], row['monotonic_s']
        self.sequence = row['sequence']
        if kind == 'allocator_clear':
            self.reset(row['allocator_size'])
        elif kind == 'alloc':
            if not self.capacity:
                self.reset(row['allocator_size'])
            allocated = row['slot_runs']
            for begin, end in allocated:
                begin = max(begin, 1)
                if end > begin:
                    self.generations[begin:end] += 1
            for block in self.blocks(allocated):
                begin=block*self.block_tokens
                self.versions[block] = self.generations[begin:begin+self.block_tokens].max()
                self.write_seen[block] = at
            if 'free_head_runs' in row:
                self.free_head = deque(slot for a,b in row['free_head_runs'] for slot in range(a,b))
            else:
                # Drop only known queue-head slots; never infer recycled addresses.
                occupied = {slot for a,b in allocated for slot in range(a,b)}
                self.free_head = deque(x for x in self.free_head if x not in occupied)
        elif kind == 'attention' and self.capacity:
            for block in self.blocks(row['read_runs']):
                self.read_seen[block] = at
            for block in self.blocks(row['write_runs']):
                self.write_seen[block] = at
        # Frees do not prove old GPU work has drained. This adapter never
        # authorizes CPU eviction, so stale observation cannot evict active KV.
        if self.capacity and at-self.last_publish >= self.publish_seconds:
            self.publish(at)

    def publish(self, at):
        def recent(values):
            return [[int(i), float(values[i])] for i in np.argsort(-values)
                    if np.isfinite(values[i]) and at-values[i] <= 30]
        free = list(dict.fromkeys(x//self.block_tokens for x in self.free_head))
        row = dict(version=1, pid=self.pid, epoch=self.epoch, sequence=self.sequence,
                   observed_monotonic_s=at, published_monotonic_s=time.monotonic(),
                   capacity_tokens=self.capacity, block_tokens=self.block_tokens,
                   block_versions=self.versions.tolist(),
                   recent_reads=recent(self.read_seen), recent_writes=recent(self.write_seen),
                   free_head_blocks=free)
        tmp = self.directory/f'.{self.pid}.tmp'
        tmp.write_text(json.dumps(row, separators=(',', ':')))
        tmp.replace(self.directory/f'{self.pid}.json')
        self.last_publish = at
        self.last_snapshot = row
