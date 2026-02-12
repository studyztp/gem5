# dev/riscv/ — RISC-V Platform Devices Context

> **Purpose:** RISC-V-specific platform devices including interrupt controllers, timers, and board definitions.

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `clint.hh/cc` | `Clint` | Core Local Interruptor — provides per-hart software interrupts and timer (mtime/mtimecmp). |
| `plic.hh/cc` | `Plic` | Platform-Level Interrupt Controller — routes external device interrupts to harts. |
| `plic_device.hh/cc` | `PlicIntDevice` | Base for PLIC-connected devices. |
| `hifive.hh/cc` | `HiFive` | SiFive HiFive board platform. |
| `lupv.hh/cc` | `LupV` | LupV educational board platform. |
| `rtc.hh/cc` | `RiscvRTC` | RISC-V real-time clock. |
| `pci_host.hh/cc` | `GenericRiscvPciHost` | RISC-V PCI host bridge. |
| `vio_mmio.hh/cc` | `RiscvMmioVirtIO` | RISC-V VirtIO MMIO transport. |

## RISC-V Interrupt Architecture

```
Device → PLIC → hart M-mode/S-mode external interrupt
Timer  → CLINT → hart M-mode timer interrupt
IPI    → CLINT → hart M-mode software interrupt
```

## LupIO Educational Devices (`dev/lupio/`)

Simple devices for learning RISC-V system design:
- `LupioPIC` — simple interrupt controller
- `LupioTMR` — timer
- `LupioTTY` — terminal
- `LupioBLK` — block device
- `LupioRNG` — random number generator
- `LupioIPI` — inter-processor interrupt
- `LupioRTC` — real-time clock
- `LupioSYS` — system controller
