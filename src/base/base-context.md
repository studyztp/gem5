# base/ — Base Utilities and Infrastructure Context

> **Purpose:** This directory contains foundational utilities, data structures, debug/trace infrastructure, statistics framework, binary loading, networking utilities, and graphical output. These are used throughout the entire gem5 codebase.

## Directory Overview

### Core Utilities

| File | Description |
|------|-------------|
| `types.hh/cc` | Fundamental types: `Tick`, `Addr`, `MicroPC`, `RegVal`, etc. |
| `bitfield.hh/cc` | Bit manipulation: `bits()`, `mbits()`, `insertBits()`, `replaceBits()`. |
| `bitunion.hh` | **BitUnion** — powerful typed bitfield access (like C bitfields but portable). |
| `flags.hh` | `Flags<T>` — type-safe flag management. |
| `intmath.hh` | Integer math: `isPowerOf2()`, `ceilLog2()`, `floorLog2()`, `divCeil()`. |
| `cast.hh` | Safe casting utilities: `safe_cast<>`. |
| `compiler.hh` | Compiler detection and attribute macros. |
| `condcodes.hh` | Condition code evaluation helpers. |
| `crc.hh` | CRC computation. |
| `str.hh/cc` | String utilities: `to_number()`, `tokenize()`, `startswith()`. |
| `chunk_generator.hh` | Generates aligned memory chunks for splitting cross-boundary accesses. |
| `addr_range.hh` | **AddrRange** — address range with interleaving support. Used extensively for memory mapping. |
| `addr_range_map.hh` | **AddrRangeMap** — maps address ranges to values (used by crossbars, port routing). |

### Debug and Tracing

| File | Description |
|------|-------------|
| `trace.hh/cc` | **Trace** system — DPRINTF macros, trace flags, output control. |
| `debug.hh/cc` | **Debug flags** — named debug categories (e.g., `DPRINTF(Cache, ...)`, `DPRINTF(Fetch, ...)`). |
| `logging.hh/cc` | **Logging** — `panic()`, `fatal()`, `warn()`, `inform()`, `hack()`. |
| `output.hh/cc` | **OutputDirectory** — manages simulation output files (stats, traces). |

### Statistics Framework

| File | Description |
|------|-------------|
| `statistics.hh/cc` | **Statistics** — comprehensive stats framework: `Scalar`, `Vector`, `Distribution`, `Histogram`, `Formula`. |
| `stats/` | Stats subsystem: `text.hh` (text output), `hdf5.hh` (HDF5 output), `group.hh` (hierarchical grouping), `info.hh` (stat metadata). |

### Binary Loading

| File | Description |
|------|-------------|
| `loader/` | Binary loaders: ELF, COFF, raw. Symbol table loading. `ObjectFile`, `SymbolTable`, `MemoryImage`. |

### Networking

| File | Description |
|------|-------------|
| `inet.hh/cc` | Network types: `EthAddr`, `IpAddress`, `TcpHeader`, `UdpHeader`, checksum utilities. |
| `socket.hh/cc` | Socket utilities for external connections (terminal, GDB). |
| `pollevent.hh/cc` | Poll-based event integration (async I/O in simulation). |

### Graphics

| File | Description |
|------|-------------|
| `framebuffer.hh/cc` | Framebuffer representation for display devices. |
| `pixel.hh/cc` | Pixel format conversion. |
| `bmpwriter.hh/cc` | BMP image writer. |
| `pngwriter.hh/cc` | PNG image writer. |
| `imgwriter.hh/cc` | Image writer base. |
| `vnc/` | VNC server for remote framebuffer viewing. |

### Data Structures

| File | Description |
|------|-------------|
| `circlebuf.hh` | Circular buffer. |
| `circular_queue.hh` | Circular queue with wrap-around. |
| `trie.hh` | Trie data structure (used in TLB). |
| `sat_counter.hh` | Saturating counter (used in branch predictors, cache replacement). |
| `free_list.hh` | Free list allocator. |
| `cache/` | Utility caches: `associative_cache.hh`, `associative_set.hh`. |
| `filters/` | Bloom filters and related structures. |

### Other

| File | Description |
|------|-------------|
| `coroutine.hh` | Coroutine support (used in memory translation processes). |
| `fiber.hh/cc` | Fiber (lightweight thread) support. |
| `callback.hh` | Callback utilities. |
| `random.hh/cc` | Random number generation. |
| `remote_gdb.hh/cc` | **GDB remote debugging** — base class for GDB stub. Each ISA extends this. |
| `named.hh` | `Named` mixin — gives objects a `name()` method. |
| `extensible.hh` | Extensible object framework (attach arbitrary extensions to objects). |
| `refcnt.hh` | Reference-counted pointers. |
| `temperature.hh/cc` | Temperature type for thermal modeling. |
| `time.hh/cc` | Host time utilities. |
| `inifile.hh/cc` | INI file parser (used for checkpoint format). |
| `match.hh/cc` | String matching utilities. |
| `memoizer.hh` | Memoization cache. |
| `stl_helpers.hh` / `stl_helpers/` | STL helper utilities. |
| `gtest/` | Google Test helper utilities. |

## Key Types (base/types.hh)

```cpp
typedef uint64_t Tick;        // Simulation time unit
typedef uint64_t Addr;        // Memory address
typedef uint16_t MicroPC;     // Micro-op PC
typedef uint64_t RegVal;      // Register value
typedef int16_t  ThreadID;    // Thread identifier
typedef int16_t  ContextID;   // Thread context identifier
```

## Debug/Trace Usage

```cpp
#include "base/trace.hh"
#include "debug/MyFlag.hh"  // Generated from SConscript DebugFlag

DPRINTF(MyFlag, "Address: %#x, Value: %d\n", addr, val);
DPRINTFR(MyFlag, "Raw debug message\n");  // No object name prefix
```

Debug flags are defined in `SConscript` files:
```python
DebugFlag('MyFlag', 'Description of my debug flag')
```

Enable at runtime: `--debug-flags=MyFlag`

## Statistics Usage

```cpp
statistics::Scalar numReads;
statistics::Vector readLatency;
statistics::Distribution accessDist;

void regStats() override {
    numReads.name(name() + ".numReads").desc("Number of reads");
    // ...
}
```
