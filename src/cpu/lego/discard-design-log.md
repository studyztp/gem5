# Discard Signal Design Log

Date: 2026-04-04

---

## Problem

With speculative fetch (PCUpdate-driven PC+4 advancement), the pipeline
produces wrong-path instructions after a taken branch. These instructions
flow through Decode and Execute before the branch redirect arrives at
Fetch. Without a mechanism to immediately clear the pipeline, we get:

1. Wrong-path instructions executing and corrupting register state
2. FetchMemRequest waiting for icache on a wrong-path address,
   blocking the redirect from reaching Fetch (deadlock)
3. Multi-cycle penalty as wrong-path data drains through the pipeline

### Current Trace (3-stage pipeline, bne taken)

```
Cycle  5: F:0x4000dc         | D:subs           | X:movz(seq1)      |
Cycle  6: F:0x4000e0(spec)   | D:bne            | X:subs(seq2)      |
Cycle  7: F:wait(icache 0xe0)| D:mov x0,#0(WRONG)| X:bne→redir 0xd8 |
Cycle  8: F:BLOCKED          | D:mov(WRONG)     | X:DISCARD(seq4)   |
Cycle  9: F:BLOCKED          | D:WRONG           | X:BLOCKED         |
  ... pipeline deadlocked because icache for 0xe0 blocks redirect ...
```

### Reference: Cortex-M4 Behavior [DDI0439D Table 3-1]

- Conditional branch NOT taken: 1 cycle (no penalty)
- Conditional branch TAKEN: 1 + P cycles (P = pipeline refill)
- Footnote c: "Conditional branch completes in a single cycle
  if the branch is not taken."

The M4 discards the instruction in Decode **immediately** in the
same cycle that Execute resolves the branch. There is no multi-cycle
drain — the pipeline is flushed instantly.

---

## Solution: Discard Signal

### Core Idea

Add a `bool discard` field to the `Redirect` data struct. When
PCUpdate detects a branch taken, it writes `redirect{discard=true}`.
This signal propagates through ALL port connections **immediately**
(ignoring stage boundaries and blocking) until it loops back to
PCUpdate itself.

### Key Properties

1. **Discard bypasses blocking.** `Output::write()` with `discard=true`
   ignores `isBlocked()` and always writes.

2. **Discard bypasses cross-stage latching.** `Input::notify()` with
   `discard=true` calls `compute()` immediately regardless of stageId.

3. **Discard propagates through the entire pipeline in one cycle.**
   Each function that receives discard clears its state, unblocks
   its ports, and propagates discard to its output.

4. **ALUExecute commits before discard propagates.** The branch
   instruction itself is REAL and must commit. Only the subsequent
   wrong-path instructions are discarded.

### Execution Order Within One Tick

```
1. ALUExecute::compute()
   → executes bne
   → writes ExecResult (REAL, not discard)

2. Same-stage notify → PCUpdate::compute()
   → processes ExecResult
   → advancePC() → detects branch taken
   → writes redirect{discard=true, target=0x4000d8}

3. Discard propagates immediately through all connections:

   redirect{discard=true}
     → FetchAddressGen: clears state, unblocks, uses target
       → writes FetchAddr{discard=true}
         → FetchMemRequest: abandons icache wait, clears, unblocks
           → writes FetchLine{discard=true}
             → InstructionDecode: clears local state, unblocks
               → writes DecodedInst{discard=true}
                 → ALUExecute: clears local state, unblocks
                   → writes ExecResult{discard=true}
                     → PCUpdate: sees its own discard → stops

4. FetchAddressGen starts fetching from redirect target (0x4000d8)
```

### Expected Trace After Fix

```
Cycle  5: F:0x4000dc         | D:subs           | X:movz(seq1)      |
Cycle  6: F:0x4000e0(spec)   | D:bne            | X:subs(seq2)      |
Cycle  7: F:0x4000d8(redir!) | D:BUBBLE         | X:bne→redir 0xd8  |
Cycle  8: F:0x4000dc(spec)   | D:subs           | X:BUBBLE           |
Cycle  9: F:0x4000e0(spec)   | D:bne            | X:subs             |
```

Branch penalty = 1 cycle (the BUBBLE at cycle 8's execute),
matching Cortex-M4's `1 + P` where P=1.

---

## Implementation Plan

### Step 1: Add `discard` to Redirect struct

```cpp
struct Redirect {
    InstSeqNum seqNum;
    Addr target;
    bool valid;
    bool discard;  // NEW
};
```

Other structs (FetchAddr, FetchLine, DecodedInst, ExecResult)
also get `bool discard = false`.

### Step 2: Modify Output::write() for discard

```cpp
bool write(const T &v) {
    if (v.discard) {
        // Discard bypasses blocking and value-unchanged checks
        value = v;
        valid = true;
        lastWritten = curTick();
        writerStageId = _stageId;
        for (auto *input : consumers)
            input->notifyDiscard();  // immediate, ignores stageId
        return true;
    }
    // ... existing write logic ...
}
```

### Step 3: Modify Input::notify() for discard

```cpp
void notifyDiscard() {
    // Discard is always immediate — no cross-stage delay
    ownerFunc->compute();
}
```

### Step 4: Each function handles discard in compute()

At the top of each function's `compute()`:

```cpp
void FetchAddressGen::compute() {
    // Check discard from redirect
    if (localRedirectValid && localRedirect.discard) {
        // Clear all state
        waitingForTranslation = false;
        translationReq = nullptr;
        fetchAddrOut.clear();
        // Use redirect target as new PC
        pendingPC = localRedirect.target;
        localRedirectValid = false;
        // Propagate discard to output
        FetchAddr discardAddr;
        discardAddr.discard = true;
        fetchAddrOut.write(discardAddr);
        // Then produce the real FetchAddr from new PC
        // ... translate and output ...
        return;
    }
    // ... normal compute ...
}
```

### Step 5: FetchMemRequest handles discard

```cpp
void FetchMemRequest::compute() {
    if (localFetchAddrValid && localFetchAddr.discard) {
        // Abandon current icache request
        waitingForCache = false;
        localFetchAddrValid = false;
        fetchAddrIn.unblockSource();
        // Propagate discard downstream
        FetchLine discardLine;
        discardLine.discard = true;
        fetchLineOut.write(discardLine);
        return;
    }
    // ... normal compute ...
}
```

### Step 6: PCUpdate sets discard on branch taken

```cpp
if (branchTaken) {
    // ... existing branch logic ...
    redir.discard = true;  // Signal pipeline flush
} else {
    redir.discard = false;
}
```

### Step 7: PCUpdate stops propagation on receiving its own discard

```cpp
void PCUpdate::compute() {
    // If ExecResult has discard, ignore — our own signal came back
    if (localExecResultValid && localExecResult.discard) {
        localExecResultValid = false;
        return;
    }
    // ... normal compute ...
}
```

---

## What This Achieves

1. **No deadlock.** Discard breaks the icache wait in FetchMemRequest,
   allowing the redirect to flow through immediately.

2. **1-cycle branch penalty.** The pipeline flushes in the same cycle
   as branch resolution. Only one BUBBLE cycle (pipeline refill).

3. **Correct execution.** Wrong-path instructions never execute —
   they're discarded before reaching ALUExecute.

4. **No separate flush mechanism.** Uses existing port connections.
   Discard is just a flag on the data structs that gets special
   treatment in write/notify.

5. **Matches Cortex-M4 behavior.** Branch taken = 1 + P cycles
   where P = 1 for pipeline refill.

---

## Open Questions

1. Should `discard` also handle non-branch flushes (exceptions,
   interrupts)? Probably yes — same mechanism.

2. When `FetchAddressGen` receives discard, should it produce the
   new FetchAddr in the SAME cycle (combinational through
   translation) or next cycle? For 1-cycle branch penalty, it
   should start the new fetch in the same cycle.

3. How does `discard` interact with `latchInputs()`? Since discard
   bypasses cross-stage latching, `latchInputs()` should check
   for discard and handle it immediately rather than waiting for
   the normal latch cycle.
