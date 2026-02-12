# arch/x86/ — x86-64 Architecture Context

> **Purpose:** x86/AMD64 ISA implementation including instruction decoding, segmentation, paging, APIC, and x86-specific system features.

## Key Files

| File | Class | Description |
|------|-------|-------------|
| `isa.hh/cc` | `X86ISA::ISA` | x86 ISA state — model-specific registers (MSRs), control registers, CPU ID. |
| `decoder.hh/cc` | `X86ISA::Decoder` | x86 instruction decoder — complex variable-length decoding with prefixes, REX, VEX. |
| `interrupts.hh/cc` | `X86ISA::Interrupts` | x86 Local APIC implementation — timer, IPI, interrupt delivery. |
| `faults.hh/cc` | | x86 exceptions: divide error, page fault, GP fault, double fault, etc. |
| `tlb.hh/cc` | `X86ISA::TLB` | x86 TLB with CR3/PCID support. |
| `pagetable_walker.hh/cc` | `X86ISA::Walker` | x86 page table walker (4-level, 5-level paging). |
| `pagetable.hh/cc` | | x86 page table entry format (PTE/PDE/PDPE/PML4E). |
| `process.hh/cc` | `X86Process` | SE-mode x86 process (64-bit and 32-bit). |
| `fs_workload.hh/cc` | `X86FsWorkload` | Full-system x86 workload. |
| `cpuid.hh/cc` | | CPUID instruction implementation. |
| `types.hh/cc` | | x86 types — ExtMachInst with all prefix/opcode info. |
| `insts/` | | x86 instruction implementations. |
| `isa/` | | ISA description files for x86 decoder. |
| `regs/` | | x86 registers: GPRs, segments, control, debug, MSRs, x87/SSE/AVX. |
| `kvm/` | | x86 KVM CPU support. |
| `linux/` | | x86 Linux syscall table. |

## x86 Specifics

- Complex instruction decoding with variable-length instructions (1-15 bytes)
- Micro-op decomposition: complex x86 instructions → micro-ops internally
- Segmentation support (mostly flat in 64-bit mode)
- Two interrupt delivery paths: Local APIC (modern) and PIC 8259 (legacy)
- CPUID for feature discovery
