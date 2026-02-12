# arch/ — ISA (Instruction Set Architecture) Context

> **Purpose:** This directory contains architecture-specific code for each supported ISA. Each subdirectory implements the instruction decoder, ISA state (registers), faults/exceptions, TLB/MMU, page tables, and process model for a specific architecture.

## Supported Architectures

| Directory | ISA | Description |
|-----------|-----|-------------|
| `arm/` | ARM (AArch32/AArch64) | Full ARM implementation including A-profile and limited M-profile. See [arm/arm-arch-context.md](arm/arm-arch-context.md). |
| `x86/` | x86-64 | x86/AMD64 implementation. See [x86/x86-arch-context.md](x86/x86-arch-context.md). |
| `riscv/` | RISC-V | RISC-V (RV32/RV64) implementation. See [riscv/riscv-arch-context.md](riscv/riscv-arch-context.md). |
| `mips/` | MIPS | MIPS32/MIPS64 implementation. |
| `sparc/` | SPARC | SPARCv9 implementation. |
| `power/` | POWER | IBM POWER implementation. |
| `null/` | Null | Minimal ISA for testing (no real instructions). |
| `generic/` | Generic | Base classes shared by all architectures. |
| `isa_parser/` | | ISA description language parser (generates decoder from `.isa` files). |

## Generic Base Classes (arch/generic/)

Every ISA implements these base interfaces:

| File | Class | Description |
|------|-------|-------------|
| `isa.hh` | `BaseISA` | Base ISA state — misc registers, clear(), get/set register values. |
| `decoder.hh` | `InstDecoder` | Base instruction decoder — `moreBytes()`, `decode()`. |
| `interrupts.hh` | `BaseInterrupts` | Base interrupt interface — `post()`, `clear()`, `checkInterrupts()`, `getInterrupt()`. |
| `mmu.hh/cc` | `BaseMMU` | Base MMU — manages ITLB + DTLB, address translation. |
| `tlb.hh` | `BaseTLB` | Base TLB — `translateAtomic()`, `translateTiming()`, `flushAll()`. |
| `pcstate.hh` | `PCStateBase` | Base PC state — current PC, micro-PC, next PC. |

## Per-Architecture Structure

Each ISA directory typically contains:

```
arch/<isa>/
  ├── <ISA>CPU.py          — CPU SimObject (sets ISA-specific defaults)
  ├── <ISA>Decoder.py      — Decoder SimObject
  ├── <ISA>ISA.py          — ISA state SimObject (misc registers)
  ├── <ISA>Interrupts.py   — Interrupt controller SimObject
  ├── <ISA>MMU.py          — MMU SimObject
  ├── <ISA>TLB.py          — TLB SimObject
  ├── <ISA>FsWorkload.py   — Full-system workload
  ├── <ISA>SeWorkload.py   — Syscall-emulation workload
  │
  ├── isa.hh/cc            — ISA state (misc registers, mode tracking)
  ├── decoder.hh/cc        — Instruction decoder
  ├── interrupts.hh/cc     — Interrupt handling
  ├── mmu.hh               — MMU (usually thin wrapper)
  ├── tlb.hh/cc            — TLB implementation
  ├── pagetable.hh/cc      — Page table entry format
  ├── pagetable_walker.hh/cc — Hardware page table walker
  ├── faults.hh/cc         — Architecture-specific faults/exceptions
  ├── process.hh/cc        — SE-mode process setup
  ├── utility.hh/cc        — Architecture utility functions
  ├── types.hh             — Architecture types (MachInst, etc.)
  ├── pcstate.hh           — PC state representation
  │
  ├── insts/               — Instruction implementations
  ├── isa/                 — ISA description files (.isa) → generates decoder
  ├── regs/                — Register definitions
  ├── linux/               — Linux-specific syscall tables
  └── kvm/                 — KVM support for this ISA
```

## ISA Description Language

Instructions are defined in `.isa` files (in `<isa>/isa/` directories) using gem5's ISA description language:
- Parsed by `src/arch/isa_parser/`
- Generates C++ decoder and instruction execute methods
- Defines instruction formats, operands, decode trees

## How ISA Connects to CPU

```
CPU (BaseCPU)
  │
  ├── ThreadContext
  │     ├── ISA (BaseISA) — misc register state
  │     ├── Decoder (InstDecoder) — decodes bytes → StaticInst
  │     └── Registers (via RegClass) — architectural registers
  │
  ├── Interrupts (BaseInterrupts) — per-CPU interrupt state
  │     └── Connected to interrupt controller (GIC/PIC/PLIC)
  │
  └── MMU (BaseMMU)
        ├── ITLB, DTLB (BaseTLB)
        └── TableWalker — hardware page table walking
```

## For Adding a New ISA

1. Create directory `src/arch/myisa/`
2. Implement all base interfaces (ISA, Decoder, Interrupts, MMU, TLB)
3. Create `.isa` files for instruction definitions
4. Create Python SimObjects
5. Add build option in `build_opts/`
6. Create `Kconfig` for build configuration
