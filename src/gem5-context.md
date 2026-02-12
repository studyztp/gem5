# gem5 Source Code Context Guide

> **Purpose:** This file serves as the top-level entry point for AI agents and developers navigating the gem5 simulator source code. It describes the high-level architecture, how components connect, and where to look for specific functionality.

## What is gem5?

gem5 is a modular, cycle-level computer architecture simulator. It models CPUs, memory hierarchies, interconnects, and I/O devices. All simulation objects derive from `SimObject` and are connected via a port-based communication system. The simulation is driven by a discrete event engine.

## Top-Level Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Python Configuration                    │
│              (configs/, src/python/gem5/)                     │
└────────────────────────┬────────────────────────────────────┘
                         │ creates & connects
┌────────────────────────▼────────────────────────────────────┐
│  SimObject Hierarchy (src/sim/sim_object.hh)                 │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐  │
│  │   CPU    │  │  Memory  │  │ Devices  │  │   System   │  │
│  │ src/cpu/ │  │ src/mem/ │  │ src/dev/ │  │  src/sim/  │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └──────┬─────┘  │
│       │              │              │               │        │
│       └──────────────┴──────┬───────┴───────────────┘        │
│                             │                                │
│                    Port Connections                           │
│              (RequestPort ↔ ResponsePort)                    │
│                     src/mem/port.hh                           │
└─────────────────────────────────────────────────────────────┘
```

## Core Class Hierarchy

```
SimObject (src/sim/sim_object.hh)
  ├── inherits: EventManager, Serializable, Drainable, statistics::Group, Named
  │
  ├── System (src/sim/system.hh) — top-level system container
  │
  ├── ClockedObject (src/sim/clocked_object.hh) — adds clock domain
  │   ├── BaseCPU (src/cpu/base.hh)
  │   ├── PioDevice (src/dev/io_device.hh) — memory-mapped I/O base
  │   │   ├── BasicPioDevice
  │   │   ├── BaseGic (src/dev/arm/base_gic.hh)
  │   │   └── DmaDevice (src/dev/dma_device.hh) — adds DMA capability
  │   ├── AbstractMemory (src/mem/abstract_mem.hh)
  │   └── BaseCache (src/mem/cache/base.hh)
  │
  └── [many other SimObjects...]
```

## Key Subsystems & Where to Find Them

| Subsystem | Directory | Context File | Description |
|-----------|-----------|--------------|-------------|
| **Simulation Core** | `src/sim/` | [sim/sim-context.md](sim/sim-context.md) | Event engine, SimObject, System, serialization, clocking, process model |
| **CPU Models** | `src/cpu/` | [cpu/cpu-context.md](cpu/cpu-context.md) | Simple, Minor, O3 CPUs; branch prediction; execution tracing |
| **Memory System** | `src/mem/` | [mem/mem-context.md](mem/mem-context.md) | Caches, crossbars, memory controllers, DRAM/NVM, Ruby coherence |
| **Devices** | `src/dev/` | [dev/dev-context.md](dev/dev-context.md) | I/O devices, interrupt controllers, timers, UART, PCI, DMA |
| **ISA / Architecture** | `src/arch/` | [arch/arch-context.md](arch/arch-context.md) | ARM, x86, RISC-V, MIPS, SPARC, POWER ISA implementations |
| **Base Utilities** | `src/base/` | [base/base-context.md](base/base-context.md) | Bit manipulation, statistics, tracing, loaders, networking utils |
| **Kernel Support** | `src/kern/` | [kern/kern-context.md](kern/kern-context.md) | OS-specific helpers for Linux, FreeBSD, Solaris |
| **GPU Compute** | `src/gpu-compute/` | [gpu-compute/gpu-compute-context.md](gpu-compute/gpu-compute-context.md) | AMD GPU compute unit, wavefront management, shader |
| **Python Bindings** | `src/python/` | [python/python-context.md](python/python-context.md) | Python configuration modules, m5 library, gem5 stdlib |
| **SystemC** | `src/systemc/` | [systemc/systemc-context.md](systemc/systemc-context.md) | SystemC wrapper/integration |
| **SST Integration** | `src/sst/` | [sst/sst-context.md](sst/sst-context.md) | SST (Structural Simulation Toolkit) integration |
| **Protocol Buffers** | `src/proto/` | [proto/proto-context.md](proto/proto-context.md) | Protobuf trace definitions |

## How Components Connect: The Port System

gem5 uses a **port-based connection model**. Every SimObject can expose named ports via `getPort(name, idx)`:

1. **RequestPort** (formerly MasterPort) — initiates memory transactions
2. **ResponsePort** (formerly SlavePort) — responds to memory transactions

Three transport modes:
- **Atomic:** Instantaneous, returns latency estimate. Used for fast-forwarding.
- **Timing:** Cycle-accurate with request/response handshake. Used for detailed simulation.
- **Functional:** Instantaneous with no side effects. Used for debugging/initialization.

**Typical connection flow:**
```
CPU InstPort ──→ L1-I Cache ──→ L2 Cache ──→ Memory Bus ──→ Memory Controller
CPU DataPort ──→ L1-D Cache ──↗                    ↑
                                                    │
Device PioPort ─────────────────────────────────────┘
Device DmaPort ─────────────────────────────────────┘
```

## How to Add a New Device (e.g., NVIC)

If you're implementing a new device like an NVIC (Nested Vectored Interrupt Controller):

1. **Start in `src/dev/`** — see [dev/dev-context.md](dev/dev-context.md)
2. **Inherit from `BasicPioDevice`** (for MMIO registers) or `BaseGic` (for interrupt controllers)
3. **For ARM-specific devices** — see [dev/arm/arm-dev-context.md](dev/arm/arm-dev-context.md)
4. **For interrupt handling** — see GIC implementations: `gic_v2.hh`, `gic_v3.hh`
5. **Create a Python SimObject** (`.py` file) for configuration parameters
6. **Register in `SConscript`** for the build system

Key files for device development:
- `src/dev/io_device.hh` — `PioDevice` / `BasicPioDevice` base classes
- `src/dev/dma_device.hh` — `DmaDevice` if your device does DMA
- `src/dev/arm/base_gic.hh` — `BaseGic` for interrupt controller base
- `src/sim/sim_object.hh` — lifecycle methods: `init()`, `startup()`, `serialize()`

## Event-Driven Simulation

gem5 uses discrete-event simulation via `EventQueue` (src/sim/eventq.hh):
- `SimObject` inherits `EventManager` — can schedule/deschedule events
- Events have timestamps (Ticks) and priorities
- Key priorities: `CPU_Tick_Pri(50)`, `Default_Pri(0)`, `Sim_Exit_Pri(100)`

## Configuration System

- **Python configs** in `configs/` define system topology
- **SimObject `.py` files** co-located with C++ sources define parameters
- `src/python/gem5/` contains the gem5 standard library (stdlib)
- Parameters flow from Python → C++ via `Params` structs

## Build System

- Uses SCons (`SConstruct`, `SConscript` files throughout)
- Build options in `build_opts/` (e.g., `ARM`, `X86`, `RISCV`)
- Build a target: `scons build/ARM/gem5.opt`

## Quick Navigation by Task

| "I want to..." | Start here |
|----------------|-----------|
| Add a new CPU model | [cpu/cpu-context.md](cpu/cpu-context.md) → `src/cpu/base.hh` |
| Add a new cache policy | [mem/mem-context.md](mem/mem-context.md) → `src/mem/cache/` |
| Add an ARM peripheral | [dev/arm/arm-dev-context.md](dev/arm/arm-dev-context.md) |
| Understand interrupt flow | [dev/arm/arm-dev-context.md](dev/arm/arm-dev-context.md) → GIC section |
| Add a branch predictor | [cpu/cpu-context.md](cpu/cpu-context.md) → `src/cpu/pred/` |
| Add a memory controller | [mem/mem-context.md](mem/mem-context.md) → `src/mem/mem_ctrl.hh` |
| Understand the ISA layer | [arch/arch-context.md](arch/arch-context.md) |
| Add a prefetcher | [mem/cache/cache-context.md](mem/cache/cache-context.md) → `prefetch/` |
| Add system call emulation | [sim/sim-context.md](sim/sim-context.md) → `syscall_emul.hh` |
| Add a new protocol (Ruby) | [mem/ruby/ruby-context.md](mem/ruby/ruby-context.md) |
