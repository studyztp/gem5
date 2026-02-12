# arch/riscv/ — RISC-V Architecture Context

> **Purpose:** RISC-V ISA implementation supporting RV32 and RV64 with standard extensions (M, A, F, D, C, V) and privilege modes (M, S, U).

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `isa.hh/cc` | `RiscvISA::ISA` | RISC-V ISA state — CSRs (mstatus, mie, mip, etc.), privilege mode. |
| `decoder.hh/cc` | `RiscvISA::Decoder` | RISC-V decoder — fixed 32-bit + compressed 16-bit (C extension). |
| `interrupts.hh/cc` | `RiscvISA::Interrupts` | RISC-V interrupt handling — M/S/U mode interrupts, delegation. |
| `faults.hh/cc` | | RISC-V exceptions: illegal instruction, ecall, page fault, etc. |
| `tlb.hh/cc` | `RiscvISA::TLB` | RISC-V TLB with Sv39/Sv48 support. |
| `pagetable_walker.hh/cc` | `RiscvISA::Walker` | RISC-V page table walker. |
| `pmp.hh/cc` | `PMP` | Physical Memory Protection unit. |
| `pma_checker.hh/cc` | `PMAChecker` | Physical Memory Attributes checker. |
| `process.hh/cc` | `RiscvProcess` | SE-mode RISC-V process. |
| `system.hh/cc` | `RiscvSystem` | RISC-V system extensions. |
| `insts/` | | RISC-V instruction implementations. |
| `isa/` | | ISA description files for RISC-V decoder. |
| `regs/` | | RISC-V registers: integer, float, vector, CSRs. |
| `linux/` | | RISC-V Linux syscall table. |

## RISC-V Privilege Levels

```
M-mode (Machine)    — highest privilege, handles traps
S-mode (Supervisor) — OS kernel
U-mode (User)       — userspace  
```

Interrupt/exception delegation via `mideleg`/`medeleg` CSRs.
