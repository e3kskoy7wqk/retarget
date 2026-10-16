# RETARGET: Coordinated GPU Scheduling and Memory Placement for LLM Colocation

RETARGET colocates online LLM inference and offline LoRA fine-tuning on the same GPU, coordinating them through transparent GPU context timeslice control and proactive UVM placement. The online model is Qwen3-14B or Llama-3-14B-Instruct-v1, while the offline model is Qwen3-14B in both cases.

## Files and Entry Points

| File | Purpose |
|---|---|
| `select.cpp` | CUDA API interception, context timeslice control, managed allocation registration, prefetching, and training submission gating |
| `experiments/control.py` | Python interfaces for shared memory, message queues, and UVM control sockets |
| `experiments/scheduler.py` | Fixed context quanta, serving load sampling, and placement coordination |
| `experiments/cache_window.py` | Bounded prefetch windows within per-layer KV allocations and incremental updates |
| `experiments/run_experiment.py` | Startup and shutdown of online serving, offline training, replay, and monitoring |
| `experiments/llama_adapter.py` | Llama cache layout validation, memory oversubscription validation, and metrics request timeout adaptation |
| `experiments/prepare_trace.py` | Request manifest generation from external BurstGPT and SQuAD data |
| `experiments/replay_tokens.py`, `replay_online.py` | Token arrival time recording, HTTP replay, and their shared utilities |
| `experiments/offline_train.py`, `monitor_gpu.py` | LoRA fine-tuning and NVML monitoring |
| `experiments/analyze_results.py`, `iteration_metrics.py` | Latency, training measurement windows, and microbatch iteration counting |
| `tools/build_preload.sh` | Building the preload library from source |
| `tools/run.py` | Running a single experiment with the paper's configuration; supports configuration inspection without execution |
| `tools/summarize.py` | Summarizing newly generated experiment records using fixed SLO thresholds |
| `configs/experiments.json` | The 19 paper cases: two-model comparison and memory, placement-policy, and timeslice ablations |
| `configs/workloads.json` | Workload-generation settings and the expected manifest checksum |
| `tools/prepare_workload.py` | Generate and verify the paper workload |
| `experiments/kv_audit/` | Common inference-only allocation/attention instrumentation for Table III |
| `experiments/slot_feedback.py`, `slot_placement.py` | Slot-feedback processing and sparse block prefetch for the Table III alternative |
| `configs/*.json` (remaining files) | Corresponding scheduling and memory policies |

## How Transparent Timeslice Control Works

### From Framework Calls to Driver Control

1. When starting online serving and offline training, the launcher sets `LD_PRELOAD` for these two GPU processes to load `a.so`, compiled from `select.cpp`. The frameworks continue to call the original CUDA APIs; the interception layer also handles symbol queries through `dlsym` and `cuGetProcAddress` / `cuGetProcAddress_v2`, covering paths that invoke CUDA through function pointers.
2. When intercepting launch interfaces such as `cuLaunchKernel`, the runtime discovers and registers the CUDA contexts actually used by the process. Timeslice control targets the driver channel group corresponding to each context, rather than simulating GPU timeslices with sleeps in Python.
3. Through reverse-engineered private CUDA export-table interfaces, the runtime obtains the NVIDIA Resource Manager client handle and channel-group handle from the context. The relevant code consists of `foo`, `SetTimeslice`, `GetTimeslice`, and the `cuGetExportTable` query during initialization.
4. The scheduler writes a new timeslice through shared memory and notifies the in-process control thread through a POSIX message queue. This thread reuses the process's existing `/dev/nvidiactl` file descriptor to submit `NVA06C_CTRL_CMD_SET_TIMESLICE` (`0xa06c0103`) through an RM control ioctl; the driver executes its existing hardware scheduling configuration path. Newly registered contexts also receive the current setting.
5. `SELECT_TIMESLICE_LOG=1` records the requested value, ioctl return value, RM status, and `GET_TIMESLICE` (`0xa06c0104`) readback value for checking whether control succeeded.

Here, “transparent” means that the source code of the NVIDIA driver, SGLang, and PyTorch does not need to be modified or recompiled. Deployment requires setting the process environment, using supported CUDA allocation/launch paths, and having SGLang expose its existing metrics interface. The private export interfaces still depend on the driver ABI; the paper uses Driver 535.261.03 on A100, and the complete system has also been validated with Driver 595.71.05.

### Fixed Timeslices During Replay

RETARGET uses 1,048,576 us for inference and 2,048 us for training throughout replay. The colocated system baselines and the default-timeslice ablation use 2,048/2,048 us. Serving load determines placement and pause transitions; it does not change the configured quanta.

RETARGET and the memory controls retain the recorded controller path with constant policy functions (`base_us=min_us=max_us=2048`). Every policy state also specifies 2048 us. The state fields formerly overridden by the constant function now explicitly match its effective value.

The scheduler applies these settings after model warmup and before replay. The preloaded worker retains the recorded implementation's 8192-us model-loading default; this is not an experiment case or a replay timeslice. Driver readback is recorded with the run.

## How Transparent UVM Works

### Allocation Conversion and Tracking

When `SELECT_UVM_ENABLE=1` and `SELECT_UVM_CONVERT_ALLOC=1`, the interception layer converts ordinary `cuMemAlloc_v2` requests into `cuMemAllocManaged(..., CU_MEM_ATTACH_GLOBAL)`. The framework still receives a CUDA device pointer, without requiring changes to the model, tensor operations, or allocation calls. CUDA UVM provides host backing storage and demand migration between the GPU and CPU, allowing managed allocations to exceed the GPU's physical memory capacity.

The runtime records the address, size, owning context/device, and allocation ID of each managed allocation, and maintains an allocation generation. Interception of `cuMemFree_v2` and context lifecycle operations updates the registry. Other interfaces, such as asynchronous allocation and CUDA virtual memory allocation, are not general substitutes for the conversion path above; the experiments retain the framework's CUDA allocation backend without enabling the `cudaMallocAsync` allocator.

### Proactive Placement and Incremental Updates

The Python controller queries allocation snapshots and submits prefetch plans through a per-process UVM socket. An in-process worker thread uses `cuMemPrefetchAsync` to prefetch specified ranges to the CPU or GPU, waits for the relevant migration stream to complete, and then reports execution results. Allocation validity and generation checks prevent invalidated plans from being applied directly to a changed allocation set.

The current model adapter identifies per-layer KV buffers by allocation size and count, and advances a bounded window using the high-water mark of `sglang:prompt_tokens_total` and `sglang:generation_tokens_total`. The window width is `W`, the lookahead is `H`, and advancement is aligned to `Q` tokens. It estimates prefetch ranges from serving progress without tracking KV slot reuse; advancement stops at the aligned capacity limit, and other accesses continue to be handled by demand UVM.

Incremental mode computes the differences between the old and new windows, processing only ranges entering and leaving the GPU window; full refresh reprocesses the complete target window and its complement in each layer, without repeatedly prefetching all model weights on every update. Initial placement also handles non-KV allocations and training allocations according to the configuration.

During memory handover, the scheduler closes the training launch gate, completes the required migrations, and then commits the new plan and resumes training. This gate blocks subsequent intercepted launches without canceling GPU operations that have already been submitted. Prefetching is a placement request, not a persistent residency quota; subsequent accesses can still trigger UVM migration.

## Runtime Environment

The hardware configuration uses a single NVIDIA A100 80GB PCIe in standard multiprocess mode. The reference host has approximately 503 GiB of memory; GPU memory oversubscription requires sufficient host memory. Each experiment uses one card, and the target GPU should be idle; when running in parallel across multiple cards, specify a different GPU, port, and output directory for each process.

| Component | Original Experiment Environment |
|---|---|
| NVIDIA Driver | 535.261.03; also validated with 595.71.05 |
| CUDA Toolkit for building | 12.2, including CUPTI development files and static libraries |
| Online/replay Python | 3.12 |
| SGLang | 0.5.10.post1 |
| Online PyTorch / Transformers | 2.9.1 / 5.3.0 |
| Offline Python | 3.13.9 |
| Offline PyTorch | 2.8.0 |
| Offline Transformers / PEFT / Accelerate | 5.11.0 / 0.19.1 / 1.14.0 |

The online/replay environment also requires `aiohttp`, `pyarrow`, `numpy`, and `nvidia-ml-py`, which provides the `pynvml` module; the training environment requires `pyarrow` and `numpy`.

GCC/G++, POSIX threads/message queues, System V shared memory, `libdl`, `librt`, and permission for the current user to access GPU devices are also required.

## External Models and Data

Prepare the following and store them outside this directory:

- The Qwen3-14B model and tokenizer (Hugging Face model ID `Qwen/Qwen3-14B`), for Qwen online inference and all offline training.
- [Llama-3-14B-Instruct-v1](https://huggingface.co/RDson/Llama-3-14B-Instruct-v1), for the other online inference configuration. This is the 60-layer model released in that repository; do not substitute another Llama model.
- `BurstGPT_1.csv` from [BurstGPT](https://github.com/HPMLL/BurstGPT), for request arrival patterns and input/output lengths. The source data is released under CC BY 4.0; retain the BurstGPT attribution when using it.
- SQuAD `plain_text/train-00000-of-00001.parquet`, for request text construction and offline training.

BurstGPT citation: Yuxin Wang et al., “BurstGPT: A Real-World Workload Dataset to Optimize LLM Serving Systems,” KDD 2025, DOI: `10.1145/3711896.3737413`.

Replace the paths in the following commands with the corresponding local installation paths. Do not set `LD_PRELOAD` for the entire shell; the launcher sets it only for GPU worker processes.

```bash
cd /path/to/RETARGET
export PYTHONNOUSERSITE=1
export PYTHONDONTWRITEBYTECODE=1

ONLINE_PY=/path/to/online-env/bin/python
OFFLINE_PY=/path/to/training-env/bin/python
QWEN_MODEL=/path/to/Qwen3-14B
LLAMA_MODEL=/path/to/Llama-3-14B-Instruct-v1
SQUAD_PARQUET=/path/to/squad/plain_text/train-00000-of-00001.parquet
BURSTGPT_CSV=/path/to/BurstGPT_1.csv
REPRO_WORK=/path/to/retarget-runs
mkdir -p "$REPRO_WORK"
```

### Building

```bash
CUDA_PATH=/usr/local/cuda-12.2 bash tools/build_preload.sh
```

The script generates a local `a.so`. The repository does not distribute CUDA/NVML headers or compiled artifacts; the build uses `cuda.h`, `nvml.h`, CUPTI, `libnvptxcompiler_static`, and `libcupti_static` from the Toolkit.

### Preparing the Shared Request Manifest

```bash
"$ONLINE_PY" -B -s tools/prepare_workload.py --kind main \
  --trace "$BURSTGPT_CSV" --squad "$SQUAD_PARQUET" \
  --tokenizer "$QWEN_MODEL" --output "$REPRO_WORK/requests.jsonl"
```

The builder reads the workload settings from `configs/workloads.json` and verifies the generated manifest against its expected checksum. Keep the generated workload and experiment outputs outside the repository.

Both online models receive the same prompt text, arrival pattern, and requested output lengths. Text is constructed using the Qwen tokenizer; Llama uses its own tokenizer to process it. Replay uses SGLang's `/generate` interface with `stream_interval=1`.

### Running the System Comparison

First, list the available configurations:

```bash
"$ONLINE_PY" -B -s tools/run.py --list
```

A single Qwen RETARGET run:

```bash
"$ONLINE_PY" -B -s tools/run.py \
  --case qwen/retarget --gpu 0 --port 30000 \
  --online-python "$ONLINE_PY" --offline-python "$OFFLINE_PY" \
  --online-model "$QWEN_MODEL" --offline-model "$QWEN_MODEL" \
  --squad "$SQUAD_PARQUET" --manifest "$REPRO_WORK/requests.jsonl" \
  --output-root "$REPRO_WORK/results"
```

Adding `--dry-run` at the end of the command prints only the resolved configuration, without starting serving, training, or GPU operations. Use `--replicate N` for a separate replicate directory. Before actual execution, the launcher verifies the workload hash, checks GPU occupancy, refuses to overwrite an existing individual run directory, validates model/KV capacity and memory allocation requirements, and saves the parameters and source hashes for the run.

For the complete system comparison, run the six configurations under each of `qwen/` and `llama/`. For Llama configurations, also change `--online-model` to `"$LLAMA_MODEL"`, while keeping `--offline-model` as `"$QWEN_MODEL"`.

| Configuration Suffix | System Definition | Online / Offline Timeslices During Replay (μs) |
|---|---|---|
| `infer_only` | Online inference only | 1048576 / no offline task |
| `reactive_um` | Colocation with demand UVM | 2048 / 2048 |
| `sp50` | KV capacity at 50% of Infer-Only, with colocated training | 2048 / 2048 |
| `sp75` | KV capacity at 75% of Infer-Only, with colocated training | 2048 / 2048 |
| `task_switch` | Pauses the inactive task according to load and prefetches its allocations to the host | 2048 / 2048 |
| `retarget` | Fixed asymmetric timeslices and incremental proactive placement | 1048576 / 2048 |

2048 us is the NVIDIA driver's default timeslice used by these colocated baselines. They use the `observe` or `task_switch` scheduling mode with the fixed quanta in `configs/experiments.json`. All paper cases disable MPS. The internal identifier `um_mps` is retained for compatibility with recorded Reactive UM metadata; it does not enable MPS.

### Running the Mechanism Ablations

Use the Qwen command above and replace `--case`:

| Paper result | Case | Meaning |
|---|---|---|
| Table I | `memory/demand-uvm` | Fixed quanta 1048576/2048 us, proactive placement disabled |
| Table I | `memory/initial-only` | Identical window initialization, runtime advancement disabled |
| Tables I and II | `memory/full-refresh` | Refresh the full window and its complement with the same fixed quanta |
| Tables I, II and IV | `qwen/retarget` | Reuse the main Qwen RETARGET run |
| Table IV | `timeslice/default-both` | Same incremental placement, quanta fixed at 2048/2048 us |
| Table III | `placement/demand-uvm` | Demand UVM with the common placement-comparison instrumentation |
| Table III | `placement/slot-feedback` | Allocation/attention feedback selects blocks for GPU-directed prefetch |
| Table III | `placement/retarget` | RETARGET incremental windows with the same instrumentation |

### Reproducing Table III

Use the same manifest, Qwen inference model, and Qwen training model as the main comparison. Run the following cases sequentially on an idle GPU:

```bash
for PLACEMENT_CASE in placement/demand-uvm placement/slot-feedback placement/retarget; do
  "$ONLINE_PY" -B -s tools/run.py \
    --case "$PLACEMENT_CASE" --gpu 0 --port 30000 \
    --online-python "$ONLINE_PY" --offline-python "$OFFLINE_PY" \
    --online-model "$QWEN_MODEL" --offline-model "$QWEN_MODEL" \
    --squad "$SQUAD_PARQUET" --manifest "$REPRO_WORK/requests.jsonl" \
    --output-root "$REPRO_WORK/results"
done

"$ONLINE_PY" -B -s tools/summarize.py "$REPRO_WORK/results/placement" \
  --output "$REPRO_WORK/placement-summary.json"
```

All three policies use identical inference-only probes and compact slot feedback, fixed 1048576/2048-us context quanta, and the same KV capacity. The launcher enables instrumentation for `placement/*` cases only. Both proactive policies use the same initialization and logical prefetch budget. The RETARGET run here is measured separately from the uninstrumented main run.

The probes wrap SGLang allocation and attention entry points at import time, including CUDA Graph replay, without changing installed framework files. They copy address indices rather than KV contents and publish per-process feedback for the controller. The common instrumentation is necessary to reproduce this policy comparison; it is disabled for the main system, memory, and timeslice experiments.

Slot feedback uses 256-slot blocks, at most four new blocks per update, a 250-ms minimum update interval, and 4096 slots of free-list lookahead. Recent reads/writes are prioritized, slot reuse invalidates prefetch history, and active blocks may be refreshed after 10 seconds. Runtime requests target the GPU; cold-page eviction remains with UVM. RETARGET instead advances token-progress windows and submits CPU/GPU differences. These are alternative placement policies, not an isolated change to address accuracy.

The summary reports TTFT/TBT violations, training iterations/s, runtime update count, requested prefetch coverage, and cumulative control time. The `measurement_group` field identifies these instrumented runs as `placement`; it identifies the other paper cases as `system`.

### Core Reproduction Parameters

| Parameter | Value |
|---|---|
| Controller sampling / state de-escalation | 0.01 s / 0.1 s |
| RETARGET inference / training quantum | 1048576 / 2048 us, fixed |
| HTTP keep-alive | 120 s |
| SGLang context length / maximum running requests | 4096 / 64 |
| Chunked prefill / mixed chunk | 256 tokens / enabled |
| Training precision / sequence length / microbatch / gradient accumulation | BF16 / 128 / 1 / 4 |
| LoRA rank / alpha | 16 / 32 |
| Minimum prefetch window update interval | 1 s; runtime updates are triggered only when running=queue=0 is sampled |

| KV Parameter | Qwen | Llama |
|---|---:|---:|
| Infer-Only token capacity | 275000 | 183332 |
| RETARGET / Reactive UM / TaskSwitch token capacity | 218112 | 145404 |
| SP-50 / SP-75 token capacity | 137500 / 206250 | 91666 / 137499 |
| Number of KV buffers | 80 | 120 |
| Bytes per buffer | 448790528 | 297795584 |
| Slots inferred from buffer size (`K` in the paper) | 219136 | 145408 |
| Bytes per token per buffer | 2048 | 2048 |
| `W` / `H` / `Q` (tokens) | 32768 / 16384 / 8192 | 21840 / 10920 / 5460 |

The `8192` Qwen window quantum is a token count, not a GPU timeslice. Capacity calculated from buffer allocation sizes differs from the usable token capacity of the server, and the window upper bound is aligned to an integer multiple of `Q`. Qwen colocation checks for managed allocations within 91–92 GiB; Llama uses allocation-size checks for the corresponding model, requiring the colocated total to exceed 80 GiB and RETARGET to achieve oversubscription without relying on padding.

### Summarizing New Results

```bash
"$ONLINE_PY" -B -s tools/summarize.py "$REPRO_WORK/results" \
  --output "$REPRO_WORK/summary.json"
```

This entry point fixes the paper's measurement definitions: the TTFT threshold is 1 s and the TBT threshold is 50 ms, and it reports the TTFT violation rate, P99 TTFT, TBT violation rate/attainment, and offline `iterations/s`. One iteration refers to one microbatch forward/backward pass; in this configuration, each complete optimizer step contains 4 iterations. Only optimizer steps that fall entirely between the replay start and the completion of the last request are counted, then converted using the gradient accumulation factor.

The same script also reports the number of window updates during replay, prefetch request coverage, and cumulative control time, with initialization operations listed separately. Coverage is the sum of CPU/GPU prefetch range lengths, not physical PCIe transfer bytes; control time uses the timing in the original scheduler logs from the pause call through migration completion to the resume call. Failed or incomplete runs are marked and excluded. Instrumented placement-policy runs retain their own `measurement_group` and case name, so they can be distinguished from the uninstrumented system comparison.

`experiments/analyze_results.py` retains the original analysis functions for use by other code; to reproduce the current paper, use `tools/summarize.py` instead of its earlier command-line entry point that automatically derives thresholds.
