# DESIGNv2 Implementation Log

## 2026-04-04: Initial v2 Framework Implementation

### What was done

Rewrote the Lego CPU from the v1 `InFlightInst`-based design to the
v2 reactive port-based dataflow model described in `DESIGNv2.md`.
All files compile and link successfully with `scons build/ARM/gem5.opt`.

### Files created (new)

| File | Purpose |
|------|---------|
| `port.hh` | `Input<T>` and `Output<T>` templates with reactive `notify()`, `PortBase` for type-erased port references |
| `fetch_functions.hh` | `FetchAddressGen`, `FetchMemRequest`, `FetchMemResponse` declarations |
| `fetch_functions.cc` | Fetch function implementations (MMU translation, icache request, response handling) |
| `execute_functions.hh` | `InstructionDecode`, `ALUExecute`, `PCUpdate` declarations |
| `execute_functions.cc` | Execute function implementations (decode via gem5 decoder, ALU stub, PC update with redirect) |

### Files rewritten from v1

| File | Key changes |
|------|-------------|
| `type.hh` | Removed `InFlightInst` struct and `PortType::MMU`/`ERROR`. Added typed port data structs: `FetchAddr`, `FetchLine`, `DecodedInst`, `ExecResult`, `Redirect` — each with `seqNum` and `operator==` |
| `stage_function.hh` | Removed v1 `compute(InFlightInst&)`, `waiting`/`blocking`/`lastSeqNum`. New: `compute()` (no args), port registration (`registerPort`/`findPort`), `notifyUpdate()` chain. `StageFunctionWithPort` uses inherited constructor. `StageFunctionTranslation` uses composition (contains `TranslationCallback` object, not inheritance) |
| `stage_function.cc` | Removed all v1 function bodies. New: `setStageId()` updates all registered ports, `notifyUpdate()` chains to substage, `findPort()` for connection wiring |
| `sub_stage.hh` | Removed `outputInst`, `runFrom()`, `currentFunc`, `resume()`, `isWaiting()`/`isBlocking()`. New: simple `compute()` that iterates functions, `recvTimingResp()` for packet dispatch |
| `sub_stage.cc` | Clean rewrite: constructor from params, `compute()` iterates functions, send chains (packet + translation), `recvTimingResp()` pops SenderState and dispatches |
| `stage.hh` | Removed `outputInst`, `runFrom()`, `currentSubStage`, `resume()`, `isWaiting()`/`isBlocking()`. New: `traceStatus()`, `setCPU()` sets stageId on all functions |
| `stage.cc` | Clean rewrite: `setCPU()` propagates stageId, `compute()` iterates substages, `traceStatus()` collects from all functions, send/recv chains |
| `lego_cpu.hh` | Removed `lastInst`, `currentInst`, `currentCycle` (Cycles type). Added `cycleStartTick` (Tick), `needUpdate` flag, `curCycle` (uint64_t), `isCurrentCycle()`, `notifyUpdate()`, `printLineTrace()`, `wireConnections()` |
| `lego_cpu.cc` | Clean rewrite of `tick()`: records `cycleStartTick`, clears `needUpdate`, computes all stages, reschedules if `needUpdate`. `notifyUpdate()` also schedules tick if outside current cycle. Added `printLineTrace()` and trace header in `startup()` |
| `LegoCPU.py` | Added all new SimObjects: `FetchAddressGen` (with `fetchWidth` param), `FetchMemRequest`, `FetchMemResponse`, `InstructionDecode`, `ALUExecute`, `PCUpdate`. Removed stale `LegoStageFunctionTranslation` reference to old class |
| `SConscript` | Updated `sim_objects` list and `Source()` entries for all new files |

### Files removed

| File | Reason |
|------|--------|
| `in_flight_inst.hh` | Replaced by typed port data structs in `type.hh` (was already deleted by user) |

### Build issues fixed during implementation

1. **`PARAMS` macro on abstract SimObjects** — gem5 doesn't generate Params for abstract classes. Fixed by using `using StageFunction::StageFunction` (inherited constructor) for `StageFunctionWithPort`, and explicit `using Params = LegoStageFunctionParams` for `StageFunctionTranslation`.

2. **`getPort` name collision** — `StageFunction::getPort()` hid the virtual `SimObject::getPort()`. Renamed to `findPort()`.

3. **`panic` not declared in template** — `port.hh` used `panic()` in a template but didn't include `base/logging.hh`. Added include.

4. **`_stageId` access in `setStageId`** — `PortBase::_stageId` was protected. Added `setStageId()` public method to `PortBase`.

5. **Decoder `moreBytes` signature** — gem5's `InstDecoder::moreBytes()` takes 2 args `(PCStateBase&, Addr)`, not 3. Data goes into `moreBytesPtr()` buffer separately.

6. **`staticInst->execute()` needs `ExecContext*`** — `ThreadContext*` is not `ExecContext*`. ALUExecute is currently **stubbed** (returns `NoFault` without executing). Proper ExecContext needed for actual execution.

### Current state: what works

- Framework compiles and links with gem5 ARM target
- Pipeline structure: `LegoCPU` → `Stage[]` → `SubStage[]` → `StageFunction[]`
- Typed port system: `Input<T>` / `Output<T>` with reactive `notify()`
- Port registration for connection wiring (`registerPort` / `findPort`)
- Send chains: packet (function → substage → stage → CPU → icache/dcache) and translation (function → substage → stage → CPU → MMU)
- Recv chains: packet response dispatched via stored pointers in `FuncSenderState`
- Translation callback via composition (`TranslationCallback` object)
- `tick()` with `needUpdate` scheduling and `isCurrentCycle()` detection
- Line tracing infrastructure (`traceStatus()` on each function, printed at end of `tick()`)
- All 6 concrete functions declared and partially implemented

### What still needs to be done (as of initial commit)

1. **ExecContext for ALUExecute** — need a Lego-specific `ExecContext` class so `staticInst->execute()` actually works. Currently stubbed.

2. **Connection wiring** — `wireConnections()` in `LegoCPU` is a TODO. Need to parse connection specifications from Python config and call `output.addConsumer(&input)` / `input.addSource(&output)`.

3. **`Input::notify()` cross-stage timing check** — the notify implementation checks `stageId` match, but `Output::stageId()` returns the stageId from `PortBase`. The stageId is set to 0 at construction and updated later by `setStageId()`. Need to verify this works correctly after `setCPU()` is called.

4. **Test config script** — Python script to instantiate LegoCPU with 2 stages and run the test program.

5. **Test binary** — ARM assembly test program (`mov r0, #10; loop: subs r0,r0,#1; bne loop`).

6. **`FetchAddressGen::compute()`** needs real PC management — currently reads from ThreadContext but doesn't handle the first-fetch bootstrap properly.

7. **`FetchMemRequest::compute()`** needs the requestorId from the CPU — currently hardcoded to 0.

---

## 2026-04-04: Continued — ExecContext, Wiring, Cleanup

### What was done

#### 1. Merged FetchMemRequest + FetchMemResponse

`FetchMemResponse` was removed. `FetchMemRequest` now handles both
the icache request (`compute()`) and the icache response
(`recvTimingResp()`), outputting `FetchLine` directly.

**Rationale:** The `FuncSenderState` routes the response back to the
function that sent the request. If `FetchMemRequest` is in stage 0
and `FetchMemResponse` is in stage 1, the response would go to
stage 0 (where the sender lives), not stage 1. Merging them avoids
this routing problem. For a 2-stage fetch, `FetchMemRequest` sits
in stage 0, and its `FetchLine` output crosses to stage 1 via the
normal cross-stage port mechanism — naturally modeling cache latency.

Files changed: `fetch_functions.hh`, `fetch_functions.cc`,
`LegoCPU.py`, `SConscript`.

#### 2. Created LegoExecContext (`exec_context.hh`)

Minimal `ExecContext` implementation that delegates register access
to `SimpleThread`. Memory operations panic (not supported in base
case). This enables `staticInst->execute()` in `ALUExecute`.

Key methods implemented:
- `getRegOperand` / `setRegOperand` — register read/write via `SimpleThread`
- `readMiscRegOperand` / `setMiscRegOperand` — misc register access
- `pcState` get/set
- `readPredicate` / `setPredicate`
- Memory ops: all panic with "not implemented"
- HTM: returns defaults (not supported)

#### 3. Added getThread() chain

Added `getThread(ThreadID)` to `SubStage` and `Stage` that chains
up to `LegoCPU::getThread()`. This eliminates all
`static_cast<LegoCPU*>` casts in function implementations.

Access pattern: `_subStage->getThread(0)` instead of casting
`_subStage->stage()->cpu()` to `LegoCPU*`.

Files changed: `sub_stage.hh/cc`, `stage.hh/cc`, `lego_cpu.hh`,
`fetch_functions.cc`, `execute_functions.cc`.

#### 4. Implemented connection wiring

`LegoCPU::wireConnections()` auto-wires sequential functions within
each SubStage by matching output→input port types. Added
`PortBase::connectTo()` for type-erased connection and
`PortBase::isOutput()` for port direction check.

`Output<T>::connectTo(PortBase*)` validates type match at runtime
and calls `addConsumer` / `addSource`.

`StageFunction::getPorts()` exposes the port map for wiring.

Cross-stage connections from Python config are still TODO.

Files changed: `port.hh`, `stage_function.hh`, `lego_cpu.hh/cc`.

#### 5. Build fixes

- Fixed `decoder->moreBytes()` to use correct 2-arg signature
  (data goes into `moreBytesPtr()` buffer separately)
- Fixed `instRequestorId()` access via `_subStage->stage()->cpu()`
  instead of cast

### Current state

All files compile and link cleanly with `scons build/ARM/gem5.opt`.
ALUExecute now uses `LegoExecContext` for real instruction execution.
Port wiring works for intra-SubStage connections.

---

## 2026-04-04 ~13:00: Connection Wiring Redesign + First Run

### What was done

#### 1. Connection wiring redesign: funcName + string pairs

Removed `VectorParam.LegoStageFunction` connection params from each
function (caused circular SimObject ownership → infinite recursion
in `SimObject.path()`).

Replaced with:
- `funcName` string param on `LegoStageFunction` base class
- `connections` string list on `LegoCPU`: `"srcFunc.port:dstFunc.port"`
- `wireConnections()` in C++ builds a `funcName → StageFunction*` map,
  parses the connection strings, and calls `wireOutput()`/`wireInput()`

Files changed: `LegoCPU.py`, `stage_function.hh/cc`, `lego_cpu.hh/cc`,
`fetch_functions.hh/cc`, `execute_functions.hh/cc`.

#### 2. ArmLegoCPU wrapper

Added `ArmLegoCPU` to `src/arch/arm/ArmCPU.py` (same pattern as
`ArmMinorCPU`). Required for ISA-specific setup (ArmMMU, ArmISA,
ArmDecoder, ArmInterrupts).

#### 3. Test binary and config

- `test/test_loop.S`: AArch64 assembly (`mov x0, #10; subs; bne; exit`)
- `test/test_lego_cpu.py`: Python config with 2-stage pipeline
- Binary path uses `os.path.abspath(__file__)` for portability

#### 4. FetchMemRequest/FetchMemResponse merge

Merged into single `FetchMemRequest` that sends request AND receives
response. `FetchMemResponse` class removed entirely.

#### 5. SubStage::recvTimingResp double-pop fix

SubStage was popping `FuncSenderState` then passing packet to function
which popped again → assertion failure. Fixed: SubStage peeks (doesn't
pop), function pops.

#### 6. FetchLine::data changed to vector<uint8_t>

Fixed `uint8_t data[4]` → `std::vector<uint8_t>` to handle variable
fetch widths. Added `lineBaseAddr` field (virtual address).
Custom `operator==` checks size then element-by-element.

#### 7. InstructionDecode PC offset fix

Was copying bytes from offset 0 in the fetch line. Fixed to calculate
`offset = pcAddr - lineBaseAddr` and copy from that offset.
Initially crashed because `lineBaseAddr` was physical, not virtual.
Fixed to use aligned virtual address.

#### 8. ExecResult expanded for PC management

Added `staticInst`, `pc`, `branchTaken` fields to `ExecResult`.
`ALUExecute` now saves PC before execute, compares after to detect
branch taken. `PCUpdate` calls `advancePC()` for sequential, uses
existing PC for branch taken.

### First successful pipeline trace (cycle 2)

```
Cycle 2 | F:0x4000d0,F:got(0x4000d0) | D:movz x0,#10,X:seq1,PC:0x4000d4 |
```

The first instruction (`movz x0, #10`) was:
- Fetched (MMU translate → icache request → response)
- Decoded (ARM decoder `moreBytes` + `decode`)
- Executed (via `LegoExecContext`)
- PC updated (sequential advance to `0x4000d4`)

### Known bugs (not yet fixed)

#### Bug 1: FetchAddressGen allows FetchMemRequest to fire before translation completes

In SE mode, MMU `translateTiming` completes synchronously (TLB hit).
The `translationComplete()` callback fires inside `sendTranslationToStage()`,
which writes `fetchAddrOut`, which triggers `FetchMemRequest::compute()`
via `notify()` — all before `FetchAddressGen::compute()` returns.

`waitingForTranslation = true` is set too late. Moving it before
`sendTranslationToStage()` fixes the flag but doesn't prevent the
synchronous cascade. This is a design question about whether
same-stage reactive cascades should fire mid-compute.

#### Bug 2: Pipeline stalls after first instruction cycle

After executing the first instruction, `FetchAddressGen` sees
`pc == lastFetchedPC` and skips fetching. The PC advanced but the
`lastFetchedPC` guard prevents re-fetching when the new PC falls
within the same already-fetched line.

Root cause: `lastFetchedPC` compares raw PCs, but multiple
instructions can live in the same fetch line. Need to either
remove this guard or compare aligned fetch addresses.

#### Bug 3: Branch target appears wrong

`bne` disassembles as `b.ne 0x4000cc` but `PCUpdate` reports
`branch redirect to 0x4000dc`. The branch target detection in
`ALUExecute` compares `pcBefore != pcState()` but the PC after
execute may not reflect the correct branch target. The interaction
between `execute()` and `advancePC()` for branches needs
investigation.

#### Bug 4: notify() cascade ordering

When `Output::write()` triggers `notify()` on a same-stage consumer,
the consumer's `compute()` fires immediately inside the producer's
`compute()` call stack. This creates deeply nested compute calls and
makes the execution order hard to reason about.

The design doc says same-stage should be combinational (immediate),
but in practice this means the SubStage's sequential iteration
(`func[0].compute()` then `func[1].compute()`) is bypassed by the
reactive cascade. The iteration order and the reactive order can
conflict.

### Pipeline trace output (first 7 cycles)

```
Cycle 1 | F:0x4000d0,F:wait(icache) | -                |
Cycle 2 | F:0x4000d0,F:got(0x4000d0) | D:movz x0,#10,X:seq1,PC:0x4000d4 |
Cycle 3 | F:0x4000d0,F:got(0x4000d0) | D:movz x0,#10,X:seq1,PC:0x4000d4 |
  (stalls — FetchAddressGen skips because pc==lastFetchedPC)
Cycle 4 | F:0x4000d0,F:wait(icache) | D:subs x0,x0,#1,X:seq2,PC:0x4000dc |
Cycle 5 | F:0x4000d0,F:wait(icache) | D:subs x0,x0,#1,X:seq2,PC:0x4000dc |
Cycle 6 | F:0x4000d0,F:got(0x4000d0) | D:b.ne 0x4000cc,X:seq3,PC:redir->0x4000dc |
Cycle 7 | F:0x4000d0,F:got(0x4000d0) | D:b.ne 0x4000cc,X:seq3,PC:redir->0x4000dc |
  (stalls again)
```

---

## 2026-04-04 ~14:00: Branch Fix + Pipeline Running

### What was done

#### 1. Fixed branch detection (Bug 3 resolved)

Root cause: `ALUExecute` compared PC before/after `execute()` to
detect branches. But AArch64 conditional branches don't modify the
PC during `execute()` — they only set condition flags. The actual
branch resolution happens in `advancePC()`.

Fix: moved all PC management to `PCUpdate`:
- `PCUpdate` saves PC before `advancePC()`
- Calls `staticInst->advancePC()` (handles both sequential and branch)
- Compares: if `pcAfter != pcBefore + instSize`, branch was taken
- `ALUExecute` no longer detects branches at all

Added `staticInst` and `pc` fields to `ExecResult` so `PCUpdate`
can call `advancePC()`.

#### 2. Pipeline successfully loops!

The test program (`mov x0, #10; subs x0, x0, #1; bne loop`)
now executes correctly:

```
Cycle  2: movz x0, #10       → PC: 0x4000d4 → 0x4000d8
Cycle  4: subs x0, x0, #1    → PC: 0x4000d8 → 0x4000dc
Cycle  6: b.ne → branch taken to 0x4000d8
Cycle  8: subs x0, x0, #1    → sequential
Cycle 10: b.ne → branch taken to 0x4000d8
... repeats correctly for multiple iterations ...
```

Branch detection works:
```
PCUpdate: branch taken to 0x4000d8 (was 0x4000dc, expected next 0x4000e0)
```

The loop body (`subs` + `bne`) takes 4 cycles per iteration
(2 cycles per instruction due to 2-stage pipeline + icache latency).

### Remaining bugs

#### Bug 1: notify() cascade (still open)

Same-stage `notify()` fires `FetchMemRequest::compute()` mid-way
through `FetchAddressGen::compute()` when MMU translates
synchronously. Doesn't cause incorrect behavior but makes execution
order confusing.

#### Bug 2: lastFetchedPC stall (partially resolved)

The pipeline no longer stalls permanently — it stalls for 1 extra
cycle when the new PC is in the same fetch line, then the next
`tick()` sees a different PC and proceeds. This adds 1 wasted
cycle per instruction within the same line. Acceptable for now.

#### Bug 5 (new): No syscall handling → program runs past exit

After the loop completes (x0 reaches 0, `bne` falls through),
the program executes `mov x0, #0; mov x8, #93; svc #0`. But
`svc` (supervisor call) isn't handled — `ALUExecute` just
executes it without trapping. The PC keeps advancing past the
program into unmapped memory, eventually crashing at `0x401000`
with a page fault (`getPaddr()` assertion).

The pipeline ran ~988 instructions over ~1978 cycles before
crashing, showing it works far beyond a single loop iteration.

#### Bug 6 (new): Undefined instructions past program end

After running past the `svc`, the CPU decodes zeroed-out memory
as `unknown (inst 0x000000)` and gets `Undefined Instruction`
faults from `execute()`, but keeps going because faults aren't
handled.

---

## 2026-04-04 ~15:00: Cross-Stage Timing, Local Latching, Debug Flags

### What was done

#### 1. Separated debug flags

Created distinct debug flags for different subsystems:
- `LegoCPU` — top-level CPU events (tick, wiring, scheduling)
- `LegoCPULineTrace` — cycle-by-cycle pipeline trace
- `LegoCPUFunc` — stage function compute details
- `LegoCPUPort` — port read/write/notify (reserved)
- `LegoCPUAll` — compound flag enabling all of the above

Files changed: `SConscript`, `lego_cpu.cc`, `fetch_functions.cc`,
`execute_functions.cc`.

#### 2. Stall cycle backfill in line trace

When the pipeline is idle (waiting for icache), no ticks fire.
The line trace now backfills missing cycles using the last trace
state, marked with `(stall)`. Uses `lastTraceLine` saved at end
of each tick, and calculates skipped cycles from tick difference.

#### 3. Memory latency investigation

Discovered gem5's `PacketQueue::schedSendEvent()` adds +1 tick
(`when = std::max(when, curTick() + 1)`) — a known gem5 quirk.
With 1000ps SimpleMemory latency, response arrives at tick 1001
instead of 1000, missing the clock edge. Fixed by using 999ps
latency to compensate.

Also configured `NoncoherentXBar` with zero latencies and infinite
bandwidth `SimpleMemory` for minimal overhead testing.

#### 4. Separate update event for async responses

Created `updateEvent` separate from `tickEvent`:
- `tickEvent` — fires at cycle boundaries, advances cycle counter,
  calls `latchInputs()` then `compute()`, prints trace
- `updateEvent` — fires mid-cycle on async response, only schedules
  `tickEvent` for next cycle (does NOT recompute stages)
- `notifyUpdate()` schedules `updateEvent`, which then schedules
  `tickEvent` for `clockEdge(Cycles(1))`

Previous bug: `notifyUpdate()` was rescheduling `tickEvent` at
`curTick()`, causing `tick()` to re-run and advance the cycle
counter incorrectly.

#### 5. Local input latching for cross-stage timing

Added local data copies (`localFetchLine`, `localRedirect`) to
functions that read cross-stage inputs. Timing is controlled by
**when** the copy happens:

- **Cross-stage**: `latchInputs()` called at start of `tick()`,
  copies from source output into local variable
- **Same-stage**: updated immediately in `compute()` when
  `getWriterStageId() == getStageId()`

Functions read from local copies, not directly from input ports.
This enforces 1-cycle cross-stage latency without timestamp checks.

Added `latchInputs()` virtual to `StageFunction`, propagated
through `SubStage::latchInputs()` → `Stage::latchInputs()`.

`tick()` flow:
```
for each stage: stage->latchInputs()  // copy cross-stage data
for each stage: stage->compute()      // use local copies
```

Files changed: `stage_function.hh`, `fetch_functions.hh/cc`,
`execute_functions.hh/cc`, `sub_stage.hh/cc`, `stage.hh/cc`,
`lego_cpu.cc`.

#### 6. Fixed FetchAddr.pc to carry instruction PC, not aligned addr

`FetchAddressGen::translationComplete()` was storing
`req->getVaddr()` (the aligned fetch address) as `FetchAddr.pc`.
But downstream needs the actual instruction PC for decoder offset
calculation. Fixed to use `tc->pcState().instAddr()`.

`InstructionDecode` now uses `line.pc` instead of
`tc->pcState().instAddr()` for offset calculation, since
ThreadContext PC may have been advanced by `PCUpdate` already.

#### 7. FetchAddressGen waits for PCUpdate

`FetchAddressGen` now waits for `PCUpdate`'s `Redirect` signal
before fetching the next instruction (except for the very first
fetch). This prevents fetch from running ahead of execute.

Result: 2 CPI (non-speculative). Correct for a non-pipelined
2-stage CPU, but not for a real pipelined in-order CPU which
would speculatively advance `PC += 4`.

#### 8. isCurrentCycle() chain

Added `isCurrentCycle(Tick)` through the hierarchy:
`StageFunction → SubStage → Stage → LegoCPU`.
Check: `tick >= cycleStartTick` (written during/after this cycle).
Moved implementations to `.cc` files to avoid incomplete type
errors with forward declarations.

#### 9. Output::write() records writerStageId

`Output::write()` now stores `writerStageId = _stageId` and
passes it to `Input::notify(writer_stage_id, written_at)`.
`Input` stores `lastWriterStageId` and `lastWrittenTick` for
functions to check in `compute()`.

### Current pipeline trace (with 999ps SimpleMemory, 1GHz clock)

```
Cycle  1 | F:0x4000d4,F:wait(icache) | -                |
Cycle  2 | (stall — icache latency)
Cycle  3 | F:0x4000d4,F:got(0x4000d4) | D:movz x0,#10,X:seq1,PC:0x4000d8 |
Cycle  4 | F:0x4000d8,F:got(0x4000d8) | D:movz (stale, seq1)             |
Cycle  5 | F:0x4000d8,F:got(0x4000d8) | D:subs x0,x0,#1,X:seq2,PC:0x4000dc |
Cycle  6 | F:0x4000dc,F:got(0x4000dc) | D:subs (stale, seq2)             |
Cycle  7 | F:0x4000dc,F:got(0x4000dc) | D:b.ne 0x4000d8,X:seq3,PC:redir->0x4000d8 |
Cycle  8 | F:0x4000d8,F:got(0x4000d8) | D:b.ne (stale, seq3)            |
Cycle  9 | F:0x4000d8,F:got(0x4000d8) | D:subs x0,x0,#1,X:seq4          |
  ... loop repeats at 2 CPI ...
Cycle 41 | F:0x4000d8                 | D:subs x0,x0,#1,X:seq20         |
Cycle 42 | F:0x4000dc                 | D:subs (stale)                   |
Cycle 43 | F:0x4000dc                 | D:b.ne,X:seq21,PC:0x4000e0      | ← falls through!
Cycle 44 | F:0x4000e0,F:wait(icache)  | D:b.ne (stale)                  |
Cycle 45 | (stall — new fetch line)
Cycle 46 | F:0x4000e0,F:got(0x4000e0) | D:mov x0,#0,X:seq22             |
Cycle 47 | F:0x4000e4                 | D:mov (stale)                    |
Cycle 48 | F:0x4000e4                 | D:mov x8,#93,X:seq23             |
Cycle 49 | F:0x4000e8                 | D:mov (stale)                    |
Cycle 50 | F:0x4000e8                 | D:svc #0,X:seq24                 | ← not handled
  ... runs past program end ...
```

### Analysis: correctness vs real hardware

The pipeline is **functionally correct** — all instructions execute
with the right results (x0 decrements from 10 to 0, branch taken
9 times, falls through on the 10th).

However, the **2 CPI** is not representative of a real pipelined
2-stage in-order CPU, which achieves **1 IPC** for sequential code
by overlapping fetch and execute:

```
Real 2-stage pipelined CPU:
Cycle 1: Fetch A      | -
Cycle 2: Fetch B      | Execute A    ← overlap
Cycle 3: Fetch C      | Execute B    ← 1 IPC
```

Our CPU:
```
Cycle 1: Fetch A      | -
Cycle 2: -            | Execute A    ← no overlap
Cycle 3: Fetch B      | -            ← waits for PC from execute
Cycle 4: -            | Execute B    ← 2 CPI
```

The difference: a real CPU **speculatively** fetches `PC + 4`
while execute runs. Only branch mispredictions cause stalls.
Our CPU **waits** for `PCUpdate` to provide the next PC.

### What still needs to be done

1. **Speculative fetch (`PC + 4`)** — `FetchAddressGen` should
   advance PC speculatively without waiting for `PCUpdate`.
   `PCUpdate` redirect only needed for branches.

2. **Handle syscalls (svc)** — detect in ALUExecute, invoke
   gem5 SE-mode syscall emulation for program exit.

3. **Handle faults** — stop pipeline on undefined instructions.

4. **Clean up debug DPRINTFs** — remove verbose debug prints
   from `FetchAddressGen::compute()` etc.
