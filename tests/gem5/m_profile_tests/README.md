# M-Profile Integration Tests

## Overview

These tests validate the gem5 Cortex-M (M-profile) implementation by
running actual ARM Thumb firmware on the simulated board and checking
the execution trace for correctness.

## Prerequisites

```bash
# Install ARM embedded toolchain
sudo apt install gcc-arm-none-eabi

# Build gem5 (ARM ISA)
scons build/ARM/gem5.opt -j$(nproc)
```

## Build test firmware

```bash
cd tests/gem5/m_profile_tests/programs
make
```

This produces `cortexm4_basic.elf` — a minimal Cortex-M4 firmware that tests:
1. Basic data processing (MOV, ADD, SUB)
2. Load/store to SRAM
3. MRS/MSR for PRIMASK and CONTROL
4. CPS (CPSID/CPSIE)
5. SVC (SVCall exception)
6. SysTick interrupt

## Run tests

### Basic run (no trace)
```bash
./build/ARM/gem5.opt tests/gem5/m_profile_tests/configs/run_m4_test.py \
    --firmware tests/gem5/m_profile_tests/programs/cortexm4_basic.elf
```

### With execution trace (for validation)
```bash
./build/ARM/gem5.opt --debug-flags=Exec \
    tests/gem5/m_profile_tests/configs/run_m4_test.py \
    --firmware tests/gem5/m_profile_tests/programs/cortexm4_basic.elf \
    --tick-limit 100000
```

### With CPSR alias trace (for debugging)
```bash
./build/ARM/gem5.opt --debug-flags=Exec,MProfileCPSR \
    tests/gem5/m_profile_tests/configs/run_m4_test.py \
    --firmware tests/gem5/m_profile_tests/programs/cortexm4_basic.elf
```

### With full decode trace
```bash
./build/ARM/gem5.opt --debug-flags=Exec,Decode,MProfileCPSR \
    tests/gem5/m_profile_tests/configs/run_m4_test.py \
    --firmware tests/gem5/m_profile_tests/programs/cortexm4_basic.elf
```

## Validation checklist

### 1. Correct boot sequence
In the Exec trace, verify:
- First instruction fetched from Reset_Handler address (read from VTOR+4)
- SP initialized to _estack (0x20020000, read from VTOR+0)
- VTOR points to 0x08000000 (flash base)

Look for:
```
system.cpu: T0 : @0x8000040 (thumb) : mov   r0, #0
```
(The exact address depends on the linker, but it should be in flash > 0x08000000)

### 2. Correct instruction decoding
Check that M-profile instructions are decoded by MDecoder:
```
MDecoder: M-profile decoded mrs: ...
MDecoder: M-profile decoded msr: ...
MDecoder: M-profile decoded cpsid: ...
MDecoder: M-profile decoded cpsie: ...
MDecoder: M-profile decoded svc: ...
MDecoder: M-profile decoded bx: ...
```

Regular Thumb instructions (MOV, ADD, LDR, STR) should NOT show
"MDecoder" in the trace — they fall through to the standard decoder.

### 3. Correct xPSR / CPSR alias behavior
With `--debug-flags=MProfileCPSR`, check:
- Every LDR/STR shows a CPSR read alias trace (reading bit[9]=0 for LE)
- No "WARNING: CPSR full write alias" messages (those mean an
  instruction we should have intercepted is writing CPSR)
- CpsrQ writes from DSP instructions show the Q flag alias trace

### 4. SVCall exception
After the `svc #0` instruction:
- The trace should show SVCall_Handler executing
- The handler writes 0xCAFECAFE to 0x20000100
- Exception return via `bx lr` (EXC_RETURN) should return to the
  instruction after SVC

### 5. SysTick interrupt
After SysTick is enabled:
- The SysTick expiry event should fire
- SysTick_Handler should execute
- The handler writes 0xCAFECAFE to 0x20000104
- Exception return via `bx lr` should return to the wait loop
- The wait loop should detect the flag and exit

### 6. Final test result
If all tests pass, the firmware writes 0xCAFECAFE to 0x20000108.
If any test fails, it writes 0xDEADDEAD.

In the trace, look for the final STR instruction:
```
: str   r0, [r4, #8]   ; r0 should be 0xCAFECAFE
```

## Test firmware memory map

| Address | Content |
|---|---|
| 0x08000000 | Vector table (initial SP, Reset_Handler, ...) |
| 0x08000040+ | Code (Reset_Handler, exception handlers) |
| 0x20000000 | SRAM base (stack grows down from 0x20020000) |
| 0x20000100 | Test result: SVCall (0xCAFECAFE = pass) |
| 0x20000104 | Test result: SysTick (0xCAFECAFE = pass) |
| 0x20000108 | Test result: Final (0xCAFECAFE = all pass) |
| 0xE000E010 | SysTick CSR (SCS device) |
| 0xE000ED08 | SCB VTOR (SCS device) |
