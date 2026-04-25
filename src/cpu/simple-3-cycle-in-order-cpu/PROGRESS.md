# Simple3CycleCPU — Implementation Progress

Tracker for the gem5 port of the Cortex-M4 Python pipeline model. The
authoritative plan is [`PORT_PLAN.md`](./PORT_PLAN.md); the
phased checklist there is the source of truth.

Last updated: Phase D.5 v4 complete (silicon-targeted refinements).
**Sum of |gem5-vs-silicon| errors: 940 cycles across 7 benchmarks
(down from 1828 — 49% reduction).** 2 benchmarks match silicon
exactly (branch, tight); worst gap is align at +307 (+13.9%); all
remaining gaps are in the same ±15% range Python sees against
silicon. Two PFU bugs fixed by tracing benchmarks against silicon
+ Python in parallel: (1) `_numInFlight` not reset on redirect
(align Loop A regression); (2) cheap forward+sameLine redirects
re-fetched the target word the speculative fall-through had
already pulled into the FIFO (alternating odd-iter overhead).

(History: the earlier "no buffer" interpretation was wrong — the user clarified that
"no cache + no prefetch" means **no ART** (no I-cache, no D-cache,
no prefetch buffer), but the **64-bit current-line latch is still
present** because it's the Flash sense-amp output, not part of ART.
Re-enabled `port_read_buffer_size = 8` and added a "first fetch
after a taken-branch redirect bypasses the buffer" mechanism (sets
`Request::UNCACHEABLE`, picked up by a new check in
PipelinedSimpleMemory) to model the silicon empirical "TAKEN bundle"
that pays the full Flash WS even when the target line is in the
buffer. `bench-branch` and `bp_tight` now match silicon **exactly**
(0% delta). `bp_longbody` and `bp_nested` are within ±7%.
`bp_forward`, `bp_alternating`, `bp_align` still overshoot
(11%–36%) — those are multi-redirect-per-iter or multi-loop kernels
where the per-redirect buffer-bypass charge is double-counted; that's
the next thing to chip away at.

---

## Build / run quick reference

```bash
# Build — user's exact pattern (do NOT use -j $(nproc) or absolute target)
cd /scr/studyztp/gem5-binary/lego/build/ARM
scons -C /home/studyztp/test_ground/gem5-lego-cpu/gem5 gem5.opt -j 10

# Phase A smoke test (CPU instantiates, ticks once, stops):
/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt \
    --debug-flags=Simple3CPU \
    test/test_simple3_phaseA.py

# Phase B (16 NOPs through M-profile decoder, hand-populated FIFO):
/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt \
    --debug-flags=Simple3CPU,Simple3CPUDecode,Simple3CPUExecute \
    test/test_simple3_phaseB.py

# Phase C (16 NOPs fetched from flash through icache_port):
/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt \
    --debug-flags=Simple3CPU,Simple3CPUResp,Simple3CPUExecute,Simple3CPUPFU \
    test/test_simple3_phaseC.py

# Phase D (hand-coded Thumb-2 loop with a real mispredict at exit):
/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt \
    --debug-flags=Simple3CPU,Simple3CPUExecute,Simple3CPUResp \
    test/test_simple3_phaseD.py

# Phase D.2 (load/store kernel: writes 42 to SRAM and reads it back):
/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt \
    --debug-flags=Simple3CPU,Simple3CPUExecute,Simple3CPULSQ \
    test/test_simple3_phaseD2.py

# Phase D.3 (synthetic bench-branch — 100-iter BNE loop matching the
# Python prototype's instruction stream):
/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt \
    test/test_simple3_phaseD3_branch.py

# Phase D.5 (parameterised — pick any of the 7 BP benchmarks):
/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt \
    test/test_simple3_phaseD5_bp.py <bench-name>
# bench-name in: branch tight longbody forward alternating nested align
# Defaults to 'branch'. Cycle count is computed inside the script
# from exit_tick / 5882 (170 MHz clock period) and printed alongside
# the silicon target.
```

Compute cycle count from the simulator: `cycles = exit_tick / clockPeriod`
(at 170 MHz, clockPeriod = 5882 ticks). Don't trust `system.cpu.tickCycles`
in stats.txt for cross-comparison — it counts Pipeline events, which can
diverge from the Cortex-M4 wall-clock cycle for the cases we want to
compare against silicon / Python.

Built binary: `/scr/studyztp/gem5-binary/lego/build/ARM/gem5.opt`.

Debug flags registered (in this CPU's `SConscript`):
`Simple3CPU`, `Simple3CPULineTrace`, `Simple3CPUFetch`,
`Simple3CPUResp`, `Simple3CPUPFU`, `Simple3CPUDecode`,
`Simple3CPUExecute`, `Simple3CPUFlash`, plus compound `Simple3CPUAll`.

---

## Phase A — done ✓

CPU compiles, links, instantiates, fires one cycle event, stops.

Verification (last run on `test_simple3_phaseA.py`):

```
system.cpu.tickCycles    1   # one cycle event fired
system.cpu.idleCycles    0
system.cpu.numCycles     0   # the legacy BaseCPU stat (independent of Ticked)
```

Files under `gem5/src/cpu/simple-3-cycle-in-order-cpu/` produced
during Phase A and still relevant:

| File | Purpose |
|---|---|
| `Simple3CycleCPU.py` | BaseCPU-derived SimObject params (mispredict_flush_cycles, pfu_fifo_words, halt_addr, plus phase_b_* test knobs) |
| `SConscript` | source list + DebugFlag definitions |
| `simple_3cycle_cpu.{hh,cc}` | `BaseCPU` subclass; owns `Pipeline`, `IcachePort` (stub), `DcachePort` (panics), thread vector, archPc, currentStreamId |
| `pipeline.{hh,cc}` | `Ticked` subclass, `evaluate()` is the gem5 equivalent of Python `tick()`, owns PFU/Decode/Execute |
| `types.hh` | `FetchWord`, `DecodedSlotEntry`, `MispredictInfo`, `BackwardRedirect` (all ISA-generic) |
| `pfu.{hh,cc}` | F stage: word FIFO, `wordFifoMinAddr` filter, `filterFifoAndRedirect` (R6 rule), `staticPredictRedirect` |
| `decode.{hh,cc}` | D stage: pulls FIFO, drives generic `InstDecoder`, static prediction, flush bubble |
| `execute.{hh,cc}` | E stage: commits, advances `archPc` (Phase B simplified — no real ALU yet) |
| `stats.{hh,cc}` | per-stage counters |

Also added (for ISA wiring, outside this directory):

- `gem5/src/arch/arm/ArmCPU.py` — `class ArmSimple3CycleCPU(Simple3CycleCPU, ArmCPU)` (A-profile wrapper, currently unused but kept for symmetry).
- `gem5/src/arch/arm/ArmMCPU.py` — `class ArmMSimple3CycleCPU(Simple3CycleCPU, ArmMCPU)` (M-profile wrapper, used by Phase B+).

---

## Phase B — done ✓

Goal: hand-populate PFU FIFO with synthetic Thumb-2 NOP words, drive
the M-profile decoder via `tc->getDecoderPtr()`, commit one
instruction per cycle. End condition: `archPc == HALT_PC` and the
pipeline is drained.

Verification (last run on `test_simple3_phaseB.py` with
`phase_b_num_words=8`, `phase_b_word_value=0xBF00BF00`,
`halt_addr=0x08000020`):

```
system.cpu.instsCommitted          16   # 8 words * 2 NOPs/word
system.cpu.tickCycles              18   # 2 cycles fill + 16 commits
system.cpu.mispredicts              0
system.cpu.staleResponsesDropped    0
```

Trace for the run shows alternating `feed addr=...` (fresh FIFO word)
and `reuse previous word (lastFetchPc=...)` (re-feed for the upper
Thumb halfword) — confirming the decoder fetch-loop logic works for
two 16-bit instructions per 32-bit fetch word.

### What's implemented

- `Pipeline::prepopulateFifoForPhaseB(startAddr, count, wordValue)` is
  called from `Simple3CycleCPU::startup()` whenever the
  `phase_b_num_words` param is non-zero. It seeds `PFU::fifo`,
  `PFU::wordFifoMinAddr`, and `Decode::nextInstrAddrToDeliver` so
  instruction-address bookkeeping stays consistent.
- `Decode::stepOneInstr` clones `tc->pcState()` and calls
  `set(nextInstrAddrToDeliver)` so the decoder's offset arithmetic
  (`offset = pc.instAddr() - fetchPC`) lines up with the FIFO's
  fake addresses, regardless of what the firmware's reset PC is.
- `Execute::runOneCycle` commits `eSlot`, advances `archPc` by
  `staticInst->size()`, takes branches when the static prediction
  said taken, and bumps `instsCommitted`. No real ALU — that's
  pushed to Phase E.
- Halt detection in `Pipeline::evaluate` now checks `cpu.archPc ==
  haltAddr()` and pipeline drained, then `stop()`s.

### Test config

`test/test_simple3_phaseB.py` builds an `ArmMSystem`-derived
`PhaseBMSystem`:

- `STM32G474REPlatform` for SCS / cpuid / memory ranges,
- `ArmMSimple3CycleCPU` (M-profile MMU, M-decoder),
- a single zero-latency NoncoherentXBar (memory is set up only so
  the workload can load — Phase B's FIFO is hand-fed, no real
  fetches happen),
- `ArmMFsWorkload(object_file=bench-branch.elf)` — only there to
  configure the M-profile decoder; the firmware is never executed.

Phase-B params:
- `phase_b_num_words = 8`
- `phase_b_word_value = 0xBF00BF00` (two 16-bit Thumb NOPs)
- `phase_b_start_pc = 0x08000000`
- `halt_addr = 0x08000020` (one address past last NOP)

Expected on success:
- 16 instructions committed
- ~3 cycles pipeline fill + 16 commits = ~19 cycles total

### Bug-trail (chronological, all fixed)

1. **A-profile decoder rejected Thumb bytes.** Initial
   `test_simple3_phaseB.py` used a regular `System` + AArch64
   `test_loop`. The AArch64 decoder asserted on `0xbf00bf00`. Fixed
   by switching to `ArmMSystem` + `ArmMFsWorkload` and using
   `ArmMSimple3CycleCPU`.

2. **A-profile MMU on M-profile system.** `ArmSimple3CycleCPU`
   inherited `mmu = ArmMMU()` (A-profile), but `STM32G474REPlatform`
   expects `mmu.stacking_port` / `mmu.stacking_barrier` (M-profile
   `ArmMMMU`). Fixed by adding `ArmMSimple3CycleCPU(Simple3CycleCPU,
   ArmMCPU)` in `ArmMCPU.py` with `mmu = ArmMMMU()`.

3. **Decoder offset blew up.** The M-decoder's `moreBytes` does
   `offset = pc.instAddr() - fetchPC`. When `tc->pcState()` was the
   firmware's reset PC (e.g. 0x08001234) and `fetchPC` was the
   FIFO word address (0x08000000), `offset` became huge and the
   `consumeBytes` assertion fired. Fixed by cloning the thread's
   `PCStateBase` and calling `set(nextInstrAddrToDeliver)` before
   passing to `moreBytes`.

4. **Decoder hung after first NOP.** After processing the first
   halfword of `0xbf00bf00`, `decoder->offset=2`, `outOfBytes=false`,
   `instReady=false`. The original loop only re-fed the decoder
   when `needMoreBytes()` was true, so it never re-triggered
   `process()` on the still-loaded data. Fixed by tracking
   `_haveLoadedWord` / `_lastFetchPc` and, when bytes remain,
   calling `decoder->moreBytes(*instrPc, _lastFetchPc)` with the
   advanced PC so `process()` reads the next halfword. Verified
   against `m_decoder.cc:137-176`. `resetFetchTracking()` is
   wired into `Decode::squashDSlot`, `Decode::reset`, and
   `Pipeline::prepopulateFifoForPhaseB`.

---

## Phase C — done ✓

Goal: end-to-end icache fetch path. `Simple3CycleCPU` writes a NOP
slide into Flash via `system->getSystemPort().sendFunctional(...)`
during `startup()`, then fetches the bytes through `icache_port`
over the timing memory system, decodes, and commits.

What landed:

| File | Change |
|---|---|
| `pfu.{hh,cc}` | `buildFetchPacket(streamId, requestorId)` builds an `INST_FETCH ReadReq` packet at `_fetchPc`; `shouldIssueFetch()` gates on `fifo.size() + numInFlight < fifoCapacityWords` and `_fetchPc < haltAddr`; `noteIssued`/`noteResponseArrived` track in-flight; `drainResponseStaging()` runs at top of cycle |
| `pipeline.{hh,cc}` | `evaluate()` step 0 drains staging into FIFO; step 5 builds-and-sends if `pfu->shouldIssueFetch()` and no pending retry; `trySendFetchPacket()` parks the packet in `pendingRetryPacket` on failure; `retrySendPendingFetch()` is invoked from `recvReqRetry`; `resetForPhaseC(entryPc)` aligns PFU/Decode after CPU writes the NOP slide |
| `simple_3cycle_cpu.{hh,cc}` | `IcachePort::recvTimingResp` extracts `pkt->getLE<uint32_t>()`, checks `pkt->req->streamId() == cpu.currentStreamId` — match → push `FetchWord` to `pfu->responseStaging`, mismatch → `staleResponsesDropped++`. Always `noteResponseArrived()` and `delete pkt`. `IcachePort::recvReqRetry` calls `pipeline->retrySendPendingFetch()`. `startup()` does the functional NOP-slide write when `phase_c_num_words > 0` |
| `Simple3CycleCPU.py` | `phase_c_num_words` / `phase_c_word_value` / `phase_c_start_pc` params (parallel to the existing Phase-B knobs) |

Verification (last run on `test_simple3_phaseC.py`):

```
system.cpu.instsCommitted          16
system.cpu.requestsIssued           8   # 8 fetch words
system.cpu.responsesReceived        8
system.cpu.staleResponsesDropped    0
system.cpu.tickCycles              28   # Phase B was 18; +10 cyc memory latency
```

Trace shows fetches issued in order (0x8000000, 0x8000004, …,
0x800001c) and responses arriving with `streamId=1` (initial
stream). Each response gets staged and drained the next cycle into
the FIFO; D consumes; E commits.

### Known limitations to revisit in Phase D

- **Stale `pendingRetryPacket` after a redirect.** If a mispredict
  fires while a previous fetch is parked in `pendingRetryPacket`,
  we still send the stale packet on the next `recvReqRetry`. The
  response will be dropped by the streamId check, so it's correct,
  but it wastes one bus cycle. Cleaner: discard the stale packet
  and bump the streamId at redirect time so the retry path issues a
  fresh request. Defer to Phase D when redirects start firing.
- **No real ALU.** Execute still treats prediction as truth; this
  worked for Phase B/C NOPs but won't survive a benchmark with a
  data-dependent branch. Phase D needs `staticInst->execute(execCtx)`.
- **Halt-time draining.** When `archPc == haltAddr`, in-flight
  fetches keep arriving; we still call `noteResponseArrived` /
  delete the packet, so no leak, but the simulator does spin
  through those responses before exiting. For larger tests
  consider also stopping the cycle event from issuing past the
  halt PC, which `shouldIssueFetch()` already does.

---

## Phase D.1 — done ✓

Goal: real ALU + real branch resolution. `staticInst->execute(execCtx)`
runs each committed instruction against a real `ExecContext` wrapping
`SimpleThread`; branch outcomes drive real mispredict squashes
through the existing `filterFifoAndRedirect` path.

What landed:

| File | Change |
|---|---|
| `exec_context.hh` (new) | Minimal `ExecContext` deriving from `gem5::ExecContext`. Delegates registers / PC / misc-regs / predicate to `SimpleThread`. Memory ops, HTM, monitor/mwait, demap all panic — Phase D.2 wires loads/stores against the dcache port. |
| `execute.{hh,cc}` | `runOneCycle` now sets `thread.pcState(*e.pcStateAtDecode)`, constructs an `ExecContext`, calls `staticInst->execute(&ctx, nullptr)`, then `staticInst->advancePC(tc)`, reads back `tc->pcState().instAddr()` as the actual next PC, and compares against the prediction (`predictTaken ? targetPc : addr+size`) to emit `MispredictInfo`. |
| `types.hh` | `DecodedSlotEntry` carries a `std::shared_ptr<PCStateBase> pcStateAtDecode` snapshot — captured by Decode after `decoder->decode()` mutates `npc = pc + size`. Execute restores it onto the thread before invoking the inst. shared_ptr keeps the entry copyable for `std::optional` / `acceptFromDecode`. |
| `decode.cc` | `stepOneInstr` clones the post-decode `instrPc` into the slot entry. |
| `pipeline.cc` | Halt detection switched from `cpu.archPc == haltAddr` to `tc->pcState().instAddr() == haltAddr`. On a static-predict redirect, also `decode->resetFetchTracking()` and `decode->setNextInstrAddrToDeliver(target)` — without this the decoder's cached fetch-word fed wrong-path bytes into the next dSlot. Mispredict path now also calls `setNextInstrAddrToDeliver(actualPc)`. |
| `simple_3cycle_cpu.{hh,cc}` | `archPc` field gone; `Execute` writes `thread.pcState()` directly. `setThreadStartPc(addr)` clones+sets the workload's initial PC for tests that need to override it. New Phase D code path: `phase_d_words` (VectorParam.UInt32) + `phase_d_start_pc` write a hand-coded program into Flash via system-port functional access at startup. |
| `Simple3CycleCPU.py` | `phase_d_words` / `phase_d_start_pc` params. |
| `test/test_simple3_phaseD.py` (new) | 5-iter Thumb-2 loop: `movs r0,#5; subs r0,#1; bne -6; nop; halt`. |

Verification (last run):

```
system.cpu.instsCommitted          12   # 1 movs + 5 subs + 5 bne + 1 nop
system.cpu.mispredicts              1   # the loop-exit BNE
system.cpu.requestsIssued          13
system.cpu.responsesReceived       13
system.cpu.staleResponsesDropped    1   # in-flight response from streamId before mispredict
system.cpu.flushBubbleCycles        2
system.cpu.tickCycles              34
```

Each loop iteration takes ~4 cycles end-to-end because the static
redirect re-fetches the loop word and the memory latency is ~6
cycles per fetch — every iteration pays the round trip. That's
expected behaviour for the architecture; an ART/buffer-line model
(PORT_PLAN §7.4) closes most of that gap and is on the Phase D.2
list for Cortex-M-accurate cycle counts.

### Bug worth remembering

The static-predict redirect (`pfu->staticPredictRedirect`) only
clears the FIFO — it doesn't touch the decoder's cached fetch
word or `_nextInstrAddrToDeliver`. Without the matching
`decode->resetFetchTracking() + setNextInstrAddrToDeliver(target)`
in `pipeline.cc` step 4, the decoder happily fed the wrong-path
upper halfword (the NOP at 0x08000006) into dSlot the next cycle,
and the loop never iterated more than once. Fixed in this round.

## Phase D.2.0 — done ✓

Goal: loads/stores via the dcache port. Single-slot LSQ, Execute
stalls on memref until the response arrives, then resumes via
`completeAcc`. Storage class for the test kernel is set up entirely
in registers (no need for stack init).

What landed:

| File | Change |
|---|---|
| `lsq.{hh,cc}` (new) | Single-slot LSQ. States: Idle / Pending / Complete. `pushReadRequest`/`pushWriteRequest` build paddr-only `Request`s (M-profile is identity-translated), wrap in `ReadReq`/`WriteReq`, `dcachePort.sendTimingReq()`. On refusal, parks the packet for `recvReqRetry`. `completeRequest` flips state to Complete; `release` deletes the packet and returns to Idle. |
| `exec_context.hh` | `initiateMemRead`/`writeMem` panics replaced with `cpu.getLsq().pushRead/WriteRequest(...)`. byte_enable is currently ignored — the test kernel uses naturally-aligned 4-byte ops. |
| `execute.{hh,cc}` | New state machine: `if (lsq.pending()) stall`. Else, if it's a memref and we haven't issued yet → `staticInst->initiateAcc(&ctx, nullptr)`, then stall. When the LSQ flips to Complete → `staticInst->completeAcc(pkt, &ctx, nullptr)`, `release()`, then commit + advancePC + check mispredict. Non-memref still calls `execute()` as before. |
| `pipeline.cc` | D→E gated by `!execute->busy()` so a stalled memref's eSlot doesn't get overwritten by the next decoded inst. (Without this the D-side keeps pushing, stomping on the stalled STR — found exactly this on the first run.) |
| `simple_3cycle_cpu.{hh,cc}` | LSQ owned by the CPU (`std::unique_ptr<LSQ> lsq`). `getDcachePort()` / `getLsq()` accessors. `DcachePort::recvTimingResp` → `lsq->completeRequest(pkt)`; `recvReqRetry` → `lsq->retrySend()`. The dcachePort no longer panics. |
| `stats.{hh,cc}` | New scalars: `dcacheRequestsIssued`, `dcacheResponsesReceived`, `memStallCycles`. |
| `SConscript` | Adds `lsq.cc` source and `Simple3CPULSQ` debug flag. |
| `test/test_simple3_phaseD2.py` (new) | `movs r2,#1; lsls r2,r2,#29; movs r0,#42; str r0,[r2]; ldr r1,[r2]; nop; halt`. r2 ends at 0x20000000 (SRAM), 42 round-trips through memory. |

Verification (last run):

```
system.cpu.instsCommitted               6   # movs/lsls/movs/str/ldr/nop
system.cpu.dcacheRequestsIssued         2   # str + ldr
system.cpu.dcacheResponsesReceived      2
system.cpu.memStallCycles               1   # SRAM responds in ~1 tick at this xbar
system.cpu.mispredicts                  0
system.cpu.tickCycles                  18
```

The bus-side counter agrees: `bus.pktCount_dcache_port = 4` (2 reqs
+ 2 resps).

### Bug worth flagging

When the STR stalled with eSlot occupied, the pipeline's D→E step
was unconditional, so the next-cycle NOP overwrote the stalled
STR. The trace told on it instantly: `completeAcc pc=0x800000a`
when STR was at 0x8000006. Fix is one line in `pipeline.cc` step 3
— gate D→E on `!execute->busy()`. Same back-pressure applies
naturally to dSlot via `Decode::stepOneInstr`'s existing
`if (dSlot.has_value()) return;` guard.

### Notes for Phase D.2.1

- ARM stores' `completeAcc` template (`StoreCompleteAcc` in
  `arch/arm/isa/templates/mem.isa`) is a no-op returning `NoFault`,
  so calling it for stores costs nothing — we don't need to
  special-case stores vs loads.
- byte_enable is ignored in the LSQ; fine for naturally-aligned
  4-byte ops, but Phase D.2.1 firmware likely does sub-word /
  unaligned accesses. Either honour byte_enable or trust the
  request size + paddr (which the dcache port + memory backing
  store already enforce).
- Phase D.2.1 will need to drop the `phase_d_*` knob, set
  `thread->pcState()` from the workload entry, and probably surface
  more corner cases (microop expansion, IT blocks, MSR/MRS to
  CONTROL, vector-table fetch behaviour).

## Phase D.3 — done (functional only) ✓

Goal: replicate the Python prototype's `bench_branch()` instruction
stream (4-NOP preamble + `MOV.W r0,#100; loop: SUBS r0,#1; BNE
loop`) byte-for-byte and compare cycle counts.

What landed:

| File | Change |
|---|---|
| `Simple3CycleCPU.py` | New `phase_d_entry_pc` Addr param. When non-zero, overrides the architectural start PC for the thread; otherwise `phase_d_start_pc` is used. Lets a kernel start at a mid-word halfword (e.g. 0x080074d2) while the memory-write base stays word-aligned (0x080074d0). |
| `simple_3cycle_cpu.cc` | `startup()` reads `phase_d_entry_pc` and uses it for `setThreadStartPc` and `pipeline->resetForPhaseC`, while still writing words at `phase_d_start_pc`. |
| `test/test_simple3_phaseD3_branch.py` (new) | Encodes the Python prototype's bench_branch kernel byte-for-byte: 1 NOP16 + 3 NOP.W (T2) + MOVW r0,#100 (T3) + SUBS r0,#1 + BNE -6, with halt at 0x080074e8. Same `default_memories(enable_art=False)` flash config the prototype verifies against. |

Functional result:

```
instsCommitted        = 205   # 4 preamble + 1 movw + 100*(subs+bne)
mispredicts           = 1     # loop-exit BNE
requestsIssued        = 106   # 6 initial + 100 redirect refetches
staleResponsesDropped = 0
exit @ tick 1588140 → 270 cycles  (1588140 / 5882)
```

### Cycle-count gap: 270 vs Python 1009

Per-iter timing measured from `Simple3CPUExecute` BNE-commit ticks:
deltas alternate **2 cy / 3 cy** (avg **2.5 cy/iter**). Python
charges **~10 cy/iter** on this kernel. Multiplying out:
2.5 × 100 + setup ≈ 270 (matches us); 10 × 100 + setup ≈ 1009
(matches the Python model and is within ±5 of measured silicon).

The gap is the
[`TAKEN_BRANCH_HCLK = 10`](../../../../prototype/stm32g4_verify_model.py#L101)
penalty Python charges on **every** correctly-predicted-taken
backward branch — not just on mispredicts. Empirical Cortex-M4
silicon observation per the prototype: pipeline flush + target-line
refetch bundled. Our pipeline only pays the actual gem5 fetch
latency, which is mostly buffer hits (~1 cy) inside the 8-byte
read buffer line that already contains the loop body.

What we don't model and would need to close the gap:

1. **Pipeline flush on every taken branch (predicted or not).** The
   Python model treats the static-predict-taken redirect as paying
   the full ~3-cycle pipeline refill. Our `pipeline.cc` step 4
   only calls `decode->startFlushBubble()` on mispredicts. To close
   the gap we'd add a smaller flush bubble on the static-predict
   redirect path too.
2. **In-flight Flash retraction blocking new fetch.** R3 in the
   prototype's design rules: a wait-stated AHB transfer can be
   retracted, but the Flash array still holds the bus until its
   physical read completes — so the redirect's target fetch waits.
   Our `PipelinedSimpleMemory` doesn't model that bus-occupancy
   stall.
3. **The `+~5 cycle` residual** the prototype documents as
   "pipeline overhead bundled into TAKEN_BRANCH" — empirical, not
   yet decomposed into specific microarchitectural events.

Closing 1+2 would probably bring us to ~5–7 cy/iter; closing 3 to
~10 needs more reverse-engineering of Cortex-M4. None of that is
trivial, and the user should decide whether matching Python is the
goal or whether functional correctness + a known scaling factor is
sufficient.

## Phase D.4 — done ✓ (timing model rework)

Goal: match the actual Cortex-M4 microarchitecture for the
bench-branch loop, against the no-ART measurement model the user
maintains in [`prototype/stm32g4_no_art.py`](../../../../prototype/stm32g4_no_art.py).

The previous model (Phase D.3 first cut) had two big errors:
1. Treated backward conditional branches as "static-predict-taken"
   at Decode and emitted a `BackwardRedirect`. The Cortex-M4 has
   **no branch predictor** — its TRM does not list one in the
   functional description, and `[DDI 0439D §3.3.1]` Table 3-1
   accounts for taken branches as `1 + P` (P = pipeline refill, 1–3)
   without prediction. What the M4 actually has is **E→D flag
   forwarding**: the BNE at Decode resolves using flags forwarded
   from the SUBS at Execute the same HCLK, redirecting F early.
2. Modelled the 64-bit current buffer (`port_read_buffer_size=8`).
   Per the user, no-ART means no caches and no current/prefetch
   buffers in Flash either. Every Flash read pays full WS+1=5 HCLK
   (4 wait states at 170 MHz Range-1 boost per
   [`RM0440 §4.3.3 Table 19`](../../../reference/stm32/RM0440_stm32g474.pdf)).

What landed:

| File | Change |
|---|---|
| `decode.{hh,cc}` | Removed static prediction. `stepOneInstr` no longer emits `BackwardRedirect`; doesn't set `predictTaken`/`targetPc` (those fields are gone). `squashDSlot` no longer touches `pendingRedirect`; `Decode::reset` cleaned up. New `markSlotEarlyResolved(npc)` so Pipeline can stash the resolved next-PC on dSlot's entry. |
| `types.hh` | `DecodedSlotEntry` no longer carries `predictTaken`/`targetPc`. New fields `earlyResolved` (bool) and `resolvedNpc` (Addr) — Pipeline's E→D forwarding step writes them; Execute reads them to no-op the inst at E. `BackwardRedirect` struct is now dead but kept for now (PORT_PLAN.md still mentions it; can drop later). |
| `pipeline.{hh,cc}` | New step **2.5 (E→D forwarding)** in `evaluate()` calls `earlyResolveBranchAtD(tc)` between Execute (step 1) and D→E (step 3). For any direct (`!isIndirectCtrl`) branch in dSlot, it: saves thread `pcState`, restores the inst's `pcStateAtDecode`, runs the inst's `execute()` speculatively to get the resolved next-PC, restores `pcState`, marks the slot `earlyResolved`, and — if taken — redirects PFU + bumps streamId + parks F-issue for `taken_branch_redirect_delay` HCLK. |
| `pfu.{hh,cc}` | `_redirectFetchDelay` counter parks `shouldIssueFetch()` for N HCLK after a taken redirect. `tickRedirectDelay()` decremented at top of each `evaluate()`. Reset on `PFU::reset`. |
| `execute.cc` | When eSlot's entry is `earlyResolved`, skip `execute()` / `advancePC` entirely — just `tc->pcState(resolvedNpc)`, increment commit count, pop. The bne is a 1-cycle no-op pass-through at E (mirrors no_art prototype's "For BNE, E is a no-op because the branch was already resolved in D"). |
| `Simple3CycleCPU.py` | New `taken_branch_redirect_delay` Cycles param (default **2**). The M4 TRM doesn't decompose where this comes from; plausible architectural sources are (a) next-HCLK rather than same-HCLK forwarding latency, and (b) 1-HCLK AHB address-phase setup before the new ICode HTRANS=NONSEQ can be driven. The agent's m-class-skill lookup confirmed neither is contradicted by the docs — both are implementation-defined. |
| `test/test_simple3_phaseD3_branch.py` | `port_read_buffer_size = [0, 0]` overridden on each `PipelinedSimpleMemory` returned by `default_memories(enable_art=False)`. The platform default is `[8, 8]` because it bakes in current-buffer behaviour even with ART off; we strip that for the no-ART target. |

Verification:

```
system.cpu.instsCommitted          205   # 4 preamble + 1 movw + 100*(subs+bne)
system.cpu.mispredicts               0   # E→D forwarding is exact, no late mispredicts
system.cpu.requestsIssued          105   # 5 setup fetches + 100 redirect refetches
system.cpu.staleResponsesDropped     0
exit @ tick 6029050 → 1025 cycles
```

Per-iter timing (BNE early-resolve deltas): exactly **10 HCLK**,
matching the no-ART silicon empirical figure of 10 HCLK/iter.
Total 1025 vs no_art prototype's 1013 (+12 ≈ +1.2%); the residual
is preamble fetch overhead — the prototype's `_with_preamble`
seeds the buffer line at the entry address so the first NOP fetch
is a hit and `_fetch_word` charges 1 HCLK instead of 5. We don't
model the buffer at all per the user's directive, so all 6 setup
fetches pay full 6-HCLK misses. The +12 is "no-buffer cost we'd
expect once the buffer is dropped."

### Bug found & fixed in this round

`Pipeline::earlyResolveBranchAtD` originally set `thread.pcState`
to the resolved npc and *forgot* to restore it before returning.
That worked for the bne-at-E commit (Execute reads the stashed
`resolvedNpc` directly), but Decode also reads `tc->pcState()` as
the clone source for `instrPc` in `stepOneInstr`. Without
restoring, decode would see the future PC mid-cycle. Fixed by
saving `thread.pcState().clone()` before the speculative execute
and restoring at the end of `earlyResolveBranchAtD`.

### Known limitations / Phase D.5 candidates

- `taken_branch_redirect_delay = 2` is empirically tuned to match
  silicon's 10 HCLK/iter. The M4 TRM doesn't isolate the
  contributing events; closing the +12 cycle setup gap or running
  other benchmarks may need a different value, or a more
  principled decomposition (e.g. separate "next-HCLK forwarding"
  bool + "1-HCLK fetch-restart" knob).
- The legacy `mispredict_flush_cycles` / `MispredictInfo` path in
  Execute is unreachable for direct branches (E→D forwarding
  always pre-resolves them). It still fires for indirect branches
  (BX/BLX reg) where we can't pre-execute. We don't have a test
  for that yet.
- `pendingRetryPacket` cleanup on redirect (Phase C known
  limitation) is still open and probably needs attention before
  we run real firmware (Phase D.2.1 / Phase D.5).
- The other 6 benchmarks (`bp_tight`, `bp_long_body`,
  `bp_forward`, `bp_alternating`, `bp_align`, `bp_nested`) are
  not yet wired up. Each is an instruction-stream + halt PC
  config like Phase D.3; should be straightforward to add now.

## Phase D.5 — done ✓ (BP benchmark sweep)

Goal: replicate all 7 BP benchmarks from
[`prototype/stm32g4_verify_model.py`](../../../../prototype/stm32g4_verify_model.py)
as gem5 tests, validate functional correctness (commit counts and
mispredicts), and compare cycle counts to silicon targets in
PORT_PLAN.md §13.

What landed:

| File | Change |
|---|---|
| `simple_3cycle_cpu.cc` (existing) — and now `pipeline.cc` | On halt detection, `Pipeline::evaluate` calls `exitSimLoop("Simple3CycleCPU reached haltAddr", 0)` so `m5.simulate()` returns promptly instead of running to MaxTick draining stale icache responses. Without this, the halt cycle was reachable in the trace (`stopping at cycle=N` DPRINTF) but `m5.curTick()` always returned MaxTick. |
| `test/simple3_bp_kernels.py` (new) | Single Python module with all 7 kernel word-streams + halt PCs + silicon targets. Each kernel mirrors the corresponding `bench_*` function in `verify_model.py` byte-for-byte (assembled via `arm-none-eabi-as`). |
| `test/test_simple3_phaseD5_bp.py` (new) | Parameterised test runner: takes a benchmark name on the command line, builds the M-profile system, runs to halt, prints `cycles = exit_tick / 5882` alongside silicon target + delta + percent. |

Each kernel uses the same preamble layout as Phase D.3 (2-byte pad
+ 1×NOP T1 + 3×NOP.W at 0x080074d2..0x080074df) so the entry PC is
0x080074d2 and the actual benchmark kernel starts at 0x080074e0,
identical to the silicon binary.

### Functional verification

| Bench | `instsCommitted` | Expected (preamble 4 + kernel) | Mispredicts |
|---|---:|---|---:|
| branch | 205 | 4 + 1 + 100·(subs+bne) = 205 ✓ | 0 ✓ |
| tight | 405 | 4 + 1 + 200·2 = 405 ✓ | 0 ✓ |
| longbody | 1005 | 4 + 1 + 100·(8nop+subs+bne) = 1005 ✓ | 0 ✓ |
| forward | 406 | 4 + 2 + 100·(cmp+beq+subs+bne) = 406 ✓ | 0 ✓ |
| alternating | 905 | 4 + 1 + 100·(ands+bne+nop+subs+bne) + 100·(ands+bne+subs+bne) = 905 ✓ | 0 ✓ |
| nested | 465 | 4 + 1 + 20·(mov+10·(subs+bne)+subs+bne) = 465 ✓ | 0 ✓ |
| align | 412 | 4 + Loop A (201) + Loop B (202) + Loop C (4) + 1 trail = 412 ✓ | 0 ✓ |

All 7 commit the right number of instructions. Mispredicts are zero
because E→D forwarding resolves every direct branch exactly. PFU
issues are bounded by `shouldIssueFetch()` and `staleResponsesDropped`
is zero across the board (no wrong-path fetches issued).

### Cycle counts vs silicon targets (after model correction)

The first-pass numbers in this section were wrong — they assumed
"no current buffer" which under-modelled silicon. After re-enabling
the 8-byte sense-amp buffer and adding the post-redirect
buffer-bypass:

| Bench | gem5 cyc | silicon cyc | Δ | Δ% | Notes |
|---|---:|---:|---:|---:|---|
| branch | **1013** | 1013 | +0 | +0.00% | ✓ exact |
| tight | **2013** | 2013 | +0 | +0.00% | ✓ exact (same per-iter as branch with 200 iters) |
| longbody | **2310** | 2213 | +97 | +4.38% | 1 redirect/iter, 5-word body — buffer hits on consecutive same-line fetches work |
| nested | **2170** | 2318 | -148 | -6.38% | We're slightly faster — inner-loop redirect overlap is too optimistic |
| forward | **2015** | 1817 | +198 | +10.90% | 2 redirects per iter — buffer-bypass charged twice |
| alternating | **4210** | 3420 | +790 | +23.10% | 200 iters × 2 redirects — same double-charge as forward but at scale |
| align | **3008** | 2211 | +797 | +36.05% | 3 loops + Loop B's MOV.W straddles a word boundary, costing an extra fetch per iter |

### Why the post-redirect buffer-bypass is needed

The earlier model (no buffer at all) gave bench-branch +12 cy
overshoot but longbody +812 overshoot. That asymmetry was the tell —
silicon for longbody runs at ~22 cy/iter, which matches a model
*with* a current buffer where 4 of the 5 same-line fetches are
~1 HCLK hits. Without the buffer, every fetch is 5+1=6 HCLK and
the loop body alone consumes ~30 HCLK/iter.

Re-enabling the buffer fixed longbody but broke bench-branch (5 cy/iter
instead of silicon's 10). The diff: silicon pays a flat ~10 HCLK on
every taken redirect (`TAKEN_BRANCH_HCLK` in the prototype's
[`verify_model.py`](../../../../prototype/stm32g4_verify_model.py#L101)),
which doesn't shrink even when the target line is sitting in the
buffer. That bundle includes: pipeline drain (~3 HCLK), AHB address
phase + new fetch (5 HCLK), and a 1-2 HCLK PFU restart residual.

The cleanest model: mark the first fetch after a taken redirect as
`Request::UNCACHEABLE` so PipelinedSimpleMemory's buffer-hit
shortcut is bypassed and the fetch pays full WS latency. That
captures the bundle cost via the actual fetch's miss latency rather
than as a separate timing event.

What landed:

| File | Change |
|---|---|
| `mem/pipelined_simple_mem.cc` | Buffer-hit shortcut now skipped when `pkt->req->isUncacheable()`. CPU front-ends use this to model conditions where the buffer line is logically stale even though the address matches. |
| `pfu.{hh,cc}` | New `_nextFetchBypassBuffer` flag set in `staticPredictRedirect`; cleared after the first `buildFetchPacket` call applies `Request::UNCACHEABLE`. Debug print includes the flag state. |

### Why the multi-redirect kernels still overshoot

`forward` and `alternating` have 2 taken branches per iter (e.g.
`forward`: BEQ taken to skip nops + BNE taken back to top). Each
redirect triggers our post-redirect buffer-bypass, charging the
~5-HCLK forced-miss cost twice per iter. Silicon empirically only
pays the bundle ONCE per iter (the second taken branch's bundle
overlaps with the pipeline that's still draining from the first).
Closing this gap properly needs either:
- A "redirect-since-last-decode" flag that suppresses the buffer-bypass
  when a recent redirect already paid it; or
- Treating only redirects with intervening commits as new bundles.

`align` has all the above PLUS Loop B's MOV.W straddles a word
boundary at 0x080074ec. Decoding it requires fetching both
0x080074e8 and 0x080074ec, where the second fetch can be a hit but
straddling-fetch ordering interacts with the redirect-bypass.

Three of seven (`branch`, `tight`, `nested`) match silicon to
within ~4%. The other four overshoot — *and the magnitude of the
overshoot scales with the number of fetch-words touched per
iteration*, which is the hallmark of "we don't model the buffer
that silicon does." The user's no-ART directive is the right
posture for the architectural model; the cost is that benchmarks
with multi-word loop bodies will systematically come in higher
than silicon.

The `nested` underrun (-93 cy) is the more interesting outlier:
our pipeline is slightly *too* optimistic for nested redirect
patterns, probably because the +2-HCLK `taken_branch_redirect_delay`
knob is calibrated against the single-loop case and undercharges
when an inner-loop redirect happens to overlap with the outer
loop's fetch stream. Worth a closer look in a future polish round.

### Why the 4 overshooting benchmarks scale the way they do

Per-iter overshoot vs silicon (rough estimate, dividing total Δ by
loop iter count, ignoring setup):

| Bench | Δ/iter | Loop body words | Notes |
|---|---:|---:|---|
| branch | 0 | 1 | 1 fetch/iter; matches |
| tight | 0 | 1 | same |
| forward | +2.13 | ~2 | CMP+BEQ in one word, SUBS+BNE in another |
| longbody | +8.12 | 5 | 4 NOP-pair words + 1 subs/bne word |
| alternating | +5.5 | ~2 | branch-around-NOP, asymmetric paths |
| align | +13.2 (~3 loops) | mixed | Loop B straddles, no-flag-set interaction in Loop C |

The Δ/iter roughly correlates with #words/iter. For each extra
word fetched per iter, silicon pays ~1 HCLK (buffer hit) but we
pay 5 HCLK (full Flash WS). Difference: ~4 HCLK per extra word.
That matches the observed slopes within rounding noise.

### Phase D.5 v3 — forward + same-line cheap-redirect fix

A side-by-side trace of `alternating` (Python verify_model vs gem5)
showed the forward BNE_odd taken @ 0x080074e8 → 0x080074ec was
costing 8 HCLK in gem5 (UNCACHEABLE forced miss + delay) but only
2 HCLK in Python (`MISPREDICT_SAME_LINE_HCLK`). Both target and
fall-through are in the same 8-byte line (0x080074e8..ef), already
in the current-buffer.

Fix in `pipeline.cc::earlyResolveBranchAtD`: distinguish backward
taken (always full TAKEN_BRANCH bundle) from forward taken with
same-line target (cheap MISPREDICT_SAME_LINE bubble). Logic:

```
const bool isForward = resolvedNpc > de.addr;
const bool sameLine =
    (resolvedNpc & ~0x7) == (fallThrough & ~0x7);
const bool cheapRedirect = isForward && sameLine;

pfu->staticPredictRedirect(resolvedNpc, /*sameLine=*/cheapRedirect);
if (!cheapRedirect)
    pfu->setRedirectFetchDelay(taken_branch_redirect_delay);
```

`PFU::staticPredictRedirect(target, sameLine)` clears the FIFO
unconditionally (kept entries past the target would mis-decode
against the wrong PC) and only differs in setting
`_nextFetchBypassBuffer = !sameLine`. For same-line forward, the
next fetch hits the buffer (1 HCLK) instead of paying the full
WS-latency miss (5 HCLK).

Why **forward only**: per Cortex-M4 silicon (and the Python
prototype's classification), backward taken always pays the full
TAKEN_BRANCH bundle (the M4 doesn't speculatively fetch the
target, so the line isn't pre-warmed in the buffer). Forward
taken is treated as a mispredict against the speculative
fall-through fetch, and if the target's line is the one we just
finished fetching, a cheap bubble suffices.

Why **clear FIFO** (vs the agent's earlier suggestion to keep
useful entries via `filterFifoAndRedirect`): for backward
same-line redirects, the speculatively-fetched FIFO entries are
PAST the target. Keeping them would feed wrong-word data into the
decoder when it tries to decode at the lower target PC. Found
this in nested: r1 was being decremented as if it were r0
(decoder fed word 0xec but tried to decode at pc 0xe8) — inner
loop exited after 3 iters instead of 10.

### Cycle counts (Phase D.5 v4 — silicon-targeted refinements)

The user clarified: **the ground truth is silicon, not Python.** Use
Python as a debugging reference but optimise for silicon match.
Two more bugs found and fixed in this round:

**Fix #1: PFU `_numInFlight` not reset on redirect.** Speculative
fetches issued past the active loop body (when `halt_pc` is far
out, e.g. `align`'s 0x08007500) filled the PFU's 3-slot in-flight
budget. After a BNE redirect, the redirect-target fetch was gated
by `shouldIssueFetch`'s capacity check until the stale in-flight
fetches drained — adding ~5 HCLK/iter to every loop. Fixed in
[`pfu.cc::staticPredictRedirect`](pfu.cc) — set `_numInFlight = 0`
on redirect (stale responses are still dropped on arrival via the
streamId check, but they no longer occupy capacity).

This was caught by tracing align's Loop A and finding it took
14.9 cy/iter despite being identical to bench-branch's 10 cy/iter
kernel — same instructions, only `halt_pc` differed.

**Fix #2: cheap-redirect kept re-fetching the target word.** For
forward+sameLine cheap redirects, the speculative fall-through
fetch had already pulled the target word into the FIFO (target is
ahead of fall-through within the same line). But our redirect
bumped streamId and cleared the FIFO, causing a re-fetch that
cost ~2 HCLK we shouldn't pay. Fixed by:
- `staticPredictRedirect(target, sameLine=true)` now uses
  `filterFifoAndRedirect` (keep entries `addr >= target`) and
  doesn't arm UNCACHEABLE.
- `Pipeline::earlyResolveBranchAtD` skips the `currentStreamId++`
  on cheap redirects so the in-flight speculative response with
  the old streamId still lands in the FIFO.

This was caught by tracing `alternating` and seeing the cheap
forward redirect was followed by a "drop stale response addr=0xec"
followed by a re-fetch of the same address.

### Cycle counts after the two fixes

```
bench          silicon    python      gem5     gem5-vs-sil    gem5-vs-py
-----          -------    ------      ----     -----------    ----------
branch            1013      1013      1013      +0 (+0%)       +0 (+0%)    ✓
tight             2013      2013      2013      +0 (+0%)       +0 (+0%)    ✓
longbody          2213      2213      2310     +97 (+4.4%)    +97 (+4.4%)
nested            2318      2153      2170    -148 (-6.4%)    +17 (+0.8%)
forward           1817      2010      2015    +198 (+10.9%)    +5 (+0.2%)
alternating       3420      3313      3610    +190 (+5.6%)   +297 (+9.0%)
align             2211      2515      2518    +307 (+13.9%)    +3 (+0.1%)
```

Sum of |gem5-vs-silicon| Δ across 7 benchmarks: was 1828 →
**940** (49% reduction). Average error: 6.3% across all kernels,
with 2 exact matches.

`align` matches Python within 3 cycles now (was +493). `alternating`
within 297 (was +897). Both fixes pulled gem5 close to Python on
those kernels; the residual silicon gap is the same gap Python has
(silicon-vs-Python: align +304, alternating -107).

### What's left (silicon-side)

- **`nested -148`**: gem5 is *faster* than silicon. Silicon spends
  ~7 extra HCLK per outer iter that we don't model (probably an
  interaction between mov r1 fetch and inner-loop redirect that
  Python doesn't capture either — Python is also -165).
- **`forward +198`**: Python is +193. So 195 of the 198 is the
  same gap Python has — a silicon optimisation neither model
  captures (probably the BEQ+nop+nop+SUBS pattern's 2-redirect-
  per-iter cost that silicon overlaps better than either model).
- **`align +307`**: Loop B (MOV.W straddles word boundary) per-iter
  cost is 15 HCLK in gem5 vs ~12 in silicon. Likely cause: silicon
  overlaps the second word fetch with decode of the first more
  aggressively than our pipeline does.
- **`longbody +97`**: small constant overhead, likely setup-fetch
  pipelining (~1 cy/iter * 100 iters).

All four remaining gaps are within the same ballpark Python sees
(±15% silicon error). To close them further we'd need either a
more nuanced PFU/decode-overlap model or empirical silicon
microbenchmarks to disambiguate which architectural events are
contributing.

`alternating` improved from +897 to +497 vs Python (the entire
+400 came from the BNE_odd cheap-redirect fix). All other
benchmarks unchanged because none of them have a forward+same-line
taken branch:

- `branch`/`tight`: only one BNE per iter, backward.
- `longbody`/`nested`: only backward branches.
- `forward`: BEQ is forward but target (0x080074f0) is on a
  different line from fall-through (0x080074ec). Pays full bundle.
- `align`: all three loops have backward BNEs.

### Phase D.6 candidates

- **The forward kernel's BEQ is forward+different-line** but Python
  still charges 2 HCLK there because it's in the same flash *line*
  (0x080074e8..ef) as the speculative fall-through 0x080074ec
  (which is in the same line as 0x080074f0... actually 0x080074f0
  is in a different line 0x080074f0..f7). Worth re-checking the
  line analysis here — maybe Python's model treats BEQ specially.
- **Word-straddling Thumb-2 in `align`** Loop B: MOV.W at
  0x080074ea spans words 0x080074e8 and 0x080074ec. Decoder gets
  the right commit count but our fetch-ordering / pipeline
  overhead adds ~5 HCLK/iter the Python model doesn't see. Needs
  per-cycle trace comparison like alternating got.
- **Polish:** Drop dead `BackwardRedirect`, drop dead
  `pendingRedirect` references, tighten `pendingRetryPacket`
  redirect handling, add line trace per PORT_PLAN §12.

## Phases E / F — TODO

Per [`PORT_PLAN.md`](./PORT_PLAN.md) — real ELF execution from
reset vector (Phase E) needs microop expansion (LDM/STM for
PUSH/POP), IT block handling, and probably semihosting hookup.
External FlashModel refactor (Phase F) is optional polish.

Per [`PORT_PLAN.md`](./PORT_PLAN.md). Not started.

---

## Key files outside this directory

- `gem5/src/arch/arm/ArmCPU.py` — added `ArmSimple3CycleCPU`.
- `gem5/src/arch/arm/ArmMCPU.py` — added `ArmMSimple3CycleCPU`.
- `test/test_simple3_phaseA.py` — Phase A smoke test.
- `test/test_simple3_phaseB.py` — Phase B M-profile smoke test.
- `test/test_simple3_phaseC.py` — Phase C M-profile icache smoke test.
- `test/test_simple3_phaseD.py` — Phase D real-ALU + mispredict test.
- `test/test_simple3_phaseD2.py` — Phase D.2 LSQ + dcache load/store test.
- `test/test_simple3_phaseD3_branch.py` — Phase D.3 synthetic bench-branch
  (matches the Python prototype's instruction stream).
- `test/simple3_bp_kernels.py` — Phase D.5 word-stream + halt PC + silicon
  target table for all 7 BP benchmarks.
- `test/test_simple3_phaseD5_bp.py` — Phase D.5 parameterised runner; pick
  one of {branch, tight, longbody, forward, alternating, nested, align}.

## Reference

- Plan: [`PORT_PLAN.md`](./PORT_PLAN.md) (this directory).
- Python model + design doc:
  `prototype/stm32g4_pipeline_model.py`,
  `prototype/STM32G4_PIPELINE_MODEL.md`.
- Approved implementation plan:
  `~/.claude/plans/proud-hugging-steele.md`.
