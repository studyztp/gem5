# cpu/ — CPU Models Context

> **Purpose:** This directory contains all CPU model implementations, branch prediction, execution tracing, and related infrastructure. All CPU models inherit from `BaseCPU` (`ClockedObject`).

## Directory Overview

| Path | Description |
|------|-------------|
| `base.hh/cc` | **BaseCPU** — abstract base for all CPU models. Defines data/instruction ports, thread contexts, interrupt interface. |
| `simple/` | **SimpleCPU** models — AtomicSimpleCPU, TimingSimpleCPU, NonCachingSimpleCPU. Single-issue, in-order. |
| `minor/` | **MinorCPU** — 4-stage in-order pipeline (fetch1, fetch2, decode, execute). |
| `o3/` | **O3CPU** — out-of-order superscalar CPU with full pipeline modeling. |
| `kvm/` | **KvmCPU** — hardware-accelerated execution via Linux KVM. |
| `pred/` | **Branch predictors** — tournament, TAGE, perceptron, bi-mode, gshare, etc. |
| `checker/` | **Checker CPU** — validates results of another CPU model for correctness. |
| `trace/` | **TraceCPU** — replays pre-recorded memory traces. |
| `testers/` | **Memory testers** — traffic generators for memory system testing. |
| `probes/` | **CPU probes** — instruction tracking, PC-count tracking. |
| `simple_thread.hh/cc` | **SimpleThread** — basic thread state implementation for simple CPUs. |
| `thread_context.hh/cc` | **ThreadContext** — abstract interface to thread architectural state. |
| `thread_state.hh/cc` | **ThreadState** — base thread state (tid, process, etc.). |
| `exec_context.hh` | **ExecContext** — interface for instruction execution to access CPU state. |
| `static_inst.hh/cc` | **StaticInst** — decoded instruction representation. |
| `reg_class.hh/cc` | **RegClass / RegId** — register class definitions (Int, Float, Vec, etc.). |
| `func_unit.hh/cc` | **FuncUnit** — functional unit definitions for pipeline modeling. |
| `pc_event.hh/cc` | **PCEvent** — triggers actions at specific PC values. |
| `exetrace.hh/cc` | **ExeTracer** — instruction execution tracing. |
| `inteltrace.hh/cc` | **IntelTrace** — Intel-format execution trace. |
| `nativetrace.hh/cc` | **NativeTrace** — compare against native execution. |

## CPU Model Hierarchy

```
BaseCPU (src/cpu/base.hh) : ClockedObject
  │
  ├── BaseSimpleCPU (src/cpu/simple/base.hh)
  │   ├── AtomicSimpleCPU  — single-cycle atomic memory access
  │   ├── TimingSimpleCPU  — timing memory access with stalls
  │   └── NonCachingSimpleCPU — atomic, bypasses caches
  │
  ├── MinorCPU (src/cpu/minor/cpu.hh)
  │   └── 4-stage pipeline: Fetch1 → Fetch2 → Decode → Execute
  │
  ├── O3CPU (src/cpu/o3/cpu.hh)
  │   └── 7-stage: Fetch → Decode → Rename → IEW → Commit
  │       with ROB, IQ, LSQ, register renaming
  │
  ├── BaseKvmCPU (src/cpu/kvm/base.hh)
  │   └── Uses host KVM for near-native execution speed
  │
  ├── CheckerCPU (src/cpu/checker/cpu.hh)
  │   └── Validates another CPU's results
  │
  └── TraceCPU (src/cpu/trace/trace_cpu.hh)
      └── Replays recorded memory traces
```

## Key Interfaces

### BaseCPU Public Interface
- `getDataPort()` / `getInstPort()` — **pure virtual**, return memory ports
- `getPort(if_name, idx)` — resolves `"dcache_port"` → data port, `"icache_port"` → inst port
- `activateContext(tid)` / `suspendContext(tid)` / `haltContext(tid)` — thread control
- `postInterrupt()` / `clearInterrupt()` / `checkInterrupts()` — interrupt handling
- `switchOut()` / `takeOverFrom(old_cpu)` — CPU switching (e.g., atomic → timing)
- `totalInsts()` / `totalOps()` — instruction counts

### ThreadContext
Abstract interface to a thread's architectural state:
- Register read/write (int, float, vec, misc)
- PC management
- Status (Active, Suspended, Halted)
- TLB access
- ISA access

### ExecContext
Interface used during instruction execution:
- Register operand read/write
- Memory access initiation
- Branch/fault handling

## Connection to Memory System

```
BaseCPU
  ├── icache_port (RequestPort) ──→ L1 ICache (ResponsePort)
  └── dcache_port (RequestPort) ──→ L1 DCache (ResponsePort)
```

Each CPU model implements port creation differently:
- **SimpleCPU:** Direct `AtomicPort` or `TimingPort` inner classes
- **MinorCPU:** Ports in Fetch1 and LSQ stages
- **O3CPU:** Ports in Fetch and LSQ units

## Subdirectory Details

### simple/ — Simple CPU Models
See [simple/simple-cpu-context.md](simple/simple-cpu-context.md) (if exists)
- `AtomicSimpleCPU` — executes one instruction per `tick()`, atomic memory
- `TimingSimpleCPU` — executes with timing memory, handles retries/stalls
- `NonCachingSimpleCPU` — atomic, sends to memory directly (no cache)

### minor/ — Minor In-Order CPU
- 4-stage pipeline with configurable functional units
- `Fetch1` → `Fetch2` (decode from cache line) → `Decode` → `Execute`
- `LSQ` handles load/store queue with timing memory
- Configurable via `MinorCPU.py` parameters

### o3/ — Out-of-Order CPU
See [o3/o3-context.md](o3/o3-context.md) (if exists)
- Full out-of-order pipeline: Fetch → Decode → Rename → IEW → Commit
- **Fetch** (`fetch.hh`) — branch prediction, instruction fetch
- **Decode** (`decode.hh`) — instruction decoding
- **Rename** (`rename.hh`) — register renaming via `rename_map.hh`
- **IEW** (`iew.hh`) — issue/execute/writeback, contains:
  - `InstQueue` (`inst_queue.hh`) — instruction scheduling
  - `LSQ` (`lsq.hh`) — load/store queue
- **Commit** (`commit.hh`) — in-order retirement via `ROB` (`rob.hh`)
- **FreeList** (`free_list.hh`) — physical register management
- **Scoreboard** (`scoreboard.hh`) — dependency tracking
- **FTQ** (`ftq.hh`) — fetch target queue
- **BAC** (`bac.hh`) — branch address calculator

### pred/ — Branch Predictors
See [pred/pred-context.md](pred/pred-context.md) (if exists)
All inherit from `BPredUnit` (`bpred_unit.hh`):
- `LocalBP` — 2-bit local predictor
- `TournamentBP` — local + global tournament
- `BiModeBP` — bi-mode predictor
- `GShareBP` — global share predictor
- `TAGE` / `TAGE_SC_L` / `LTAGE` — TAgged GEometric predictors
- `MultiperspectivePerceptron` variants — neural predictors
- `ITTAGE` — indirect target predictor
- BTBs: `SimpleBTB`, `BTB` (branch target buffer)
- `RAS` — return address stack
- `IndirectPredictor` / `SimpleIndirectPredictor` — indirect branch prediction

### kvm/ — KVM CPU
- `BaseKvmCPU` — base for KVM-accelerated execution
- `vm.hh` — KVM virtual machine management
- `device.hh` — KVM device passthrough
- Architecture-specific extensions in `src/arch/*/kvm/`

### testers/ — Memory System Testers
- `memtest/` — random memory access tester
- `rubytest/` — Ruby coherence protocol tester
- `traffic_gen/` — configurable traffic generator
- `garnet_synthetic_traffic/` — Garnet network tester
- `gpu_ruby_test/` — GPU memory tester
- `directedtest/` — directed coherence tests
- `spatter_gen/` — spatter-based traffic generator

## For Adding a New CPU Model

1. Inherit from `BaseCPU`
2. Implement `getDataPort()`, `getInstPort()` returning `Port&`
3. Implement `activateContext()`, `suspendContext()`, `wakeup()`
4. Create a Python SimObject `.py` file with parameters
5. Register in `SConscript`
6. Handle instruction execution via `ExecContext` interface
7. Wire up to memory system via request ports

## For Adding a Branch Predictor

1. Inherit from `BPredUnit` (src/cpu/pred/bpred_unit.hh)
2. Implement `lookup()`, `update()`, `uncondBranch()`, `squash()`
3. Add Python class in `BranchPredictor.py`
4. Register in `pred/SConscript`
