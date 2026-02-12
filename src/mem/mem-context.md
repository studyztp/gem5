# mem/ — Memory System Context

> **Purpose:** This directory contains the entire memory system: ports, packets, caches, crossbars (buses), memory controllers, DRAM/NVM interfaces, address translation, and the Ruby coherence protocol engine.

## Directory Overview

| Path | Description |
|------|-------------|
| `port.hh/cc` | **RequestPort / ResponsePort** — memory port classes implementing atomic/timing/functional protocols. The primary communication mechanism between memory system components. |
| `packet.hh/cc` | **Packet** — the unit of communication. Contains command, address, data, size, and sender state. |
| `request.hh` | **Request** — encodes memory request attributes (address, flags, requestor, etc.). |
| `xbar.hh/cc` | **BaseXBar** — crossbar (bus) base class. Routes packets between ports. |
| `coherent_xbar.hh/cc` | **CoherentXBar** — coherent crossbar with snoop support. Connects caches. |
| `noncoherent_xbar.hh/cc` | **NoncoherentXBar** — non-coherent crossbar. Connects I/O devices. |
| `cache/` | **Cache hierarchy** — L1/L2/L3 caches, tags, prefetchers, replacement policies, compressors. See [cache/cache-context.md](cache/cache-context.md). |
| `ruby/` | **Ruby** — flexible coherence protocol framework using SLICC. See [ruby/ruby-context.md](ruby/ruby-context.md). |
| `abstract_mem.hh/cc` | **AbstractMemory** — base for memory objects (backing store). |
| `simple_mem.hh/cc` | **SimpleMemory** — simple latency-only memory model. |
| `mem_ctrl.hh/cc` | **MemCtrl** — memory controller with scheduling and timing. |
| `mem_interface.hh/cc` | **MemInterface** — base for DRAM/NVM media interfaces. |
| `dram_interface.hh/cc` | **DRAMInterface** — detailed DRAM timing model (ranks, banks, refresh, etc.). |
| `nvm_interface.hh/cc` | **NVMInterface** — non-volatile memory interface model. |
| `hbm_ctrl.hh/cc` | **HBMCtrl** — High Bandwidth Memory controller (per-pseudo-channel). |
| `hetero_mem_ctrl.hh/cc` | **HeteroMemCtrl** — heterogeneous memory controller (DRAM+NVM). |
| `bridge.hh/cc` | **Bridge** — connects two memory buses with buffering and latency. |
| `comm_monitor.hh/cc` | **CommMonitor** — monitors traffic between ports (stats, tracing). |
| `addr_mapper.hh/cc` | **AddrMapper** — address remapping between buses. |
| `snoop_filter.hh/cc` | **SnoopFilter** — reduces unnecessary snoops in coherent system. |
| `physical.hh/cc` | **PhysicalMemory** — manages physical memory backing stores. |
| `page_table.hh/cc` | **PageTable** — page table for SE mode address translation. |
| `port_proxy.hh/cc` | **PortProxy** — simplified read/write interface to memory. |
| `serial_link.hh/cc` | **SerialLink** — serialization link between buses (die-to-die). |
| `token_port.hh/cc` | **TokenPort** — flow-control port using tokens. |
| `qos/` | **QoS framework** — quality of service for memory controllers. |
| `probes/` | **Memory probes** — monitoring points for memory traffic. |
| `protocol/` | **Coherence protocol definitions** — protocol messages. |
| `slicc/` | **SLICC** — protocol specification language for Ruby. |
| `htm.hh/cc` | **HTM** — Hardware Transactional Memory support. |

## Memory System Topology

```
CPU                          CPU
 │ icache_port  dcache_port   │
 ▼              ▼             ▼
┌────┐        ┌────┐        ┌────┐
│L1-I│        │L1-D│        │L1-D│
└──┬─┘        └──┬─┘        └──┬─┘
   │             │              │
   └──────┬──────┘              │
          ▼                     │
      ┌──────┐                  │
      │  L2  │                  │
      └──┬───┘                  │
         │                      │
         └──────────┬───────────┘
                    ▼
              ┌───────────┐
              │CoherentXBar│  (system bus)
              └─────┬─────┘
         ┌──────────┼──────────┐
         ▼          ▼          ▼
    ┌────────┐ ┌────────┐ ┌────────────┐
    │MemCtrl │ │MemCtrl │ │NonCoherent │
    │(DRAM)  │ │(NVM)   │ │   XBar     │
    └────────┘ └────────┘ └─────┬──────┘
                                │
                          ┌─────┼─────┐
                          ▼     ▼     ▼
                       Device Device Device
                       (PIO)  (PIO)  (DMA→)
```

## Key Class Hierarchy

```
Port (src/sim/port.hh) — base
  ├── RequestPort (src/mem/port.hh)
  │   ├── + AtomicRequestProtocol
  │   ├── + TimingRequestProtocol
  │   └── + FunctionalRequestProtocol
  └── ResponsePort (src/mem/port.hh)
      ├── + AtomicResponseProtocol
      ├── + TimingResponseProtocol
      └── + FunctionalResponseProtocol

ClockedObject
  ├── AbstractMemory → SimpleMemory
  ├── MemCtrl → HBMCtrl, HeteroMemCtrl
  ├── BaseXBar → CoherentXBar, NoncoherentXBar
  ├── BaseCache → Cache, NoncoherentCache
  └── MemInterface → DRAMInterface, NVMInterface
```

## Packet and Request

**Packet** (`packet.hh`) carries:
- `MemCmd` — command type (ReadReq, WriteReq, ReadResp, Writeback, CleanEvict, etc.)
- `Addr` — address
- `data` — pointer to data buffer
- `size` — data size
- `SenderState` — linked list for state tracking as packet traverses hierarchy
- `req` — pointer to underlying `Request`

**Request** (`request.hh`) carries:
- Physical/virtual address
- Requestor ID
- Flags (uncacheable, instruction fetch, locked, etc.)
- Architecture-specific flags
- Context ID, thread ID

## Crossbar Routing

- **CoherentXBar** — handles snooping for cache coherence. Used between L1/L2 and system bus.
- **NoncoherentXBar** — no snooping. Used for I/O bus connecting devices.
- Routing based on `AddrRange` returned by `ResponsePort::getAddrRanges()`.

## Subdirectory Details

### cache/ — Cache Hierarchy
See [cache/cache-context.md](cache/cache-context.md)

### ruby/ — Ruby Coherence
See [ruby/ruby-context.md](ruby/ruby-context.md)

### qos/ — Quality of Service
- `QoS::MemCtrl` — QoS-aware memory controller base
- `QoS::Policy` — scheduling policies (Fixed, PF, Prop Fair, TurnaroundPolicy)
- `QoS::MemSinkCtrl` — QoS memory controller for testing

## For Adding a New Memory Controller

1. Inherit from `MemCtrl` or `ClockedObject`
2. Implement request scheduling logic
3. Use `MemInterface` (or `DRAMInterface`) for media timing
4. Create ResponsePort for incoming requests
5. Create Python SimObject
6. Register in `SConscript`

## For Adding a New Memory Device

1. Inherit from `AbstractMemory` for simple memories
2. Or inherit from `ClockedObject` and implement ports directly
3. Implement port interface (`recvTimingReq`, `recvAtomic`, `recvFunctional`, `getAddrRanges`)
