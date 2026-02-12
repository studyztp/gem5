# arch/arm/ — ARM Architecture Context

> **Purpose:** ARM ISA implementation including AArch32 (ARMv7) and AArch64 (ARMv8+) A-profile support. This is the most feature-complete ISA in gem5. **Critical for M-profile development (ARMv6-M, ARMv7-M for NVIC).**

## Key Files

### Core ISA

| File | Class | Description |
|------|-------|-------------|
| `isa.hh/cc` | `ArmISA::ISA` | ARM ISA state. Manages misc registers (system registers), decoder flavor, PMU, timer, GICv3 CPU interface references. |
| `decoder.hh/cc` | `ArmISA::Decoder` | ARM instruction decoder. Handles ARM, Thumb, Thumb-2, AArch64 encodings. |
| `interrupts.hh/cc` | `ArmISA::Interrupts` | ARM interrupt handling. Checks IRQ, FIQ, and manages interrupt signals from GIC. |
| `system.hh/cc` | `ArmSystem` | ARM-specific system extensions. Tracks security state, privilege levels, multiprocessor config, GIC version. |
| `faults.hh/cc` | `ArmFault` hierarchy | ARM exceptions/faults: Reset, UndefinedInstruction, SupervisorCall (SVC), PrefetchAbort, DataAbort, IRQ, FIQ, HypervisorCall (HVC), SecureMonitorCall (SMC). |
| `utility.hh/cc` | | ARM utility functions: EL detection, security state, condition codes. |
| `types.hh` | | ARM-specific types: `ExtMachInst`, `OperatingMode`, etc. |
| `pcstate.hh` | `ArmISA::PCState` | ARM PC state with Thumb/Jazelle mode tracking, instruction size. |

### Memory/TLB

| File | Class | Description |
|------|-------|-------------|
| `mmu.hh/cc` | `ArmISA::MMU` | ARM MMU — wraps ITLB + DTLB + stage2 TLB. |
| `tlb.hh/cc` | `ArmISA::TLB` | ARM TLB with VMSA support, ASID, VMID, security state. |
| `table_walker.hh/cc` | `ArmISA::TableWalker` | Hardware page table walker — supports LPAE, AArch64 4KB/16KB/64KB granules. |
| `stage2_lookup.hh/cc` | `Stage2LookupBase` | Stage 2 translation for virtualization (EL2). |
| `pagetable.hh/cc` | | ARM page table entry formats. |
| `tlbi_op.hh/cc` | | TLB invalidation operation types. |

### Performance/Debug

| File | Class | Description |
|------|-------|-------------|
| `pmu.hh/cc` | `ArmISA::PMU` | ARM Performance Monitor Unit — event counters, cycle counter. |
| `self_debug.hh/cc` | `ArmISA::SelfDebug` | ARM self-hosted debug — breakpoints, watchpoints. |
| `pauth_helpers.hh/cc` | | Pointer Authentication (ARMv8.3) helpers. |
| `qarma.hh/cc` | | QARMA cipher for pointer authentication. |
| `htm.hh/cc` | | Hardware Transactional Memory (ARMv9 TME). |
| `mpam.hh/cc` | | Memory Partitioning and Monitoring (MPAM). |

### ABI/Process

| File | Class | Description |
|------|-------|-------------|
| `process.hh/cc` | `ArmProcess` | SE-mode ARM process — sets up stack, loads binary, configures ABI. |
| `aapcs32.hh` | | AArch32 calling convention. |
| `aapcs64.hh` | | AArch64 calling convention. |
| `reg_abi.hh/cc` | | Register access ABI helpers. |
| `fs_workload.hh/cc` | `ArmFsWorkload` | Full-system workload — kernel loading, ATAGs/DTB setup. |
| `se_workload.hh` | `ArmSEWorkload` | Syscall emulation workload. |

### Registers

| File | Description |
|------|-------------|
| `regs/` | Register definitions — int regs, float/SIMD, SVE/SME vector regs, misc (system) register enums. |
| `isa_device.hh/cc` | `BaseISADevice` — interface for ISA-accessible devices (PMU, timer, GIC CPU interface). |

### Instructions & Decoder

| Directory | Description |
|-----------|-------------|
| `insts/` | Instruction implementations: data processing, memory, branch, NEON/SVE, crypto, etc. |
| `isa/` | `.isa` files — ISA description generating the decoder. Organized by encoding (ARM, Thumb, AArch64). |

### KVM

| Directory | Description |
|-----------|-------------|
| `kvm/` | ARM KVM CPU support — maps ARM state to/from KVM, handles register transfer. |

### Other

| File | Description |
|------|-------------|
| `fastmodel/` | ARM Fast Model integration (external ISS). |
| `tracers/` | ARM-specific execution tracers. |
| `nativetrace.hh/cc` | Compare against native ARM execution. |
| `remote_gdb.hh/cc` | ARM GDB remote debug support. |
| `semihosting.hh/cc` | ARM semihosting interface. |
| `matrix.hh` | SME matrix register support. |
| `linux/` | ARM Linux syscall tables.  |
| `freebsd/` | ARM FreeBSD syscall tables. |

## ARM Exception Levels

```
EL3 — Secure Monitor (Trust Zone)
EL2 — Hypervisor
EL1 — OS Kernel
EL0 — User Application
```

Each level has its own system registers, translation regime, and interrupt routing.

## ARM Interrupt Flow (Detail)

```
1. Device → GIC (sendInt/sendPPInt)
2. GIC Distributor → target redistributor → CPU interface
3. GIC CPU Interface → sets ISA signal (IRQ/FIQ)
4. ArmISA::Interrupts::checkInterrupts()
   → reads GIC ICC registers (via ISA device interface)
   → checks priority masking, group enable
5. Returns ArmFault::Interrupt or ArmFault::FastInterrupt
6. CPU takes exception → vectors to EL1/EL2/EL3 handler
```

## M-Profile Considerations (ARMv6-M/ARMv7-M)

Current gem5 ARM support is primarily A-profile. For M-profile (Cortex-M class):

1. **Execution modes differ** — M-profile uses Thread/Handler modes instead of EL0-EL3
2. **Exception model** — NVIC replaces GIC; vector table at fixed/configurable address
3. **Instruction set** — Thumb-only (no ARM instructions)
4. **System registers** — different from A-profile (no coprocessor, uses SCS memory-mapped regs)
5. **Key files to modify/extend:**
   - `interrupts.hh/cc` — M-profile interrupt model
   - `faults.hh/cc` — M-profile exception entry/return
   - `isa.hh/cc` — M-profile system registers (CONTROL, PRIMASK, BASEPRI, FAULTMASK)
   - `decoder.hh/cc` — Thumb-only decoding
   - May need new `isa/` files for M-profile-specific instruction variants
