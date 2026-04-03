# First Attempt: Minimal Lego CPU — 04/03/2026

Goal: Build the simplest possible Lego CPU that boots and runs a tight loop
(`subs r0, #1; bne loop`) on the existing gem5 memory system. Two stages,
no scoreboard, no data memory, no branch prediction. Just fetch + execute.

---

## Target Pipeline

```
Stage 1 (Fetch):
    SubStage("fetch", [
        FetchAddressGen,
        FetchMemRequest,
        FetchMemResponse,
    ])

Stage 2 (Execute):
    SubStage("execute", [
        InstructionDecode,
        ALUExecute,
        BranchResolve,
        PCUpdate,
        InOrderRetire,
    ])
```

## Test Program

```asm
    mov r0, #10
loop:
    subs r0, r0, #1
    bne loop
    // done — semihosting exit
```

Success = program runs to completion with correct cycle count.

---

## Step-by-Step Plan

### Step 1: Directory Structure and Build Files

Create the directory layout and SConscript so gem5 builds the new CPU.

```
src/cpu/lego/
    SConscript
    LegoCPU.py              # Python SimObject
    lego_cpu.hh             # LegoCPU class declaration
    lego_cpu.cc             # LegoCPU implementation
    stage.hh                # Stage class
    stage.cc                # Stage implementation
    sub_stage.hh            # SubStage class
    stage_function.hh       # StageFunction base class
    in_flight_inst.hh       # InFlightInst shared struct
    DESIGN.md               # (already exists)
```

**Deliverable:** `scons build/ARM/gem5.opt` compiles with empty stubs.

### Step 2: InFlightInst Shared Structure

Define the minimal shared instruction structure.

```cpp
struct InFlightInst {
    InstSeqNum seqNum;
    Addr pc;

    // Stage tracking
    unsigned currentStage;
    bool squashed;
    bool completed;

    // Fetch
    PacketPtr fetchPacket;
    bool fetchComplete;

    // Decode
    StaticInstPtr staticInst;

    // Execute
    Fault fault;

    // Branch
    std::unique_ptr<PCStateBase> pcBefore;  // saved before execute
    bool branchTaken;
};
```

**Deliverable:** Header compiles. No logic yet.

### Step 3: StageFunction Base Class

Define the abstract interface that all function units implement.

```cpp
class StageFunction {
  public:
    virtual ~StageFunction() = default;

    // Compute results. Read shared state, write to InFlightInst.
    // Return true if produced new output.
    virtual bool execute(InFlightInst &inst) = 0;

    // Commit side effects to shared state (ThreadContext, ports).
    // Only called after all evaluate/re-evaluate settle.
    virtual void post() {}

    // Clear pending side effects (called before re-evaluate).
    virtual void clearPending() {}
};
```

**Deliverable:** Header compiles. No concrete implementations yet.

### Step 4: SubStage and Stage Classes

SubStage: ordered list of StageFunctions, runs them sequentially.

```cpp
class SubStage {
    std::string name;
    std::vector<StageFunction *> functions;
  public:
    bool evaluate(InFlightInst &inst);
    void post();
    void clearPending();
};
```

Stage: contains SubStages, manages evaluate/post/reEvaluate cycle.

```cpp
class Stage {
    std::string name;
    unsigned stageId;
    std::vector<SubStage *> subStages;
  public:
    void evaluate(std::list<InFlightInst> &insts);
    void post();
    void reEvaluate(std::list<InFlightInst> &insts);
    void rePost();
};
```

**Deliverable:** Classes compile. evaluate() iterates shared inst list,
filters by `inst.currentStage == stageId`, calls SubStage::evaluate().

### Step 5: LegoCPU — BaseCPU Skeleton

Inherit from BaseCPU. Set up:
- icachePort (for fetch)
- ThreadContext
- Pipeline tick event
- Shared instruction list

```cpp
class LegoCPU : public BaseCPU {
    EventFunctionWrapper tickEvent;
    std::list<InFlightInst> inFlightInsts;
    std::vector<Stage *> stages;
    InstSeqNum nextSeqNum;

    // ICache port (like MinorCPU)
    class IcachePort : public RequestPort { ... };
    IcachePort icachePort;

    void tick();
    bool recvTimingResp(PacketPtr pkt);  // icache response
};
```

`tick()` implements the evaluation cycle:

```cpp
void LegoCPU::tick() {
    // Step 1: evaluate all stages
    for (auto *stage : stages)
        stage->evaluate(inFlightInsts);

    // Step 2: post all stages
    for (auto *stage : stages)
        stage->post();

    // Step 3: advance instructions (bump currentStage)
    for (auto &inst : inFlightInsts)
        if (inst.completed && inst.currentStage < numStages)
            inst.currentStage++;

    // Step 4: retire from head
    while (!inFlightInsts.empty() && head is retired)
        inFlightInsts.pop_front();

    numCycles++;  // always, including stalls
    schedule(tickEvent, nextCycle());
}
```

**Deliverable:** LegoCPU instantiates, connects to memory system,
ticks every cycle. Does nothing useful yet.

### Step 6: LegoCPU.py — Python SimObject

```python
class LegoCPU(BaseCPU):
    type = "LegoCPU"
    cxx_header = "cpu/lego/lego_cpu.hh"
    cxx_class = "gem5::LegoCPU"

    # For now, hardcode 2-stage pipeline.
    # Later: make stages configurable via VectorParam.
```

**Deliverable:** Can instantiate LegoCPU in a Python config script.
`m5.instantiate()` succeeds.

### Step 7: FetchAddressGen

First concrete StageFunction. Reads PC from ThreadContext,
produces a fetch address in the InFlightInst.

```cpp
class FetchAddressGen : public StageFunction {
    ThreadContext &tc;
  public:
    bool execute(InFlightInst &inst) override {
        inst.pc = tc.pcState().instAddr();
        inst.fetchAddr = alignToFetchSize(inst.pc);
        return true;
    }
};
```

Also creates a new InFlightInst entry and adds it to the shared list
(this is the "inject" point — only Fetch stage creates new entries).

**Deliverable:** Each tick, a new InFlightInst appears in the shared
list with the correct PC.

### Step 8: FetchMemRequest + FetchMemResponse

FetchMemRequest: creates a Packet, sends via icachePort in `post()`.

```cpp
void FetchMemRequest::post() {
    if (pendingPacket && icachePort.sendTimingReq(pendingPacket))
        pendingPacket = nullptr;
}
```

FetchMemResponse: checks if `inst.fetchComplete` is true (set by
`LegoCPU::recvTimingResp`). If yes, the fetch data is available.

`recvTimingResp` deposits the response and triggers Stage::reEvaluate():

```cpp
bool LegoCPU::recvTimingResp(PacketPtr pkt) {
    // Find inst, mark fetchComplete
    // Call stages[0]->reEvaluate() + rePost()
}
```

**Deliverable:** LegoCPU fetches instruction bytes from SimpleMemory.
Can see fetch requests/responses in the trace.

### Step 9: InstructionDecode

Uses gem5's existing Thumb decoder to decode raw bytes into a
StaticInstPtr. Stores in `inst.staticInst`.

```cpp
bool InstructionDecode::execute(InFlightInst &inst) {
    if (!inst.fetchComplete) return false;
    // Use gem5 decoder to decode inst.fetchData at inst.pc
    inst.staticInst = decoder.decode(machInst, inst.pc);
    return true;
}
```

**Deliverable:** Instructions are decoded. Can see decoded instruction
names in DPRINTF trace.

### Step 10: ALUExecute

Calls `staticInst->execute()` using an ExecContext.

```cpp
bool ALUExecute::execute(InFlightInst &inst) {
    if (!inst.staticInst) return false;
    inst.pcBefore = tc.pcState().clone();
    ExecContext ctx(cpu, tc, inst);
    inst.fault = inst.staticInst->execute(&ctx, nullptr);
    return true;
}
```

**Deliverable:** `subs r0, #1` executes, r0 decrements.

### Step 11: BranchResolve + PCUpdate

BranchResolve: compares PC before and after execute. If different,
produces a redirect.

```cpp
bool BranchResolve::execute(InFlightInst &inst) {
    auto target = tc.pcState().clone();
    inst.staticInst->advancePC(*target);
    if (*inst.pcBefore != *target) {
        inst.branchTaken = true;
        inst.resolvedTarget = target->instAddr();
        // Signal redirect to fetch stage
    }
    return true;
}
```

PCUpdate: writes the resolved PC to ThreadContext in `post()`.

**Deliverable:** `bne loop` correctly redirects fetch to loop target.
The program loops.

### Step 12: InOrderRetire

Marks instruction as retired. Removes from head of shared list.
Only retires if the instruction is at the commit stage and completed.

**Deliverable:** Instructions flow through the pipeline and retire.
No memory leak (inFlightInsts doesn't grow unbounded).

### Step 13: End-to-End Test

Run the test program:

```python
system = System()
system.cpu = LegoCPU()
system.mem = SimpleMemory(range=AddrRange(0x08000000, size='256KiB'))
# Load test binary
# Run simulation
# Verify: program exits, cycle count is reasonable
```

**Deliverable:** The test loop runs to completion. Semihosting exit
triggers simulation end. Cycle count matches expectation
(~10 × pipeline_depth for 10 iterations).

---

## What This Does NOT Include (for later)

- [ ] Python-configurable stages (hardcoded 2-stage for now)
- [ ] Typed port connections (data flows through InFlightInst directly)
- [ ] Data memory (LoadAddressGen, StoreAddressGen, MemResponse)
- [ ] Scoreboard / dependency tracking
- [ ] Branch prediction
- [ ] Multi-cycle functional units (Multiply, Divide, FPU)
- [ ] Multiple SubStages per Stage
- [ ] Re-evaluate from branch redirect (redirect is 1-cycle penalty for now)
- [ ] Interrupt handling
- [ ] Multi-threading
- [ ] Configurable fetch width / line buffer
- [ ] DCode port

---

## File Dependency Order

Build in this order to keep each step compilable:

```
Step 1:  SConscript, directory
Step 2:  in_flight_inst.hh
Step 3:  stage_function.hh
Step 4:  sub_stage.hh, stage.hh, stage.cc
Step 5:  lego_cpu.hh, lego_cpu.cc  (skeleton)
Step 6:  LegoCPU.py
         --- At this point: compiles, instantiates, ticks, does nothing ---
Step 7:  fetch_address_gen.hh
Step 8:  fetch_mem_request.hh, fetch_mem_response.hh
         --- At this point: fetches from memory ---
Step 9:  instruction_decode.hh
Step 10: alu_execute.hh
Step 11: branch_resolve.hh, pc_update.hh
Step 12: in_order_retire.hh
         --- At this point: runs the test program ---
Step 13: test config script
```
