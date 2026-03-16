# Simulation Infrastructure - Navigation

## What This Directory Contains
Core simulation framework: the foundational classes for systems, processes, workloads,
event-driven simulation, clocking, draining (checkpoints), and power modeling.

## Key Files

### System and Workload
| File | Purpose |
|------|---------|
| `system.hh` / `system.cc` | `System` (SimObject): owns threads, physical memory, device memory maps, workload. Base class for `ArmSystem`, etc. |
| `System.py` | Python SimObject with memory ranges, workload, multi-processor config. |
| `workload.hh` / `workload.cc` | `Workload` base class. Subclassed by `SEWorkload` (syscall emulation) and `KernelWorkload` (full system). |
| `se_workload.hh` / `se_workload.cc` | `SEWorkload`: syscall-emulation workload base. |
| `kernel_workload.hh` / `kernel_workload.cc` | `KernelWorkload`: full-system kernel workload. Loads kernel, initrd, DTB. |
| `process.hh` / `process.cc` | `Process`: SE-mode process abstraction. Memory image, file descriptors, mmap. |

### Simulation Engine
| File | Purpose |
|------|---------|
| `eventq.hh` / `eventq.cc` | Event queue: priority-based event scheduling, the core simulation loop driver. |
| `simulate.cc` | Main simulation loop (`simulate()` function). |
| `cur_tick.hh` / `cur_tick.cc` | Current simulation tick tracking. |
| `core.hh` / `core.cc` | Simulation core setup. |
| `init.hh` / `init.cc` | Simulation initialization. |
| `root.hh` / `root.cc` | Root SimObject. |
| `sim_object.hh` / `sim_object.cc` | `SimObject` base class for all simulation objects. |

### Clocking and Power
| File | Purpose |
|------|---------|
| `clock_domain.hh` / `clock_domain.cc` | Clock domain management. |
| `clocked_object.hh` / `clocked_object.cc` | Base for clocked SimObjects. |
| `dvfs_handler.hh` / `dvfs_handler.cc` | DVFS (Dynamic Voltage/Frequency Scaling). |
| `power/` | Power models and thermal modeling. |

### Other
| File | Purpose |
|------|---------|
| `drain.hh` / `drain.cc` | Drain/resume for checkpoint support. |
| `faults.hh` / `faults.cc` | `FaultBase` — the abstract base class that all ISA fault classes inherit from. |
| `full_system.hh` | `FullSystem` global flag (FS vs SE mode). |
| `guest_abi/` | Guest ABI definitions for syscall argument passing. |
| `fd_array.hh/cc`, `fd_entry.hh` | File descriptor management for SE mode. |
| `syscall_*` | Syscall emulation infrastructure. |
| `probe/` | Probe point infrastructure for instrumentation. |

## Relevance to M-Profile
- `System` is the base for `ArmSystem` — M-profile may need an `ArmMProfileSystem` or
  a flag on `ArmSystem` to distinguish profiles.
- `Workload` and `KernelWorkload` handle loading binaries — M-profile binaries (raw bin
  or ELF with vector table at 0x0) may need a new workload class.
- `FaultBase` in `faults.hh` is the root of all fault/exception classes.
- `FullSystem` flag determines FS vs SE mode.

## Where to Look Next

| Task | Go To |
|------|-------|
| Modify ARM system (reset, boot) | `../arch/arm/system.hh` (inherits from `system.hh` here) |
| Add M-profile workload | `workload.hh` (base class), `../arch/arm/fs_workload.cc` (ARM FS workload) |
| Understand event-driven simulation | `eventq.hh` |
| Understand FaultBase (root of exception hierarchy) | `faults.hh` |
