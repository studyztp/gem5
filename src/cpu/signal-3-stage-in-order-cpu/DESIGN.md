# Signal-Driven 3-Stage In-Order CPU — Design

**Target**: STM32G474 Cortex-M4 (no ART, no caches, Flash WS=4)
**Predecessor**: `cpu/simple-3-cycle-in-order-cpu/` (fixed-pipeline; 940 cy
sum-|delta| residual error)
**Goal**: zero residual error on the BP suite by replacing fixed-pipeline
state machines with RTL-style signals + latches. This is the cycle-accurate
calibration baseline that future Lego CPU primitives will be derived from.

---

## 1. Why a rewrite, not a patch

The fixed-pipeline `Simple3CycleCPU` introduces ~410 cy of irreducible
slop because some signals only become available "next cycle" even when
the silicon resolves them combinationally:

| Path                                  | Cortex-M4 silicon | `Simple3CycleCPU` |
|---------------------------------------|-------------------|-------------------|
| Memory response -> Decode consumable  | same cycle        | next cycle (+1)   |
| D -> F branch target                  | same cycle        | next cycle (+1)   |
| E -> D flag forwarding                | same cycle        | partial (manual)  |
| F issue right after D resolves branch | same cycle        | depends on order  |

Patching individual cases in the fixed pipeline produced order-sensitive
fragility (Phase D.5 v3 -> v4 nested-loop regression). The right fix is
to model the abstraction silicon actually uses: a **clock edge** that
samples latches and a **combinational settle** between edges where any
signal can propagate to any consumer in zero time.

**Non-goal**: this CPU is *not* a generalizable framework. It models one
3-stage in-order microarchitecture as accurately as possible. The Lego
framework will be derived from the patterns this model exposes — not the
other way around.

---

## 2. The signal/latch primitive

Two ingredients:

```
Signal<T>        — a value-bearing wire. Has a current value, a writer,
                   and zero or more subscribers. Writing notifies
                   subscribers immediately (combinational).

Latch<T>         — a Signal<T> wrapper that captures its input on the
                   rising edge of the next clock and presents the
                   captured value on its output until the following edge.
```

A **stage** is a callable that reads inputs (some Signals, some Latch
outputs), does work, and writes outputs (some Signals, some Latch
inputs). The pipeline has no intrinsic "F runs before D runs before E"
ordering — stages re-fire whenever any of their input signals change
within the same cycle, until the system settles.

### Cycle phases

Each tick has three phases, in order:

1. **Edge** — every Latch samples its input and updates its output.
   Memory async responses that arrived during the previous cycle and
   were placed in their target Latch input now become visible.
2. **Settle** — combinational signals propagate. A stage may re-fire
   multiple times within Settle if a downstream signal feeds back into
   one of its inputs (e.g. D resolves a branch -> F's target signal
   changes -> F may issue a new fetch this same cycle). Settle
   iterates until no signal value changed in a pass (a fixed-point
   iteration; bounded by graph depth).
3. **Schedule** — outgoing memory requests issued during Settle are
   actually `sendTimingReq`'d. Latch inputs sampled by the next Edge
   are the values they hold at the end of Schedule.

This three-phase split is the **only** structural assumption. Every
latency knob is data-driven (Section 6).

### Same-cycle async response

If `recvTimingResp` fires at tick T and the cycle boundary is also at
T (gem5 schedules events at exact tick values), the response is folded
into the **current** cycle's Settle, not the next one. We detect this
by comparing the response's arrival tick against `cycleStartTick`. This
matches the behavior `Simple3CycleCPU` already has via
`isCurrentCycle()` — we keep it because it's correct, not because it's
a delay.

---

## 3. Stage decomposition

Three stages — F, D, E — each a class with explicit Signal/Latch
members. No "sub-step" ordering inside `evaluate()`; the Settle phase
handles it.

### F — Prefetch & Fetch Issue

**Signals (combinational outputs):**
- `f_word_out : Signal<FetchWord>` — head of the FIFO; valid iff FIFO non-empty.
- `f_can_accept : Signal<bool>` — true iff D popped this cycle (or FIFO had room).

**Latches:**
- `fifo_state : Latch<deque<FetchWord>>` — registered FIFO contents.
  (In practice the FIFO is a member, and we register it in the
  conventional way — write-through-on-Edge semantics.)
- `fetch_pc : Latch<Addr>` — registered next-fetch PC.
- `in_flight : Latch<unsigned>` — registered in-flight count.

**Inputs (signals from other stages):**
- `redirect_target : Signal<optional<RedirectInfo>>` — driven by D
  (mispredicted conditional, unconditional branch resolved at D) or
  E (mispredicted at E). When present during Settle, F clears its
  forwarded fetch_pc to the target and may issue immediately.

**Memory port:**
- Receives icache responses asynchronously. On `recvTimingResp`, the
  word is written to a "staging signal" that the F stage's Settle pass
  reads. If the response arrives same-cycle (Edge tick == response
  tick), it folds into the current Settle.

**Settle behavior:**
1. Drain staging into FIFO (filtered by streamId and `wordFifoMinAddr`).
2. If `redirect_target` is asserted, react to it (filter or clear FIFO).
3. If FIFO has room AND no redirect-imposed hold AND `fetch_pc` is
   below `halt_addr`, build a packet and issue. Increment `in_flight`.
4. Update `f_word_out` to the new FIFO head.

### D — Decode

**Signals:**
- `d_decoded_out : Signal<optional<DecodedSlot>>` — combinational
  output to E. Valid iff D successfully decoded an instruction this
  Settle pass.
- `d_pop_request : Signal<bool>` — asks F to pop its head (drives F's
  internal logic during Settle).
- `d_redirect_to_f : Signal<optional<Addr>>` — combinational redirect
  target that flows into F's `redirect_target` input during the same
  Settle. Asserted when D resolves a conditional branch using
  forwarded flags from E (the canonical Cortex-M4 E->D path).

**Latches:**
- `d_slot : Latch<optional<DecodedSlot>>` — the in-flight decoded
  instruction (held between Edge and the next Edge if E is stalled).

**Inputs:**
- `f_word_out` (signal) — head of FIFO from F.
- `e_flags_forwarded : Signal<CCFlags>` — combinational E->D forwarding
  of NZCV. **No latency** — that's the entire point of this rewrite.

**Settle behavior:**
1. If `d_slot` (latched) is non-empty and E accepted it last cycle,
   clear it — we're free to decode a new instruction.
2. If `d_slot` is empty and `f_word_out` is valid, run the gem5
   generic decoder. On a 16-bit Thumb instruction with 2 bytes of
   tail, hold the word for the next cycle's pair.
3. If decoded instruction is a conditional branch AND
   `e_flags_forwarded` is current (i.e. E is producing the NZCV this
   same cycle for the dependent instruction), evaluate the condition
   and assert `d_redirect_to_f` to the resolved target.
4. If decoded instruction is an unconditional branch, assert
   `d_redirect_to_f` directly.
5. Drive `d_pop_request` so F sees the FIFO consumption.

### E — Execute

**Signals:**
- `e_flags_out : Signal<CCFlags>` — NZCV produced by this cycle's
  execute. Drives `d_e_flags_forwarded` combinationally.
- `e_redirect_to_f : Signal<optional<Addr>>` — taken-branch redirect
  not caught at D (e.g. computed branch target that needed E to
  resolve). Combinational into F.
- `e_accept_d : Signal<bool>` — true iff E will accept `d_slot` this
  cycle (false on LSQ stall).

**Latches:**
- `e_slot : Latch<optional<DecodedSlot>>` — the instruction being
  executed.
- `lsq_state : Latch<{Idle, Pending, Complete}>` — single-slot LSQ.

**Inputs:**
- `d_decoded_out` (signal) — D's combinational decoded instruction.

**Settle behavior:**
1. If `lsq_state` is `Pending`, check the data port. If complete this
   cycle (same-cycle async), advance to `Complete`. Otherwise stall
   (assert `e_accept_d = false`, stay on current `e_slot`).
2. If `e_slot` is empty (or completed last cycle), accept
   `d_decoded_out` (sample for the next Edge).
3. Run the StaticInst's `execute()` with the minimal ExecContext
   wrapper (reused from Simple3CycleCPU).
4. Drive `e_flags_out` from the result.
5. If the instruction is a taken branch that D could not pre-resolve
   (e.g. BLX, BX, computed target), assert `e_redirect_to_f`.

---

## 4. Connections (combinational graph)

```
                 Memory port
                      |
                      v (async, may fold same-cycle)
  +---+   word    +---+ decoded   +---+
  | F |---------> | D |---------> | E |
  +---+           +---+           +---+
    ^               ^               |
    |               |               |
    | redirect      | flags forward |
    +---------------+---------------+
    |                               |
    | redirect (E branch)           |
    +-------------------------------+
```

The graph is acyclic per Settle pass (no signal feeds itself in a
combinational loop). Loops exist between cycles via Latches, never
within Settle.

---

## 5. Cortex-M4 references that drive the design

Everything below is directly observable in the silicon TRM or our
silicon measurements; nothing is invented.

| Source                              | Behavior modeled                              |
|-------------------------------------|-----------------------------------------------|
| Cortex-M4 TRM Sect 3.3.1            | F/D/E pipeline, no BP, E->D flag forward      |
| Cortex-M4 TRM Table 3-1             | Taken branch = 1+P cycles, P=1..3             |
| STM32G474 RM Sect 3 (Flash)         | 4 wait states at 170 MHz, no ART (we disable) |
| Our silicon: bench-branch=1013 cy   | Sequential ALU baseline; constrains F latency |
| Our silicon: bp_tight=2013 cy       | Tight loop; constrains taken-branch bundle    |
| Python verify_model: TAKEN_BRANCH=10 | Empirical bundle for diff-line redirect       |
| Python verify_model: SAME_LINE=2    | Cheap bubble for in-line forward branch       |

---

## 6. Parameters (data-driven)

**Rule**: a parameter exists only if a specific silicon datapoint
demands it. We do **not** add knobs preemptively. Each entry below
cites the constraining benchmark.

### Implemented from day one (Cortex-M4 TRM is explicit)

| Name                  | Default | Range    | Justification                            |
|-----------------------|---------|----------|------------------------------------------|
| `pfu_fifo_words`      | 3       | 1..16    | Cortex-M4 TRM: small instruction queue   |
| `flash_wait_states`   | 4       | 0..15    | STM32G474 @ 170 MHz                      |
| `halt_addr`           | 0       | any      | Test infrastructure (smoke-test stop)    |

### Added only after a benchmark requires (placeholders, NOT defaults)

These knobs are documented here so the architecture supports adding
them — they are NOT instantiated until we have a silicon datapoint
that the model currently misses by an amount consistent with the knob.

| Knob (potential)              | Triggering observation needed                     |
|-------------------------------|---------------------------------------------------|
| `taken_branch_extra_hclk`     | Diff-line taken branches systematically off by N  |
| `same_line_extra_hclk`        | In-line forward branches off by N                 |
| `redirect_fetch_holdoff`      | Post-redirect first fetch off by N (AHB drain)    |
| `lsq_load_to_use_hclk`        | Load-use sequences off by N                       |

**Process**: when an unmodeled benchmark misses by M cycles in a
direction consistent with a candidate knob, we add the knob with
default = M / (number of triggering events in that benchmark), then
verify the other benchmarks still match. Only commit a knob if it
helps overall sum-|delta|.

---

## 7. Files

```
signal-3-stage-in-order-cpu/
  DESIGN.md                  this file
  SignalCPU.py               SimObject params (only the Section 6 knobs)
  signal_cpu.{hh,cc}         BaseCPU subclass; ports; threads; LSQ owner
  pipeline.{hh,cc}           Ticked subclass; runs Edge/Settle/Schedule
  signal.hh                  Signal<T> / Latch<T> templates
  fetch.{hh,cc}              F stage (PFU + word FIFO)
  decode.{hh,cc}             D stage (gem5 generic decoder driver)
  execute.{hh,cc}            E stage + LSQ
  exec_context.hh            Minimal ExecContext (reused pattern)
  types.hh                   FetchWord, DecodedSlot, RedirectInfo, CCFlags
  stats.{hh,cc}              Cycle, instruction, redirect, mem stats
  SConscript                 Source list + DebugFlags
```

---

## 8. Implementation order (each phase ends with a passing benchmark)

Each phase commits only when the named test passes AND the previously
passing tests still pass (no regression).

1. **S0 — Signal/Latch primitive + smoke test.** Two-stage toy
   (counter -> latch -> printer) verifies Edge/Settle/Schedule order.
   Pass criterion: counter increments deterministically over 100 cycles.

2. **S1 — F-only loopback.** F issues fetches into a stub icache that
   replies with synthetic words; no D, no E. Pass criterion: stat
   `requestsIssued == fifo_capacity * cycles_to_fill_then_drain` over
   a known scenario.

3. **S2 — F + D, sequential ALU.** Add D with the gem5 decoder. No
   branches. Pass criterion: `bench-branch` (which is just sequential
   ALU after the BP-disable prologue, ~1013 cy on silicon) matches
   exactly. **This is the floor**; if S2 doesn't match, nothing later
   will.

4. **S3 — Add E + LSQ.** Wire the StaticInst execute path. Pass
   criterion: a synthetic load-store kernel matches a hand-computed
   cycle count.

5. **S4 — Branches at E (no D resolution yet).** Unconditional
   branches resolve at E and redirect F. Pass criterion: `bp_tight`
   matches silicon (2013 cy).

6. **S5 — E->D flag forwarding (the key combinational path).**
   Conditional branches resolve at D using `e_flags_forwarded`. Pass
   criterion: `bp_alt`, `bp_loops_nested` come within 50 cy.

7. **S6 — Same-line vs diff-line redirect handling.** If, and only
   if, `bp_table_dispatch` and `align` still miss, introduce the
   candidate knobs from Section 6 driven by the actual gap.

At every phase, the question is: **what does silicon do that I'm
not modeling?** Not: **what knob can I add?**

---

## 9. What this design explicitly does NOT include

- No `mispredict_flush_cycles` knob. The fixed pipeline needed it
  because flushes were modeled as N empty cycles; the signal model
  drains naturally because consumers re-evaluate when their inputs
  change.
- No `taken_branch_redirect_delay` knob until S6 silicon data
  demands one. The fixed pipeline needed it because F, D, E ran in a
  fixed order that couldn't represent "redirect arrived mid-cycle and
  F re-issued same cycle".
- No `Request::UNCACHEABLE` bypass plumbing until we observe a
  benchmark that requires it. The 64-bit Flash sense-amp latch in
  `PipelinedSimpleMemory` may already produce the right behavior in
  the signal model — we'll measure first.
- No prefetch/ART/cache. Silicon data is collected with these
  disabled; modeling them would diverge from ground truth.
- No multi-issue, no out-of-order, no SMT. One stage, one slot, one
  thread.

---

## 10. Verification rig

Reuse the test harness from `cpu/simple-3-cycle-in-order-cpu/`:

- `test/test_signal_*.py` — phase tests, gated by params on the
  SimObject (analogous to `phase_b/c/d_*` in Simple3CycleCPU).
- `test/simple3_bp_kernels.py` — same 7 BP benchmarks. Reused as-is.
- Comparison script: a small wrapper that runs both
  `Simple3CycleCPU` and `SignalCPU` over the same kernels, prints
  side-by-side cycle counts and per-benchmark deltas vs silicon.

The pass bar at the end of S7 is: **sum |delta vs silicon| <= 100 cy
across the 7 BP benchmarks**, with no single benchmark off by more
than 30 cy. If we can't hit that, the rewrite was wrong and we revisit
the Edge/Settle model.

---

## 11. Risks and open questions

1. **Settle convergence.** A combinational loop (F redirect feeds D
   which feeds F again same cycle) could theoretically not converge.
   Mitigation: cap Settle iterations at graph-depth (= 3 for this CPU),
   assert if reached. Cortex-M4 itself has bounded combinational
   depth, so a real loop indicates a model bug.
2. **Async response timing.** gem5's event scheduling means a
   response could land at tick T and our Edge could also be at T —
   the order in which gem5 fires them is not guaranteed. We rely on
   the `cycleStartTick` comparison the existing CPU uses; if that
   proves fragile, the fallback is to defer the response to the next
   cycle (matches what Simple3CycleCPU does today).
3. **gem5 generic decoder state.** The decoder holds the
   16-bit-Thumb tail across `moreBytes()` calls. The Latch model has
   to capture this between cycles correctly — Simple3CycleCPU's
   `_haveLoadedWord` / `_lastFetchPc` pattern is the proven solution
   and we'll port it.

---

End of design. Implementation begins at S0 (signal.hh + smoke test).
