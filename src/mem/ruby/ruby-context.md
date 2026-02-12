# mem/ruby/ — Ruby Coherence Protocol Engine Context

> **Purpose:** Ruby is gem5's flexible coherence protocol framework. It uses SLICC (Specification Language for Implementing Cache Coherence) to define protocol state machines that are compiled into C++.

## Architecture Overview

```
┌──────────────────────────────────────────────────────┐
│                    RubySystem                         │
│                                                      │
│  ┌──────────┐  ┌──────────┐  ┌───────────────┐      │
│  │Sequencer │  │Sequencer │  │  Directory    │      │
│  │(L1 Cache)│  │(L1 Cache)│  │  Controller   │      │
│  └────┬─────┘  └────┬─────┘  └───────┬───────┘      │
│       │              │                │              │
│       └──────────────┼────────────────┘              │
│                      │                               │
│              ┌───────▼────────┐                      │
│              │   Network      │                      │
│              │  (Garnet/      │                      │
│              │   SimpleNet)   │                      │
│              └────────────────┘                      │
└──────────────────────────────────────────────────────┘
```

## Directory Structure

| Directory | Description |
|-----------|-------------|
| `common/` | Shared types: Address, DataBlock, MachineID, WriteMask, etc. |
| `network/` | Network models — Garnet (detailed NoC) and SimpleNetwork (abstract). |
| `profiler/` | Protocol profiling and statistics. |
| `protocol/` | SLICC protocol specifications (.sm files) — compiled into C++. |
| `slicc_interface/` | C++ interface classes that SLICC-generated code calls into. |
| `structures/` | Cache/directory/TBE structures used by protocol controllers. |
| `system/` | RubySystem, Sequencer, RubyPort — system-level Ruby components. |

## Key Components

### system/
- **RubySystem** — top-level Ruby object. Manages all controllers, network, profiler.
- **Sequencer** — CPU-facing interface. Translates gem5 memory requests into Ruby protocol messages.
- **RubyPort** — port adapter between gem5 classic memory system and Ruby.
- **GPUCoalescer** — GPU-specific sequencer with coalescing.

### slicc_interface/
- **AbstractController** — base class for all SLICC-generated protocol controllers.
- **AbstractCacheEntry** — base for cache entries in protocol state machines.
- **RubySlicc_Util** — utility functions available in SLICC code.

### structures/
- **CacheMemory** — Ruby's cache structure (separate from classic cache).
- **DirectoryMemory** — directory storage for coherence.
- **TBE (Transaction Buffer Entry)** — tracks in-flight coherence transactions.
- **PerfectCacheMemory** — ideal cache for protocol development.
- **PersistentTable** — for token coherence protocols.

### network/
- **SimpleNetwork** — simple message-passing network (point-to-point, buses).
- **Garnet** — detailed Network-on-Chip model with routers, links, virtual channels.

### protocol/
Contains `.sm` (SLICC state machine) files for various protocols:
- **MESI_Two_Level** — two-level MESI protocol
- **MESI_Three_Level** — three-level MESI with L0/L1/L2
- **MOESI_hammer** — AMD Hammer-style MOESI
- **MOESI_CMP_directory** — MOESI with directory
- **MOESI_CMP_token** — token-based MOESI
- **MI_example** — simple MI protocol for learning
- **GPU_VIPER** — GPU coherence protocol

## SLICC Protocol Development

SLICC files define state machines with:
- **States** — cache/directory states (e.g., I, S, M, E)
- **Events** — triggers (e.g., Load, Store, Inv, Data)
- **Transitions** — state × event → actions + new state
- **Actions** — operations performed during transitions

SLICC compiler (`src/mem/slicc/`) generates C++ code from `.sm` files.

## For Adding a New Protocol

1. Create `.sm` files in `protocol/` for each controller type
2. Define states, events, transitions, actions
3. Add protocol to build system (`protocol/SConscript`)
4. Create build option in `build_opts/` (e.g., `NULL_MY_PROTOCOL`)
5. Test with Ruby testers in `src/cpu/testers/rubytest/`
