# Lego CPU Design v2: Reactive Port-Based Pipeline

Status: Work in progress.

---

## Core Idea

Replace the monolithic `InFlightInst` struct with **typed input/output
ports** on each StageFunction. Data flows through explicit connections.
When an output port is written, it triggers `compute()` on any function
whose input port is connected to it. This is a **reactive dataflow**
model.

---

## Class Hierarchy

```
StageFunction (base: SimObject)
├── StageFunctionWithPort      (interacts with icache/dcache via packets)
│   └── contains FuncSenderState (Packet::SenderState)
└── StageFunctionTranslation   (interacts with MMU)
    └── contains TranslationCallback (BaseMMU::Translation)

SubStage: ordered group of StageFunctions (parallel within a Stage)
Stage:    group of SubStages (one pipeline cycle)
LegoCPU:  top-level, owns Stages and gem5 BaseCPU resources
```

### MMU Translation via Composition (not inheritance)

`StageFunctionTranslation` contains a `TranslationCallback` object
instead of inheriting from `BaseMMU::Translation`. This avoids
diamond inheritance with `SimObject`.

```cpp
class StageFunctionTranslation : public StageFunction
{
    class TranslationCallback : public BaseMMU::Translation
    {
        StageFunctionTranslation &owner;
      public:
        TranslationCallback(StageFunctionTranslation &o)
            : owner(o) {}
        void markDelayed() override {}
        void finish(const Fault &fault, const RequestPtr &req,
                    ThreadContext *tc, BaseMMU::Mode mode) override
        {
            owner.translationComplete(fault, req, tc, mode);
        }
    };

    TranslationCallback translationCB{*this};

  protected:
    RequestPtr translationReq;
    virtual void translationComplete(const Fault &fault,
        const RequestPtr &req, ThreadContext *tc,
        BaseMMU::Mode mode) = 0;
};
```

---

## Port System

Each StageFunction declares named, typed input and output ports.
Connections are validated at construction time.

```cpp
template<typename T>
class Output {
    T value;
    bool valid = false;
    bool blocked = false;
    unsigned stageId;
    Tick lastWritten = 0;
    std::vector<Input<T>*> consumers;

  public:
    void write(const T &v) {
        // Don't trigger if value unchanged
        if (valid && value == v) return;
        // Check if any consumer rejects this value
        for (auto *input : consumers) {
            if (!input->askForUnblock(v)) {
                blocked = true;
                return;  // consumer rejected, we're blocked
            }
        }
        // All consumers accepted
        blocked = false;
        value = v;
        valid = true;
        lastWritten = curTick();
        for (auto *input : consumers)
            input->notify();
    }
    bool isBlocked() const { return blocked; }
    const T &read() const { return value; }
    bool hasData() const { return valid; }
    void clear() { valid = false; }
};

template<typename T>
class Input {
    Output<T> *source = nullptr;
    unsigned stageId;
    bool blocked = false;
    std::function<bool(const T&)> blockFilter = nullptr;

  public:
    // Called by Output::write() before writing.
    // Returns true if the value is accepted, false if blocked.
    bool askForUnblock(const T &value) {
        if (!blocked) return true;
        if (blockFilter && blockFilter(value))
            return true;   // filter says let it through
        return false;       // stay blocked
    }

    // Block this input. Optionally provide a filter that
    // selectively allows certain values through.
    void block(std::function<bool(const T&)> filter = nullptr) {
        blocked = true;
        blockFilter = filter;
    }

    void unblock() {
        blocked = false;
        blockFilter = nullptr;
        // Trigger producer to retry — it may have been blocked
        if (source)
            source->retryWrite();
    }

    void notify() {
        if (source->stageId == stageId) {
            ownerFunc->compute();
        } else {
            ownerFunc->notifyUpdate();
        }
    }
    const T &read() const { return source->read(); }
    bool hasData() const { return source->hasData(); }
    bool isBlocked() const { return blocked; }
};
```

### Blocking and Back-Pressure

Blocking propagates **backward through ports**, not through the
stage/substage hierarchy. A function checks `output.isBlocked()`
before computing. If blocked, it doesn't compute, which means its
own inputs are effectively blocked too — back-pressure propagates
naturally.

```cpp
// Any function checks before computing:
void ALUExecute::compute()
{
    if (resultOut.isBlocked()) return;  // can't produce
    // ... do work ...
    resultOut.write(result);
}
```

### Selective Unblocking (askForUnblock)

Some functions need to accept specific values while rejecting
others. Example: `InOrderRetire` only accepts the next expected
seqNum:

```cpp
void InOrderRetire::compute()
{
    // Only accept the instruction we're waiting for
    instInput.block([this](const ExecResult &r) {
        return r.seqNum == nextExpectedSeqNum;
    });
}
```

Flow for in-order retire:

```
inst #5 (seqNum=5) tries to write → askForUnblock(#5)
  → filter: 5 == nextExpected(4)? No → rejected, blocked

inst #4 (seqNum=4) tries to write → askForUnblock(#4)
  → filter: 4 == nextExpected(4)? Yes → accepted, flows in
  → InOrderRetire commits #4
  → nextExpectedSeqNum = 5
  → input.unblock() then re-block with new filter
  → triggers producer retry → #5 now passes filter
```

### Unblock Triggers Retry

When `Input::unblock()` is called, it tells the connected output
to retry its last write via `source->retryWrite()`. This follows
the same reactive model — unblocking is just another trigger.
Same-stage vs cross-stage timing rules apply.

### Infinite Loop Prevention

`Output::write()` checks if the value actually changed before
triggering consumers. If a cascade produces the same value, no
further triggers fire. This guarantees termination.

---

## Data Types Flowing Through Ports

Each data struct includes a `seqNum` for instruction identity.
The `seqNum` is assigned by `FetchAddressGen` when a new PC is
consumed.

```cpp
struct FetchAddr {
    InstSeqNum seqNum;
    Addr pc;
    Addr alignedAddr;
    unsigned size;
};

struct FetchLine {
    InstSeqNum seqNum;
    Addr baseAddr;
    uint8_t data[4];
    unsigned validBytes;
};

struct DecodedInst {
    InstSeqNum seqNum;
    StaticInstPtr staticInst;
    Addr pc;
    // operands, opclass, etc.
};

struct ExecResult {
    InstSeqNum seqNum;
    RegVal result;
    RegVal flags;
    bool isBranch;
};

struct Redirect {
    InstSeqNum seqNum;
    Addr target;
    bool valid;
};
```

---

## Connections

### Within a SubStage

Sequential: output of function[i] → input of function[i+1].
Inferred automatically from the function order in Python config.

### Cross-SubStage and Cross-Stage

Declared explicitly as string pairs in Python config:

```python
LegoStage(subStages=[sub0, sub1], connections=[
    ("sub0.ALUExecute.result", "sub1.RegisterWriteback.result"),
])

LegoCPU(stages=[s0, s1], connections=[
    ("S1.BranchResolve.redirect", "S0.FetchAddressGen.redirect"),
])
```

In C++, each function registers its ports by name in a map.
At construction, the CPU/Stage walks the connection list, looks up
ports by name, validates type match, and wires them
(`input.source = &output`). Type mismatch → fatal at config time.

---

## Timing Model

### Within a Stage: Same-Cycle (Combinational)

Functions in the same stage see each other's outputs immediately.
When an output is written, connected inputs in the same stage
trigger `compute()` right away.

### Between Stages: Next-Cycle (Registered)

Data crossing a stage boundary has 1-cycle latency. Outputs
written in cycle N are delivered to inputs in a different stage
at the start of cycle N+1.

This applies to ALL cross-stage signals, including backward
control (branch redirects). This matches real in-order hardware
where stage boundaries are register/latch boundaries.

---

## Instruction Identity: seqNum

- `seqNum` is assigned by **FetchAddressGen** when it consumes a
  new PC input. Fetch is the only creator of new instructions.
- The `seqNum` flows through every data struct in the ports.
- Squashing, retire, and ordering all use `seqNum`.

### PC generation

The PC lives in `ThreadContext` and is maintained by the **fetch
stage**:
- Default: `PC += instruction_size` (sequential)
- Override: branch redirect from execute stage (next-cycle signal)
- Override: branch prediction from predict substage (same-cycle)

Execute only sends a `Redirect` signal back to fetch. It does NOT
update the PC directly.

---

## Squashing

Squash is a **per-stage control signal**, not per-function.

When a squash is triggered (branch misprediction, exception):
1. The stage that detects it (e.g., execute) writes to a
   `Redirect` output port.
2. This is a cross-stage signal → delivered next cycle.
3. At the start of the next cycle, the receiving stage (fetch)
   sees the redirect and uses the new target PC.
4. All stages between fetch and execute have stale instructions.
   Each stage checks: if `inst.seqNum > redirect.seqNum`, the
   instruction is squashed → clear ports, don't commit.

Each `Stage` has a `squash()` method:
```cpp
void Stage::squash(InstSeqNum after_seq)
{
    // Clear all ports holding data with seqNum > after_seq
    for (auto *sub : subStages)
        sub->squash(after_seq);
}
```

### When squashing is needed

- Branch misprediction (execute resolves differently than predicted)
- Exception / fault detected at execute or commit
- Interrupt taken between instructions

---

## Scheduling: needUpdate Flag

A single `needUpdate` flag on the CPU controls tick scheduling.

### The flag

```cpp
class LegoCPU : public BaseCPU
{
    bool needUpdate = false;
    Tick cycleStartTick = 0;
};
```

### How it gets set

When an output port `write()` has a cross-stage consumer:

```
Output::write()  [detects cross-stage consumer]
  → func->notifyUpdate()
    → _subStage->notifyUpdate()
      → _stage->notifyUpdate()
        → _cpu->notifyUpdate()
          → needUpdate = true
```

Same-stage writes trigger immediate cascades but do NOT set
needUpdate.

### How tick() uses it

```
tick():
  Step 1: cycleStartTick = curTick()
          needUpdate = false

  Step 2: Deliver cross-stage data
      For each stage, push outputs from OTHER stages that
      were written before cycleStartTick into receiving
      stages' input ports. This triggers reactive cascades.

  Step 3: Feed new PC into Stage 0 (fetch)
      Triggers reactive cascade in fetch stage.

  Step 4: Cascades settle within each stage.

  Step 5: Reschedule
      if (needUpdate && !tickEvent.scheduled())
          schedule(tickEvent, clockEdge(Cycles(1)))

  Step 6: Print line trace (if debug enabled)
```

### Same-Cycle Response Detection

```cpp
bool
LegoCPU::isCurrentCycle() const
{
    return curTick() < cycleStartTick + clockPeriod();
}
```

`cycleStartTick` is recorded at the start of `tick()`. Any async
response arriving before `cycleStartTick + clockPeriod()` is still
within this cycle — its cascade fires immediately as part of the
current evaluation. Responses arriving later trigger
`notifyUpdate()` and schedule the next tick.

Note: `clockEdge(Cycles(0))` is NOT safe for this check because
it rounds up to the next edge when called mid-cycle.

---

## Async Responses (MMU, Cache)

Async responses write to output ports using the same mechanism.
No special resume/runFrom machinery — the reactive cascade handles
both sync and async uniformly.

### Translation response

```cpp
void
StageFunctionTranslation::translationComplete(
    const Fault &fault, const RequestPtr &req,
    ThreadContext *tc, BaseMMU::Mode mode)
{
    // Write result to output port → triggers cascade
    fetchAddr.write(req->getPaddr());

    // If outside current cycle, schedule next tick
    if (!_cpu->isCurrentCycle()) {
        _cpu->notifyUpdate();
        if (!_cpu->tickEvent.scheduled())
            _cpu->schedule(_cpu->tickEvent,
                           _cpu->clockEdge(Cycles(1)));
    }
}
```

### Cache response

```
port.recvTimingResp(pkt)
  → cpu.recvTimingResp(pkt)
    → ss->stage->recvTimingResp(pkt)
      → ss->subStage->recvTimingResp(pkt)
        → ss->func->recvTimingResp(pkt)
          → writes to output port → triggers cascade
```

Uses `FuncSenderState` with stored pointers (func, subStage,
stage) for O(1) dispatch on the return path.

---

## Communication Chains

### Packet send (function → CPU → port)
```
function.compute()
  → _subStage->sendPacketToStage(pkt)
    → _stage->sendPacketToCPU(pkt)
      → _cpu->sendPacketToPort(pkt)
        → switch(portType): ICACHE / DCACHE
```

### Translation send (function → CPU → MMU)
```
function.compute()
  → _subStage->sendTranslationToStage(req, callback, mode)
    → _stage->sendTranslationToCPU(req, callback, mode)
      → _cpu->sendTranslationToMMU(req, callback, mode)
        → mmu->translateTiming(req, tc, callback, mode)
```

### FuncSenderState

```cpp
struct FuncSenderState : public Packet::SenderState
{
    StageFunctionWithPort *func;
    SubStage *subStage;
    Stage *stage;
    InstSeqNum instSeqNum;
    PortType portType;  // ICACHE or DCACHE
};
```

Pointers filled in as the packet travels up (send), used for
direct dispatch on the way back (recv). No searching.

---

## SubStage vs Stage

**Stage** = one pipeline cycle. All computation within a stage
happens in the same cycle. Stage-to-stage data takes 1 cycle.

**SubStage** = a group of functions that can run in parallel with
other SubStages in the same stage. Use cases:
- Multiple ALUs in the execute stage
- Separate fetch and predict paths in the fetch stage
- Memory and ALU paths running in parallel

SubStages within a Stage are all same-cycle. The SubStage boundary
is for organizational grouping, not timing.

---

## Pipeline Functions Reference

### Fetch

| Function | Inputs | Outputs | Notes |
|---|---|---|---|
| `FetchAddressGen` | PC, redirect, prediction | fetchAddr | Aligns PC, translates via MMU. Assigns seqNum. |
| `FetchMemRequest` | fetchAddr | (icache request) | Sends fetch request to icachePort. |
| `FetchMemResponse` | (icache response) | fetchLine | Receives bytes from icache. |
| `FetchLineBuffer` | fetchLine, PC | alignedInst | Aligns variable-width instructions. |

### Decode

| Function | Inputs | Outputs | Notes |
|---|---|---|---|
| `InstructionDecode` | alignedInst | decodedInst | Decodes opcode, extracts operands. |
| `BranchDetect` | decodedInst | branchInfo | Identifies branches for prediction. |
| `BranchPredict` | branchInfo | prediction | BTB/predictor lookup. |

### Issue

| Function | Inputs | Outputs | Notes |
|---|---|---|---|
| `ScoreboardCheck` | decodedInst, scoreboard | ready/stall | Checks operand availability. |
| `FUDispatch` | decodedInst, FU status | fuAssignment | Routes to correct FU. |

### Execute

| Function | Inputs | Outputs | Notes |
|---|---|---|---|
| `ALUExecute` | decodedInst | execResult | Arithmetic/logic, 1-cycle. |
| `MultiplyExecute` | decodedInst | execResult | Configurable latency. |
| `DivideExecute` | decodedInst | execResult | Multi-cycle, non-pipelined. |
| `FPUExecute` | decodedInst | execResult | Multi-cycle, pipelined. |
| `ShiftExecute` | decodedInst | execResult | Barrel shifter. |
| `BranchResolve` | decodedInst, execResult | redirect | Compares with prediction, squash on mispredict. |

### Memory Access

| Function | Inputs | Outputs | Notes |
|---|---|---|---|
| `LoadAddressGen` | decodedInst | (dcache request) | Computes EA for loads. |
| `StoreAddressGen` | decodedInst | (dcache request) | Computes EA + store data. |
| `MemResponse` | (dcache response) | loadResult | Receives data from dcache. |

### Commit / Writeback

| Function | Inputs | Outputs | Notes |
|---|---|---|---|
| `RegisterWriteback` | execResult | (writes TC) | Writes to arch register file. |
| `PCUpdate` | execResult, redirect | (writes TC) | Updates PC in ThreadContext. |
| `InOrderRetire` | execResult | retireSignal | Retires in program order. Exception check. |
| `ScoreboardClear` | retireSignal | (updates scoreboard) | Frees dst register. |

### Pipeline Control

| Function | Inputs | Outputs | Notes |
|---|---|---|---|
| `SquashControl` | redirect, exception | squashSignal | Squashes younger instructions per stage. |
| `StallControl` | FU busy, scoreboard | stallSignal | Back-pressure propagation. |
| `InterruptCheck` | interrupt controller | exceptionRedirect | Samples pending interrupts. |

---

## Line Tracing

RTL-style cycle-by-cycle trace printed at the end of each `tick()`.
Each cycle is one line. Each stage is a column identified by index.

### Example Output

```
Cycle |  S0              |  S1              |  S2              |  S3
------+------------------+------------------+------------------+---------------
    1 | F:0x1000         |                  |                  |
    2 | F:0x1004         | D:0x1000         |                  |
    3 | F:wait(MMU)      | D:0x1004         | X:0x1000         |
    4 | F:wait(icache)   |        -         | X:0x1004         | C:0x1000
    5 | F:0x2000         |                  | X:redir->2000    | C:0x1004
    6 | F:0x2004         | D:0x2000         | X:0x1008(squash) | C:0x1008
```

### How It Works

Each `StageFunction` provides a short status string:

```cpp
class StageFunction
{
  public:
    virtual std::string traceStatus() const { return ""; }
};
```

Each `Stage` collects trace strings from all its functions:

```cpp
std::string
Stage::traceStatus() const
{
    std::string result;
    for (auto *sub : subStages) {
        for (auto *func : sub->functions) {
            std::string s = func->traceStatus();
            if (!s.empty()) {
                if (!result.empty()) result += ",";
                result += s;
            }
        }
    }
    return result.empty() ? "-" : result;
}
```

Printed at the end of `tick()`:

```cpp
void
LegoCPU::printLineTrace()
{
    std::string line = csprintf("Cycle %6d |", curCycle);
    for (auto *stage : stages)
        line += csprintf(" %-16s |", stage->traceStatus());
    DPRINTF(LegoCPU, "%s\n", line);
}
```

Header printed once at startup. Enabled with
`--debug-flags=LegoCPU`.

---

## Buffering: Per-Function, Not Per-Stage

All buffering is internal to functions. The port system holds only
the current value — no FIFOs. Functions that need queuing (store
buffer, multi-cycle divider, reorder buffer) manage their own
internal state.

This works because the blocking mechanism handles dispatch
naturally. Example with two parallel ALU substages:

```
inst #6 tries to write to ALU_0.input
  → askForUnblock → ALU_0 is busy → rejected
  → tries ALU_1.input (second consumer on same output)
  → askForUnblock → ALU_1 is free → accepted
  → inst #6 flows into ALU_1
```

No stage-level buffer or dispatch logic needed. The port
connections and blocking filters handle routing automatically.
Functions stay decoupled from the stage/substage hierarchy.
