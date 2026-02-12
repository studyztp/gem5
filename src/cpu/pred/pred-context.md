# cpu/pred/ — Branch Prediction Context

> **Purpose:** This directory contains all branch predictor implementations used by gem5 CPU models. All predictors inherit from `BPredUnit`.

## Architecture

```
BPredUnit (bpred_unit.hh) — base class, manages BTB + RAS + conditional predictor
  │
  ├── Conditional Predictors (predict taken/not-taken):
  │   ├── LocalBP (2bit_local) — 2-bit saturating counters, local history
  │   ├── TournamentBP — local + global tournament with chooser
  │   ├── BiModeBP — bi-mode to reduce aliasing
  │   ├── GShareBP — global history XOR'd with PC
  │   ├── TAGE_base → TAGE — TAgged GEometric history length
  │   ├── LTAGE — Loop predictor + TAGE
  │   ├── TAGE_SC_L — TAGE + Statistical Corrector + Loop
  │   ├── MultiperspectivePerceptron — neural branch predictor
  │   └── MultiperspectivePerceptronTAGE — hybrid neural+TAGE
  │
  ├── Branch Target Buffers (target prediction):
  │   ├── SimpleBTB — simple direct-mapped BTB
  │   └── BTB — default branch target buffer
  │
  ├── Indirect Predictors:
  │   ├── SimpleIndirectPredictor — simple indirect target cache
  │   └── IndirectPredictor — base class
  │
  └── Return Address Stack:
      └── RAS — call/return stack predictor
```

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `bpred_unit.hh/cc` | `BPredUnit` | Base branch predictor unit. Manages BTB, RAS, conditional predictor. Called by CPU fetch stage. |
| `conditional.hh/cc` | `Conditional` | Base for conditional direction predictors. Pure virtual `lookup()`, `update()`. |
| `btb.hh/cc` | `BTB` | Branch Target Buffer base interface. |
| `simple_btb.hh/cc` | `SimpleBTB` | Simple direct-mapped BTB implementation. |
| `ras.hh/cc` | `RAS` | Return Address Stack for call/return prediction. |
| `indirect.hh/cc` | `IndirectPredictor` | Base for indirect branch target prediction. |
| `2bit_local.hh/cc` | `LocalBP` | 2-bit local predictor with configurable table size. |
| `tournament.hh/cc` | `TournamentBP` | Tournament between local and global predictors. |
| `bi_mode.hh/cc` | `BiModeBP` | Bi-mode predictor (Lee et al.). |
| `gshare.hh/cc` | `GShareBP` | GShare predictor (McFarling). |
| `tage_base.hh/cc` | `TAGEBase` | Core TAGE algorithm (Seznec). |
| `tage.hh/cc` | `TAGE` | TAGE conditional predictor. |
| `ltage.hh/cc` | `LTAGE` | Loop + TAGE hybrid. |
| `loop_predictor.hh/cc` | `LoopPredictor` | Loop iteration count predictor. |
| `tage_sc_l.hh/cc` | `TAGE_SC_L` | TAGE + Statistical Corrector + Loop (championship winner). |
| `statistical_corrector.hh/cc` | `StatisticalCorrector` | Statistical corrector filter for TAGE. |
| `it_tage.hh/cc` | `ITTAGE` | Indirect Target TAGE predictor. |
| `multiperspective_perceptron.hh/cc` | `MultiperspectivePerceptron` | Neural perceptron predictor. |
| `BranchPredictor.py` | | Python SimObject definitions for all predictors. |

## BPredUnit Interface

Key virtual methods to implement:
- `lookup(tid, inst_pc, bp_history)` — predict direction for a branch
- `update(tid, inst_pc, taken, bp_history, squashed, inst, target_pc)` — update predictor state
- `squash(tid, bp_history)` — handle misprediction squash
- `uncondBranch(tid, pc, bp_history)` — note unconditional branch

## Adding a New Predictor

1. Create `my_predictor.hh/cc` — inherit from `Conditional` (for direction) or `BTB` (for target)
2. Implement required virtual methods
3. Add Python class to `BranchPredictor.py`
4. Register in `SConscript`
5. Use via Python config: `cpu.branchPred = MyPredictor()`
