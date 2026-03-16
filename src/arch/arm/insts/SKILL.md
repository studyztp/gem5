# ARM Instruction Implementations - Navigation

## What This Directory Contains
C++ implementations of ARM instruction classes. These are the runtime execution
implementations that the ISA-generated decoder instantiates.

## Key Files

| File | Purpose |
|------|---------|
| `static_inst.hh` / `static_inst.cc` | `ArmStaticInst` base class. Handles Thumb/ARM instruction length (`(!machInst.thumb \|\| machInst.bigThumb) ? 4 : 2`), PC offset calculation, condition code evaluation, shift operations. |
| `branch.hh` / `branch.cc` | AArch32 branch instructions (B, BL, BX, BLX). |
| `branch64.hh` / `branch64.cc` | AArch64 branch instructions. |
| `data64.hh` / `data64.cc` | AArch64 data processing instructions. |
| `mem.hh` / `mem.cc` | AArch32 memory access instructions (LDR, STR base classes). |
| `mem64.hh` / `mem64.cc` | AArch64 memory access instructions. |
| `macromem.hh` / `macromem.cc` | Macro-memory operations (LDM, STM, PUSH, POP — critical for M-profile exception stacking). |
| `misc.hh` / `misc.cc` | AArch32 miscellaneous instructions (MRS, MSR, CPS, barriers, hints). |
| `misc64.hh` / `misc64.cc` | AArch64 miscellaneous instructions. |
| `vfp.hh` / `vfp.cc` | VFP/floating-point instructions. |
| `fplib.hh` / `fplib.cc` | Floating-point library. |
| `crypto.hh` / `crypto.cc` | Cryptographic extension instructions. |
| `sve.hh` / `sve.cc` | SVE vector instructions. |
| `sme.hh` / `sme.cc` | SME matrix instructions. |
| `pred_inst.hh` / `pred_inst.cc` | Predicated instruction support. |
| `pseudo.hh` / `pseudo.cc` | Pseudo-instructions (used by gem5 internally). |
| `mult.hh` | Multiply instruction helpers. |

## Where to Look Next

| Task | Go To |
|------|-------|
| Add M-profile MRS/MSR for special registers (MSP, PSP, CONTROL, etc.) | `misc.hh` / `misc.cc` — AArch32 MRS/MSR implementation |
| Modify exception stacking (PUSH/POP for M-profile context save) | `macromem.hh` / `macromem.cc` |
| Understand base instruction class | `static_inst.hh` |
| Add M-profile-specific instructions | Create entries in `misc.cc` or add new file |
