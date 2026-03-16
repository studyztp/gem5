# ARM Fast Model Integration - Navigation

## What This Directory Contains
Integration with ARM's proprietary Fast Models for cycle-approximate CPU simulation.
Provides CortexA76 (A-profile) and CortexR52 (R-profile) via Iris interface.

## Key Subdirectories

| Path | Purpose |
|------|---------|
| `CortexA76/` | A-profile 64-bit CPU model: `FastModelCortexA76.py`, `cortex_a76.hh/cc`, `evs.hh/cc`, `thread_context.hh/cc`. |
| `CortexR52/` | R-profile real-time CPU: `FastModelCortexR52.py`, `cortex_r52.hh/cc`, `evs.hh/cc`, `thread_context.hh/cc`. Supports CortexR52x1 through CortexR52x4 cluster configs. `RVBARADDR` param for reset vector. |
| `GIC/` | Fast Model GIC integration. |
| `PL330_DMAC/` | PL330 DMA controller. |
| `iris/` | Iris interface base classes for Fast Model CPU communication. |
| `common/` | Shared utilities. |
| `protocol/` | Protocol definitions. |
| `reset_controller/` | Reset controller integration. |

## Key Files

| File | Purpose |
|------|---------|
| `FastModel.py` | Python base classes for Fast Model SimObjects. |
| `arm_fast_model.py` | ARM-specific Fast Model setup. |
| `fastmodel.cc` | C++ Fast Model integration glue. |

## Relevance to M-Profile
- No M-profile Fast Models are integrated. If ARM provides a Fast Model for Cortex-M
  processors, this directory's patterns could be reused.
- The R-profile CortexR52 provides a reference for how non-A-profile CPUs are integrated.

## Where to Look Next

| Task | Go To |
|------|-------|
| Understand how R-profile differs from A-profile in gem5 | `CortexR52/` |
| Add a new Fast Model CPU | Follow pattern in `CortexA76/` or `CortexR52/` |
| Iris interface details | `iris/` |
