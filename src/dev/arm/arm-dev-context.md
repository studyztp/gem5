# dev/arm/ — ARM Device Models Context

> **Purpose:** This directory contains ARM-specific peripheral device models including the Generic Interrupt Controller (GIC), generic timers, UART (PL011), display, SMMUv3, RealView board platform, watchdogs, and various ARM-specific controllers. **This is the key directory for developing ARM-class peripherals like NVIC.**

## Directory Overview

### Interrupt Controllers (GIC)

| File | Class | Description |
|------|-------|-------------|
| `base_gic.hh/cc` | `BaseGic` | Abstract base for all GIC implementations. Defines `sendInt()`, `clearInt()`, `sendPPInt()`, `clearPPInt()`. Inherits from `PioDevice`. |
| `gic_v2.hh/cc` | `GicV2` | GICv2 implementation — distributor + CPU interfaces. Manages SPIs, PPIs, SGIs. |
| `gic_v2m.hh/cc` | `GicV2m` | GICv2m MSI frame — MSI (Message Signaled Interrupt) support for GICv2. |
| `gic_v3.hh/cc` | `GicV3` | GICv3 top-level — manages distributor + redistributors + CPU interfaces. |
| `gic_v3_distributor.hh/cc` | `Gicv3Distributor` | GICv3 distributor — routes SPIs, manages interrupt groups, affinity routing. |
| `gic_v3_redistributor.hh/cc` | `Gicv3Redistributor` | GICv3 redistributor — per-PE, handles SGIs/PPIs, LPIs. |
| `gic_v3_cpu_interface.hh/cc` | `Gicv3CPUInterface` | GICv3 CPU interface — system register interface to PE, priority/group management. |
| `gic_v3_its.hh/cc` | `Gicv3Its` | GICv3 ITS (Interrupt Translation Service) — translates device interrupts to LPIs. |
| `vgic.hh/cc` | `VGic` | Virtual GIC — GICv2 virtualization support. |

### Interrupt Pin Infrastructure

Defined in `base_gic.hh`:
```
ArmInterruptPinGen (SimObject) — generates interrupt pins
  ├── ArmSPIGen — creates SPI pins (Shared Peripheral Interrupt)
  ├── ArmPPIGen — creates PPI pins (Private Peripheral Interrupt)
  └── ArmSigInterruptPinGen — creates signal-based pins

ArmInterruptPin (Serializable) — abstract interrupt pin
  ├── ArmSPI — raises/clears SPI on GIC
  ├── ArmPPI — raises/clears PPI on GIC (per-CPU)
  └── ArmSigInterruptPin — signal port based
```

### Timers

| File | Class | Description |
|------|-------|-------------|
| `generic_timer.hh/cc` | `GenericTimer` / `GenericTimerFrame` / `GenericTimerMem` | ARM Generic Timer — system counter, per-CPU timers (EL1/EL2/EL3), memory-mapped timer frames. |
| `timer_cpulocal.hh/cc` | `CpuLocalTimer` | ARM Cortex-A9 local timer (TWD). |
| `timer_sp804.hh/cc` | `Sp804` | ARM SP804 dual timer. |
| `watchdog_generic.hh/cc` | `GenericWatchdog` | ARM SBSA generic watchdog. |
| `watchdog_sp805.hh/cc` | `Sp805` | ARM SP805 watchdog timer. |

### UART/Serial

| File | Class | Description |
|------|-------|-------------|
| `pl011.hh/cc` | `Pl011` | ARM PL011 UART — full implementation with FIFO, interrupts, DMA support. |
| `kmi.hh/cc` | `PL050KMI` | ARM PL050 keyboard/mouse interface. |

### Platform / Board

| File | Class | Description |
|------|-------|-------------|
| `realview.hh/cc` | `RealView` | RealView/Versatile Express platform — ties together all ARM board components. Inherits `Platform`. |
| `rv_ctrl.hh/cc` | `RealViewCtrl` | RealView board controller — system ID, clock control, resets. |
| `a9scu.hh/cc` | `A9SCU` | Cortex-A9 Snoop Control Unit. |
| `ssc.hh/cc` | `SSystemCounter` | System Security Controller. |
| `energy_ctrl.hh/cc` | `EnergyCtrl` | DVFS energy controller. |
| `fvp_base_pwr_ctrl.hh/cc` | `FVPBasePwrCtrl` | FVP power controller — CPU on/off. |

### SMMU (System Memory Management Unit)

| File | Class | Description |
|------|-------|-------------|
| `smmu_v3.hh/cc` | `SMMUv3` | ARM SMMUv3 — IOMMU for device address translation. |
| `smmu_v3_transl.hh/cc` | | Translation process. |
| `smmu_v3_caches.hh/cc` | | TLB/configuration caches. |
| `smmu_v3_cmdexec.hh/cc` | | Command queue execution. |
| `smmu_v3_deviceifc.hh/cc` | | Device interface (per-stream). |
| `smmu_v3_ports.hh/cc` | | Port definitions. |

### Display

| File | Class | Description |
|------|-------|-------------|
| `hdlcd.hh/cc` | `HDLcd` | ARM HDLCD display controller. Reads framebuffer via DMA. |
| `pl111.hh/cc` | `Pl111` | ARM PL111 LCD controller (CLCD). |
| `display.hh/cc` | `Display` | Generic display device interface. |

### Other

| File | Class | Description |
|------|-------|-------------|
| `flash_device.hh/cc` | `FlashDevice` | Flash memory device. |
| `gpu_nomali.hh/cc` | `NoMaliGpu` | Stub GPU (NoMali). |
| `pci_host.hh/cc` | `GenericArmPciHost` | ARM PCI host bridge. |
| `ufs_device.hh/cc` | `UFSHostDevice` | UFS storage device. |
| `vio_mmio.hh/cc` | `MmioVirtIO` | VirtIO MMIO transport for ARM. |
| `amba.hh` | `AmbaDevice` | AMBA bus device base (adds AMBA ID registers). |
| `amba_device.hh/cc` | `AmbaIntDevice` | AMBA device with interrupt pin. |
| `amba_fake.hh/cc` | `AmbaFake` | Fake AMBA device stub. |
| `doorbell.hh` | `Doorbell` | Doorbell register abstraction. |
| `mpam.hh/cc` | `MPAM` | Memory Partitioning and Monitoring. |
| `css/` | | Compute Subsystem: MHU (Message Handling Unit), SCMI (System Control), SCP. |

## Interrupt Flow (ARM)

```
Device raises interrupt:
  device->interrupt->raise()  // ArmSPI or ArmPPI
    │
    ▼
BaseGic::sendInt(num) or sendPPInt(num, cpu)
    │
    ▼
GIC Distributor routes to target CPU(s)
    │
    ▼
GIC CPU Interface → sets IRQ/FIQ signal
    │
    ▼
CPU checks interrupts: BaseCPU::checkInterrupts()
    │
    ▼
ArmISA::Interrupts::getInterrupt() → returns Fault
    │
    ▼
CPU takes exception → vector table → OS ISR
```

## Developing an NVIC (Nested Vectored Interrupt Controller)

For an ARMv6-M/ARMv7-M NVIC implementation:

1. **Inherit from `BaseGic`** — the NVIC is an interrupt controller
   - Or inherit from `BasicPioDevice` if you don't need the full GIC interface
   - NVIC has simpler interface than GIC but same fundamental role

2. **Key NVIC features to implement:**
   - MMIO registers at `0xE000E000` (SCS region): ISER, ICER, ISPR, ICPR, IPR, etc.
   - Priority-based preemption with configurable priority groups
   - Tail-chaining (back-to-back interrupt handling)
   - Late arrival optimization
   - Vector table lookup and exception entry/return

3. **Reference existing code:**
   - `gic_v2.hh/cc` — closest existing pattern for a simpler interrupt controller
   - `base_gic.hh` — the interface your NVIC should implement
   - `generic_timer.hh` — example of device with per-CPU state

4. **Integration points:**
   - Register with the `Platform` (RealView or custom)
   - Wire to CPU's `BaseInterrupts` interface
   - Handle SysTick timer (often part of NVIC block)
   - Implement system control registers (SCB, MPU if needed)

5. **Files to create:**
   - `src/dev/arm/nvic.hh` / `nvic.cc` — C++ implementation
   - `src/dev/arm/Nvic.py` — Python SimObject
   - Update `src/dev/arm/SConscript` — build registration
   - Possibly `src/arch/arm/interrupts.hh` modifications for M-profile

## For Deeper Investigation

| Topic | Where to look |
|-------|--------------|
| GIC internals | `gic_v2.hh`, `gic_v3.hh`, `gic_v3_distributor.hh` |
| Timer implementation | `generic_timer.hh` |
| UART device model | `pl011.hh` |
| DMA device pattern | `dma_device.hh`, `hdlcd.hh` (DMA display) |
| SMMU/IOMMU | `smmu_v3.hh` |
| Board/platform setup | `realview.hh`, `RealView.py` |
| AMBA bus protocol | `amba.hh`, `amba_device.hh` |
| CSS/SCMI power mgmt | `css/` directory |
