"""Observe attention address metadata; never read or change KV tensor contents.

Copies integer indices to pinned host memory on the producing CUDA stream.
A background thread waits for copy completion and logs lossless runs of indices.
This is diagnostic instrumentation, not part of RETARGET's production policy.
"""
import atexit
import functools
import importlib.abc
import importlib.machinery
import json
import os
from pathlib import Path
import queue
import sys
import threading
import time
import traceback

_writer = None


def encode_runs(values):
    """Lossless half-open consecutive runs, preserving order and multiplicity."""
    runs = []
    for value in values:
        value = int(value)
        if runs and runs[-1][1] == value:
            runs[-1][1] = value + 1
        else:
            runs.append([value, value + 1])
    return runs


class Writer:
    def __init__(self):
        audit_dir = os.environ.get('RETARGET_KV_AUDIT_DIR')
        self.file = None
        if audit_dir:
            root = Path(audit_dir)
            root.mkdir(parents=True, exist_ok=True)
            self.file = (root / f'kv_access.{os.getpid()}.jsonl').open('a', buffering=1)
        self.feedback = None
        if os.environ.get('RETARGET_SLOT_FEEDBACK_DIR'):
            from experiments.slot_feedback import SlotFeedback
            self.feedback = SlotFeedback(os.environ['RETARGET_SLOT_FEEDBACK_DIR'])
        self.last_attention = -float('inf')
        self.pending = queue.Queue(maxsize=128)
        self.sequence = 0
        self.thread = threading.Thread(target=self.work, daemon=True, name='kv-address-audit')
        self.thread.start()
        atexit.register(self.close)

    def submit(self, kind, tensors=None, **fields):
        import torch
        start = time.monotonic()
        # Diagnostics retain every attention observation. Deployment without
        # an audit log samples attention metadata; allocator events remain exact.
        if kind == 'attention' and self.file is None:
            if start - self.last_attention < .05:
                return
            self.last_attention = start
        data = {}
        # No GPU allocations or auxiliary GPU kernels are introduced here.
        for key, value in (tensors or {}).items():
            if value is None:
                continue
            assert value.ndim == 1 and value.is_contiguous(), (key, value.shape)
            host = torch.empty(value.shape, dtype=value.dtype, device='cpu', pin_memory=True)
            host.copy_(value, non_blocking=True)
            data[key] = (host, value)  # Retain the source until the D2H copy completes.
        event = None
        if data:
            event = torch.cuda.Event()
            event.record(torch.cuda.current_stream())
        row = dict(event=kind, monotonic_s=start, sequence=self.sequence, pid=os.getpid(), **fields)
        self.sequence += 1
        row['probe_enqueue_seconds'] = time.monotonic() - start
        self.pending.put((row, data, event))

    def work(self):
        try:
            while True:
                item = self.pending.get()
                try:
                    if item is None:
                        return
                    row, data, event = item
                    if event is not None:
                        event.synchronize()
                    for key, (host, source) in data.items():
                        row[key + '_runs'] = encode_runs(host.tolist())
                        row[key + '_count'] = host.numel()
                    row['logged_monotonic_s'] = time.monotonic()
                    if self.feedback is not None:
                        self.feedback.consume(row)
                    if self.file is not None:
                        self.file.write(json.dumps(row, separators=(',', ':')) + '\n')
                finally:
                    self.pending.task_done()
        except BaseException:
            traceback.print_exc()
            if self.file is not None:
                self.file.flush()
            os._exit(89)

    def close(self):
        self.pending.join()
        self.pending.put(None)
        self.thread.join(timeout=10)
        if self.file is not None:
            self.file.close()


def emit(kind, tensors=None, **fields):
    global _writer
    if _writer is None:
        _writer = Writer()
    _writer.submit(kind, tensors, **fields)


def geometry(backend):
    if getattr(backend, '_kv_audit_geometry', False):
        return
    pool = backend.token_to_kv_pool_allocator._kvcache
    buffers = pool.k_buffer + pool.v_buffer
    import torch
    segments = torch.cuda.memory_snapshot()
    entries = [dict(address=x.data_ptr(), shape=list(x.shape),
                    strides=list(x.stride()), element_bytes=x.element_size(),
                    slot_bytes=x.stride(0)*x.element_size(), bytes=x.numel()*x.element_size())
               for x in buffers]
    assert len(entries) == 80 and all(x['slot_bytes'] == 2048 for x in entries), entries[:1]
    for entry in entries:
        matches = [s for s in segments if s['address'] <= entry['address']
                   and entry['address'] + entry['bytes'] <= s['address'] + s['total_size']]
        assert len(matches) == 1, (entry, matches)
        segment = matches[0]
        entry['allocation_address'] = segment['address']
        entry['allocation_bytes'] = segment['total_size']
        entry['allocation_offset_bytes'] = entry['address'] - segment['address']
        assert entry['allocation_bytes'] == 448790528, entry
        assert entry['allocation_offset_bytes'] == 0, entry
    emit('geometry', allocations=entries, allocator_size=backend.token_to_kv_pool_allocator.size,
         need_sort=backend.token_to_kv_pool_allocator.need_sort,
         sliding_window_size=backend.sliding_window_size, use_mla=backend.use_mla)
    backend._kv_audit_geometry = True


def patch_backend(module):
    cls = module.TritonAttnBackend
    original = cls.init_forward_metadata

    @functools.wraps(original)
    def wrapped(self, forward_batch, *args, **kwargs):
        result = original(self, forward_batch, *args, **kwargs)
        import torch
        if torch.cuda.is_current_stream_capturing():
            return result
        geometry(self)
        mode = forward_batch.forward_mode
        assert forward_batch.spec_info is None, 'Speculative attention not supported by this audit'
        n = int(forward_batch.seq_lens_sum) if mode.is_decode_or_idle() else sum(forward_batch.extend_prefix_lens_cpu)
        emit('attention', {'read': self.forward_metadata.kv_indices[:n],
                           'write': forward_batch.out_cache_loc},
             path='eager', mode=str(mode), batch_size=forward_batch.batch_size,
             reads='all_cached_kv' if mode.is_decode_or_idle() else 'cached_prefix_only')
        return result

    cls.init_forward_metadata = wrapped


def patch_graph(module):
    cls = module.CudaGraphRunner
    original = cls.replay_prepare

    @functools.wraps(original)
    def wrapped(self, forward_batch, *args, **kwargs):
        result = original(self, forward_batch, *args, **kwargs)
        backend = self.model_runner.attn_backend
        geometry(backend)
        assert forward_batch.spec_info is None and forward_batch.forward_mode.is_decode_or_idle()
        n = int(forward_batch.seq_lens_sum)
        emit('attention', {'read': backend.cuda_graph_kv_indices[:n],
                           'write': self.buffers.out_cache_loc[:self.raw_num_token]},
             path='cuda_graph', mode=str(forward_batch.forward_mode),
             batch_size=self.raw_bs, padded_batch_size=self.bs, reads='all_cached_kv')
        return result

    cls.replay_prepare = wrapped


def patch_allocator(module):
    cls = module.TokenToKVPoolAllocator
    original_alloc, original_free, original_clear = cls.alloc, cls.free, cls.clear

    @functools.wraps(original_alloc)
    def alloc(self, need_size):
        result = original_alloc(self, need_size)
        if result is not None:
            observed = {'slot': result}
            now = time.monotonic()
            if (os.environ.get('RETARGET_SLOT_FEEDBACK_DIR') and
                    now - getattr(self, '_retarget_free_head_at', -float('inf')) >= .05):
                observed['free_head'] = self.free_pages[:4096]
                self._retarget_free_head_at = now
            emit('alloc', observed, allocator_size=self.size, need_sort=self.need_sort)
        return result

    @functools.wraps(original_free)
    def free(self, free_index):
        if free_index.numel():
            emit('free', {'slot': free_index}, grouped=not self.is_not_in_free_group)
        return original_free(self, free_index)

    @functools.wraps(original_clear)
    def clear(self):
        result = original_clear(self)
        emit('allocator_clear', allocator_size=self.size, need_sort=self.need_sort)
        return result

    cls.alloc, cls.free, cls.clear = alloc, free, clear


PATCHES = {
    'sglang.srt.layers.attention.triton_backend': patch_backend,
    'sglang.srt.mem_cache.allocator': patch_allocator,
    'sglang.srt.model_executor.cuda_graph_runner': patch_graph,
}


class Loader(importlib.abc.Loader):
    def __init__(self, original, name):
        self.original, self.name = original, name

    def create_module(self, spec):
        return self.original.create_module(spec)

    def exec_module(self, module):
        self.original.exec_module(module)
        PATCHES[self.name](module)
        print('RETARGET_KV_AUDIT_PATCHED ' + self.name, file=sys.stderr, flush=True)


class Finder(importlib.abc.MetaPathFinder):
    def find_spec(self, fullname, path=None, target=None):
        if fullname not in PATCHES:
            return None
        spec = importlib.machinery.PathFinder.find_spec(fullname, path)
        if spec is not None:
            spec.loader = Loader(spec.loader, fullname)
        return spec


def install():
    sys.meta_path.insert(0, Finder())
