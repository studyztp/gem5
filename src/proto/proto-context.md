# proto/ — Protocol Buffers Trace Definitions Context

> **Purpose:** Protocol Buffer message definitions for gem5's trace infrastructure. Used for recording and replaying detailed simulation traces.

## Overview

gem5 uses Protocol Buffers (protobuf) to define structured trace formats:

- **Instruction traces** — record executed instructions for replay
- **Memory traces** — record memory access patterns for TraceCPU
- **Packet traces** — record bus/cache traffic

## Key Trace Types

| Proto file | Description |
|-----------|-------------|
| `inst.proto` | Instruction trace records (PC, opcode, registers). |
| `packet.proto` | Memory packet traces (address, command, timestamp). |
| `inst_dep_record.proto` | Instruction dependency records for TraceCPU. |

## Usage

1. **Recording:** Run simulation with trace recording enabled → generates `.pb` trace files
2. **Replay:** Use `TraceCPU` (`src/cpu/trace/`) to replay recorded traces
3. **Analysis:** Parse `.pb` files with protobuf tools for offline analysis

## Related

- `src/cpu/trace/trace_cpu.hh` — TraceCPU that replays protobuf traces
- `src/cpu/inst_pb_trace.hh` — instruction protobuf trace recorder
