# gem5 Python Standard Library - Navigation

## What This Directory Contains
The Python standard library for gem5: high-level abstractions for boards, processors,
cache hierarchies, memory systems, and simulation configuration. This is the user-facing
API for building simulation configurations.

## Key Subdirectories

| Path | Purpose |
|------|---------|
| `components/` | Reusable simulation components. |
| `components/boards/` | Board definitions: `ArmBoard` (ARMv8 FS), `X86Board`, `RiscvBoard`, `SimpleBoard`. |
| `components/processors/` | Processor abstractions: `SimpleProcessor`, `SimpleSwitchableProcessor`. CPU types include ARM variants (Atomic, Timing, Minor, O3, KVM). |
| `components/cachehierarchies/` | Cache hierarchy configs: classic, CHI, Ruby protocols. |
| `components/memory/` | Memory system configurations (DDR3, DDR4, HBM, etc.). |
| `components/devices/` | Device configurations (GPU). |
| `simulate/` | Simulation driver: `Simulator` class, exit events. |
| `resources/` | Resource management: workloads, disk images, kernels. |
| `prebuilt/` | Pre-built configurations (demo boards). |
| `utils/` | Utility functions. |

## Key Files

| File | Purpose |
|------|---------|
| `__init__.py` | Package init. |
| `isas.py` | ISA enum definitions. |
| `coherence_protocol.py` | Coherence protocol enum. |
| `runtime.py` | Runtime utilities. |
| `gem5_default_config.py` | Default configuration values. |

## ARM Board (`components/boards/arm_board.py`)

`ArmBoard` extends `AbstractBoard` and `KernelDiskWorkload`:
- Uses `ArmSystem` with `VExpress_GEM5_V1` (or V2/Foundation) platform
- Sets up GIC, GenericTimer, DTB generation
- ARMv8-A full-system oriented

## Relevance to M-Profile
- No M-profile board exists. An `ArmMProfileBoard` would be created here or in
  `components/boards/`.
- The board would use a different platform (not VExpress), different interrupt
  controller (NVIC not GIC), and different workload type.
- Processor abstractions (`SimpleProcessor`) can be reused; they are CPU-model
  wrappers that are ISA-aware but not profile-specific.

## Where to Look Next

| Task | Go To |
|------|-------|
| Create M-profile board | `components/boards/` (follow `arm_board.py` pattern) |
| Configure ARM processor | `components/processors/` |
| Understand simulation flow | `simulate/` |
| Add M-profile workload resource | `resources/` |
