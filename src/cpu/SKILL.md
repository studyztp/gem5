# CPU Models - Navigation

## What This Directory Contains
All CPU model implementations in gem5. CPU models are ISA-independent at the base level,
with ISA-specific behavior injected via mixin classes and the ISA interface.

## Key Files

| File | Purpose |
|------|---------|
| `base.hh` / `base.cc` | `BaseCPU`: abstract CPU base class. Thread management, interrupt handling, `createThreads()`. |
| `BaseCPU.py` | Python SimObject for CPUs. `isa` list, `decoder` list, `createThreads()` method. |
| `simple_thread.hh` / `simple_thread.cc` | `SimpleThread`: thread context with flat register file, used by Simple and Minor CPUs. |
| `thread_context.hh` / `thread_context.cc` | `ThreadContext` interface: register read/write, PC state, misc regs. |
| `exec_context.hh` | Execution context interface for instruction execution. |
| `reg_class.hh` / `reg_class.cc` | Register class definitions (int, float, vec, pred, cc, misc). |
| `static_inst.hh` / `static_inst.cc` | Base `StaticInst` class for all instructions. |
| `pc_event.hh` / `pc_event.cc` | PC-based events (breakpoints, function call tracking). |

## CPU Model Subdirectories

| Path | Purpose |
|------|---------|
| `simple/` | Simple CPU models: `AtomicSimpleCPU` (single-cycle), `TimingSimpleCPU` (timing-accurate memory). Fastest to simulate. |
| `minor/` | Minor CPU: in-order pipelined CPU with configurable pipeline stages. Good balance of accuracy and speed. |
| `o3/` | Out-of-order CPU: detailed superscalar model with rename, ROB, LSQ. Most accurate, slowest. |
| `kvm/` | KVM CPU: uses host hardware virtualization for near-native speed. ARM KVM support in `../arch/arm/kvm/`. |
| `checker/` | Checker CPU for verifying other CPU models. |
| `testers/` | Memory system testers (traffic generators). |
| `pred/` | Branch predictor models. |
| `probes/` | CPU probe points for instrumentation. |
| `trace/` | Trace-driven CPU. |

## ARM CPU Integration

ARM CPUs use a mixin pattern defined in `../arch/arm/ArmCPU.py`:
```python
class ArmCPU:
    ArchDecoder = ArmDecoder
    ArchMMU = ArmMMU
    ArchInterrupts = ArmInterrupts
    ArchISA = ArmISA
```

Concrete ARM CPU classes: `ArmAtomicSimpleCPU`, `ArmTimingSimpleCPU`, `ArmMinorCPU`, `ArmO3CPU`.

## Relevance to M-Profile
- Existing CPU models (especially `AtomicSimpleCPU` and `TimingSimpleCPU`) can be used
  for M-profile simulation without modification. The CPU model is ISA-independent.
- M-profile differences are handled in the ISA layer (`ArmISA`), not in the CPU model.
- Thread/Handler mode switching is managed by the ISA's fault/exception system.

## Where to Look Next

| Task | Go To |
|------|-------|
| Use a simple CPU for M-profile testing | `simple/` (AtomicSimpleCPU is simplest) |
| Understand thread context / register access | `thread_context.hh`, `simple_thread.hh` |
| Modify branch prediction | `pred/` |
| CPU performance counters | `probes/` |
