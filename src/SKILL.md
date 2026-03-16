# gem5 Source Tree - Navigation

## What This Directory Contains
The core simulation source code organized by subsystem. Each subdirectory contains both C++
implementation files and Python SimObject definitions.

## Key Subdirectories

| Path | Purpose |
|------|---------|
| `arch/` | ISA implementations (ARM, x86, RISC-V, SPARC, MIPS, POWER, null, generic). Each ISA has its own subdirectory. |
| `cpu/` | CPU models (Simple, Minor, O3, KVM, checkers, testers). See `cpu/SKILL.md`. |
| `mem/` | Memory system: caches, coherence (Ruby), crossbar, DRAM controllers, ports, packets, TLBs. |
| `dev/` | Device models organized by ISA: `dev/arm/` (GIC, timer, UART, RealView), `dev/x86/`, `dev/net/`, `dev/storage/`, `dev/virtio/`, `dev/pci/`. |
| `sim/` | Simulation infrastructure: System, Process, Workload, events, drain, clocking. See `sim/SKILL.md`. |
| `kern/` | OS-specific kernel abstractions (Linux, FreeBSD syscall interfaces). |
| `python/` | Python standard library (`gem5/`): boards, processors, cache hierarchies, resources. See `python/gem5/SKILL.md`. |
| `base/` | Foundation utilities: statistics, ELF/object loaders, VNC, bitunions, logging, filters. |
| `gpu-compute/` | GPU compute models (AMD GCN/VEGA). |
| `proto/` | Protocol buffer definitions for traces and packets. |
| `systemc/` | SystemC/TLM bridge and core implementations. |

## Build Integration
- Each subdirectory has a `SConscript` that registers source files.
- `SConscript` at this level walks all subdirectories recursively.
- `Kconfig` at this level aggregates ISA and feature selections.

## Where to Look Next

| Task | Go To |
|------|-------|
| ARM ISA, registers, faults, decoder | `arch/arm/SKILL.md` |
| ARM devices (GIC, timer, platform) | `dev/arm/SKILL.md` |
| CPU model modifications | `cpu/SKILL.md` |
| Memory system / caches | `mem/` |
| Simulation core (System, Workload) | `sim/SKILL.md` |
| Python board/processor configs | `python/gem5/SKILL.md` |
| ELF loading, object files | `base/loader/` |
