# cpu/o3/ — Out-of-Order CPU Context

> **Purpose:** The O3 (Out-of-Order) CPU is gem5's most detailed CPU model, implementing a superscalar out-of-order pipeline with register renaming, speculative execution, and precise exceptions.

## Pipeline Stages

```
┌───────┐   ┌────────┐   ┌────────┐   ┌──────────────────┐   ┌────────┐
│ Fetch │──→│ Decode │──→│ Rename │──→│ IEW (Issue/Exec/ │──→│ Commit │
│       │   │        │   │        │   │    Writeback)     │   │  (ROB) │
└───┬───┘   └────────┘   └────┬───┘   └──────────────────┘   └────────┘
    │                         │              │
    │                   ┌─────┴─────┐   ┌────┴────┐
    │                   │ FreeList  │   │  LSQ    │
    │                   │ RenameMap │   │ InstQ   │
    │                   └───────────┘   └─────────┘
    │
┌───┴───────┐
│ BPredUnit │
│ FTQ / BAC │
└───────────┘
```

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `cpu.hh/cc` | `O3CPU` | Top-level CPU class. Orchestrates all pipeline stages. Manages thread contexts, interrupts, switching. |
| `fetch.hh/cc` | `Fetch` | Instruction fetch with branch prediction integration. Manages fetch buffers, handles branch mispredictions. |
| `decode.hh/cc` | `Decode` | Decodes fetched instructions, checks for special instructions (serializing, etc.). |
| `rename.hh/cc` | `Rename` | Register renaming. Maps architectural → physical registers. Handles squashing. |
| `iew.hh/cc` | `IEW` | Issue/Execute/Writeback stage. Contains instruction queue and LSQ. Schedules and executes instructions. |
| `commit.hh/cc` | `Commit` | In-order commit from ROB head. Handles exceptions, interrupts, squash signals. |
| `rob.hh/cc` | `ROB` | Reorder Buffer — maintains program-order instruction list for commit. |
| `inst_queue.hh/cc` | `InstructionQueue` | Instruction scheduling queue. Wakes dependents when results available. |
| `lsq.hh/cc` | `LSQ` | Load/Store Queue — manages memory ordering, forwarding, speculation. |
| `lsq_unit.hh/cc` | `LSQUnit` | Per-thread LSQ unit with store buffer and load queue. |
| `free_list.hh/cc` | `UnifiedFreeList` | Pool of free physical registers for renaming. |
| `rename_map.hh/cc` | `UnifiedRenameMap` | Architectural-to-physical register mapping. |
| `regfile.hh/cc` | `PhysRegFile` | Physical register file storage. |
| `scoreboard.hh/cc` | `Scoreboard` | Tracks which physical registers have been written. |
| `dyn_inst.hh/cc` | `DynInst` | Dynamic instruction — in-flight instruction instance with all execution state. |
| `comm.hh` | `TimeBuffer` structs | Inter-stage communication wires (fetch→decode, etc.). |
| `store_set.hh/cc` | `StoreSet` | Store set predictor for memory disambiguation. |
| `mem_dep_unit.hh/cc` | `MemDepUnit` | Memory dependency tracking for LSQ. |
| `ftq.hh/cc` | `FTQ` | Fetch Target Queue — buffers branch prediction results. |
| `bac.hh/cc` | `BAC` | Branch Address Calculator — early branch resolution. |
| `thread_context.hh/cc` | `O3ThreadContext` | O3-specific thread context implementation. |
| `thread_state.hh/cc` | `O3ThreadState` | Per-thread state in O3 CPU. |
| `checker.hh/cc` | `O3Checker` | Optional checker for result verification. |
| `probe/` | | Probe points for pipeline events (fetch, rename, IEW, commit, etc.). |

## SMT (Simultaneous Multithreading) Support

O3CPU supports SMT with configurable fetch/commit policies:
- **Fetch policies:** RoundRobin, IQCount, LSQCount, Branch
- **Commit policies:** RoundRobin, OldestReady
- Defined in `SMT.py` and configured via `smtNumFetchingThreads`, `smtFetchPolicy`, etc.

## Key Data Structures

- **DynInst** — the instruction object that flows through the pipeline. Contains source/dest register mappings, execution results, PC, fault info, memory request info.
- **TimeBuffer** — templated communication wire between pipeline stages with configurable delay (in `timebuf.hh`).
- **ROB** — circular buffer of in-flight `DynInst` pointers; head commits in program order.
- **InstructionQueue** — dependency-graph-based scheduler; wakes instructions when operands ready.

## Pipeline Communication Flow

```
Fetch → fetchQueue → Decode → decodeQueue → Rename → iewQueue → IEW → iewQueue → Commit
  ↑                                                                               │
  └─────────────────────── commitInfo (squash, redirect) ─────────────────────────┘
```

## Memory Access Path

```
Execute (in IEW) → LSQUnit::executeLoad/Store()
  → LSQ::pushRequest() → creates Packet
  → RequestPort::sendTimingReq() → L1 DCache
  → Response: LSQUnit::completeDataAccess() → writeback
```

## Configuration (Python SimObject)

Key parameters in `O3CPU.py` / `BaseO3CPU.py`:
- `fetchWidth`, `decodeWidth`, `renameWidth`, `dispatchWidth`, `issueWidth`, `commitWidth`
- `numROBEntries`, `numIQEntries`, `LQEntries`, `SQEntries`
- `numPhysIntRegs`, `numPhysFloatRegs`, `numPhysVecRegs`
- `branchPred` — branch predictor type
- `fuPool` — functional unit pool configuration

## For Modifying O3 Pipeline

1. **Adding a pipeline stage:** Create new stage class, add TimeBuffer connections in `cpu.hh`, wire in `cpu.cc::tick()`
2. **Modifying scheduling:** Edit `inst_queue.cc` — `InstructionQueue::scheduleReadyInsts()`
3. **Modifying commit:** Edit `commit.cc` — `Commit::commitInsts()`
4. **Adding new FU types:** Edit `FuncUnitConfig.py` and `FUPool.py`
