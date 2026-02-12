# dev/ — Devices Context

> **Purpose:** This directory contains all I/O device models: interrupt controllers, timers, UART, PCI, network, storage, display, DMA, and platform-specific peripherals. This is the primary location for adding new hardware device models.

## Directory Overview

| Path | Description |
|------|-------------|
| `io_device.hh/cc` | **PioDevice / BasicPioDevice** — base classes for memory-mapped I/O devices. Every MMIO device inherits from these. |
| `dma_device.hh/cc` | **DmaDevice / DmaPort** — base for devices that perform bus-mastering DMA. Extends PioDevice. |
| `platform.hh/cc` | **Platform** — abstract platform class linking devices to interrupt controller. |
| `arm/` | **ARM peripherals** — GIC, timers, RealView board, SMMU, UART (PL011), display, etc. See [arm/arm-dev-context.md](arm/arm-dev-context.md). |
| `x86/` | **x86 peripherals** — i8259 PIC, i8254 PIT, i8042 keyboard, IOAPIC, CMOS, south bridge, IDE. |
| `riscv/` | **RISC-V peripherals** — CLINT, PLIC, HiFive board, RTC. |
| `pci/` | **PCI subsystem** — PCI device base, PCI host bridge, PCI bus, copy engine. |
| `serial/` | **Serial/UART** — terminal, UART base, UART 8250, simple serial. |
| `net/` | **Network devices** — Ethernet controllers (i8254xGBe, NS Gige, Sinic), switches, links. |
| `storage/` | **Storage** — IDE controller/disk, disk images, simple disk. |
| `virtio/` | **VirtIO** — VirtIO devices: block, console, RNG, 9P filesystem, PCI transport. |
| `lupio/` | **LupIO** — educational RISC-V peripherals (PIC, timer, TTY, block, RNG, IPI, RTC, SYS). |
| `i2c/` | **I2C** — I2C bus controller. |
| `ps2/` | **PS/2** — keyboard and mouse devices. |
| `qemu/` | **QEMU integration** — fw_cfg device for passing data to guest firmware. |
| `hsa/` | **HSA** — Heterogeneous System Architecture signal/queue devices. |
| `amdgpu/` | **AMD GPU** — AMD GPU device models. |
| `mips/` | **MIPS peripherals** — Malta board devices. |
| `sparc/` | **SPARC peripherals** — SPARC-specific devices. |
| `baddev.hh/cc` | **BadDevice** — returns errors for unimplemented regions. |
| `isa_fake.hh/cc` | **IsaFake** — returns fixed values for fake devices (stub). |
| `intel_8254_timer.hh/cc` | **Intel8254Timer** — 8254 PIT timer implementation. |
| `mc146818.hh/cc` | **MC146818** — RTC chip base. |
| `pixelpump.hh/cc` | **PixelPump** — pixel output engine for display devices. |
| `reg_bank.hh` | **RegisterBank** — framework for modeling device register banks with automatic read/write dispatch. |

## Device Class Hierarchy

```
SimObject
  └── ClockedObject
      └── PioDevice (io_device.hh)          — has PioPort (ResponsePort)
          ├── BasicPioDevice                 — adds pioAddr, pioSize, pioDelay
          │   ├── IsaFake                    — stub device
          │   ├── BadDevice                  — error device
          │   └── [most simple devices]
          │
          ├── BaseGic (arm/base_gic.hh)      — interrupt controller base
          │   ├── GicV2 (arm/gic_v2.hh)
          │   └── GicV3 (arm/gic_v3.hh)
          │
          └── DmaDevice (dma_device.hh)      — adds DmaPort (RequestPort)
              ├── PL011 (arm/pl011.hh)       — ARM UART (receives DMA)
              ├── CopyEngine (pci/copy_engine.hh)
              ├── IGbE (net/i8254xGBe.hh)   — Intel Gigabit Ethernet
              ├── IDE (storage/ide_ctrl.hh)  — IDE disk controller
              └── [complex DMA devices]
```

## Device Connection Pattern

```
Device (PioDevice)
  │
  ├── pioPort (ResponsePort) ──→ connected to IoXBar / system bus
  │     Handles: CPU reads/writes to device registers (MMIO)
  │
  └── [For DmaDevice additionally:]
      dmaPort (RequestPort) ──→ connected to system bus
        Handles: Device-initiated DMA reads/writes to memory
```

## Implementing a New Device

### Step 1: Choose Base Class

| Base Class | Use When |
|-----------|----------|
| `BasicPioDevice` | Simple MMIO-only device (registers, timers, simple controllers) |
| `DmaDevice` | Device that initiates memory transfers (DMA, NIC, disk controller) |
| `BaseGic` | Interrupt controller |
| `PciDevice` | PCI-attached device |

### Step 2: Required Implementations

For `BasicPioDevice`:
```cpp
class MyDevice : public BasicPioDevice {
    Tick read(PacketPtr pkt) override;   // Handle register reads
    Tick write(PacketPtr pkt) override;  // Handle register writes
    // pioAddr, pioSize set in constructor via params
};
```

For `DmaDevice`, also handle DMA:
```cpp
class MyDmaDevice : public DmaDevice {
    Tick read(PacketPtr pkt) override;
    Tick write(PacketPtr pkt) override;
    AddrRangeList getAddrRanges() const override;
    // Use dmaPort.dmaAction() for DMA transfers
};
```

### Step 3: Python SimObject

Create `MyDevice.py`:
```python
from m5.params import *
from m5.objects.Device import BasicPioDevice

class MyDevice(BasicPioDevice):
    type = 'MyDevice'
    cxx_header = 'dev/my_device.hh'
    my_param = Param.Int(42, "description")
    int_pin = Param.ArmInterruptPin(NULL, "interrupt output")
```

### Step 4: Interrupt Signaling (ARM example)

Devices signal interrupts through interrupt pins:
```cpp
// In device constructor:
interrupt = params.int_pin;  // ArmInterruptPin*

// To raise interrupt:
interrupt->raise();

// To clear interrupt:
interrupt->clear();
```

The interrupt pin connects to the GIC, which routes to the CPU.

### Step 5: Build Integration

Add to `SConscript`:
```python
SimObject('MyDevice.py', sim_objects=['MyDevice'])
Source('my_device.cc')
```

## RegisterBank Framework

`reg_bank.hh` provides a powerful framework for modeling device registers:
- Typed register definitions with automatic endianness handling
- Read/write callbacks per register
- Register arrays and reserved regions
- Simplifies MMIO `read()`/`write()` implementation

## For Deeper Investigation

| Topic | Where to look |
|-------|--------------|
| ARM devices (GIC, timers, UART) | [arm/arm-dev-context.md](arm/arm-dev-context.md) |
| x86 devices (PIC, PIT, IOAPIC) | `x86/` directory |
| RISC-V devices (CLINT, PLIC) | `riscv/` directory |
| PCI device development | `pci/device.hh`, `pci/host.hh` |
| Network device development | `net/etherdevice.hh` |
| VirtIO device development | `virtio/base.hh` |
| DMA engine internals | `dma_device.hh` — DmaPort class |
