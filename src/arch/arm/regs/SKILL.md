# ARM Register Definitions - Navigation

## What This Directory Contains
All ARM register definitions: integer registers, miscellaneous/system registers (800+),
vector/SIMD registers, condition code registers, and matrix registers. This is the primary
location for adding new ARM registers (including M-profile registers).

## Key Files

| File | Purpose |
|------|---------|
| `misc.hh` | **`MiscRegIndex` enum** — sequential enum of all system/misc registers (CPSR, SPSRs, CP14, CP15, AArch64 system regs, timer, GIC, etc.). Also defines `MiscRegNum32` (AArch32 MCR/MRC encoding) and `MiscRegNum64` (AArch64 MSR/MRS encoding), plus the `miscRegName[]` string array. |
| `misc.cc` | Decode tables: `miscRegNum32ToIdx` and AArch64 decode maps. Functions: `decodeCP14Reg()`, `decodeCP15Reg()`, `decodeAArch64SysReg()`. Banking helpers: `snsBankedIndex()`, `unflattenMiscReg()`, `preUnflattenMiscReg()`. |
| `misc_types.hh` | **BitUnion types** for system registers: `CPSR`, `SCTLR`, `SCR`, `HCR`, `TCR`, `ESR`, `FPCR`, `FPSCR`, `AA64PFR0`, etc. Use `BitUnion32`/`BitUnion64` to define field layouts. |
| `misc_info.hh` | Register metadata: `MiscRegInfo` flags (IMPLEMENTED, BANKED, SERIALIZING, etc.), `MiscRegLUTEntry` (reset value, res0/res1/raz/rao masks, access control), and `MiscRegLUTEntryInitializer` (chainable builder API). |
| `misc_info.cc` | Implementation of `MiscRegLUTEntryInitializer` methods. |
| `misc_accessors.hh` | Accessor helpers for misc register fields. |
| `int.hh` | Integer register indices: R0-R15, banked registers (SVC, MON, HYP, ABT, UND, IRQ, FIQ), AArch64 X0-X31. Mode-specific maps: `RegUsrMap`, `RegSvcMap`, etc. |
| `int.cc` | Integer register implementation details. |
| `vec.hh` | Vector/SIMD register classes for NEON/SVE. |
| `cc.hh` | Condition code register definitions. |
| `mat.hh` | Matrix registers for SME. |

## How to Add a New Misc Register

1. **Add enum entry** in `misc.hh` → `MiscRegIndex` (before `NUM_PHYS_MISCREGS` if physical).
   If banked: add parent, then `_NS` (parent+1), then `_S` (parent+2).
2. **Add name string** in `miscRegName[]` array in `misc.hh` (must match enum order).
3. **Add decode mapping** in `misc.cc` for AArch32 (`miscRegNum32ToIdx`) or AArch64 equivalent.
4. **Add BitUnion type** in `misc_types.hh` if the register has named bitfields.
5. **Add InitReg metadata** in `isa.cc` → `initializeMiscRegMetadata()`:
   ```cpp
   InitReg(MISCREG_MY_REG)
     .reset(0x...)
     .res0(mask).res1(mask)
     .implemented()
     .priv()  // or .user(), .hyp(), .mon(), etc.
   ```
6. **Add special read/write handling** in `isa.cc` → `readMiscReg()`/`setMiscReg()` if needed.

## Banking Model
- **AArch32**: parent `.banked()`, children `.bankedChild()`. Enum: parent, parent+1 (_NS), parent+2 (_S). Selected via SCR.NS.
- **AArch64**: `.banked64()` flag, same +1/+2 pattern.

## Where to Look Next

| Task | Go To |
|------|-------|
| Add M-profile registers (VTOR, MSP, PSP, CONTROL, etc.) | Start in `misc.hh` (enum), then `misc_types.hh` (BitUnions), then `../isa.cc` (InitReg + read/write) |
| Understand register access control | `misc_info.hh` (MiscRegLUTEntry, access flags) |
| Modify integer register banking | `int.hh` (maps and indices) |
| Add vector/SIMD registers | `vec.hh` |
