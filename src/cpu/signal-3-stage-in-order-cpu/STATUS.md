# Signal-3-Stage-In-Order-CPU — Implementation Log & Current State

Last updated: 2026-04-25

## What this CPU is

A signal-driven (RTL-style) cycle-accurate model of the Cortex-M4
3-stage in-order pipeline (F / D / E), targeting STM32G474 silicon.
Replacement for the older fixed-pipeline `simple-3-cycle-in-order-cpu/`,
which reached **940 cy sum-|delta|** vs silicon over 7 BP benchmarks.

The signal model uses explicit **Signal<T>** (combinational wires) and
**Latch<T>** (registered) primitives plus a three-phase Edge / Settle /
Schedule cycle, so cross-stage forwarding (D->F redirect, E->D flag
forward, mid-cycle memory response) propagates within one Settle pass
instead of being serialized across cycles.

## What's been built

| Phase | What landed | Status |
|---|---|---|
| **S0** | Signal/Latch primitive + smoke test | ✓ PASS |
| **S1** | F stage + icache port + retry path | ✓ PASS |
| **S2** | D stage + gem5 generic decoder + Thumb-tail handling | ✓ PASS |
| **S3** | E stage + LSQ + dcache port + same-cycle response folding | ✓ PASS |
| **S4** | E->F redirect for branches resolved at E | ✓ PASS |
| **S5** | E->D flag forwarding for early branch resolution at D | ✓ PASS |
| **S6** | UNCACHEABLE bypass on redirect (Flash WS bundle) | ✓ PASS |
| **IRQ** | Cortex-M exception entry/return via NVIC + stacking_barrier | ✓ PASS |
| **bin** | Real ELF execution (no synthetic kernel injection) | ✓ PASS |

## File map

```
gem5/src/cpu/signal-3-stage-in-order-cpu/
  DESIGN.md                  architectural design (signal model + cycle phases)
  STATUS.md                  this file
  SConscript                 sources + DebugFlag definitions
  SignalCPU.py               SimObject params (only halt_addr, pfu_fifo_words)
  SignalSmokeTest.py         S0 toy SimObject for primitive validation

  signal.{hh,cc}             Signal<T>, Latch<T>, Stage, SignalGraph primitives
  signal_smoke_test.{hh,cc}  S0 counter->latch->printer toy

  signal_cpu.{hh,cc}         BaseCPU subclass; IcachePort/DcachePort
  pipeline.{hh,cc}           Ticked subclass; Edge/Settle/Schedule + IRQ + EXC_RETURN
  fetch.{hh,cc}              F stage (PFU + word FIFO)
  decode.{hh,cc}             D stage (gem5 decoder + early-resolve branches)
  execute.{hh,cc}            E stage (4-path dispatch: stall/complete/memref/normal)
  lsq.{hh,cc}                single-slot LSQ with completionSignal
  exec_context.hh            minimal ExecContext (LSQ-routed memrefs)
  types.hh                   FetchWord, DecodedSlot, PortSide
  stats.{hh,cc}              instsCommitted, requestsIssued, mispredicts, etc.

gem5/src/arch/arm/
  ArmCPU.py                  + ArmSignalCPU (A-profile, mostly unused)
  ArmMCPU.py                 + ArmMSignalCPU (M-profile, this is what we run)

microbench/                  standalone STM32G4 microbench test programs
  link.ld                    Flash 0x08000000 (512K), RAM 0x20000000 (128K)
  common.inc                 SCS/SysTick addresses, common macros
  Makefile                   builds .elf + .lst + .sym for each .s
  mb01_seq.s                 100 sequential ADDS  (PASS @ 133 cy)
  mb02_loop.s                100-iter backward branch  (PASS @ 999 cy)
  mb03_memref.s              str + ldr to SRAM   (PASS @ 15 cy)
  mb04_systick.s             SysTick IRQ + handler + EXC_RETURN  (PASS @ 1096 cy)

test/
  test_signal_S0_smoke.py    S0 primitive smoke test
  test_signal_S1_fetch.py    (DEPRECATED — used phase_c synth kernel)
  test_signal_S2_decode.py   (DEPRECATED — used phase_c synth kernel)
  test_signal_S3_lsq.py      (DEPRECATED — used phase_d synth kernel)
  test_signal_S4_bp.py       (DEPRECATED — used simple3_bp_kernels synth words)
  test_signal_microbench.py  real-binary runner (current test path)
```

## Architectural decisions (forward vs latch paths)

Documented in `DESIGN.md` and validated by silicon-matching tests:

| Path | Class | Rationale |
|---|---|---|
| D->F redirect (taken branch from D) | **FORWARD** | Cortex-M3 TRM §1.5 — speculative branch forwarding |
| E->D flag forwarding | **FORWARD** | M4 TRM, user-confirmed; required for bp_alt/bp_loops_nested |
| E->F redirect (indirect / E-resolved) | **FORWARD** (pulse) | Originally LATCH per plan, but works as forward; revisit at S6 if cycle data demands |
| dSlot D->E | **LATCH** | Stage boundary register |
| eSlot internal to E | plain state | E's settle owns it across cycles |
| LSQ state machine | plain state | Cycle-spanning; transitions in settle() |
| LSQ completion -> E | **FORWARD** (pulse signal) | Mid-cycle dcache response folding |
| Memory response -> F (mid-cycle) | **FORWARD** when in [cycleStart, cycleStart+period) | Handled by Pipeline::onAsyncResponse |

## Cycle-accuracy results (BP suite, S5+S6, no IRQ)

Sum |delta vs silicon| at the end of S6: **2300 cy** (predecessor: 940 cy).

| Bench | Silicon | Signal | Δ |
|---|---|---|---|
| branch | 1013 | 715 | -298 |
| tight | 2013 | 1415 | -598 |
| longbody | 2213 | 2012 | -201 |
| forward | 1817 | 1717 | -100 |
| alternating | 3420 | 3412 | -8 |
| nested | 2318 | 1572 | -746 |
| align | 2211 | 1862 | -349 |

All deltas are **negative** (we're faster than silicon). Missing
costs are likely:
- The taken-branch refetch latency knob (predecessor used
  `mispredict_flush_cycles=3` + `taken_branch_redirect_delay=2`
  to add ~5 cy/branch).
- Intra-cycle wrong-path decode work (we squash via redirect
  cleanly but may be saving cycles silicon doesn't).

## Microbenchmark results (real ELF execution)

All 4 PASS:

| Bench | Cycles | Insts | Notes |
|---|---|---|---|
| mb01_seq | 133 | 101 | 100 adds + halt |
| mb02_loop | 999 | 201 | 100-iter loop, 99 mispredicts |
| mb03_memref | 15 | 5 | 1 str + 1 ldr through LSQ |
| mb04_systick | 1096 | 330 | SysTick IRQ + ISR + EXC_RETURN |

## Key bugs found and fixed (recent rounds)

1. **shouldIssueFetch's halt_addr gate** — blocked F from fetching
   exception handlers (which live above halt_addr).  Removed.
2. **applyRedirect leak** — wrong-path packets in `pendingIssueQueue`
   weren't dropped, polluting in_flight count after a redirect.
   Fixed by clearing the queue and `delete`ing the packets.
3. **D's slot decision flip-flop** — D's "consume vs hold" decision
   could flip during a Settle re-fire (e.g. after E went Pending),
   overwriting a freshly-decoded successor slot.  Locked the
   decision once per cycle via `_slotDecisionMadeThisCycle`.
4. **D's `_nextInstrAddrToDeliver` not reset on early-resolve** —
   D early-resolved a taken branch but kept advancing
   nextInstrAddrToDeliver to the fall-through address.  F redirected
   to the loop target but D's R6 filter then dropped the loop-target
   words.  Fixed by setting nextInstrAddrToDeliver = resolvedNpc.
5. **`isControl()` predicate too narrow** — gem5's ARM ISA does NOT
   always mark the bx-lr / EXC_RETURN-completing inst as
   `isControl()`, so our redirect-on-mismatch check missed it.
   Switched to a pure address comparison
   `actualNextPc != fallThrough`.
6. **Decoder reset on redirect** — gem5's InstDecoder carries
   internal state (mid-Thumb-2 flag); without `decoder->reset()`
   on redirect, post-redirect bytes are mis-classified as the
   second halfword of a 32-bit Thumb-2 inst.
7. **Vector table assembler** — initial assembly used
   `.word Handler + 1`, but `.thumb_func` already adds the Thumb
   LSB.  Removed the manual `+ 1`.

## Known limitations / what's NOT implemented

### Significant gaps

- **No multi-cycle ALU instructions** — we treat every non-memref
  StaticInst as 1-cycle.  Cortex-M4 has multi-cycle MUL, MLA, UDIV
  (TRM Table 3-1).  If a benchmark uses these, our cycle count
  will be too low.
- **No FPU** — Cortex-M4F's FPU isn't modelled.  FPU instructions
  would either fault or execute at 1 cycle (depending on ISA
  decoder behaviour).
- **No ART** — by design (silicon data is collected with ART
  disabled).
- **No write buffer** — Cortex-M4 has a 1-entry write buffer; we
  treat stores as fully blocking via the LSQ Pending state.

### Minor gaps

- **Same-line vs diff-line redirect** — `staticPredictRedirect` was
  designed in the predecessor with a `sameLine` parameter to model
  the Python verify_model's `MISPREDICT_SAME_LINE_HCLK=2` cheap
  bubble.  We don't propagate this distinction yet.
- **`taken_branch_redirect_delay`** — predecessor had a knob to add
  N cycles between branch resolve and the post-redirect first
  fetch.  Not yet ported.  Likely the source of part of the
  remaining 2300 cy gap.
- **Indirect-branch (BX/BLX) cycle accuracy** — we treat them
  identically to direct branches (forward redirect).  Cortex-M4
  TRM Table 3-1 says they cost 1+P; we may be undercosting.
- **Late-arrival / tail-chaining** — when a higher-priority IRQ
  arrives during exception entry, real Cortex-M4 abandons the
  current entry and switches.  Not modelled (we wait for the
  current IRQ to fully take, including the 24-cy stacking).
- **Interrupt at non-instruction-boundary** — we defer IRQ taking
  if `_lsq->idle() == false` or `_execute->busy() == true`.  This
  is conservative: real silicon takes IRQs at any cycle boundary,
  re-issuing the abandoned memref on EXC_RETURN.

### Test-infra gaps

- **Deprecated S1-S5 tests** still exist on disk but use the old
  `phase_b/c/d_*` Python params (now removed from `SignalCPU.py`).
  They will fail at instantiation.  Either delete or rewrite.
- **No automated cycle-budget regression** — `signal_compare.py`
  was sketched in the plan but not built.  We have to manually
  re-run BP benches to detect regressions.

## Things that may still be broken (suspected, not confirmed)

1. **mb04 cycle count looks high** — 1096 cy for 330 insts =
   3.3 cy/inst.  Stacking overhead is real but Cortex-M4 silicon
   would do this in ~80 cy (12 cy IRQ entry + 6 cy handler + 12 cy
   IRQ exit + ~50 cy main spinning).  Our model is ~10x slower.
   Likely the stacking_barrier holding is over-blocking F for
   longer than it should.

2. **No same-line redirect optimization** — every taken branch
   pays the full UNCACHEABLE WS bundle in S6, even if the target
   is in the current Flash line.  Silicon has a 2-cy cheap path
   for this (`MISPREDICT_SAME_LINE_HCLK`).

3. **`pendingIssueQueue` order vs streamId** — when applyRedirect
   bumps streamId mid-cycle but `pendingIssueQueue` still has
   packets queued from BEFORE the redirect, our drop-on-redirect
   handles it.  But if the redirect happens DURING `scheduleOutgoing`
   (the schedule phase between Settle and the next cycle), order
   could matter.  Untested edge case.

4. **No serialization checkpoint support** — `regStats()` calls
   `_pipeline->regStats()` which registers Ticked's `numCycles`,
   but we have no `serialize`/`unserialize` overrides.  Won't
   support gem5 checkpointing.

5. **Decoder cache may leak across IRQ entry** — we call
   `decoder->reset()` in checkAndTakeInterrupts but the decoder
   has per-thread state.  If two threads ever exist (we currently
   `fatal_if(numThreads != 1)`), this would need per-thread reset.

6. **Write to MISCREG_M_CONTROL on IRQ exit** — should clear
   CONTROL.SPSEL when returning to handler mode and restore it
   when returning to thread mode.  Handled by gem5's
   excReturnUnstack — but only invoked via the MMU translate path.
   Our `checkExcReturn` falls back path may miss CONTROL update;
   check by tracing CONTROL through an interrupted thread-mode
   PSP-using program.

## Next steps (in priority order)

1. **Bring up real STM32G4 firmware** (`bench-branch.elf` etc.)
   from `/scr/studyztp/benchmarks/stm32-board-microbenchmarks/`.
   These run end-to-end through C runtime startup, main(), and
   exit.  Goal: produce silicon-matching cycle counts on the same
   ELFs the predecessor was tuned against.
2. **Investigate mb04 cycle count** — why 1096 cy instead of
   ~80.  Likely stacking_barrier holding too aggressive.
3. **Port `mispredict_flush_cycles` / `taken_branch_redirect_delay`**
   knobs from the predecessor; may close the 2300 cy gap on the
   BP suite.
4. **Delete or rewrite** the deprecated S1-S5 tests using
   real binaries.
5. **Build `signal_compare.py`** for automated cycle regression.
