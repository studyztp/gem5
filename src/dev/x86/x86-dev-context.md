# dev/x86/ — x86 Platform Devices Context

> **Purpose:** x86-specific platform devices including legacy PC peripherals and south bridge components.

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `pc.hh/cc` | `Pc` | x86 PC platform — ties together south bridge, interrupt routing. |
| `south_bridge.hh/cc` | `SouthBridge` | South bridge containing legacy devices (PIC, PIT, CMOS, etc.). |
| `i8259.hh/cc` | `I8259` | Intel 8259 PIC — Programmable Interrupt Controller (legacy). |
| `i82094aa.hh/cc` | `I82094AA` | IO APIC — I/O Advanced PIC for SMP interrupt routing. |
| `i8254.hh/cc` | `I8254` | Intel 8254 PIT — Programmable Interval Timer. |
| `i8042.hh/cc` | `I8042` | Intel 8042 keyboard/mouse controller. |
| `i8237.hh/cc` | `I8237` | Intel 8237 DMA controller. |
| `cmos.hh/cc` | `Cmos` | CMOS/RTC — real-time clock and NVRAM. |
| `speaker.hh/cc` | `PcSpeaker` | PC speaker. |
| `ide_ctrl.hh/cc` | `X86IdeController` | x86 IDE controller. |
| `qemu_fw_cfg.hh/cc` | `QemuFwCfg` | QEMU firmware configuration device. |
| `intdev.hh` | `IntDevice` | x86 interrupt device base (local APIC messages). |

## x86 Interrupt Architecture

```
Device IRQ → I/O APIC (i82094aa) → APIC Bus → Local APIC (in CPU) → CPU interrupt
             or legacy: → PIC (i8259) → CPU INTR pin
```
