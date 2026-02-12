# cpu/minor/ — Minor In-Order CPU Context

> **Purpose:** MinorCPU is a configurable in-order pipeline CPU model with 4 stages. More detailed than SimpleCPU but simpler than O3.

## Pipeline Structure

```
┌─────────┐   ┌─────────┐   ┌─────────┐   ┌──────────┐
│ Fetch1  │──→│ Fetch2  │──→│ Decode  │──→│ Execute  │
│ (ICache)│   │(line→inst)│  │  (→μops) │  │(FUs+LSQ) │
└─────────┘   └─────────┘   └─────────┘   └──────────┘
```

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `cpu.hh/cc` | `MinorCPU` | Top-level CPU, inherits BaseCPU. Creates pipeline stages. |
| `pipeline.hh/cc` | `Pipeline` | Connects all stages together via inter-stage buffers. |
| `fetch1.hh/cc` | `Fetch1` | First fetch stage — sends requests to ICache, manages fetch buffer. |
| `fetch2.hh/cc` | `Fetch2` | Second fetch stage — extracts instructions from fetched lines, branch prediction. |
| `decode.hh/cc` | `Decode` | Decodes macro-ops into micro-ops. |
| `execute.hh/cc` | `Execute` | Execution stage with functional units, scoreboard, and LSQ. Handles branching. |
| `lsq.hh/cc` | `LSQ` | Load/Store Queue with timing memory interface. |
| `scoreboard.hh/cc` | `Scoreboard` | Tracks register availability for hazard detection. |
| `dyn_inst.hh/cc` | `MinorDynInst` | Dynamic instruction for Minor pipeline. |
| `func_unit.hh/cc` | `MinorFU` | Functional unit modeling with latency/pipeline depth. |
| `buffers.hh` | Various | Inter-stage communication buffers (templated). |
| `pipe_data.hh/cc` | | Data types flowing between pipeline stages. |
| `activity.hh/cc` | | Activity tracking for pipeline stages. |

## Configuration

Key parameters in `MinorCPU.py`:
- `fetch1LineSnapWidth`, `fetch1LineWidth` — fetch line parameters
- `executeInputWidth` — instructions per cycle to execute stage
- `executeFuncUnits` — list of functional units
- Scoreboard forwarding configuration

## Compared to Other Models

- More detailed than SimpleCPU: models pipeline hazards, stalls, forwarding
- Less detailed than O3: no out-of-order execution, no register renaming, no speculation beyond branches
- Good for in-order cores like ARM Cortex-A53 class
