# ARM ISA Description Files - Navigation

## What This Directory Contains
The ISA description language (`.isa`) files that define instruction encoding, decoding,
operand mapping, and code generation templates for the ARM architecture. These are processed
by gem5's ISA parser to generate C++ decoder and instruction classes.

## Key Files

| File | Purpose |
|------|---------|
| `main.isa` | Entry point. Includes all other `.isa` files in order: includes, bitfields, operands, templates, instructions, formats, decoder. |
| `includes.isa` | C++ include directives for generated code. |
| `bitfields.isa` | Bitfield definitions for `ExtMachInst` (the extended machine instruction used by the decoder). |
| `operands.isa` | Operand definitions mapping ISA description names to register classes and indices. |
| `arminstobjparams.isa` | Instruction object parameter definitions. |

## Subdirectories

| Path | Purpose |
|------|---------|
| `decoder/` | **Decoder dispatch logic**. `decoder.isa` is the top-level decode tree: `DEBUGSTEP → ILLEGALEXEC → DECODERFAULT → THUMB → AARCH64`. Sub-files: `arm.isa` (AArch32 ARM mode), `thumb.isa` (Thumb-16/32), `aarch64.isa` (AArch64). |
| `formats/` | Instruction format definitions: `basic.isa`, `branch.isa`, `data.isa`, `mem.isa`, `misc.isa`, `aarch64.isa`, `fp.isa`, `sve_*.isa`, `sme.isa`, `neon64.isa`, `crypto64.isa`, `pred.isa`, `pseudo.isa`, `breakpoint.isa`. |
| `insts/` | Instruction behavior definitions (ISA language): `data.isa`, `branch.isa`, `mem.isa`, `ldr.isa`, `str.isa`, `misc.isa`, `data64.isa`, `aarch64.isa`, `sve.isa`, `sme.isa`, etc. |
| `templates/` | C++ code generation templates for instruction classes. |

## Decoder Flow
```
decode DEBUGSTEP {
  0: decode ILLEGALEXEC {
    0: decode DECODERFAULT {
      0: decode THUMB {
        0: decode AARCH64 {
          0: arm.isa      // AArch32 ARM mode
          1: aarch64.isa  // AArch64
        }
        1: thumb.isa      // AArch32 Thumb mode
      }
    }
  }
}
```

M-profile processors use Thumb-only, so the `thumb.isa` decoder path is directly relevant.
The `THUMB` bitfield comes from `ExtMachInst.thumb` (bit 36 of `types.hh`).

## Where to Look Next

| Task | Go To |
|------|-------|
| Modify Thumb instruction decoding | `decoder/thumb.isa` |
| Add new instruction format | `formats/` |
| Add new instruction behavior | `insts/` |
| Add M-profile-specific MRS/MSR for special registers | `insts/misc.isa` or create new entries; also `formats/misc.isa` |
| Understand operand mapping | `operands.isa` |
| Force Thumb-only mode for M-profile | `decoder/decoder.isa` (skip ARM mode path) |
