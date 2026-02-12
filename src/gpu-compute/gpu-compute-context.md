# gpu-compute/ — GPU Compute Unit Context

> **Purpose:** AMD GPU compute unit model for GPGPU simulation. Models wavefront-based execution with SIMD pipelines, register files, and memory hierarchy.

## Architecture Overview

```
┌─────────────────────────────────────────────────┐
│                    Shader                        │
│  ┌──────────────┐  ┌──────────────┐             │
│  │ Compute Unit │  │ Compute Unit │  ...        │
│  │  ┌────────┐  │  │              │             │
│  │  │Wavefront│  │  │              │             │
│  │  │Wavefront│  │  │              │             │
│  │  │  ...   │  │  │              │             │
│  │  └────────┘  │  │              │             │
│  └──────────────┘  └──────────────┘             │
│                                                  │
│  ┌──────────────────────────────────┐           │
│  │         GPU Command Processor     │           │
│  └──────────────────────────────────┘           │
└─────────────────────────────────────────────────┘
```

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `shader.hh/cc` | `Shader` | Top-level GPU shader component. Manages compute units. |
| `compute_unit.hh/cc` | `ComputeUnit` | GPU Compute Unit — contains wavefronts, SIMD pipelines, register files, LSU. |
| `wavefront.hh/cc` | `Wavefront` | GPU wavefront (warp) — group of work-items executing in lockstep. |
| `gpu_command_processor.hh/cc` | `GPUCommandProcessor` | Dispatches kernel launches from CPU to GPU. |
| `gpu_compute_driver.hh/cc` | `GPUComputeDriver` | Kernel-space driver for GPU (handles ioctls). |
| `dispatcher.hh/cc` | `GPUDispatcher` | Dispatches work-groups to compute units. |

### Pipeline Stages

| File | Class | Description |
|------|-------|-------------|
| `fetch_stage.hh/cc` | `FetchStage` | Instruction fetch. |
| `fetch_unit.hh/cc` | `FetchUnit` | Per-wavefront fetch unit. |
| `scoreboard_check_stage.hh/cc` | `ScoreboardCheckStage` | Dependency checking. |
| `schedule_stage.hh/cc` | `ScheduleStage` | Instruction scheduling. |
| `exec_stage.hh/cc` | `ExecStage` | Execution stage. |

### Memory Pipelines

| File | Class | Description |
|------|-------|-------------|
| `global_memory_pipeline.hh/cc` | `GlobalMemPipeline` | Global memory access pipeline. |
| `local_memory_pipeline.hh/cc` | `LocalMemPipeline` | Local (shared) memory pipeline. |
| `scalar_memory_pipeline.hh/cc` | `ScalarMemPipeline` | Scalar memory pipeline. |

### Register Files

| File | Class | Description |
|------|-------|-------------|
| `register_file.hh/cc` | `RegisterFile` | Base register file. |
| `vector_register_file.hh/cc` | `VectorRegisterFile` | SIMD vector register file. |
| `scalar_register_file.hh/cc` | `ScalarRegisterFile` | Scalar register file. |
| `register_manager.hh/cc` | `RegisterManager` | Register allocation management. |

### Instructions

| File | Class | Description |
|------|-------|-------------|
| `gpu_static_inst.hh/cc` | `GPUStaticInst` | GPU static instruction base. |
| `gpu_dyn_inst.hh/cc` | `GPUDynInst` | GPU dynamic instruction instance. |
| `gpu_exec_context.hh/cc` | `GPUExecContext` | Execution context for GPU instructions. |

### Other

| File | Class | Description |
|------|-------|-------------|
| `lds_state.hh/cc` | `LdsState` | Local Data Share (shared memory). |
| `pool_manager.hh/cc` | `PoolManager` | Resource pool management. |
| `scheduler.hh/cc` | `Scheduler` | Wavefront scheduling. |
| `comm.hh/cc` | | Communication pipes between stages. |

## GPU Memory System

GPU connects to the memory system via Ruby coherence protocols (GPU_VIPER protocol) or classic memory system with specialized caches in `src/mem/cache/`.

## Related Directories

- `src/dev/amdgpu/` — AMD GPU device model (MMIO, firmware)
- `src/dev/hsa/` — HSA signal/queue devices
- `src/arch/amdgpu/` — AMD GPU ISA (GCN/CDNA instructions)
