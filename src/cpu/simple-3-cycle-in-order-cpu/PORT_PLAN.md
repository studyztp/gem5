# Porting the Cortex-M4 Python Model to a gem5 CPU

Plan for translating
[prototype/stm32g4_pipeline_model.py](../../../../prototype/stm32g4_pipeline_model.py)
(design spec:
[prototype/STM32G4_PIPELINE_MODEL.md](../../../../prototype/STM32G4_PIPELINE_MODEL.md))
into a gem5 `BaseCPU` model living in this directory.

Scope of this document: **CPU only**. The Flash / AHB bus is modelled
inside the CPU's fetch unit initially (matching the Python prototype);
a later refactor can replace it with a proper external SimObject without
touching the pipeline logic — §8 sketches the seam.

---

## 1. Why a new CPU and what it costs

The Python model is a single-threaded, in-order, 3-stage (F / D / E)
pipeline whose cycle count is dominated by a single Flash resource, a
64-bit buffer latch, a 3-word PFU FIFO, static branch prediction at D,
and a mispredict bubble at E. It does not have caches, virtual memory,
speculation, OoO, or multi-threading.

Mapping that onto gem5:

- `MinorCPU` is closest in shape (in-order, `Latch`-connected stages,
  `Ticked` base class) but is 4-stage and carries a lot of machinery we
  don't need (LSQ, func-unit model, scoreboard). It's a **reference**
  for how to structure a timed in-order CPU in gem5, not a base class.
- `BaseSimpleCPU` is single-stage (one instruction per cycle) — too
  coarse to reproduce the per-cycle PFU / Flash interaction.
- The existing `LegoCPU` in [../lego/](../lego/) is a reactive
  port-based framework; we can reuse its communication pattern later,
  but for a first cut we follow the `MinorCPU` template so we stay on
  the documented gem5 idioms.

Cost estimate: ~1500-2000 LOC of C++ across ~10 files, plus a
Python `BaseXxxCPU.py` param file. Small because the model is small.

---

## 2. Mental model: ticks vs cycles

This is the single most important design decision. gem5 uses **ticks**
(1 ps by default), not cycles. One CPU cycle =
`clockPeriod() = cyclesToTicks(1)` ticks. A 170 MHz clock is therefore
~5882 ticks/cycle.

The Python model's `tick()` collapses six sub-operations into one
atomic cycle step:

```
Python tick():
  1. Execute            (E fires; may set mispredict flags)
  2. Mispredict squash  (before D→E)
  3. D → E latch
  4. FIFO → D latch
  5. F service          (decrement Flash busy, deliver on completion)
  6. F start new        (if free + not flushing + FIFO has room)
```

In gem5 we must reproduce the *effect* of this ordering while living in
a world where:

- `recvTimingResp()` from the icache can fire at any tick, not just
  cycle boundaries.
- A branch resolved in E must cause D's FIFO-pop choice to change *in
  the same cycle* — which in C++ terms means the mispredict handler
  must run before the FIFO-pop function within the same `process()`
  call.

Chosen approach: **single scheduled event per cycle, sub-tick ordering
inside that event.** One `Ticked`-style cycle event runs every
`clockPeriod()` ticks; inside it we call the six sub-operations in the
same fixed order as Python. Async events that arrive mid-cycle (Flash
response, retry signal) enqueue into staging buffers that the next
cycle event drains.

```
                    tick T                  tick T + clockPeriod
                      |                             |
                      v                             v
    cycleEvent:  [1][2][3][4][5][6]              [1][2][3][4][5][6]
                                  |
        recvTimingResp at tick ~~>| stashes response in "arrived
                                     this cycle" bucket; cycleEvent
                                     at T+clockPeriod consumes it in
                                     step 5
```

Rationale — alternatives considered:

| Alternative | Why rejected |
|-------------|--------------|
| Six separate events at tick `T`, `T+1`, `T+2`, ... within one cycle | The 1-tick offsets have no physical meaning and complicate async response handling; also fragile under cycle-period changes. |
| One event per stage (3 cycle-period events staggered by a third of a cycle) | Pipeline ordering becomes cycle-period-dependent and async memory responses race against them. |
| gem5 `TickedObject` base with `evaluate()` once per cycle | This *is* the chosen approach, with `evaluate()` doing the six sub-ops in order. Listed separately so it's explicit. |

---

## 3. Directory layout

```
gem5/src/cpu/simple-3-cycle-in-order-cpu/
├── PORT_PLAN.md                  ← this file
├── SConscript                    ← build registration
├── Simple3CycleCPU.py            ← params + Python SimObject
│
├── simple_3cycle_cpu.{hh,cc}     ← top-level BaseCPU; owns stages + ports
├── pipeline.{hh,cc}              ← owns the cycleEvent; drives stages
│
├── types.hh                      ← plain structs: FetchWord, DecodedInstr, …
├── flash_model.{hh,cc}           ← Flash + AHB-Lite buffer-latch model
├── pfu.{hh,cc}                   ← F stage: fetch unit + FIFO
├── decode.{hh,cc}                ← D stage: Thumb decode + static predict
├── execute.{hh,cc}               ← E stage: ALU + branch resolve
├── arch_state.{hh,cc}            ← r0/r1/Z mirror; will be replaced by
│                                   SimpleThread/ThreadContext eventually
└── stats.{hh,cc}                 ← per-stage counters
```

A small directory. Each file < 300 lines. All in `namespace gem5 {
namespace simple3 { ... }}`.

---

## 4. Class hierarchy

```
BaseCPU  (gem5 core)
└── Simple3CycleCPU                      (simple_3cycle_cpu.*)
     ├── owns: SimpleThread[]           (one thread, from BaseCPU)
     ├── owns: IcachePort                ← RequestPort (AHB master side)
     ├── owns: Pipeline                  (pipeline.*)
     │    ├── owns: FlashModel           (flash_model.*) — inside CPU
     │    ├── owns: PFU         (F stage, pfu.*)
     │    ├── owns: Decode      (D stage, decode.*)
     │    └── owns: Execute     (E stage, execute.*)
     │    └── schedules: cycleEvent (EventFunctionWrapper)
     └── getInstPort() → IcachePort
```

`Pipeline` owns the single event that fires every `clockPeriod()`
ticks. It holds non-owning pointers back to the stages so the
`process()` method can dispatch in order.

---

## 5. State-variable map (Python → C++ member)

This is the one-to-one translation table. Everything on the right-hand
side lives in the stage that owns it (or in `Pipeline` if shared).

| Python variable          | C++ location                                    | Type                                |
|--------------------------|-------------------------------------------------|-------------------------------------|
| `buffer_line`            | `FlashModel::bufferLine`                        | `Addr`                              |
| `flash_busy`             | `FlashModel::busyCyclesRemaining`               | `Cycles`                            |
| `flash_word`             | `FlashModel::inFlightWord`                      | `Addr` (0 = none)                   |
| `flash_discard`          | `FlashModel::discardInFlight`                   | `bool`                              |
| `fetch_pc`               | `PFU::fetchPc`                                  | `Addr`                              |
| `pfu_next_addr`          | `PFU::nextAddrToDeliver`                        | `Addr`                              |
| `fifo`                   | `PFU::fifo`                                     | `std::deque<DecodedInstr>`          |
| `pending_upper`          | `PFU::pendingUpper`                             | `std::optional<PendingUpper>`       |
| `d_slot`                 | `Decode::dSlot`                                 | `std::optional<DecodedInstr>`       |
| `e_slot`                 | `Execute::eSlot`                                | `std::optional<DecodedInstr>`       |
| `flush_remaining`        | `Decode::flushRemaining`                        | `Cycles`                            |
| `arch_pc`                | `thread->pcState()` (existing gem5 ThreadContext) | —                                 |
| `arch.r0/r1/z`           | `thread->getIntReg(...)` / CPSR bits            | —                                 |

Note the arch state is NOT duplicated — we use gem5's `SimpleThread` so
we get PC, register file, CPSR flags, and ISA decode for free. The
`Execute` stage calls `StaticInstPtr::execute()` with an
`ExecContext`, same idiom as `MinorCPU`.

---

## 6. The cycle event — authoritative C++ skeleton

The whole model hinges on this function. It is the gem5 equivalent of
Python `tick()`.

```cpp
// pipeline.cc

void
Pipeline::process()
{
    ++cycle;                                     // for stats only

    // ---- 1. Execute ----
    MispredictInfo mp = execute.runOneCycle(thread);

    // ---- 2. Mispredict: squash D BEFORE D→E ----
    if (mp.happened) {
        decode.squashDSlot();
        pfu.filterFifoAndRedirect(mp.actualPc);  // keeps addr >= target
        decode.startFlushBubble(MISPREDICT_FLUSH_CYCLES);
        flash.retractInFlight();                 // discard=true, not cancel
    }

    // ---- 3. D → E latch ----
    if (decode.hasInstrReadyForExecute()) {
        execute.acceptFromDecode(decode.popForExecute());
    }

    // ---- 4. FIFO → D latch (skipped during flush bubble) ----
    if (!decode.isFlushing()) {
        auto maybe_instr = pfu.popFrontOfFifo();
        if (maybe_instr) {
            auto redirect = decode.acceptFromPfu(*maybe_instr);
            if (redirect)                        // backward → predict taken
                pfu.staticPredictRedirect(redirect->targetPc);
        }
    }

    // ---- 5. Flash service: tick down, deliver on completion ----
    auto delivered = flash.advanceOneCycle();    // returns the completed
    if (delivered)                               //   word, or nullopt
        pfu.deliverWord(*delivered);

    // ---- 6. F: start a new Flash transfer if possible ----
    if (flash.isFree() && !decode.isFlushing() && pfu.fifoHasRoom()) {
        flash.issueFetch(pfu.fetchPcAndAdvance());
    }

    decode.decrementFlushCounter();              // after step 4 gate

    // Re-arm for next cycle
    cpu.schedule(cycleEvent, clockEdge(Cycles(1)));
}
```

A one-to-one port of the Python algorithm. Each helper in the six
sub-steps is a short method on the stage it belongs to.

---

## 7. Stage-by-stage breakdown

### 7.1 `Execute` (execute.hh/cc)

Single public API consumed by step 1 of the cycle:

```cpp
MispredictInfo runOneCycle(SimpleThread *thread);
```

Internally:

1. If `eSlot` is empty, return `{happened=false}`.
2. Decode-time branch target is already on the instr (written by
   `Decode`). Use gem5's `StaticInstPtr::execute()` through a local
   `ExecContext` to apply the effect and get the actual next PC.
3. If the instr is a conditional branch, compare actual direction vs
   predicted (carried on `DecodedInstr`). Build and return
   `MispredictInfo` accordingly.
4. Commit to `thread->pcState()`; bump retired-instruction counter for
   stats.
5. Clear `eSlot`.

### 7.2 `Decode` (decode.hh/cc)

Three public APIs:

```cpp
bool hasInstrReadyForExecute() const;
DecodedInstr popForExecute();
std::optional<BackwardRedirect> acceptFromPfu(const DecodedInstr &in);
void squashDSlot();
void startFlushBubble(Cycles n);
void decrementFlushCounter();
bool isFlushing() const;
```

The static-predict-at-D logic is a single conditional inside
`acceptFromPfu`: if the instr is a conditional branch with `target <=
addr`, annotate it as `predictTaken=true` AND emit a `BackwardRedirect`
so the pipeline can steer the PFU in step 4.

### 7.3 `PFU` (pfu.hh/cc)

This is the F stage *and* the FIFO owner. The FIFO is a
`std::deque<DecodedInstr>`. Why `DecodedInstr` and not raw bytes:
Thumb-2 decode is cheap and stateless; doing it at FIFO-push time
simplifies the `pfu_next_addr` filter (we're comparing instruction
addresses) and matches the Python model.

Key APIs:

```cpp
// produce the word we're about to request from Flash, update fetchPc
Addr fetchPcAndAdvance();

// called by Pipeline step 5 with a Flash-completed word
void deliverWord(const FetchWord &word);

// called by Pipeline step 4 when D pops a backward cond branch
void staticPredictRedirect(Addr target);

// called by Pipeline step 2 on mispredict — the ONE non-obvious API
void filterFifoAndRedirect(Addr actualPc);

bool fifoHasRoom() const;                // len(fifo) < PFU_FIFO_WORDS*2
std::optional<DecodedInstr> popFrontOfFifo();
```

`filterFifoAndRedirect` is the §5 R6 rule and is the whole reason the
model gets `bp_alternating` right:

```cpp
void
PFU::filterFifoAndRedirect(Addr actualPc)
{
    // Keep entries already delivered from AHB that happen to be on
    // the correct path (addr >= actualPc).  Wrong-path entries are
    // dropped.  Reset pendingUpper — any in-flight upper halfword is
    // speculative.
    std::deque<DecodedInstr> kept;
    for (auto &i : fifo) if (i.addr >= actualPc) kept.push_back(i);
    fifo = std::move(kept);
    pendingUpper.reset();

    if (!fifo.empty()) {
        const auto &last = fifo.back();
        nextAddrToDeliver = last.addr + last.size;
    } else {
        nextAddrToDeliver = actualPc;
    }
    fetchPc = nextAddrToDeliver;
}
```

### 7.4 `FlashModel` (flash_model.hh/cc)

The CPU-local model of Flash + 64-bit AHB buffer latch. Keeps the
Python semantics: on completion, buffer updates even if we marked the
fetch as discarded; in-flight fetches cannot be cancelled mid-access —
they tick down and release the bus.

Public APIs:

```cpp
bool isFree() const;                     // busyCyclesRemaining == 0
void issueFetch(Addr word);              // sets busy = hit?1:WS+1
void retractInFlight();                  // discardInFlight = true
std::optional<FetchWord> advanceOneCycle();   // decrement; on complete,
                                             //   update bufferLine,
                                             //   return word iff !discard
```

Parameters (`Simple3CycleCPU.py` exposes these):

```python
fetch_hit_cycles  = Param.Cycles(1)
fetch_miss_cycles = Param.Cycles(5)      # WS+1 at 170 MHz, WS=4
```

Note we pass `Cycles` not raw ticks — gem5 converts through
`clockPeriod()` when we schedule.

### 7.5 `Simple3CycleCPU` (simple_3cycle_cpu.hh/cc)

The gem5-facing BaseCPU subclass. Responsibilities:

- Own `SimpleThread[]`, `Pipeline`, `IcachePort`.
- Implement `BaseCPU` pure-virtuals: `totalInsts`, `totalOps`,
  `activateContext`, `suspendContext`, `wakeup`, `getInstPort`,
  `getDataPort`.
- Define the `IcachePort` inner class (see §8).
- In `startup()`, schedule the first `cycleEvent` at `clockEdge(Cycles(1))`.

Mostly boilerplate; compare [../lego/lego_cpu.hh](../lego/lego_cpu.hh)
for the same skeleton.

---

## 8. Memory port — two phases

### Phase 1 (initial implementation): Flash modelled inside the CPU

The `FlashModel` in §7.4 does **not** use `IcachePort.sendTimingReq()`.
It's a pure cycle-counting model sitting inside the CPU. The
`IcachePort` in `Simple3CycleCPU` is declared (required by
`BaseCPU::getInstPort()` contract) but stubbed — `sendTimingReq()` is
never called, and `recvTimingResp()` / `recvReqRetry()` fail loudly if
ever invoked.

This phase reproduces the Python model exactly: predictable, matches
the measured cycle counts on the 7 benchmarks within 0.5%–8% per the
numbers in [STM32G4_PIPELINE_MODEL.md](../../../../prototype/STM32G4_PIPELINE_MODEL.md).

### Phase 2 (later refactor): Real external Flash SimObject

Move `FlashModel` out of the CPU and make it a sibling SimObject on
`system.mem_ranges`:

- CPU's `PFU` calls `IcachePort.sendTimingReq()` with the word address.
- `FlashModel` is a `SimpleMemobj`-style slave that completes the
  request after `hit?1:miss` cycles.
- On `recvTimingResp()`, the response goes into a staging buffer and
  the next `cycleEvent` hands it to `PFU::deliverWord()`.
- Retraction (the R3 rule) is modelled by the CPU suppressing the
  response when it arrives if its seqNum was marked stale.

Why this matters for planning: the `PFU` interface in §7.3 (`issueFetch`,
`advanceOneCycle`) stays the same between Phase 1 and Phase 2 — only
its implementation swaps. Keep the interface clean now so Phase 2 is a
local change.

---

## 9. Handling gem5-isms

### 9.1 `recvTimingResp()` at an arbitrary tick (Phase 2 preview)

Memory responses arrive at the tick the memory system decides. They
must not mutate pipeline state directly because the cycle event might
be mid-execution or not yet scheduled.

Pattern:

```cpp
bool
Simple3CycleCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    cpu.pipeline.stageResponse(pkt);             // push onto bucket
    // Ensure cycleEvent is scheduled; it drains the bucket.
    if (!cpu.pipeline.cycleEvent.scheduled())
        cpu.schedule(cpu.pipeline.cycleEvent,
                     cpu.clockEdge(Cycles(0)));  // next cycle edge
    return true;
}
```

`stageResponse()` pushes into a std::vector. `Pipeline::process()`
step 5 pops and feeds each one to `PFU::deliverWord()` in order.
**The response is never applied before the cycle event** — that
guarantees the Python model's ordering.

### 9.2 Halting / draining

gem5 workloads end when the ISA says to. For `BaseCPU`:

- `activateContext(tid)` schedules the first `cycleEvent`.
- `suspendContext(tid)` cancels `cycleEvent` if scheduled.
- Drain/checkpoint: implement `Drainable` later; for the first cut we
  only support starting from fresh state.

### 9.3 Warm-up / pipeline pre-fill

The Python model leaves a ~3-cycle undershoot vs measured because the
real pipeline is already full when DWT CYCCNT is enabled. For the gem5
port we have two options:

1. **Do nothing.** Accept the constant 3-cycle offset on matching
   benchmarks.
2. **Prefill on `startup()`.** At startup, synthesise a 3-cycle
   "spin-up" so that at the first `cycleEvent` the E slot is already
   filled. This matches the measured values exactly.

Option 1 for the first cut. Option 2 is trivial to add later if we
decide reporting exactly-measured numbers matters.

---

## 10. Python-side configuration

`Simple3CycleCPU.py` — mirrors `BaseMinorCPU.py`:

```python
class Simple3CycleCPU(BaseCPU):
    type = 'Simple3CycleCPU'
    cxx_class = 'gem5::Simple3CycleCPU'
    cxx_header = 'cpu/simple-3-cycle-in-order-cpu/simple_3cycle_cpu.hh'

    @classmethod
    def memory_mode(cls):
        return 'timing'

    fetch_hit_cycles  = Param.Cycles(1, "Flash AHB buffer-line hit")
    fetch_miss_cycles = Param.Cycles(5, "Flash new-line miss (WS+1)")
    mispredict_flush_cycles = Param.Cycles(
        3, "Pipeline refill after mispredict (Cortex-M4 TRM P=1..3)")
    pfu_fifo_words = Param.Unsigned(3, "PFU prefetch FIFO depth")
```

All four knobs match the Python model's module-level constants.

---

## 11. Build registration

`SConscript`:

```python
Import('*')

SimObject('Simple3CycleCPU.py', sim_objects=['Simple3CycleCPU'])

Source('simple_3cycle_cpu.cc')
Source('pipeline.cc')
Source('flash_model.cc')
Source('pfu.cc')
Source('decode.cc')
Source('execute.cc')
Source('arch_state.cc')
Source('stats.cc')

DebugFlag('Simple3CPU')
DebugFlag('Simple3CPULineTrace')
DebugFlag('Simple3CPUFlash')
DebugFlag('Simple3CPUPFU')
DebugFlag('Simple3CPUDecode')
DebugFlag('Simple3CPUExecute')
CompoundFlag('Simple3CPUAll',
    ['Simple3CPULineTrace', 'Simple3CPUFlash', 'Simple3CPUPFU',
     'Simple3CPUDecode', 'Simple3CPUExecute'])
```

Also add this dir's path to the top-level `cpu/Kconfig` as an option.

---

## 12. Debug tracing

The Python prototype uses one-line-per-cycle ASCII traces that proved
invaluable for finding the bp_alternating R6 bug. Reproduce this in
gem5 using `--debug-flags=Simple3CPULineTrace`:

```
c=  81 pc=e8 r0=196 buf=e8 F:f0/5      fl=0 fifo=[] D:BNE@e8 E:-
```

Implementation: `Pipeline::dumpLineTrace()` is called at the end of
`process()`; it uses `DPRINTF(Simple3CPULineTrace, ...)` with exactly
the same field layout as the Python trace. This lets us diff gem5
output against Python output line-by-line, which is how we'll verify
correctness.

---

## 13. Verification plan

The seven measurements from
[STM32G4_PIPELINE_MODEL.md §6](../../../../prototype/STM32G4_PIPELINE_MODEL.md#6-verification-simulated-vs-measured-build-stm32-binaries)
are the acceptance criteria:

| Benchmark             | Target (measured) | Python model | Target for gem5 port |
|-----------------------|-------------------|--------------|-----------------------|
| bench-branch          | 1013              | 1009         | within ±5 of 1009     |
| bench-bp_tight        | 2013              | 2009         | within ±5 of 2009     |
| bench-bp_long_body    | 2213              | 2210         | within ±5 of 2210     |
| bench-bp_forward      | 1817              | 1813         | within ±5 of 1813     |
| bench-bp_alternating  | 3420              | 3312         | within ±5 of 3312     |
| bench-bp_align        | 2211              | 2128         | within ±5 of 2128     |
| bench-bp_nested       | 2318              | 2509         | within ±5 of 2509     |

We target the **Python model's numbers**, not the measured numbers —
anything the Python model gets "wrong" (the R7 pipeline pre-fill and
the bp_nested PFU-arbiter timing detail) is a known, documented
under-approximation and not something a faithful port should
accidentally fix or worsen.

Test harness: run each `bench-*.elf` through the CPU with a trivial
`test_simple3_cpu.py` config; compare final `system.cpu.numCycles`
against the table.

---

## 14. Phased implementation checklist

Order matters — each phase produces a runnable artefact.

**Phase A — skeleton compiles and links**
- [ ] Create all files with empty/stub bodies.
- [ ] `SConscript` + `Simple3CycleCPU.py` register.
- [ ] `scons build/ARM/gem5.opt` succeeds.
- [ ] Gem5 boots, CPU's `startup()` schedules `cycleEvent` once and the
      sim exits.

**Phase B — no memory, fake instruction stream**
- [ ] Hand-populate `PFU::fifo` with a short instruction sequence in a
      unit-test style setup.
- [ ] `cycleEvent` progresses D → E → commit.
- [ ] `totalInsts()` returns the expected count.
- [ ] Line trace matches Python for the trivial case.

**Phase C — FlashModel inside CPU (Phase 1 of §8)**
- [ ] `FlashModel` + `PFU` fully implemented, including pfu_next_addr
      filter and pending_upper straddle.
- [ ] Branch prediction + mispredict + R6 FIFO filter implemented.
- [ ] Run bench-branch, bp_tight, bp_long_body — all within ±5 of
      Python.

**Phase D — remaining branch benchmarks**
- [ ] bp_forward, bp_alternating, bp_align, bp_nested match the Python
      numbers in the table above.
- [ ] Diff line traces against Python for any off-by-one debug.

**Phase E — real ARM ISA + ThreadContext**
- [ ] Replace `arch_state` with `SimpleThread` / real ISA decode.
- [ ] Boot a minimal bare-metal ELF through `Simple3CycleCPU`.
- [ ] The 7 benchmarks run to halt and produce the targeted numbers.

**Phase F (optional, later) — external FlashModel**
- [ ] Move `FlashModel` out of CPU onto the memory bus.
- [ ] CPU uses `IcachePort.sendTimingReq()`.
- [ ] Same benchmarks still produce the same numbers (regression gate).

---

## 15. Open questions

1. **Exact tick at which the PFU arbiter issues a new fetch after a
   same-line hit completes.** This is the bp_nested +8% residual in
   the Python model. Leaving open for Phase E measurement against the
   gem5 results — if the gem5 port closes the gap naturally because
   `Ticked` enforces a different sub-cycle ordering than the Python
   model, we learn something. If not, document as a known residual.
2. **Interrupt / exception handling.** Not needed for the 7 kernels;
   deferred to after Phase E. Cortex-M4 NMI / IRQ entry is 12 cycles
   (TRM §3.9.1) — model as a pipeline flush + vector-table fetch.
3. **DWT CYCCNT instruction hookup.** Currently each benchmark uses a
   store-to-DWT_CTRL to start/stop counting. In gem5 we can just read
   `numCycles` directly; if we want to model DWT accesses as timed AHB
   writes to the Private Peripheral Bus, that's a bit of extra work
   and may or may not matter.
