# cpu/simple/ — Simple CPU Models Context

> **Purpose:** Simple CPUs are the most basic CPU models in gem5. They execute one instruction at a time with no pipeline modeling.

## Models

| Class | File | Description |
|-------|------|-------------|
| `BaseSimpleCPU` | `base.hh/cc` | Common base for all simple CPUs. Handles instruction fetch, decode, execute, and PC advance. |
| `AtomicSimpleCPU` | `atomic.hh/cc` | Uses atomic memory accesses (instantaneous). Fastest simulation. No cache timing. Called via `tick()` event. |
| `TimingSimpleCPU` | `timing.hh/cc` | Uses timing memory accesses. Models cache latency and contention. Stalls waiting for memory responses. |
| `NonCachingSimpleCPU` | `noncaching.hh/cc` | Atomic mode that bypasses caches entirely. Sends directly to memory. |

## Class Hierarchy

```
BaseCPU (src/cpu/base.hh)
  └── BaseSimpleCPU (simple/base.hh)
      ├── AtomicSimpleCPU (simple/atomic.hh)
      │   └── NonCachingSimpleCPU (simple/noncaching.hh)
      └── TimingSimpleCPU (simple/timing.hh)
```

## Execution Flow

### AtomicSimpleCPU
```
tick() event fires:
  1. preExecute()     — fetch + decode instruction
  2. Execute inst     — may call readMem/writeMem (atomic, returns immediately)
  3. postExecute()    — update stats
  4. advancePC()      — advance program counter
  5. Schedule next tick() for next cycle
```

### TimingSimpleCPU
```
1. fetch() → sendTimingReq to icache
2. icache response → completeIfetch() → decode + execute
3. If memory inst → sendTimingReq to dcache → wait
4. dcache response → completeDataAccess() → finish instruction
5. advanceInst() → schedule next fetch
```

## Key Differences

| Feature | AtomicSimple | TimingSimple | NonCaching |
|---------|-------------|-------------|-----------|
| Memory mode | Atomic | Timing | Atomic |
| Uses caches | Yes (atomic) | Yes (timing) | No |
| Speed | Fast | Slow (accurate) | Fastest |
| Use case | Fast-forward, warmup | Detailed simulation | KVM co-sim |

## Adding Functionality

To modify simple CPU behavior, the key method is the execution loop:
- `AtomicSimpleCPU::tick()` — the main execution method
- `TimingSimpleCPU::fetch()` / `completeIfetch()` / `completeDataAccess()`
- `BaseSimpleCPU::preExecute()` / `postExecute()` — shared pre/post logic
