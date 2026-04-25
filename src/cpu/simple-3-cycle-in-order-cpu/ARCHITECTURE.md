# Simple3CycleCPU — Architecture Reference

A detailed reference for the current (Phase D.5 v4) state of the
`Simple3CycleCPU` gem5 model: its structure, every stage's behaviour
in every case, every latency knob, and the cycle-by-cycle interaction
between the CPU, the PFU, the LSQ, and the AHB-style Flash memory
model on the bus.

This document describes **what the model does**, not why or how it
was built — see [`PORT_PLAN.md`](PORT_PLAN.md) for the original
design intent and [`PROGRESS.md`](PROGRESS.md) for the historical
phase-by-phase build log.

---

## Contents

1. [Scope and design summary](#1-scope-and-design-summary)
2. [SimObject hierarchy](#2-simobject-hierarchy)
3. [Top-level dataflow diagram](#3-top-level-dataflow-diagram)
4. [The cycle event — `Pipeline::evaluate()`](#4-the-cycle-event--pipelineevaluate)
5. [Stage F — PFU (Prefetch Unit)](#5-stage-f--pfu-prefetch-unit)
6. [Stage D — Decode](#6-stage-d--decode)
7. [Stage E — Execute](#7-stage-e--execute)
8. [LSQ — Load/Store Queue](#8-lsq--loadstore-queue)
9. [Branch handling — every case](#9-branch-handling--every-case)
10. [Memory subsystem — AHB / Flash modelling](#10-memory-subsystem--ahb--flash-modelling)
11. [Latency table](#11-latency-table)
12. [Parameter reference](#12-parameter-reference)
13. [Statistics](#13-statistics)
14. [Debug flags](#14-debug-flags)
15. [Halt and exit](#15-halt-and-exit)
16. [Test-mode hooks](#16-test-mode-hooks)
17. [Known limitations](#17-known-limitations)

---

## 1. Scope and design summary

`Simple3CycleCPU` is a **3-stage in-order single-issue CPU** modelled
after the Cortex-M4. Pipeline stages: **F** (Fetch / PFU) → **D**
(Decode) → **E** (Execute). All stages execute in a single
gem5-tick `evaluate()` call per CPU clock cycle.

Key properties:

- **Single thread.** `numThreads` must be 1; enforced by `fatal_if`
  in the constructor.
- **In-order, single-issue.** One instruction commits per cycle in
  the steady state.
- **No branch predictor.** The Cortex-M4 has no predictor; we model
  its actual behaviour: **E→D forwarding** resolves direct
  conditional branches at Decode using the flag write the prior
  inst produced at Execute the same cycle.
- **ISA-agnostic core.** No `arch/<isa>/` includes — the CPU uses
  gem5's generic `InstDecoder`, `StaticInst`, and `ExecContext`.
  Tested with ARMv7-M (Thumb-2). Should work for any ISA whose
  decoder follows the generic interface.
- **Memory model — STM32G4 Flash.** Connect `icache_port` to a
  `PipelinedSimpleMemory` configured per
  `gem5/src/python/gem5/prebuilt/cortexm/platforms.py::STM32G474REPlatform.default_memories(enable_art=False)`.
  The CPU does not model Flash internally; it models only the
  interaction protocol (taken-branch redirect bypassing the
  current-line buffer, in-flight stream tagging, FIFO back-pressure).
- **dcache port.** Wired to a single-slot LSQ. Loads/stores stall E
  until the response arrives. No dcache itself — connect the
  `dcache_port` to the same memory bus as `icache_port`.

---

## 2. SimObject hierarchy

```
gem5::BaseCPU                              (gem5 base class)
└── gem5::simple3::Simple3CycleCPU
     ├── threads[]      : std::vector<SimpleThread*>      (1 entry)
     ├── icachePort     : IcachePort  (RequestPort)        — instr fetch
     ├── dcachePort     : DcachePort  (RequestPort)        — load/store
     ├── pipeline       : std::unique_ptr<Pipeline>
     │    ├── pfu       : std::unique_ptr<PFU>             — F stage
     │    ├── decode    : std::unique_ptr<Decode>          — D stage
     │    └── execute   : std::unique_ptr<Execute>         — E stage
     ├── lsq            : std::unique_ptr<LSQ>             — single-slot load/store
     └── simple3Stats   : Simple3Stats                     — per-CPU stats group
```

`Pipeline` derives from `gem5::Ticked`; its `evaluate()` is
scheduled every `clockPeriod()` ticks while
`Simple3CycleCPU::activateContext()` has been called.

Each stage (`PFU`, `Decode`, `Execute`) is a plain C++ class — not a
SimObject. They live as members of `Pipeline`, which dispatches into
them in fixed order each cycle.

---

## 3. Top-level dataflow diagram

```
                 ┌────────────────────────────────────────────────┐
                 │             gem5 memory bus / xbar              │
                 │  (icache traffic + dcache traffic + functional)  │
                 └─┬──────────────────────────────────┬───────────┘
                   │                                  │
        IcachePort │ icache_port                      │ dcache_port  DcachePort
                   ▼                                  ▼
   ┌───────────────────────────┐         ┌───────────────────────────┐
   │  PFU                      │         │  LSQ (single slot)         │
   │  ───────────────────────  │         │  ─────────────────────────  │
   │  • _fetchPc               │         │  states: Idle / Pending /   │
   │  • _wordFifoMinAddr       │         │          Complete           │
   │  • fifo<FetchWord> [3]    │         │  one in-flight memref       │
   │  • _numInFlight           │         └───────┬───────────────┬─────┘
   │  • _redirectFetchDelay    │                 │               │
   │  • _nextFetchBypassBuffer │            push │         release│
   │  • responseStaging[]      │                 │               │
   └───────┬───────────────────┘                 │               │
           │ deliverWord (drain at top of cy)    │               │
           ▼                                     │               │
   ┌──────────────────────┐ ─popForExecute─►┌────┴───────────────┴───┐
   │  Decode              │                 │  Execute                │
   │  ──────────────────  │                 │  ─────────────────────  │
   │  • dSlot (1 inst)    │                 │  • eSlot (1 inst)       │
   │  • flushRemaining    │                 │  • LSQ memref handling  │
   │  • _haveLoadedWord   │ ◄── E→D fwd ────│  • staticInst->execute  │
   │  • _lastFetchPc      │  (read flags)   │  • staticInst->advancePC│
   │  • _nextInstrAddr…   │                 │  • thread.pcState write │
   └──────────────────────┘                 └─────────────────────────┘
           ▲                                         │
           │       Pipeline::evaluate()              │
           │       step 2.5 (E→D early resolve)      ▼
           │       redirects PFU + sets dSlot         tc->pcState()
           │       earlyResolved + resolvedNpc        (architectural state)
           │
           └─ ExecContext (in earlyResolveBranchAtD speculative pre-run)
```

Every cycle, `Pipeline::evaluate()` walks 6 sub-steps in fixed order
(see §4). Within a single `evaluate()`, data flows **forward** from
PFU → Decode → Execute via the `dSlot` and `eSlot` registers, and
**backward** for redirects and back-pressure.

---

## 4. The cycle event — `Pipeline::evaluate()`

`evaluate()` runs **once per CPU clock cycle**. It executes the
following six sub-steps **in this exact order, atomically within
the cycle**:

```
┌─────────────────────────────────────────────────────────────────────┐
│ STEP 0 — Drain async events                                          │
│    pfu.drainResponseStaging()                                        │
│       ↳ for each FetchWord in responseStaging: deliverWord(w)        │
│         (filters wholly-below-target words; pushes valid into FIFO)  │
│    pfu.tickRedirectDelay()                                           │
│       ↳ if _redirectFetchDelay > 0: --_redirectFetchDelay            │
├─────────────────────────────────────────────────────────────────────┤
│ STEP 1 — Execute (E)                                                 │
│    mp = execute.runOneCycle(tc)                                      │
│       ↳ commits eSlot inst (1 cy unless LSQ stall)                   │
│       ↳ may write thread regs, flags, PC                             │
│       ↳ returns MispredictInfo (only set for indirect branches)      │
├─────────────────────────────────────────────────────────────────────┤
│ STEP 2 — Legacy mispredict path (rare — indirect branches only)      │
│    if mp.happened:                                                   │
│       decode.squashDSlot(); decode.setNextInstrAddrToDeliver(actual) │
│       pfu.filterFifoAndRedirect(actual); decode.startFlushBubble(P)  │
│       cpu.currentStreamId++                                          │
├─────────────────────────────────────────────────────────────────────┤
│ STEP 2.5 — E→D forwarding (the M-profile branch resolution)          │
│    earlyResolveBranchAtD(tc)                                         │
│       ↳ if dSlot is a direct branch (isControl && !isIndirectCtrl): │
│            speculatively run staticInst->execute() against the       │
│            just-committed flags (saved/restored thread.pcState),     │
│            mark dSlot.earlyResolved=true, save resolvedNpc.          │
│            If taken: redirect F (full-bundle or cheap-fwd-sameline). │
├─────────────────────────────────────────────────────────────────────┤
│ STEP 3 — D → E (back-pressured)                                      │
│    if !execute.busy() and decode.hasInstrReadyForExecute():          │
│       execute.acceptFromDecode(decode.popForExecute())               │
│    (back-pressure prevents stomping on a memref-stalled eSlot)       │
├─────────────────────────────────────────────────────────────────────┤
│ STEP 4 — D step (FIFO → D, gated by flushBubble)                     │
│    if !decode.isFlushing():                                          │
│       decode.stepOneInstr(pfu, tc)                                   │
│         ↳ pull a word from FIFO (or re-feed cached word)             │
│         ↳ run InstDecoder.process() then .decode()                   │
│         ↳ on success: dSlot = new entry                              │
│    decode.decrementFlushCounter()                                    │
├─────────────────────────────────────────────────────────────────────┤
│ STEP 5 — F (issue fetch)                                             │
│    if !pendingRetryPacket and pfu.shouldIssueFetch():                │
│       pkt = pfu.buildFetchPacket(streamId, requestorId)              │
│       trySendFetchPacket(pkt)                                        │
│         ↳ sendTimingReq; on false → pendingRetryPacket = pkt         │
├─────────────────────────────────────────────────────────────────────┤
│ STEP 6 — Halt detection                                              │
│    if haltAddr != 0 && tc.pcState().instAddr() == haltAddr           │
│       && !execute.busy() && !decode.busy():                          │
│         stop(); exitSimLoop("…", 0)                                  │
└─────────────────────────────────────────────────────────────────────┘
```

**Why this order matters:**

- **Step 1 before 2.5** so the flag write from the prior inst is
  visible to the branch's speculative pre-execute.
- **Step 2.5 before step 3** so the same-cycle redirect propagates
  to F before it issues on step 5; `dSlot` is updated with
  `earlyResolved=true` before D→E moves it to `eSlot`.
- **Step 3 (D→E) before step 4 (D step)** so a fresh decode in step
  4 doesn't get immediately drained and clobbered.
- **Step 5 last** so all upstream redirects + state updates apply
  before the new fetch is built.
- **Async events (icache responses, dcache responses,
  recvReqRetry) NEVER mutate pipeline state directly**; they push
  into staging buffers (`pfu->responseStaging`,
  `lsq->_state = Complete`) that the next `evaluate()` consumes in
  step 0 / step 1.

---

## 5. Stage F — PFU (Prefetch Unit)

**Files:** [`pfu.hh`](pfu.hh), [`pfu.cc`](pfu.cc)

The PFU owns the architectural fetch pointer and a 32-bit-word
FIFO into which icache responses are drained. It issues new icache
requests while there's room and the halt PC hasn't been crossed.

### State

| Field | Meaning |
|---|---|
| `_fetchPc` | Word-aligned address of the **next** fetch the PFU will issue. |
| `_wordFifoMinAddr` | Per-instruction filter. Words whose `addr+4 ≤ this` are dropped at deliver-time (post-redirect cleanup). |
| `fifo` | `std::deque<FetchWord>`, capacity = `pfu_fifo_words` (default 3). |
| `_numInFlight` | Count of icache requests sent that haven't returned. **Reset to 0 on every redirect** so stale in-flight don't gate new issues. |
| `_redirectFetchDelay` | Cycles until F may issue after a non-cheap redirect. Decremented at top of every cycle. |
| `_nextFetchBypassBuffer` | If true, the **next** `buildFetchPacket` will set `Request::UNCACHEABLE` so PipelinedSimpleMemory skips its buffer-hit shortcut. |
| `responseStaging` | Words staged by `IcachePort::recvTimingResp` (async) for the next cycle's drain. |

### `shouldIssueFetch()` — when F may issue

Returns **true** iff ALL of:

1. `_redirectFetchDelay == 0` — not in a post-redirect hold-off.
2. `fifo.size() + _numInFlight < fifoCapacityWords` — room in the FIFO budget.
3. If `haltAddr != 0`, `_fetchPc < haltAddr` — never speculate past halt.

### `buildFetchPacket(streamId, requestorId)` — what F issues

```
flags = INST_FETCH
if _nextFetchBypassBuffer:
    flags |= UNCACHEABLE
    _nextFetchBypassBuffer = false        // one-shot
req = make_shared<Request>(_fetchPc, 4, flags, requestorId)
req.setStreamId(streamId)
pkt = new Packet(req, ReadReq); pkt.allocate()
_fetchPc += 4
return pkt
```

The `streamId` tags the request so async responses can be matched
against `cpu.currentStreamId` in `recvTimingResp` and dropped if
the stream has changed (i.e. there's been a redirect since issue).

### `staticPredictRedirect(targetPc, sameLine)` — taken-branch redirect

Two paths, switched by the `sameLine` flag (set by Pipeline based
on whether target and fall-through are in the same 8-byte Flash
line):

```
if sameLine:
    // Cheap forward+same-line redirect (Pipeline only sets sameLine
    // when the branch is forward AND target line == fall-through line).
    filterFifoAndRedirect(targetPc)        // keep entries addr ≥ target
    _nextFetchBypassBuffer = false         // buffer hit is correct
else:
    // Full TAKEN_BRANCH bundle — backward, or forward to diff line.
    fifo.clear()
    _wordFifoMinAddr = targetPc
    _fetchPc = targetPc & ~0x3
    _nextFetchBypassBuffer = true          // arm UNCACHEABLE
_numInFlight = 0                           // stale in-flight no longer
                                           //   gates capacity check
```

### `filterFifoAndRedirect(actualPc)` — R6 rule

Used by both the cheap-redirect path and the legacy mispredict
path. Implements the prototype's R6 rule: **keep FIFO words whose
address is ≥ `targetWord = actualPc & ~0x3`**, drop the rest.
Resets `_wordFifoMinAddr = actualPc` so subsequent `deliverWord`
calls drop late-arriving wrong-path responses.

### `deliverWord(word)` — async response → FIFO

Called from `drainResponseStaging` at the top of every cycle. Drops
words wholly below `_wordFifoMinAddr` (post-redirect filtering) and
appends the rest to `fifo`.

### `noteIssued()` / `noteResponseArrived()`

Bookkeeping called by Pipeline (on successful `sendTimingReq`) and
by `IcachePort::recvTimingResp` respectively, to maintain
`_numInFlight`.

---

## 6. Stage D — Decode

**Files:** [`decode.hh`](decode.hh), [`decode.cc`](decode.cc)

D pulls one 32-bit word per cycle from the PFU FIFO, feeds it to
the per-thread `InstDecoder`, and on success pushes a
`DecodedSlotEntry` into `dSlot`.

### State

| Field | Meaning |
|---|---|
| `dSlot` | `std::optional<DecodedSlotEntry>` — the single decoded inst waiting to advance to E. |
| `flushRemaining` | Cycles remaining in a post-mispredict flush bubble. While > 0, `stepOneInstr` doesn't run. |
| `_nextInstrAddrToDeliver` | The PC the decoder is **about** to decode at. Distinct from the FIFO word address — the decoder may decode 2 16-bit Thumb insts from one fetch word, so this advances by `inst.size()` per decoded inst. |
| `_haveLoadedWord` | True iff the decoder's internal data buffer holds bytes from a previously-pulled word that hasn't been fully consumed. |
| `_lastFetchPc` | Address of the word currently in the decoder buffer. Used for `decoder.moreBytes(pc, lastFetchPc)` re-feeds. |

### `stepOneInstr(pfu, tc)` — the one-decode-per-cycle loop

```
if dSlot.has_value(): return    // back-pressure: E hasn't taken it yet

instrPc = clone(tc.pcState())
instrPc.set(_nextInstrAddrToDeliver)

if not decoder.instReady():
    if decoder.needMoreBytes() or not _haveLoadedWord:
        // pull fresh from FIFO, skipping wrong-path leftovers
        do:
            w = pfu.popFrontOfFifo()
        while w and w.addr + 4 <= _nextInstrAddrToDeliver
        if not w: return                      // FIFO empty, wait for fetch
        memcpy(decoder.moreBytesPtr(), &w.data, ≤4)
        decoder.moreBytes(instrPc, w.addr)
        _haveLoadedWord = true
        _lastFetchPc = w.addr
    else:
        // re-feed cached word for next halfword (Thumb-2 split)
        decoder.moreBytes(instrPc, _lastFetchPc)

if not decoder.instReady():
    return                                    // need more bytes, wait

staticInst = decoder.decode(instrPc)
if not staticInst: return

dSlot = DecodedSlotEntry {
    addr: _nextInstrAddrToDeliver,
    staticInst,
    pcStateAtDecode: clone(instrPc),         // post-decode state with npc set
    earlyResolved: false,
    resolvedNpc: 0,
}
_nextInstrAddrToDeliver += staticInst.size()
```

The `pcStateAtDecode` snapshot is critical for ARM: the decoder
mutates `instrPc.npc` to `addr + size` and sets ISA-specific bits
(Thumb mode, IT state). E will restore this onto the thread before
calling `staticInst->execute()` so PC-relative ALU ops and branches
read the correct PC.

### `squashDSlot()` — clear the slot on a mispredict

Drops the dSlot entry and calls `resetFetchTracking()` to
invalidate the decoder's cached word — without this, the cached
word's leftover bytes would be re-decoded against the new PC.

### `markSlotEarlyResolved(npc)` — Pipeline E→D forwarding hook

Called by `Pipeline::earlyResolveBranchAtD()` after speculative
execute. Sets `dSlot.earlyResolved = true` and
`dSlot.resolvedNpc = npc`. When this entry reaches E, Execute
takes the no-op fast path (no `execute()`, no `advancePC` — just
update `tc.pcState()` to `resolvedNpc` and bump commit counter).

### Flush bubble

`startFlushBubble(N)` sets `flushRemaining = N`. While `> 0`,
`stepOneInstr` is not called (D is idle). `decrementFlushCounter`
is called once per cycle in step 4 of `evaluate()`, and the cycle
is counted in `flushBubbleCycles`.

Used currently only on the **legacy mispredict path** (indirect
branches that escaped E→D forwarding). Default
`mispredict_flush_cycles = 3`.

---

## 7. Stage E — Execute

**Files:** [`execute.hh`](execute.hh), [`execute.cc`](execute.cc)

E commits one inst per cycle from `eSlot` to architectural state.
Holds for an `LSQ::Pending` cycle when a load/store is mid-flight.

### `runOneCycle(tc)` — the one-commit-per-cycle dispatch

The function has **four distinct fast paths**, dispatched in this
order:

```
LSQ &lsq = cpu.getLsq()

────────────────────────────────────────────────────────────────────
PATH A — LSQ-pending stall
────────────────────────────────────────────────────────────────────
if lsq.pending():
    memStallCycles++
    return MispredictInfo{false, 0}     // hold eSlot, no commit
                                        // (mem response will flip
                                        //  LSQ to Complete next cy)

────────────────────────────────────────────────────────────────────
PATH B — eSlot empty
────────────────────────────────────────────────────────────────────
if !eSlot:
    return MispredictInfo{false, 0}     // bubble, no commit

────────────────────────────────────────────────────────────────────
PATH C — Early-resolved direct branch (Pipeline did the work in
         step 2.5 — see §9)
────────────────────────────────────────────────────────────────────
e = *eSlot
if e.earlyResolved:
    eSlot.reset()
    instsCommitted++
    tc.pcState().set(e.resolvedNpc)     // PC jumps to resolved target
    return MispredictInfo{false, 0}     // no execute(), no advancePC

────────────────────────────────────────────────────────────────────
PATH D — Normal commit (non-branch, indirect branch, or memref)
────────────────────────────────────────────────────────────────────
thread.pcState(e.pcStateAtDecode)       // restore decode-time state
ctx = ExecContext(cpu, thread)

if lsq.complete():
    // resuming a memref: response just arrived
    pkt = lsq.completedPacket()
    fault = e.staticInst->completeAcc(pkt, ctx, nullptr)
    lsq.release()                       // delete pkt, LSQ → Idle
elif e.staticInst->isMemRef():
    // first touch of a memref: issue and stall
    fault = e.staticInst->initiateAcc(ctx, nullptr)
        ↳ via ExecContext::initiateMemRead/writeMem → LSQ::push…
    if lsq.pending():
        return MispredictInfo{false, 0} // eSlot stays, await response
    // (predicate failed — fall through and treat like non-mem)
else:
    // non-memory inst: full execute in 1 cy
    fault = e.staticInst->execute(ctx, nullptr)

eSlot.reset()
instsCommitted++
e.staticInst->advancePC(tc)             // pc = npc

actualNextPc = tc.pcState().instAddr()
fallThrough = e.addr + e.staticInst->size()
isIndirectMispred = e.staticInst->isControl()
                    && e.staticInst->isIndirectCtrl()
                    && actualNextPc != fallThrough
return MispredictInfo{isIndirectMispred, actualNextPc}
```

### `acceptFromDecode(e)` — D→E latch

Called from Pipeline step 3, gated by `!busy()`. Sets `eSlot = e`.

### `busy()` — back-pressure indicator

`return eSlot.has_value();`. Pipeline step 3 won't push into eSlot
while this is true (e.g. during an LSQ-pending stall).

### Why direct branches NEVER take Path D

`Pipeline::earlyResolveBranchAtD()` runs at every cycle's step 2.5
and marks every dSlot direct branch with `earlyResolved=true`.
By the time the entry reaches eSlot in the next cycle's step 3,
it's pre-resolved → Path C. This is the **M-profile E→D
forwarding mechanism** — see §9.

Indirect branches (BX/BLX with register operand) cannot be
pre-resolved because the target depends on a register the prior
inst may still be computing. They go through Path D, and if the
actual target differs from the sequential fall-through, they
trigger the legacy mispredict path in `Pipeline::evaluate()` step 2.

---

## 8. LSQ — Load/Store Queue

**Files:** [`lsq.hh`](lsq.hh), [`lsq.cc`](lsq.cc)

A **single-slot in-order LSQ**. The CPU is in-order single-issue,
so at most one outstanding memory access exists at a time.

### State machine

```
        ┌──────────┐
   ┌────┤   Idle   │◄─────────────────┐
   │    └─────┬────┘                  │
   │          │ pushReadRequest       │
   │          │ pushWriteRequest      │ release()
   │          │ (called from          │ (called from Execute
   │          │  ExecContext via      │  after completeAcc)
   │          │  initiateAcc)         │
   │          ▼                       │
   │    ┌──────────┐ completeRequest ┌┴─────────┐
   │    │ Pending  ├────────────────►│ Complete │
   │    └──────────┘ (from           └──────────┘
   │       │ ▲       DcachePort.
   │       │ │       recvTimingResp)
   │       │ │
   │       │ │ retrySend()
   │       │ │ (from DcachePort.
   │       │ │  recvReqRetry)
   │       │ │
   │  trySend│ │
   │  (refused)
   │  parks  │
   │ _needsRetry
   │  = true │
   └─────────┘
```

| Method | Caller | Effect |
|---|---|---|
| `pushReadRequest(paddr, size, flags)` | `ExecContext::initiateMemRead` (load `initiateAcc`) | Build `ReadReq` packet, allocate buffer, `trySend`. Idle → Pending. |
| `pushWriteRequest(data, paddr, size, flags)` | `ExecContext::writeMem` (store `initiateAcc`) | Build `WriteReq` packet, allocate, copy data in, `trySend`. Idle → Pending. |
| `trySend(pkt)` | internal | `dcachePort.sendTimingReq(pkt)`. On true: count `dcacheRequestsIssued`. On false: `_needsRetry = true`. State stays Pending. |
| `retrySend()` | `DcachePort::recvReqRetry` | Re-call `sendTimingReq` on the parked packet. |
| `completeRequest(pkt)` | `DcachePort::recvTimingResp` | Pending → Complete. Stash `_pkt`. Count `dcacheResponsesReceived`. |
| `release()` | `Execute` after `completeAcc()` | `delete _pkt`. Complete → Idle. |

### Memory request build

```cpp
RequestPtr req = make_shared<Request>(paddr, size, flags,
                                       cpu.dataRequestorId());
PacketPtr pkt = new Packet(req, MemCmd::ReadReq /* or WriteReq */);
pkt->allocate();   // allocates response data buffer
```

The CPU uses **physical-address-only** requests (no MMU
translation). M-profile addresses are physical (identity TLB) per
[`arch/arm/ArmMMMU.py`](../../arch/arm/ArmMMMU.py).

### Why single slot is enough

The CPU is in-order single-issue: a memref in `eSlot` blocks
Execute (Path A stall) until the response arrives. No subsequent
inst reaches E until the load/store completes, so a second outstanding
memref is impossible.

---

## 9. Branch handling — every case

Cortex-M4 has **no branch predictor**. Branch handling in this
model mirrors the hardware:

- **Direct conditional branches** (B<cond> with PC-relative target)
  resolve at **D** via E→D flag forwarding.
- **Direct unconditional branches** (B, BL with PC-relative target)
  resolve at **D** unconditionally.
- **Indirect branches** (BX/BLX with register target) resolve at
  **E** because the target depends on a register the prior inst
  may still be computing.

### 9.1 Direct branch — E→D forwarding (the common case)

`Pipeline::earlyResolveBranchAtD()` runs at every cycle's step 2.5,
**after** Execute committed (step 1) and any flag write is visible:

```
if !dSlot:                         return    // no inst at D
e = *dSlot
if e.earlyResolved:                return    // already done
si = e.staticInst
if !si->isControl():               return    // not a branch
if si->isIndirectCtrl():           return    // can't pre-execute

// ---- Speculative pre-execute against the just-committed flags ----
savedPc = clone(thread.pcState())
thread.pcState(e.pcStateAtDecode)            // restore decode-time PC
ctx = ExecContext(cpu, thread)
fault = si->execute(ctx, nullptr)            // updates npc if taken
si->advancePC(tc)                            // pc = npc
resolvedNpc = thread.pcState().instAddr()
thread.pcState(savedPc)                      // RESTORE — bne hasn't
                                              //   actually retired yet
decode.markSlotEarlyResolved(resolvedNpc)

fallThrough = e.addr + si->size()
if resolvedNpc == fallThrough:
    return                                    // not taken, no redirect
```

When the branch resolves taken, F is redirected based on the
**relative position of target vs fall-through**:

```
isForward    = (resolvedNpc > e.addr)
sameLine     = (resolvedNpc & ~0x7) == (fallThrough & ~0x7)
                                              // 8-byte Flash line
cheapRedirect = isForward && sameLine
```

#### 9.1a Full-bundle redirect (the common, expensive case)

Triggered when `cheapRedirect == false`, i.e.:

- **Backward taken branch** (BNE jumping to loop top), or
- **Forward taken branch with target on a different Flash line**.

```
pfu.staticPredictRedirect(resolvedNpc, sameLine=false):
    fifo.clear()
    _wordFifoMinAddr = resolvedNpc
    _fetchPc = resolvedNpc & ~0x3
    _nextFetchBypassBuffer = true       // arm UNCACHEABLE
    _numInFlight = 0
pfu.setRedirectFetchDelay(taken_branch_redirect_delay)   // default 2
decode.resetFetchTracking()
decode.setNextInstrAddrToDeliver(resolvedNpc)
cpu.currentStreamId++                    // drop in-flight stale
```

Cost (silicon's "TAKEN_BRANCH bundle ≈ 10 HCLK"):

- 1 cy bne pass-through at E (next cycle).
- `taken_branch_redirect_delay` cy F-issue hold-off (default 2).
- Flash WS+1 = 5 cy data phase + 1 cy address phase = **6 cy** for
  the UNCACHEABLE first fetch (no buffer hit even if the line is in
  the latch).
- Pipeline drain (decode + commit) overlaps with the fetch.

#### 9.1b Cheap forward+sameLine redirect (the optimization)

Triggered when target is **forward** AND target's 8-byte Flash line
== fall-through's line. Models silicon's
`MISPREDICT_SAME_LINE_HCLK ≈ 2 HCLK`.

```
pfu.staticPredictRedirect(resolvedNpc, sameLine=true):
    filterFifoAndRedirect(resolvedNpc)   // keep entries addr ≥ target
    _nextFetchBypassBuffer = false       // buffer hit IS correct
    _numInFlight = 0
// NOT calling setRedirectFetchDelay — no F hold-off
decode.resetFetchTracking()
decode.setNextInstrAddrToDeliver(resolvedNpc)
// NOT bumping currentStreamId — speculative in-flight contains the
// target word and IS useful (target is between fall-through and
// any later-fetched address)
```

Why this works: the branch's fall-through speculation already
issued fetches PAST the branch sequentially. The target is forward
of the branch but BEFORE those speculative fetches; if the target
word is one of them, it's already in the FIFO or in-flight on the
correct streamId, so no re-fetch is needed.

Cost: ~2 cy (just decode + execute pass-through latency).

### 9.2 Indirect branch (BX/BLX register) — legacy mispredict path

These can't be pre-executed at D (target unknown), so they go
through Execute Path D and emit `MispredictInfo{true, actualPc}`
when the actual target differs from the sequential fall-through.

```
// In Pipeline::evaluate() step 2:
if mp.happened:
    decode.squashDSlot()                                 // drop wrong-path inst at D
    decode.setNextInstrAddrToDeliver(mp.actualPc)
    pfu.filterFifoAndRedirect(mp.actualPc)               // R6 keep
    decode.startFlushBubble(mispredict_flush_cycles)     // default 3
    mispredicts++
    cpu.currentStreamId++
```

Cost: `mispredict_flush_cycles` (3 cy, models M4 P=3 worst case)
+ icache fetch latency.

### 9.3 Branch resolution decision tree

```
                  branch reaches D
                         │
              isControl()│
                         ▼
                ┌────────────────┐
                │ isIndirectCtrl │
                │       ?        │
                └───┬─────────┬──┘
                yes │         │ no
                    │         │
                    ▼         ▼
        ┌─────────────────┐  ┌──────────────────────┐
        │ Skip Pipeline   │  │ earlyResolveBranchAtD│
        │ step 2.5;       │  │ (speculative execute)│
        │ resolve at E.   │  └─────────┬────────────┘
        │ If taken:       │            │
        │ legacy          │            ▼
        │ mispredict      │    ┌────────────────────┐
        │ (MispredictInfo │    │ resolvedNpc ==     │
        │  in step 2 next │    │ fallThrough ?      │
        │  cycle).        │    └───┬────────┬───────┘
        │ Cost: 3-cy      │     yes│        │ no (taken)
        │ flushBubble +   │        │        │
        │ icache miss.    │        ▼        ▼
        └─────────────────┘   ┌────────┐  ┌────────────────────┐
                              │ no     │  │ isForward && same- │
                              │redirect│  │ Line ?             │
                              │(0 cy)  │  └─────┬────────────┬─┘
                              └────────┘    yes │            │ no
                                                ▼            ▼
                                       ┌──────────────┐ ┌─────────────┐
                                       │ cheap-fwd-   │ │ full-bundle │
                                       │ sameline     │ │ redirect    │
                                       │ ─ ~2 cy      │ │ ─ ~10 cy    │
                                       │ ─ keep FIFO  │ │ ─ clear FIFO│
                                       │ ─ no streamId│ │ ─ ++stream  │
                                       │ ─ no UNCACH  │ │ ─ UNCACH    │
                                       │ ─ no delay   │ │ ─ delay = 2 │
                                       └──────────────┘ └─────────────┘
```

### 9.4 Steady-state per-iteration cycle count

For a single-fetch tight loop (`subs r0,#1; bne loop` in the same
32-bit Flash word — bench-branch's pattern):

```
T:    SUBS commits at E.    BNE early-resolves at D
                            (full-bundle redirect, taken backward).
                            F redirect, UNCACHEABLE, delay=2.
T+1:  BNE retires at E (no-op pass-through, sets pc=target).
      F idle (delay=2 → 1).
T+2:  F idle (delay=1 → 0).
T+3:  F issues UNCACHEABLE fetch of word @ target.
T+8:  Response arrives (1 addr + 5 WS data = 6 cy).
T+9:  Drain at top of cycle. D decodes SUBS. dSlot=SUBS.
T+10: D→E SUBS. D decodes BNE (re-feed cached word). dSlot=BNE.
T+11: SUBS commits. BNE early-resolves. — start of next iter.
```

**Per iter = 10 HCLK.** Matches Cortex-M4 silicon `bench-branch`
exactly (1013 cycles for 100 iters + setup).

---

## 10. Memory subsystem — AHB / Flash modelling

### What's modelled in the CPU

Only the **interaction protocol** with the bus and Flash:

- `Request` flag `INST_FETCH` is set on every icache request.
- `Request::UNCACHEABLE` is set on the first fetch after a
  full-bundle taken-branch redirect — this asks the memory model
  to skip its buffer-hit shortcut and pay full WS latency
  (silicon's TAKEN_BRANCH bundle).
- `Request::setStreamId(currentStreamId)` tags every fetch, and
  `IcachePort::recvTimingResp` drops responses whose streamId
  doesn't match the current — this models the M4 PFU dropping
  wrong-path data after a redirect.
- `IcachePort::recvReqRetry` re-issues the parked
  `pendingRetryPacket` (gem5 timing-request retry contract).

### What's modelled in the memory (PipelinedSimpleMemory)

The CPU connects `icache_port` to a `PipelinedSimpleMemory` (gem5
prebuilt in `gem5/src/mem/`) configured per
`STM32G474REPlatform.default_memories(enable_art=False)`:

| Knob | Value | Models |
|---|---|---|
| `latency` | 29412 ps (≈5 HCLK at 170 MHz) | Flash data phase = WS+1 = 5 HCLK |
| `address_phase_latency` | 1 ns (≈1 HCLK) | AHB address phase |
| `port_read_buffer_size` | `[8, 8]` (2 ports) | 64-bit current-line latch (Flash sense-amp output, always present even with ART off — see RM0440 §4.3.4) |
| `buffer_hit_latency` | 0 ns (default) | Hit returns next cycle (1 cy total inc. address phase) |
| `port_priority` | `[0, 1]` | ICode low pri, DCode high pri |

A change made in this work to
[`mem/pipelined_simple_mem.cc`](../../mem/pipelined_simple_mem.cc):
the buffer-hit shortcut now checks `pkt->req->isUncacheable()` and
**bypasses the buffer** if set, forcing the full Flash WS latency.
This is the mechanism by which the CPU tells the memory "treat
this fetch as a cold miss."

### Per-fetch latency table

| Scenario | Cycles | Notes |
|---|---:|---|
| Buffer hit, `UNCACHEABLE=0` | **1** | 1 cy address phase + buffer transfer (latency=0). |
| Buffer miss, `UNCACHEABLE=0` (or set) | **6** | 1 cy address + 5 cy data (4 WS + 1). |
| Pipelined sequential miss, throughput | **5/word** | Address phase of fetch N+1 overlaps last data cy of N. |
| First fetch after taken-branch redirect | **6** | Always charged as miss (UNCACHEABLE), even on buffer hit. Models silicon TAKEN_BRANCH bundle. |

---

## 11. Latency table

All latencies in **HCLK** (CPU clock cycles). Default frequency
170 MHz → 5882 ticks/cycle.

| Event | Latency | Source / configurable |
|---|---:|---|
| Decode (one Thumb-16 inst) | 1 cy | Pipeline architecture |
| Decode (one Thumb-32 inst, both halfwords in same fetch word) | 1 cy | Pipeline (single decode step) |
| Decode (Thumb-32 straddling a fetch-word boundary) | 2 cy + extra fetch | Decoder requires 2 `moreBytes` calls |
| Execute (non-mem ALU) | 1 cy | Pipeline architecture |
| Execute (early-resolved direct branch — "no-op pass-through") | 1 cy | Path C, no `execute()` call |
| Execute (load/store, response on same cycle as issue) | ≥1 cy stall | LSQ Pending → Complete on `recvTimingResp` |
| Pipeline refill after taken direct branch (full-bundle) | 1 (BNE pass-through) + `taken_branch_redirect_delay` (=2) + 6 (UNCACHEABLE fetch) ≈ **~10 HCLK** | `taken_branch_redirect_delay` param |
| Pipeline refill after taken cheap-fwd-sameline | ~2 HCLK | No delay, no UNCACHEABLE, FIFO hit |
| Pipeline refill after indirect-branch mispredict | `mispredict_flush_cycles` (=3) + icache fetch latency | `mispredict_flush_cycles` param |
| ICache fetch — buffer hit | 1 cy | PipelinedSimpleMemory |
| ICache fetch — buffer miss | 6 cy | PipelinedSimpleMemory (1 addr + 5 data) |
| ICache fetch — pipelined sequential throughput | 5 cy/word | PipelinedSimpleMemory |
| DCache load/store — depends on memory backing | 1 cy (SRAM hit) to N cy (Flash miss) | Connected memory configuration |
| Halt detection cycle | 1 cy | Pipeline step 6 |

---

## 12. Parameter reference

All parameters live on
[`Simple3CycleCPU.py`](Simple3CycleCPU.py) and are accessible via
`cpu.params()` in C++.

### Architectural

| Param | Type | Default | Purpose |
|---|---|---:|---|
| `mispredict_flush_cycles` | Cycles | 3 | Pipeline-refill bubble after a *legacy* mispredict (indirect branch only). M4 TRM Table 3-1 footnote c, P ∈ {1,2,3}. |
| `taken_branch_redirect_delay` | Cycles | 2 | F-issue hold-off after E→D-resolved taken branch. Models AHB address-phase setup + next-HCLK forwarding latency. |
| `pfu_fifo_words` | Unsigned | 3 | PFU prefetch FIFO depth (matches Cortex-M4 PFU = 3 32-bit words). |
| `halt_addr` | Addr | 0 | When `tc.pcState().instAddr() == halt_addr` and pipeline is drained, CPU stops and exits sim loop. 0 = disabled (halts after one cycle). |

### Test-mode hooks (see §16)

| Param | Type | Default | Purpose |
|---|---|---:|---|
| `phase_b_num_words` | Unsigned | 0 | Hand-populate FIFO at startup (no icache traffic). |
| `phase_b_word_value` | UInt32 | 0 | Each FIFO word's value. |
| `phase_b_start_pc` | Addr | 0 | First FIFO word's address. |
| `phase_c_num_words` | Unsigned | 0 | Write N copies of `phase_c_word_value` to memory at startup. |
| `phase_c_word_value` | UInt32 | 0 | Word value to write. |
| `phase_c_start_pc` | Addr | 0 | Memory base for the write + thread entry PC. |
| `phase_d_words` | VectorParam.UInt32 | `[]` | Vector of 32-bit words to write at startup (each can differ). |
| `phase_d_start_pc` | Addr | 0 | Memory base for writes. |
| `phase_d_entry_pc` | Addr | 0 | Optional override for the architectural entry PC (when kernel starts mid-word). |

---

## 13. Statistics

All under `system.cpu.*` in `m5out/stats.txt`. Defined in
[`stats.hh`](stats.hh) / [`stats.cc`](stats.cc).

| Stat | Increment site | Meaning |
|---|---|---|
| `instsCommitted` | `Execute::runOneCycle` (Paths C/D) | Number of insts the E stage retired. |
| `mispredicts` | `Pipeline::evaluate` step 2 | Indirect-branch mispredicts (legacy path). E→D forwarding for direct branches is exact, so this should be 0 for correctness verification. |
| `requestsIssued` | `Pipeline::trySendFetchPacket` (on success) | Icache requests sent over `icache_port`. |
| `responsesReceived` | `IcachePort::recvTimingResp` | Icache responses accepted (all responses, both matching and stale). |
| `staleResponsesDropped` | `IcachePort::recvTimingResp` (mismatch) | Icache responses dropped because their `streamId` didn't match `currentStreamId`. |
| `flushBubbleCycles` | `Pipeline::evaluate` step 4 (post-decrement check) | Cycles D was inhibited by `flushRemaining > 0`. |
| `dcacheRequestsIssued` | `LSQ::trySend` / `LSQ::retrySend` (on success) | Dcache requests sent over `dcache_port`. |
| `dcacheResponsesReceived` | `LSQ::completeRequest` | Dcache responses accepted. |
| `memStallCycles` | `Execute::runOneCycle` Path A | Cycles E was stalled waiting for an LSQ response. |
| `flashHits` / `flashMisses` | (informational — currently unused; kept for symmetry with prototype) | — |

---

## 14. Debug flags

Registered in [`SConscript`](SConscript). Use any combination via
`--debug-flags=A,B,C` on the gem5 command line.

| Flag | What it traces |
|---|---|
| `Simple3CPU` | Top-level events (`startup`, `activateContext`, `wakeup`, halt) |
| `Simple3CPULineTrace` | One line per cycle (currently just `cyc=N`; expand for full pipeline state) |
| `Simple3CPUFetch` | (reserved) |
| `Simple3CPUResp` | `IcachePort::recvTimingResp` accepts and stale drops, `recvReqRetry` |
| `Simple3CPUPFU` | `PFU::buildFetchPacket` (addr, streamId, uncacheable flag) |
| `Simple3CPUDecode` | `Decode::stepOneInstr` — feed-fresh-word, reuse-cached-word, decoded-inst |
| `Simple3CPUExecute` | `Execute::runOneCycle` — commits, early-resolve events, redirect kind, mem stall |
| `Simple3CPUFlash` | (reserved — informational counter) |
| `Simple3CPULSQ` | `LSQ::push…`, `complete`, `retry`, `release` |
| `Simple3CPUAll` | Compound: enables ALL of the above |

A representative trace with `--debug-flags=Simple3CPUExecute,Simple3CPUResp`
on `bench-branch`:

```
  41174: stage response addr=0x80074d0 data=0xbf00bf00 streamId=1
  52938: commit pc=0x80074d2 size=2 nextPc=0x80074d4 indirectMispred=0
  …
 158814: commit pc=0x80074e4 size=2 nextPc=0x80074e6 indirectMispred=0
 158814: early-resolve at D: pc=0x80074e6 size=2 resolvedNpc=0x80074e4
                              fallThrough=0x80074e8 taken=1
 158814:   redirect kind=full-bundle (forward=0 sameLine=0)
 164696: commit (early-resolved) pc=0x80074e6 size=2 nextPc=0x80074e4
```

---

## 15. Halt and exit

The CPU exits cleanly when:

1. `halt_addr != 0` (configured).
2. `tc.pcState().instAddr() == halt_addr` (architectural PC reached
   the halt point; this is updated by Execute Paths C/D each
   commit).
3. `!execute.busy() && !decode.busy()` (pipeline drained — no
   stalled memref, no pending decode).

When all three hold:

```cpp
DPRINTF(Simple3CPU, "halt: pc=%#x reached, ...\n", haltAddr);
stop();                                                  // Ticked: stop scheduling
exitSimLoop("Simple3CycleCPU reached haltAddr", 0);      // sim_exit.hh: exit code 0
```

Without `exitSimLoop`, `m5.simulate()` would run to MaxTick
draining stale icache responses (that have nothing more to deliver
into a halted pipeline). The exit makes Python's `m5.curTick()`
return the actual halt cycle.

If `halt_addr == 0`, the CPU stops and exits after one
`evaluate()` cycle (vacuous test).

---

## 16. Test-mode hooks

Three independent paths in `Simple3CycleCPU::startup()` for
synthetic testing — they're orthogonal but at most one is typically
used at a time.

### Phase B — pre-populate the FIFO directly

Bypasses the icache port entirely. `Pipeline::prepopulateFifoForPhaseB`
seeds N copies of one word value at sequential addresses into the
PFU FIFO and resets the thread PC. Used to test decode + execute
without involving the memory model.

### Phase C — write one word value N times

Issues `system->getSystemPort().sendFunctional(WriteReq pkt)` for
each word, writing `phase_c_num_words` copies of `phase_c_word_value`
at sequential addresses starting at `phase_c_start_pc`. Then resets
PFU and thread PC. The CPU then fetches normally over `icache_port`.

### Phase D — write a vector of distinct words (a hand-coded kernel)

Same mechanism as Phase C but `phase_d_words` is a vector — each
entry is its own word. This is how the BP benchmark suite is
loaded into Flash for comparing cycle counts to silicon.
`phase_d_entry_pc` optionally overrides the architectural entry PC
(useful when the kernel's first inst is a 16-bit Thumb at
`addr+2` of an 8-byte-aligned line).

---

## 17. Known limitations

These are all documented for future work; none affect functional
correctness on the current test suite.

1. **`Pipeline::dumpLineTrace` is a stub.** Per-cycle pipeline
   state ASCII trace (mirroring the prototype's `_iteration` trace
   in `prototype/stm32g4_no_art.py`) is not yet implemented. The
   debug flag `Simple3CPULineTrace` only emits `cyc=N`.

2. **`pendingRetryPacket` not invalidated on stream change.** If
   `sendTimingReq` returns false right before a redirect, the
   parked retry packet has the old streamId. When `recvReqRetry`
   fires after the redirect, we resend it; the response arrives
   stream-mismatched and is dropped, but a bus cycle was wasted.
   Documented since Phase C; impact on cycle counts is negligible
   for current tests.

3. **`flashHits` / `flashMisses` stats are unused.** Reserved for
   future Flash-side instrumentation.

4. **Indirect-branch mispredict path is untested.** No current test
   exercises BX/BLX with a register-computed target. The legacy
   mispredict path in `Pipeline::evaluate()` step 2 is wired but
   has no regression coverage.

5. **`mispredict_flush_cycles = 3` is empirical** (M4 TRM P=1..3
   worst case). Could in principle vary with target alignment or
   "speculate the address early" condition per TRM, but the M4 TRM
   doesn't decompose this so we use the worst case uniformly.

6. **`taken_branch_redirect_delay = 2` is also empirical.** Tuned
   to match silicon's `bench-branch` exactly. Plausible
   architectural sources: (a) next-HCLK rather than same-HCLK
   forwarding latency, (b) 1-HCLK AHB address-phase setup before
   the new ICode HTRANS=NONSEQ can be driven. The M4 TRM doesn't
   isolate these.

7. **Cycle-count error vs silicon for multi-word loop bodies.**
   `align` (+13.9% over silicon) and `forward` (+10.9%) overshoot
   silicon by an amount the Python reference model also overshoots
   by — a silicon optimization neither model captures. See
   [PROGRESS.md](PROGRESS.md) "Phase D.5 v4" section for full
   analysis.

8. **No support for multi-thread, no SMT, no checkpointing.** All
   are out of scope for a Cortex-M4 model; would need rework if
   ever extended.

9. **Faults from `staticInst->execute()` panic the simulator.**
   No fault-injection or exception-vector handling. M-profile
   exception entry (NVIC) is not modelled. Documented as Phase E
   future work.

---

## Appendix — File index

| File | Purpose |
|---|---|
| `Simple3CycleCPU.py` | SimObject params + Python-side definition |
| `simple_3cycle_cpu.{hh,cc}` | `BaseCPU` subclass; owns Pipeline, PFU, Decode, Execute, LSQ, IcachePort, DcachePort. `startup()` runs the test-mode hooks. |
| `pipeline.{hh,cc}` | `Ticked` subclass. `evaluate()` is the per-cycle dispatcher. Owns the E→D forwarding (`earlyResolveBranchAtD`) and fetch-issue retry logic. |
| `pfu.{hh,cc}` | F stage. FIFO, fetchPc, redirect modes, response staging. |
| `decode.{hh,cc}` | D stage. Decoder driver, `dSlot`, flush bubble, fetch tracking. |
| `execute.{hh,cc}` | E stage. `runOneCycle` four-path dispatch. |
| `exec_context.hh` | Header-only `gem5::ExecContext` impl wrapping `SimpleThread`. Routes mem ops through the LSQ. |
| `lsq.{hh,cc}` | Single-slot LSQ. Idle/Pending/Complete state machine. |
| `types.hh` | `FetchWord`, `DecodedSlotEntry`, `MispredictInfo`, `BackwardRedirect`. |
| `stats.{hh,cc}` | Per-CPU stats group. |
| `SConscript` | Build registration + DebugFlag definitions. |
| `PORT_PLAN.md` | Original design intent. |
| `PROGRESS.md` | Phase-by-phase build log + benchmark verification table. |
| `ARCHITECTURE.md` | This document. |
