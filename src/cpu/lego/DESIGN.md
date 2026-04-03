# Lego CPU: Configurable In-Order Pipeline

A configurable in-order CPU model for gem5 where pipeline stages are assembled
from composable functions, like building blocks (legos). Stages are defined in
Python. All in-flight instructions live in a shared structure; stages operate
on instructions at their stage via counters.

---

## gem5 Integration: BaseCPU Resources

LegoCPU inherits from BaseCPU, which provides shared hardware resources.
StageFunction units that need hardware access get references at construction
time. The resources are owned by the CPU; functions just use them.

### Resource Mapping

| BaseCPU Resource | StageFunction Users | How Accessed |
|---|---|---|
| `icachePort` | `FetchMemRequest`, `FetchMemResponse` | Send fetch requests, receive responses |
| `dcachePort` | `LoadAddressGen`, `StoreAddressGen`, `MemResponse` | Send load/store requests, receive responses |
| `MMU` (iTLB) | `FetchMemRequest` | Translate fetch VA → PA before ICode request |
| `MMU` (dTLB) | `LoadAddressGen`, `StoreAddressGen` | Translate data VA → PA before DCode request |
| `BranchPredictor` | `BranchPredict`, `BranchResolve` | Predict: lookup. Resolve: update/squash history |
| `ThreadContext` | `RegisterWriteback`, `PCUpdate`, `BranchResolve` | Read/write arch register file, PC state |
| `InterruptController` | `InterruptCheck` | Sample pending interrupts |

### How It Works

```cpp
class LegoCPU : public BaseCPU
{
    // Inherited from BaseCPU:
    //   RequestPort icachePort;
    //   RequestPort dcachePort;
    //   BaseMMU *mmu;
    //   branch_prediction::BPredUnit *branchPred;
    //   ThreadContext *tc;
    //   InterruptController *interrupts;

    // Pipeline structure (built from Python config):
    std::vector<Stage *> stages;

    // Shared instruction list:
    std::list<InFlightInst> inFlightInsts;
};
```

StageFunction units receive resource references during construction:

```python
# Python configuration — resources bound automatically
Pipeline([
    Stage("fetch", [
        SubStage("fetch", [
            FetchAddressGen(),
            FetchMemRequest(),    # automatically gets icachePort, iTLB
            FetchMemResponse(),   # automatically gets icachePort
            FetchLineBuffer(),
        ]),
        SubStage("predict", [
            BranchDetect(),
            BranchPredict(),      # automatically gets branchPredictor
        ]),
    ]),
    Stage("execute", [
        SubStage("alu", [
            ALUExecute(),
        ]),
        SubStage("mem", [
            LoadAddressGen(),     # automatically gets dcachePort, dTLB
            StoreAddressGen(),    # automatically gets dcachePort, dTLB
            MemResponse(),        # automatically gets dcachePort
        ]),
        SubStage("commit", [
            BranchResolve(),      # automatically gets branchPredictor
            RegisterWriteback(),  # automatically gets ThreadContext
            PCUpdate(),           # automatically gets ThreadContext
            InOrderRetire(),
        ]),
    ]),
])
```

In C++, each StageFunction declares what resources it needs:

```cpp
class FetchMemRequest : public StageFunction
{
    RequestPort &icachePort;  // ref to BaseCPU::icachePort
    BaseMMU &mmu;             // ref to BaseCPU::mmu (iTLB)

  public:
    FetchMemRequest(RequestPort &port, BaseMMU &mmu)
        : icachePort(port), mmu(mmu) {}

    bool execute(InFlightInst &inst) override
    {
        // Translate VA → PA via iTLB
        // Send request via icachePort
    }
};

class BranchPredict : public StageFunction
{
    branch_prediction::BPredUnit &predictor;

  public:
    BranchPredict(branch_prediction::BPredUnit &bp)
        : predictor(bp) {}

    bool execute(InFlightInst &inst) override
    {
        // Lookup predictor for branch direction + target
    }
};

class BranchResolve : public StageFunction
{
    branch_prediction::BPredUnit &predictor;

  public:
    BranchResolve(branch_prediction::BPredUnit &bp)
        : predictor(bp) {}

    bool execute(InFlightInst &inst) override
    {
        // Compare prediction with actual result
        // Update/squash predictor history
        // Produce redirect if mispredicted
    }
};
```

### Memory Response Handling

When `icachePort` or `dcachePort` receives a response (via `recvTimingResp`),
LegoCPU deposits it into the shared InFlightInst and triggers re-evaluate
on the appropriate stage:

```cpp
bool LegoCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    // Find the InFlightInst this response belongs to
    InFlightInst &inst = findByFetchPacket(pkt);
    inst.fetchPacket = pkt;
    inst.fetchComplete = true;

    // Trigger re-evaluate on the stage containing FetchMemResponse
    Stage *fetchStage = findStageContaining<FetchMemResponse>();
    fetchStage->reEvaluate();
    fetchStage->rePost();

    return true;
}

bool LegoCPU::DcachePort::recvTimingResp(PacketPtr pkt)
{
    InFlightInst &inst = findByDataPacket(pkt);
    inst.dataPacket = pkt;
    inst.memComplete = true;

    Stage *memStage = findStageContaining<MemResponse>();
    memStage->reEvaluate();
    memStage->rePost();

    return true;
}
```

This eliminates the event priority problem from MinorCPU — the response
is deposited directly and the stage re-evaluates immediately, all within
the same cycle.

---

## Typed Port System

Every input and output has a concrete data type. When connecting ports in
Python, the types must match or the configuration fails with a clear error.
This prevents wiring a `FetchLine` output to an `ExecResult` input.

### Data Types

```cpp
// --- Fetch types ---
struct FetchAddr {
    Addr pc;
    Addr aligned_addr;
    unsigned size;
    InstSeqNum seqNum;
};

struct FetchLine {
    Addr baseAddr;
    uint8_t data[4];       // 32-bit ICode bus width
    unsigned validBytes;
    InstSeqNum seqNum;
};

// --- Decode types ---
struct DecodedInst {
    StaticInstPtr staticInst;
    Addr pc;
    RegId srcRegs[4];
    RegId dstRegs[2];
    unsigned numSrcs;
    unsigned numDsts;
    OpClass opClass;
    InstSeqNum seqNum;
};

// --- Execute types ---
struct ExecResult {
    RegVal result;
    RegVal flags;          // condition codes
    InstSeqNum seqNum;
    bool isBranch;
};

// --- Memory types ---
struct MemRequest {
    Addr addr;
    unsigned size;
    bool isLoad;
    RegVal storeData;      // for stores
    InstSeqNum seqNum;
};

struct MemResponse {
    RegVal data;
    bool completed;
    InstSeqNum seqNum;
};

// --- Control types ---
struct Redirect {
    Addr target;
    InstSeqNum seqNum;     // squash everything younger
    bool valid;
};

struct StallSignal {
    bool stalled;
};

struct ScoreboardQuery {
    RegId reg;
    bool busy;             // response: is this reg busy?
    InstSeqNum readyAt;    // which seqNum will produce it?
};

struct ScoreboardUpdate {
    RegId reg;
    bool busy;
    InstSeqNum producer;
};
```

### Typed Port Template

```cpp
template<typename T>
class DataInput {
    T value;
    bool valid = false;
  public:
    const T &read() const { assert(valid); return value; }
    bool hasData() const { return valid; }
    void deposit(const T &v) { value = v; valid = true; }
    void clear() { valid = false; }

    using Type = T;  // for connection type checking
};

template<typename T>
class DataOutput {
    T value;
    bool valid = false;
  public:
    void write(const T &v) { value = v; valid = true; }
    bool hasData() const { return valid; }
    const T &peek() const { return value; }
    void clear() { valid = false; }

    using Type = T;  // for connection type checking
};
```

### Connection Type Checking

In Python, when connecting ports, types are verified:

```python
# This works — both are FetchLine type:
Connect("fetch.line_out", "decode.line_in")

# This FAILS at config time — type mismatch:
Connect("fetch.line_out", "execute.result_in")
# Error: Cannot connect FetchLine output to ExecResult input

# Control connections are also typed:
Connect("execute.redirect_out", "fetch.redirect_in")  # both Redirect type ✓
Connect("execute.redirect_out", "fetch.stall_in")      # Redirect ≠ StallSignal ✗
```

In C++, the connection logic verifies at construction:

```cpp
template<typename T>
void connect(DataOutput<T> &src, DataInput<T> &dst)
{
    // Type T must match — enforced by template
    // Register: when src writes, dst gets the value
}
```

### StageFunction Port Declarations

Each StageFunction declares its ports with explicit types:

```cpp
class FetchAddressGen : public StageFunction
{
  public:
    // Data
    DataOutput<FetchAddr>    addr_out;

    // Control
    ControlInput<Redirect>   redirect_in;   // from branch resolve
    ControlInput<StallSignal> stall_in;     // from downstream
};

class FetchMemRequest : public StageFunction
{
  public:
    // Data
    DataInput<FetchAddr>     addr_in;       // from FetchAddressGen
    // (no data output — sends to icachePort directly)

    // Control
    ControlOutput<StallSignal> stall_out;   // if bus rejects request
};

class FetchMemResponse : public StageFunction
{
  public:
    // Data
    DataInput<MemResponse>   resp_in;       // async from icachePort
    DataOutput<FetchLine>    line_out;      // to FetchLineBuffer
};

class InstructionDecode : public StageFunction
{
  public:
    DataInput<FetchLine>     line_in;       // from fetch stage
    DataOutput<DecodedInst>  inst_out;      // to issue/execute
};

class ALUExecute : public StageFunction
{
  public:
    DataInput<DecodedInst>   inst_in;
    DataOutput<ExecResult>   result_out;
    ControlInput<StallSignal> stall_in;
};

class BranchResolve : public StageFunction
{
  public:
    DataInput<DecodedInst>   inst_in;
    DataOutput<ExecResult>   result_out;
    ControlOutput<Redirect>  redirect_out;  // backward to fetch
};

class LoadAddressGen : public StageFunction
{
  public:
    DataInput<DecodedInst>   inst_in;
    DataOutput<MemRequest>   mem_req_out;   // to dcachePort
};

class MemResponseFunc : public StageFunction
{
  public:
    DataInput<MemResponse>   resp_in;       // async from dcachePort
    DataOutput<ExecResult>   result_out;    // load result
};

class RegisterWriteback : public StageFunction
{
  public:
    DataInput<ExecResult>    result_in;
    ControlOutput<ScoreboardUpdate> scoreboard_out;
    // ThreadContext is written in post(), NOT in execute()
};
```

---

## Shared State Update Rule

**Critical invariant:** shared mutable state (ThreadContext, register file,
PC, scoreboard) is ONLY written during a stage's `post()`, NEVER during
`evaluate()`.

### Why

During `evaluate()`, a stage computes results and stores them locally.
If a re-evaluate is triggered (memory response, branch redirect), the
stage re-computes. If `evaluate()` had already written to ThreadContext,
the re-evaluate would see the wrong state.

By deferring all shared writes to `post()`:
- `evaluate()` is pure computation (no side effects on shared state)
- `re-evaluate()` just re-runs evaluate() — safe, no cleanup needed
- `post()` writes the FINAL result to shared state
- `re-post()` overwrites with the corrected result — clean

### What Goes Where

```
evaluate():
    ✓ Read from shared state (ThreadContext, scoreboard, register file)
    ✓ Read from input ports (data from previous stage)
    ✓ Compute results, store in LOCAL stage variables
    ✓ Write to output data ports (for downstream stage next cycle)
    ✗ DO NOT write to ThreadContext
    ✗ DO NOT update scoreboard
    ✗ DO NOT update PC
    ✗ DO NOT send memory requests (side effect)

post():
    ✓ Write to ThreadContext (register writeback)
    ✓ Update scoreboard (mark regs busy/free)
    ✓ Update PC (branch target or sequential)
    ✓ Send memory requests (fetch, load, store)
    ✓ Update branch predictor (train on resolve)
    ✓ Publish results for next stage to read next cycle
```

### Example: RegisterWriteback

```cpp
class RegisterWriteback : public StageFunction
{
    // Local storage — written during evaluate, committed during post
    RegId pendingDstReg;
    RegVal pendingResult;
    bool hasPendingWrite = false;

  public:
    bool execute(InFlightInst &inst) override
    {
        // Compute what to write, but DON'T write yet
        pendingDstReg = inst.dstRegs[0];
        pendingResult = inst.result;
        hasPendingWrite = true;
        return true;
    }

    void post(ThreadContext &tc) override
    {
        // NOW write to shared state
        if (hasPendingWrite) {
            tc.setReg(pendingDstReg, pendingResult);
            hasPendingWrite = false;
        }
    }
};
```

### Example: FetchMemRequest

```cpp
class FetchMemRequest : public StageFunction
{
    PacketPtr pendingRequest = nullptr;

  public:
    bool execute(InFlightInst &inst) override
    {
        // Compute the request, but DON'T send yet
        pendingRequest = createFetchPacket(inst.fetchAddr);
        return true;
    }

    void post(RequestPort &icachePort) override
    {
        // NOW send the request
        if (pendingRequest) {
            if (icachePort.sendTimingReq(pendingRequest))
                pendingRequest = nullptr;
            // If rejected, keep for next cycle retry
        }
    }
};
```

### Re-evaluate Safety

```
Cycle N:
  evaluate(): RegisterWriteback computes pendingResult = 42
  post():     tc.setReg(r3, 42)  ← committed to ThreadContext

  Mid-cycle: branch redirect! re-evaluate triggered on this stage
  re-evaluate(): RegisterWriteback now sees squashed inst
                 pendingResult = <nothing>, hasPendingWrite = false
  re-post():     nothing written to tc  ← previous write is WRONG

  Problem: tc already has r3=42 from the first post()!
```

Solution: `post()` for downstream stages (commit/writeback) should
only run ONCE at the end of the cycle, after all re-evaluates settle.
Or: `post()` uses a "commit list" that is cleared on re-evaluate:

```
evaluate():   commitList.add(r3 = 42)
post():       for each in commitList: tc.setReg(...)
re-evaluate(): commitList.clear()  ← wipes the pending write
               commitList.add(...)  ← adds corrected writes (if any)
re-post():    for each in commitList: tc.setReg(...)  ← correct values only
```

---

## Data and Control Paths

Every level (StageFunction, SubStage, Stage) has explicit **data ports** and
**control ports**. These are connected in Python configuration, making the
pipeline's wiring visible and configurable.

- **Data path**: instruction data flowing through the pipeline
  (fetch bytes, decoded operands, results, memory responses)
- **Control path**: signals that affect pipeline behavior
  (stall, squash, redirect, branch prediction, scoreboard status)

### Port Types

```
DataPort:
  - Typed (e.g., FetchData, DecodedInst, ExecResult, MemRequest)
  - Direction: input or output
  - Connected between producer and consumer

ControlPort:
  - Typed (e.g., Stall, Redirect, Squash, ScoreboardQuery)
  - Direction: input or output
  - Can cross stage boundaries (backward signals)
```

### Connection Levels

**Level 1: StageFunction ports → SubStage internal wiring**

Each StageFunction declares what data/control ports it needs. The SubStage
connects them internally — output of one function feeds input of the next.

```python
# Inside a SubStage, functions are wired in sequence:
SubStage("alu_path", [
    ScoreboardCheck(),   # ctrl_in: scoreboard_state
                         # ctrl_out: ready/stall
    FUDispatch(),        # data_in: decoded_inst (from ScoreboardCheck pass-through)
                         # ctrl_in: fu_availability
                         # data_out: fu_assignment
    ALUExecute(),        # data_in: fu_assignment + operands
                         # data_out: result + flags
])
# Automatic: ScoreboardCheck.data_out → FUDispatch.data_in → ALUExecute.data_in
# Automatic: if ScoreboardCheck stalls, FUDispatch and ALUExecute don't run
```

**Level 2: SubStage ports → Stage internal wiring**

A Stage has multiple SubStages. The Stage exposes the union of its SubStages'
external ports and connects SubStages that need to communicate.

```python
Stage("execute", [
    SubStage("alu", [...]),       # data_out: alu_result
    SubStage("mem", [...]),       # data_out: mem_result
    SubStage("commit", [...]),    # data_in: needs results from alu OR mem
], connections=[
    # alu.result → commit.result_in
    # mem.result → commit.result_in
    # commit.stall_out → alu.stall_in, mem.stall_in
])
```

**Level 3: Stage ports → Pipeline inter-stage wiring**

Stages are connected in the Pipeline. Data flows forward (1-cycle latency
via post/read). Control can flow backward (0-cycle via re-evaluate).

```python
Pipeline([
    Stage("fetch",   ...),  # data_out: fetch_line
    Stage("decode",  ...),  # data_in: fetch_line, data_out: decoded_inst
    Stage("execute", ...),  # data_in: decoded_inst
], connections=[
    # Forward data (through post mechanism, 1-cycle latency):
    Connect("fetch.data_out",   "decode.data_in"),
    Connect("decode.data_out",  "execute.data_in"),

    # Backward control (triggers re-evaluate, 0-cycle):
    Connect("execute.redirect_out",  "fetch.redirect_in"),
    Connect("decode.stall_out",      "fetch.stall_in"),
    Connect("execute.stall_out",     "decode.stall_in"),

    # Cross-stage control:
    Connect("execute.scoreboard_update", "execute.scoreboard_query"),  # within same stage
])
```

### Port Declaration in StageFunction

Each StageFunction declares its ports as part of its interface:

```cpp
class ALUExecute : public StageFunction
{
  public:
    // Data ports
    DataInput<DecodedInst>  inst_in;       // decoded instruction + operands
    DataOutput<ExecResult>  result_out;    // result value + flags

    // Control ports
    ControlInput<bool>      stall_in;      // stall from downstream
    ControlOutput<bool>     stall_out;     // stall to upstream (if multi-cycle)

    bool execute(InFlightInst &inst) override;
};

class BranchResolve : public StageFunction
{
  public:
    // Data ports
    DataInput<DecodedInst>  inst_in;
    DataOutput<ExecResult>  result_out;

    // Control ports — backward signal
    ControlOutput<Redirect> redirect_out;  // triggers re-evaluate upstream

    bool execute(InFlightInst &inst) override;
};

class FetchMemResponse : public StageFunction
{
  public:
    // Data ports
    DataInput<MemResp>      mem_resp_in;   // from memory system (async)
    DataOutput<FetchLine>   line_out;

    // This is an async input — when a response arrives mid-cycle,
    // it deposits into mem_resp_in and triggers re-evaluate.
    bool execute(InFlightInst &inst) override;
};
```

### How Connections Enable Re-evaluate

When a backward control signal fires (e.g., `execute.redirect_out`), the
pipeline traces the connection to find which stage it connects to
(`fetch.redirect_in`). That stage is marked for re-evaluate + re-post.

```
Execute produces redirect
  → Pipeline sees: redirect_out connected to fetch.redirect_in
  → Pipeline calls: fetch.reEvaluate()
  → Pipeline calls: fetch.rePost()
  → Next cycle: decode reads fetch's corrected post
```

When an async event arrives (memory response):
```
Memory response arrives
  → Pipeline deposits into FetchMemResponse.mem_resp_in
  → Pipeline sees: mem_resp_in is in Stage "fetch"
  → Pipeline calls: fetch.reEvaluate()
  → Pipeline calls: fetch.rePost()
  → Next cycle: decode reads fetch's corrected post
```

---

## Class Hierarchy

Three layers of abstraction, from leaf to root:

```
StageFunction          (leaf — one fixed operation)
    │
SubStage               (ordered sequence of StageFunctions)
    │
Stage                  (top level — owns sub-stages, handles inter-stage data)
```

### 1. StageFunction

The smallest building block. Each StageFunction does exactly one thing.
It reads from the shared instruction structure and writes back to it.

```cpp
class StageFunction
{
  public:
    // Called during evaluate. Reads/writes the shared InFlightInst.
    // Returns true if it produced new data (triggers re-evaluate
    // if this happens mid-cycle from an async event).
    virtual bool execute(InFlightInst &inst) = 0;

    // Can this function handle this instruction?
    // (e.g., ALUExecute only handles ALU ops, not loads)
    virtual bool canHandle(const InFlightInst &inst) = 0;
};
```

Examples: `FetchAddressGen`, `ALUExecute`, `BranchResolve`, etc.

### 2. SubStage

An ordered sequence of StageFunctions that process one instruction
through a specific path. Functions within a SubStage execute sequentially
in the same cycle. A SubStage represents one "lane" or "pipeline" within
a stage.

```cpp
class SubStage
{
    std::vector<StageFunction *> functions;  // ordered

  public:
    // Execute all functions in order on the given instruction.
    // Returns true if any function produced new data.
    bool evaluate(InFlightInst &inst)
    {
        bool changed = false;
        for (auto *func : functions) {
            if (func->canHandle(inst))
                changed |= func->execute(inst);
        }
        return changed;
    }
};
```

Examples:
- `SubStage("alu_path", [ScoreboardCheck, FUDispatch, ALUExecute])`
- `SubStage("mem_path", [LoadAddressGen, MemResponse])`
- `SubStage("fetch", [FetchAddressGen, FetchMemRequest, FetchMemResponse])`

### 3. Stage

The top-level unit. A Stage contains one or more SubStages that run in
parallel (each handles different instructions or different aspects of
the same instruction). The Stage is responsible for:

- **evaluate()**: run all sub-stages on instructions at this stage
- **post()**: publish results so the next stage can read them next cycle
- **re-evaluate()**: triggered when an async event (memory response,
  branch redirect) changes data mid-cycle
- **re-post()**: re-publish corrected results after re-evaluation

```cpp
class Stage
{
    std::vector<SubStage *> subStages;  // parallel lanes
    StageID stageId;

    // Data posted for the next stage to read next cycle.
    // Written during post(), read by next stage during evaluate().
    PostedData postedOutput;

    // Data read from previous stage (posted last cycle).
    const PostedData *inputFromPrevStage;

  public:
    // Phase 1: Compute results from current inputs.
    // Reads inputFromPrevStage (posted last cycle by upstream).
    // Stores results locally in the shared InFlightInst.
    void evaluate()
    {
        for each inst where inst.currentStage == stageId:
            for (auto *sub : subStages)
                sub->evaluate(inst);
    }

    // Phase 2: Publish results for downstream stage.
    // Called after ALL stages finish evaluate().
    void post()
    {
        postedOutput = <gather results from processed instructions>;
    }

    // Async trigger: something changed mid-cycle
    // (memory response arrived, branch resolved upstream).
    // Re-compute and re-publish so next cycle sees correct data.
    void reEvaluate()
    {
        evaluate();  // re-run with updated data
        rePost();    // re-publish corrected results
    }

    void rePost()
    {
        postedOutput = <gather updated results>;
    }
};
```

---

## Evaluation Flow Per Cycle

```
Cycle N:

  ┌─────────────────────────────────────────────────────────┐
  │ Step 1: All stages evaluate()                           │
  │                                                         │
  │   Stage 1 reads postedData from Stage 0 (last cycle)    │
  │   Stage 2 reads postedData from Stage 1 (last cycle)    │
  │   Stage 3 reads postedData from Stage 2 (last cycle)    │
  │   ...                                                   │
  │   Each stage computes results, stores in shared struct  │
  └─────────────────────────────────────────────────────────┘
                          │
                          ▼
  ┌─────────────────────────────────────────────────────────┐
  │ Step 2: All stages post()                               │
  │                                                         │
  │   Each stage publishes its results as postedData        │
  │   (Available for next stage to read NEXT cycle)         │
  └─────────────────────────────────────────────────────────┘
                          │
                          ▼
  ┌─────────────────────────────────────────────────────────┐
  │ Step 3: Mid-cycle updates (if any)                      │
  │                                                         │
  │   Memory response arrives:                              │
  │     → Deposit into shared InFlightInst                  │
  │     → Affected stage: reEvaluate() + rePost()           │
  │                                                         │
  │   Branch redirect produced (from downstream stage):     │
  │     → Upstream stage: reEvaluate() + rePost()           │
  │                                                         │
  │   (If no mid-cycle updates, skip this step)             │
  └─────────────────────────────────────────────────────────┘
                          │
                          ▼
  ┌─────────────────────────────────────────────────────────┐
  │ Step 4: Advance                                         │
  │                                                         │
  │   Instructions that completed their stage:              │
  │     inst.currentStage++                                 │
  │   Committed instructions: removed from shared list      │
  │   numCycles++ (always, including stalls)                │
  └─────────────────────────────────────────────────────────┘

Cycle N+1:
  Step 1: stages read the (possibly re-posted) data from Cycle N
```

### Re-evaluate Example: Memory Response

```
Cycle N:
  Step 1: Fetch stage evaluates.
          Instruction X is waiting for memory response.
          Response not here yet → Fetch stalls, posts "no data".

  Step 2: All stages post.

  Step 3: Memory response arrives for instruction X!
          → Deposit response into X's shared structure.
          → Fetch stage: reEvaluate()
            Now X has data → Fetch completes, produces instruction line.
          → Fetch stage: rePost()
            Posts the completed fetch line.

  Step 4: Advance. X moves to next stage.

Cycle N+1:
  Step 1: Decode reads Fetch's re-posted data → sees instruction line.
          No 1-cycle delay! The response was incorporated in the SAME cycle.
```

### Re-evaluate Example: Branch Redirect

```
Cycle N:
  Step 1: All stages evaluate normally.
          Fetch produces instruction A (sequential).
          Execute resolves a branch → MISPREDICTED.

  Step 2: All stages post.
          Execute posts: redirect to target T.

  Step 3: Fetch sees redirect from Execute.
          → Fetch: reEvaluate()
            Discards instruction A, generates fetch for target T.
          → Fetch: rePost()
            Posts the new fetch (for T) instead of A.

  Step 4: Advance.

Cycle N+1:
  Decode sees the fetch for target T (correct path).
  Only 1-cycle branch penalty (the instruction in Decode is wrong,
  but Fetch already corrected itself).
```

---

## Inter-Stage Data Posting

Stages communicate through posted data, not buffers. Each stage maintains:

- **inputFromPrevStage**: pointer to previous stage's postedOutput
  (read-only, set at pipeline construction time)
- **postedOutput**: this stage's results, readable by next stage next cycle

The posted data is NOT a copy — it is a reference/view into the shared
instruction list. "Posting" means marking which instructions are ready
for the next stage.

```
Stage 1 postedOutput: "instruction X fetch complete, data at inst[X]"
Stage 2 reads this:    looks up inst[X] in shared list, decodes it
Stage 2 postedOutput: "instruction X decoded, info at inst[X]"
Stage 3 reads this:    looks up inst[X], executes it
```

The shared instruction structure is the single source of truth.
Posted data is just a signal saying "I'm done with this instruction,
your turn."

---

## Pipeline Functions Reference

### Fetch

| Function | Reads | Produces | Notes |
|---|---|---|---|
| `FetchAddressGen` | PC, redirect, prediction | memory request (addr, size) | Generates next fetch address. Consumes redirects. |
| `FetchMemRequest` | fetch address | ICode bus request | Issues request to memory port. Handles bus stall. |
| `FetchMemResponse` | memory response packet | raw instruction bytes | Receives data from memory. Marks fetch complete. |
| `FetchLineBuffer` | raw bytes, PC alignment | aligned instruction(s) | Aligns variable-width instructions (Thumb 16/32). |

### Decode

| Function | Reads | Produces | Notes |
|---|---|---|---|
| `InstructionDecode` | raw bytes, PC | decoded instruction (opcode, regs, imm, flags) | Identifies instruction, extracts operands. Micro-op generation. |
| `BranchDetect` | decoded instruction, PC | branch type, target (if direct) | Identifies branches for early redirect or prediction. |
| `BranchPredict` | branch type, target, history | prediction, predicted target | BTB/predictor lookup. Redirect if predicted taken. |

### Issue

| Function | Reads | Produces | Notes |
|---|---|---|---|
| `ScoreboardCheck` | src/dst regs, scoreboard | ready/stall | Checks if source operands available. Stalls on dependency. |
| `FUDispatch` | decoded inst, FU availability | FU assignment, scoreboard reservation | Selects FU. Reserves dst register in scoreboard. |

### Execute

| Function | Reads | Produces | Notes |
|---|---|---|---|
| `ALUExecute` | decoded inst, operands | result, flags | Arithmetic/logic. 1-cycle. |
| `MultiplyExecute` | decoded inst, operands | result | 1-cycle (M4) or multi-cycle (M0). Configurable pipelined/not. |
| `DivideExecute` | decoded inst, operands | result | Multi-cycle, non-pipelined. |
| `FPUExecute` | decoded inst, FP operands | FP result | Multi-cycle, pipelined. |
| `BranchResolve` | branch, operands, flags | resolved target, redirect | Compares with prediction. Redirect + squash on mispredict. |
| `ShiftExecute` | decoded inst, operands | result | Barrel shifter. May be part of ALU. |

### Memory Access

| Function | Reads | Produces | Notes |
|---|---|---|---|
| `LoadAddressGen` | decoded inst, base reg, offset | DCode bus request (addr, size) | Computes effective address for loads. |
| `StoreAddressGen` | decoded inst, base reg, offset | DCode bus request (addr, size, data) | Computes effective address + store data. |
| `MemResponse` | memory response packet | load result value | Receives data from DCode bus. |

### Commit / Writeback

| Function | Reads | Produces | Notes |
|---|---|---|---|
| `RegisterWriteback` | result, dst reg | register file update | Writes to arch register file. |
| `PCUpdate` | branch result, next seq PC | new PC | Updates PC: sequential or branch target. |
| `InOrderRetire` | head of in-flight queue | retire signal, exception check | Retires in program order. Exception check before commit. |
| `ScoreboardClear` | retired inst's dst reg | scoreboard update | Frees register so dependents can issue. |

### Pipeline Control

| Function | Reads | Produces | Notes |
|---|---|---|---|
| `SquashControl` | redirect or exception | squash signal | Marks younger instructions squashed. Resets fetch. |
| `StallControl` | FU busy, scoreboard, memory | per-stage stall | Back-pressure propagation. Stall = upstream stalls. |
| `InterruptCheck` | interrupt controller, priority | exception redirect | Samples pending interrupts between instructions. |

---

## Example Configurations

### Cortex-M0 (3-stage, simple)

```python
Pipeline([
    Stage("fetch", [
        SubStage("fetch", [
            FetchAddressGen(),
            FetchMemRequest(),
            FetchMemResponse(),
            FetchLineBuffer(),
        ]),
    ]),
    Stage("decode", [
        SubStage("decode", [
            InstructionDecode(),
            BranchDetect(),
        ]),
    ]),
    Stage("execute", [
        SubStage("alu", [
            ALUExecute(),
            MultiplyExecute(latency=32, pipelined=False),
            BranchResolve(),
        ]),
        SubStage("mem", [
            LoadAddressGen(),
            StoreAddressGen(),
            MemResponse(),
        ]),
        SubStage("commit", [
            RegisterWriteback(),
            PCUpdate(),
            InOrderRetire(),
        ]),
    ]),
])
```

### Cortex-M4 (3-stage, with prediction)

```python
Pipeline([
    Stage("fetch", [
        SubStage("fetch", [
            FetchAddressGen(),
            FetchMemRequest(),
            FetchMemResponse(),
            FetchLineBuffer(),
        ]),
        SubStage("predict", [
            BranchDetect(),
            BranchPredict(),
        ]),
    ]),
    Stage("decode", [
        SubStage("decode", [
            InstructionDecode(),
        ]),
    ]),
    Stage("execute", [
        SubStage("issue_alu", [
            ScoreboardCheck(),
            FUDispatch(),
            ALUExecute(),
            MultiplyExecute(latency=1, pipelined=True),
        ]),
        SubStage("mem", [
            LoadAddressGen(),
            StoreAddressGen(),
            MemResponse(),
        ]),
        SubStage("commit", [
            BranchResolve(),
            RegisterWriteback(),
            PCUpdate(),
            InOrderRetire(),
            ScoreboardClear(),
        ]),
    ]),
])
```

### Cortex-M7 (6-stage)

```python
Pipeline([
    Stage("fetch1", [
        SubStage("fetch", [
            FetchAddressGen(),
            FetchMemRequest(),
            FetchMemResponse(),
        ]),
    ]),
    Stage("fetch2", [
        SubStage("line", [
            FetchLineBuffer(),
        ]),
        SubStage("predict", [
            BranchDetect(),
            BranchPredict(),
        ]),
    ]),
    Stage("decode", [
        SubStage("decode", [
            InstructionDecode(),
        ]),
    ]),
    Stage("issue", [
        SubStage("issue", [
            ScoreboardCheck(),
            FUDispatch(),
        ]),
    ]),
    Stage("execute", [
        SubStage("alu", [
            ALUExecute(),
            ShiftExecute(),
        ]),
        SubStage("mul", [
            MultiplyExecute(latency=2, pipelined=True),
        ]),
        SubStage("fpu", [
            FPUExecute(latency=4, pipelined=True),
        ]),
        SubStage("div", [
            DivideExecute(latency=12, pipelined=False),
        ]),
        SubStage("mem", [
            LoadAddressGen(),
            StoreAddressGen(),
        ]),
        SubStage("branch", [
            BranchResolve(),
        ]),
    ]),
    Stage("commit", [
        SubStage("mem_complete", [
            MemResponse(),
        ]),
        SubStage("writeback", [
            RegisterWriteback(),
            PCUpdate(),
            InOrderRetire(),
            ScoreboardClear(),
        ]),
    ]),
])
```

### Cortex-A5 class (8-stage)

```python
Pipeline([
    Stage("fetch1", [
        SubStage("fetch", [FetchAddressGen(), FetchMemRequest()]),
    ]),
    Stage("fetch2", [
        SubStage("fetch", [FetchMemResponse(), FetchLineBuffer()]),
        SubStage("predict", [BranchPredict()]),
    ]),
    Stage("decode", [
        SubStage("decode", [InstructionDecode(), BranchDetect()]),
    ]),
    Stage("issue", [
        SubStage("issue", [ScoreboardCheck(), FUDispatch()]),
    ]),
    Stage("execute", [
        SubStage("alu", [ALUExecute()]),
        SubStage("mul", [MultiplyExecute(latency=2, pipelined=True)]),
        SubStage("fpu", [FPUExecute(latency=4, pipelined=True)]),
        SubStage("mem", [LoadAddressGen(), StoreAddressGen()]),
    ]),
    Stage("memory1", [
        SubStage("mem", [MemResponse()]),
    ]),
    Stage("memory2", [
        SubStage("mem", [MemResponse()]),  # cache miss path
    ]),
    Stage("writeback", [
        SubStage("commit", [
            RegisterWriteback(), InOrderRetire(),
            ScoreboardClear(), PCUpdate(),
        ]),
    ]),
])
```

---

## Shared Instruction Structure

All in-flight instructions live in a shared list. Each entry contains:

```cpp
struct InFlightInst
{
    // Identity
    InstSeqNum seqNum;
    Addr pc;
    ThreadID threadId;

    // Stage tracking
    StageID currentStage;
    bool squashed;
    bool completed;

    // Fetch data
    PacketPtr fetchPacket;
    bool fetchComplete;

    // Decoded data
    StaticInstPtr staticInst;
    RegId srcRegs[];
    RegId dstRegs[];

    // Execution data
    RegVal operands[];
    RegVal result;
    Fault fault;

    // Branch data
    bool isBranch;
    bool predictedTaken;
    Addr predictedTarget;
    bool resolvedTaken;
    Addr resolvedTarget;

    // Memory data
    PacketPtr dataPacket;
    bool memComplete;

    // Functional unit
    FUIndex fuIndex;
    Cycles fuLatencyRemaining;
};
```
