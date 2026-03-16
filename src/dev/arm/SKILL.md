# ARM Device Models - Navigation

## What This Directory Contains
Device models for ARM-based platforms: interrupt controllers (GIC), timers, UART, display,
storage, and the RealView/VExpress platform definitions. These are the peripheral devices
that connect to the ARM system bus.

## Key Files

### Interrupt Controllers
| File | Purpose |
|------|---------|
| `base_gic.hh` / `base_gic.cc` | `BaseGic` abstract base (PioDevice): `sendInt()`, `clearInt()`, `sendPPInt()`, `clearPPInt()`. Also defines `ArmInterruptPin`, `ArmSPI`, `ArmPPI` interrupt pin hierarchy. |
| `Gic.py` | Python SimObjects for GicV2, GicV3, interrupt pin generators. |
| `gic_v2.hh` / `gic_v2.cc` | GICv2 implementation: distributor + CPU interface, banked per-CPU. |
| `gic_v3.hh` / `gic_v3.cc` | GICv3 implementation. |
| `gic_v3_distributor.hh/cc` | GICv3 distributor. |
| `gic_v3_redistributor.hh/cc` | GICv3 redistributor. |
| `gic_v3_cpu_interface.hh/cc` | GICv3 CPU interface (friend of `ArmISA::ISA`). |
| `gic_v3_its.hh/cc` | GICv3 Interrupt Translation Service. |
| `vgic.hh/cc` | Virtual GIC. |

### Timers
| File | Purpose |
|------|---------|
| `generic_timer.hh` / `generic_timer.cc` | ARM Generic Timer: `SystemCounter`, `ArchTimer`, `GenericTimer` (per-core EL3/EL1/EL2 timers), `GenericTimerFrame`, `GenericTimerMem`. |
| `GenericTimer.py` | Timer Python config. |
| `generic_timer_miscregs_types.hh` | Timer-related misc register BitUnion types. |
| `timer_cpulocal.hh/cc` | CPU-local timer (e.g., ARM local timer). |
| `timer_sp804.hh/cc` | SP804 dual timer. |
| `watchdog_generic.hh/cc` | Generic watchdog. |
| `watchdog_sp805.hh/cc` | SP805 watchdog. |

### Platform
| File | Purpose |
|------|---------|
| `realview.hh` / `realview.cc` | `RealView` platform (extends `Platform`): GIC ownership, PCI interrupt routing. |
| `RealView.py` | **Platform hierarchy**: `VExpress_GEM5_V1` (GICv2), `VExpress_GEM5_V2` (GICv3), `VExpress_GEM5_Foundation`. Defines `_on_chip_devices()`, `_off_chip_devices()`, `attachOnChipIO()`, `attachIO()`. |
| `VExpressFastmodel.py` | Fast Model VExpress platform. |

### Peripherals
| File | Purpose |
|------|---------|
| `pl011.hh/cc` | PL011 UART. |
| `kmi.hh/cc` | Keyboard/mouse interface. |
| `hdlcd.hh/cc`, `display.hh/cc` | Display controllers. |
| `flash_device.hh/cc` | Flash storage. |
| `energy_ctrl.hh/cc` | Energy controller (DVFS). |
| `smmu_v3*.hh/cc` | SMMUv3 system MMU. |
| `amba_device.hh/cc`, `amba_fake.hh/cc` | AMBA bus device base. |
| `fvp_base_pwr_ctrl.hh/cc` | FVP power controller. |
| `css/` | Compute Subsystem: SCP, SCMI, MHU. |

## Device Connection Flow
```
Platform (RealView) owns GIC + device list
  → attachOnChipIO(bus) connects on-chip devices to iobus
  → attachIO(bus) connects off-chip devices
  → Devices use ArmInterruptPin (SPI/PPI) for interrupts
  → GIC routes interrupts to CPU via sendInt/clearInt
```

## Relevance to M-Profile
- M-profile uses **NVIC** (Nested Vectored Interrupt Controller), not GIC.
  A new NVIC device model would be needed here.
- M-profile uses **SysTick** timer instead of the Generic Timer.
  A SysTick model would also go here.
- A new M-profile platform class (not RealView/VExpress) would be defined here.
- The M-profile SCS (System Control Space) at 0xE000E000 is memory-mapped and would
  be implemented as a PioDevice.

## Where to Look Next

| Task | Go To |
|------|-------|
| Add NVIC for M-profile | Create new `nvic.hh/cc` here, based on `base_gic.hh` patterns |
| Add SysTick timer | Create new `systick.hh/cc` here |
| Create M-profile platform | Create new Python file here or extend `RealView.py` |
| Understand GIC integration | `base_gic.hh`, `Gic.py` |
| Understand timer integration | `generic_timer.hh`, `GenericTimer.py` |
