# python/ — Python Configuration & Standard Library Context

> **Purpose:** This directory contains the Python infrastructure for gem5: the configuration system (m5), SimObject parameter framework, and the gem5 standard library (stdlib) for building simulation configurations.

## Directory Structure

| Path | Description |
|------|-------------|
| `m5/` | **Core m5 module** — SimObject parameter system, object instantiation, simulation control. |
| `gem5/` | **gem5 stdlib** — high-level components library for building simulations. |
| `pybind11/` | pybind11 bindings for C++ ↔ Python interface. |
| `embedded.hh/cc` | Embedded Python modules compiled into gem5 binary. |

## m5/ — Core Configuration Framework

| File | Description |
|------|-------------|
| `SimObject.py` | **SimObject base** — Python base class for all SimObjects. Defines parameter system. |
| `params/` | Parameter types: `Param.Int`, `Param.Addr`, `Param.String`, `VectorParam`, `Port`, etc. |
| `proxy.py` | **Proxy** — `Self`, `Parent` proxy objects for parameter resolution. |
| `core.py` | Core simulation control: `setMaxTick()`, `setClockFrequency()`, etc. |
| `simulate.py` | `simulate()` function — runs the simulation. |
| `event.py` | Python event interface. |
| `objects/` | Auto-generated imports of all SimObject `.py` files. |
| `stats/` | Statistics collection and output in Python. |
| `options.py` | Standard command-line options. |
| `main.py` | Main entry point for running gem5. |
| `util/` | Utility functions for configuration scripts. |

### SimObject Parameter System

Every C++ SimObject has a corresponding Python class:
```python
# Example: src/dev/arm/Gic.py
class GicV3(BaseGic):
    type = 'GicV3'
    cxx_header = 'dev/arm/gic_v3.hh'
    
    dist_addr = Param.Addr("Address for distributor")
    redist_addr = Param.Addr("Address for redistributors")
    it_lines = Param.UInt32(256, "Number of interrupt lines")
```

The `m5.params` module handles type checking, default values, and C++ Params struct generation.

## gem5/ — Standard Library (stdlib)

| Path | Description |
|------|-------------|
| `components/` | Pre-built component library. |
| `components/boards/` | Board configurations: `SimpleBoard`, `ArmBoard`, `X86Board`, `RiscvBoard`. |
| `components/processors/` | Processor configurations: `SimpleProcessor`, `SwitchableProcessor`. |
| `components/memory/` | Memory configurations: `SingleChannelDDR4_2400`, etc. |
| `components/cachehierarchies/` | Cache hierarchies: `ClassicCacheHierarchy`, `RubyCacheHierarchy`. |
| `components/devices/` | Device helpers. |
| `simulate/` | Simulation runner: `Simulator` class for running and managing simulation. |
| `resources/` | Resource management for downloading/using disk images, binaries, etc. |
| `isas.py` | ISA enumeration. |
| `coherence_protocol.py` | Coherence protocol enumeration. |
| `utils/` | Utility functions. |

## Configuration Script Flow

```python
# Typical gem5 config script:
from gem5.components.boards import SimpleBoard
from gem5.components.processors import SimpleProcessor
from gem5.components.memory import SingleChannelDDR4_2400
from gem5.simulate import Simulator

board = SimpleBoard(
    processor=SimpleProcessor(cpu_type=CPUTypes.TIMING, num_cores=1),
    memory=SingleChannelDDR4_2400(size="1GB"),
    cache_hierarchy=ClassicCacheHierarchy(l1d_size="32kB", l1i_size="32kB"),
)
board.set_workload(...)
sim = Simulator(board=board)
sim.run()
```

## Key Concepts

1. **SimObject `.py` files** are co-located with their C++ source
2. **`Params` struct** is auto-generated from Python class definitions
3. **Port connections** are made in Python: `cpu.dcache_port = l1d.cpu_side`
4. **Proxy objects** (`Self`, `Parent`) allow relative references in parameters
5. **`m5.instantiate()`** creates all C++ objects from Python config tree
