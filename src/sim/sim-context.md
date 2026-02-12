# sim/ — Simulation Core Context

> **Purpose:** This directory contains the core simulation infrastructure: the event engine, SimObject base class, System, serialization/checkpointing, clock domains, power modeling, process/workload management, and syscall emulation.

## Directory Overview

| File/Group | Description |
|------------|-------------|
| `sim_object.hh/cc` | **SimObject** — base class for ALL simulation objects. Provides lifecycle (`init`, `startup`, `regStats`), port binding (`getPort`), serialization, draining, and probe infrastructure. |
| `system.hh/cc` | **System** — top-level container. Manages thread contexts, memory mode (atomic/timing), requestor IDs, symbol tables, workloads. Every major component references `System*`. |
| `eventq.hh/cc` | **Event / EventQueue** — discrete-event simulation engine. Events are scheduled by tick+priority. `SimObject` inherits `EventManager` for scheduling. |
| `clocked_object.hh/cc` | **ClockedObject** — extends SimObject with clock domain support (cycle/tick conversion, frequency, voltage). Base for CPUs, caches, devices. |
| `port.hh/cc` | **Port** (base) — abstract port for connecting SimObjects. Not to be confused with `mem/port.hh` which adds memory protocols. |
| `simulate.hh/cc` | **simulate()** — main simulation loop. Drives event processing. |
| `serialize.hh/cc` | **Serialization** — checkpoint save/restore framework. |
| `drain.hh/cc` | **Draining** — mechanism to quiesce simulation for checkpointing/switching. |
| `process.hh/cc` | **Process** — SE (syscall emulation) mode process model. Manages virtual memory, file descriptors, program loading. |
| `workload.hh/cc` | **Workload** — base workload class; `kernel_workload.hh` for FS kernel loading. |
| `clock_domain.hh/cc` | **ClockDomain** — clock frequency management; `DerivedClockDomain` for dividers. |
| `voltage_domain.hh/cc` | **VoltageDomain** — voltage levels for DVFS. |
| `dvfs_handler.hh/cc` | **DVFSHandler** — dynamic voltage/frequency scaling controller. |
| `power_state.hh/cc` | **PowerState** — power state machine (ON, CLK_GATED, OFF, etc.). |
| `power_domain.hh/cc` | **PowerDomain** — groups objects sharing power state. |
| `faults.hh/cc` | **FaultBase** — base class for architectural faults/exceptions. |
| `syscall_emul.hh/cc` | **Syscall emulation** — implements Linux/FreeBSD syscalls for SE mode. |
| `syscall_desc.hh/cc` | **SyscallDesc** — descriptor table mapping syscall numbers to handlers. |
| `mem_state.hh/cc` | **MemState** — process virtual memory state (brk, mmap, stack). |
| `fd_array.hh/cc` | **FDArray** — file descriptor table for SE mode processes. |
| `pseudo_inst.hh/cc` | **Pseudo instructions** — m5 ops for simulation control (exit, reset stats, checkpoint). |
| `stat_control.hh/cc` | **Statistics control** — stat dump scheduling and management. |
| `root.hh/cc` | **Root** — root SimObject; manages global simulation time and parameters. |
| `sub_system.hh/cc` | **SubSystem** — grouping SimObject for hierarchy. |
| `probe/` | **Probe system** — non-intrusive monitoring points for stats/tracing. |
| `power/` | **Power modeling** — thermal and power models. |
| `guest_abi/` | **Guest ABI** — framework for reading/writing guest function arguments. |

## Key Class Relationships

```
SimObject (base for everything)
  ├── EventManager    — schedule/deschedule events on EventQueue
  ├── Serializable    — checkpoint save/restore
  ├── Drainable       — quiesce for switching/checkpointing
  ├── statistics::Group — hierarchical stats
  └── Named           — object naming

ClockedObject : SimObject + Clocked
  └── Has ClockDomain*, PowerState*

System : SimObject
  ├── SystemPort (RequestPort for debug access)
  ├── Threads container (ThreadContext* objects)
  ├── Memory mode management
  ├── RequestorID management
  └── Workload*
```

## SimObject Lifecycle

1. **Construction** — Python creates C++ object via Params
2. **`init()`** — called after all objects created, ports connected
3. **`regStats()`** — register statistics
4. **`regProbePoints()`** / **`regProbeListeners()`** — set up probes
5. **`initState()`** — fresh start initialization (or `loadState()` for checkpoint resume)
6. **`startup()`** — final init; schedule initial events here
7. **Simulation runs** — events processed
8. **`drain()`** — quiesce before checkpoint/CPU switching
9. **`serialize()`** / **`unserialize()`** — checkpoint

## Event System Details

Events are the heart of gem5 timing simulation:

- **Schedule:** `schedule(event, when)` — schedule at absolute tick
- **Reschedule:** `reschedule(event, when)` — move already-scheduled event
- **Deschedule:** `deschedule(event)` — cancel event
- **Priority levels** (lower = earlier at same tick):
  - `Minimum_Pri`, `Debug_Enable_Pri(-101)`, `CPU_Switch_Pri(-31)`
  - `Default_Pri(0)`, `CPU_Tick_Pri(50)`, `Stat_Event_Pri(90)`, `Sim_Exit_Pri(100)`

## Memory Modes

System operates in one of:
- **Atomic** — instantaneous memory access, no timing. Used for fast-forward.
- **Timing** — cycle-accurate request/response. Used for detailed simulation.
- **AtomicNoncaching** — atomic without caching, for KVM fast-forward.

## For Deeper Investigation

| Topic | Where to look |
|-------|--------------|
| How simulation starts | `main.cc`, `simulate.hh`, `root.hh` |
| How objects connect | `sim_object.hh::getPort()`, `port.hh::bind()` |
| Syscall emulation | `syscall_emul.hh`, `process.hh`, `fd_array.hh` |
| Clock/power management | `clock_domain.hh`, `voltage_domain.hh`, `dvfs_handler.hh` |
| Checkpoint/restore | `serialize.hh`, `globals.hh` |
| Guest ABI for function calls | `guest_abi/` directory |
| Probe/tracing infrastructure | `probe/` directory |
