# ARM Architecture Implementation - Navigation

## What This Directory Contains
The complete ARM ISA implementation for gem5, supporting AArch32 (ARMv7-A) and AArch64 (ARMv8-A)
application-profile processors. Currently does **not** support M-profile (Cortex-M) or
R-profile (natively; CortexR52 is via Fast Models only).

## Key Files

### Core ISA
| File | Purpose |
|------|---------|
| `isa.hh` / `isa.cc` | Central `ArmISA::ISA` class. Holds `miscRegs[]` array, implements `readMiscReg`/`setMiscReg`, register banking, VHE redirection. |
| `ArmISA.py` | Python SimObject for ISA. Params: `midr`, ID registers, `release_se`, `sve_vl_se`, `decoderFlavor`. |
| `types.hh` | Core types: `MachInst`, `ExtMachInst` (decoder bitfields), `ExceptionLevel`, `OperatingMode`, `ExceptionClass`. |
| `pcstate.hh` | `PCState` with `thumb()`, `aarch64()`, `nextThumb()`, `nextAArch64()` flags. |

### Faults and Exceptions
| File | Purpose |
|------|---------|
| `faults.hh` / `faults.cc` | `ArmFault` hierarchy: Reset, IRQ, FIQ, SVC, DataAbort, etc. `getVector()` and `getVector64()` compute exception entry addresses from VBAR/MVBAR. `invoke32()`/`invoke64()` handle CPSR/SPSR save and mode switch. |
| `interrupts.hh` / `interrupts.cc` | Interrupt handling: INT_RST, INT_IRQ, INT_FIQ, INT_SEV. Masking via CPSR, SCR, HCR. |

### System
| File | Purpose |
|------|---------|
| `system.hh` / `system.cc` | `ArmSystem` class: reset address, generic timer, GIC, semihosting, `ArmRelease` extensions. |
| `ArmSystem.py` | Python params: `reset_addr`, `highest_el_is_64`, `release`, GIC address, SVE/SME lengths. |
| `fs_workload.hh` / `fs_workload.cc` | Full-system workload: kernel loading, DTB, bootloader, `Reset().invoke(tc)` for each thread. |
| `ArmFsWorkload.py` | Full-system workload Python params. |

### Memory Management
| File | Purpose |
|------|---------|
| `mmu.hh` / `mmu.cc` | ARM MMU with TLB lookup, table walk interface. |
| `tlb.hh` / `tlb.cc` | TLB implementation. |
| `table_walker.hh` / `table_walker.cc` | Page table walker for short/long descriptor formats. |
| `pagetable.hh` / `pagetable.cc` | Page table entry types. |
| `stage2_lookup.hh` / `stage2_lookup.cc` | Stage 2 translation for virtualization. |

### Decoder and Instructions
| File | Purpose |
|------|---------|
| `decoder.hh` / `decoder.cc` | `ArmDecoder` class: fetches and decodes instructions. |
| `ArmDecoder.py` | Decoder Python config. |
| `isa/` | ISA description language files. See `isa/SKILL.md`. |
| `insts/` | C++ instruction implementations. See `insts/SKILL.md`. |

### Other
| File | Purpose |
|------|---------|
| `process.hh` / `process.cc` | Syscall-emulation process for ARM. |
| `self_debug.hh` / `self_debug.cc` | Breakpoints, watchpoints, software step. |
| `pmu.hh` / `pmu.cc` | Performance Monitoring Unit. |
| `semihosting.hh` / `semihosting.cc` | ARM semihosting interface. |
| `utility.hh` / `utility.cc` | Helpers: `currEL()`, `inAArch64()`, `ELIs32()`, `isSecure()`, `getMPIDR()`. |
| `remote_gdb.hh` / `remote_gdb.cc` | GDB remote debugging. |

## Subdirectories

| Path | Purpose |
|------|---------|
| `regs/` | Register definitions (integer, misc/system, vector, condition codes). See `regs/SKILL.md`. |
| `isa/` | ISA description files (decoder, instruction formats, operands). See `isa/SKILL.md`. |
| `insts/` | C++ instruction implementations. See `insts/SKILL.md`. |
| `fastmodel/` | ARM Fast Model integration (CortexA76, CortexR52). See `fastmodel/SKILL.md`. |
| `kvm/` | KVM-based ARM CPU models. |
| `linux/` | Linux-specific workload/process handling. |
| `freebsd/` | FreeBSD support. |
| `tracers/` | Tarmac trace recording. |
| `gdb-xml/` | GDB register description XML files. |

## Where to Look Next

| Task | Go To |
|------|-------|
| Add/modify system registers | `regs/SKILL.md` (start with `regs/misc.hh`) |
| Change exception/fault handling | `faults.hh` / `faults.cc` |
| Modify instruction decoding | `isa/SKILL.md` |
| Change boot sequence | `system.cc`, `fs_workload.cc`, `faults.cc` (Reset class) |
| Add M-profile support | `regs/misc.hh` (registers), `faults.cc` (exception model), `types.hh` (modes), `isa.cc` (read/write logic) |
| Modify ARM platform/devices | `../dev/arm/SKILL.md` |
| CPU model integration | `ArmCPU.py` (mixin: ArchDecoder, ArchMMU, ArchISA, ArchInterrupts) |
