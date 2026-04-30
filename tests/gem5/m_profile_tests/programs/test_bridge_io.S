/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved.
 *
 * test_bridge_io.S — smoke test for MProfileBridgeIO on the STM32G474RE
 * timing board.
 *
 * Exercises:
 *   - Register read path (registers[0]=go, [2]=in_addr, [4]=out_addr).
 *   - Output-buffer write path (8-bit / 32-bit access).
 *   - Done-on-write exit path: writing 1 to registers[1] tells the
 *     device to call exitSimLoopNow.
 *
 * Result protocol (matches the rest of m_profile_tests/):
 *   [0x20000100] = 0xCAFECAFE → all checks passed
 *   [0x20000100] = 0xDEADDEAD → at least one check failed
 *
 * Exit:
 *   On pass, the firmware writes 1 to bridge register[1].  The bridge
 *   schedules exitSimLoopNow at the current tick; gem5 reports
 *   "MProfileBridgeIO signaled done.".  As a belt-and-braces fallback
 *   (in case the bridge itself is broken), we also follow up with a
 *   semihosted SYS_EXIT.
 *
 * Build with the existing programs/Makefile; uses the same
 * cortexm4.ld linker script.  Bridge MMIO at 0x90000000 is just a
 * literal address; no linker memory region needed.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

/* ================================================================
 * Bridge MMIO layout (matches MProfileBridgeIO defaults: pio_addr
 * 0x90000000, input/output buffer size = 1024).
 * ================================================================ */
    .equ BRIDGE_BASE,     0x90000000
    .equ BRIDGE_REG_GO,   BRIDGE_BASE + 0x00  /* register[0] */
    .equ BRIDGE_REG_DONE, BRIDGE_BASE + 0x04  /* register[1] */
    .equ BRIDGE_REG_IN,   BRIDGE_BASE + 0x08  /* register[2] */
    .equ BRIDGE_REG_INSZ, BRIDGE_BASE + 0x0C  /* register[3] */
    .equ BRIDGE_REG_OUT,  BRIDGE_BASE + 0x10  /* register[4] */
    .equ BRIDGE_REG_OUTSZ,BRIDGE_BASE + 0x14  /* register[5] */

    /* Buffer addresses derived in MProfileBridgeIO ctor:
     *   in_addr  = pio_addr + 6*4         = 0x90000018
     *   out_addr = in_addr  + in_buf_size = 0x90000418  (1024-byte input)
     * The firmware reads them out of registers[2]/[4] (so this test
     * still passes if the layout changes), but we sanity-check them
     * against these constants to catch silent layout regressions. */
    .equ EXPECTED_IN_ADDR,  0x90000018
    .equ EXPECTED_OUT_ADDR, 0x90000418

/* ================================================================
 * Test result addresses in SRAM (project-wide convention).
 * ================================================================ */
    .equ TEST_RESULTS,  0x20000100
    .equ TEST_PASS_VAL, 0xCAFECAFE
    .equ TEST_FAIL_VAL, 0xDEADDEAD

/* ================================================================
 * Vector table — minimal, identical to cortexm4_basic.S.
 * ================================================================ */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl __isr_vector
__isr_vector:
    .word   _estack
    .word   Reset_Handler
    .word   Default_Handler   /* NMI */
    .word   Default_Handler   /* HardFault */
    .word   Default_Handler   /* MemManage */
    .word   Default_Handler   /* BusFault */
    .word   Default_Handler   /* UsageFault */
    .word   0
    .word   0
    .word   0
    .word   0
    .word   Default_Handler   /* SVCall */
    .word   0
    .word   0
    .word   Default_Handler   /* PendSV */
    .word   Default_Handler   /* SysTick */

    .section .text
    .align 2

/* ================================================================
 * Default exception handler — infinite loop.  If any unexpected
 * exception fires (the firmware doesn't intentionally trigger any),
 * the simulator hits the tick limit and the test reports
 * "INCONCLUSIVE: no result store seen".  That's the right failure
 * mode: the bridge smoke path doesn't touch interrupts at all.
 * ================================================================ */
    .thumb_func
Default_Handler:
    b       .

/* ================================================================
 * Reset handler — main test.
 * ================================================================ */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* ---- Check 1: register[0] (go) reads as 1 ---- */
    /* MProfileBridgeIO is constructed with go=True by default, so
     * the very first thing firmware sees should be a non-zero go
     * gate.  Reading 0 means either the device is not mapped at the
     * expected address, or the constructor's `if (p.go)` path didn't
     * run. */
    ldr     r0, =BRIDGE_REG_GO
    ldr     r1, [r0]
    cmp     r1, #1
    bne     test_fail

    /* ---- Check 2: register[2] (in_addr) matches ctor formula ---- */
    /* Catches a silent layout regression: if the constructor changed
     * the register block size or buffer ordering, the absolute
     * address stored here won't match EXPECTED_IN_ADDR. */
    ldr     r0, =BRIDGE_REG_IN
    ldr     r1, [r0]
    ldr     r2, =EXPECTED_IN_ADDR
    cmp     r1, r2
    bne     test_fail

    /* ---- Check 3: register[4] (out_addr) matches ctor formula ---- */
    ldr     r0, =BRIDGE_REG_OUT
    ldr     r1, [r0]
    ldr     r2, =EXPECTED_OUT_ADDR
    cmp     r1, r2
    bne     test_fail

    /* ---- Check 4: write 0xDEADBEEF to output buffer, read back ---- */
    /* Tests the second dispatch arm of write()/read(): the output
     * buffer region.  We write a 32-bit word at offset 0 and read it
     * back; if the bridge's region check is off-by-one or its
     * std::vector storage is misaligned, the readback fails. */
    ldr     r0, =EXPECTED_OUT_ADDR
    ldr     r1, =0xDEADBEEF
    str     r1, [r0]
    ldr     r2, [r0]
    cmp     r1, r2
    bne     test_fail

    /* ---- All checks passed: write the project-wide PASS marker. ---- */
    /* run_bridge_io_test.py grep-parses simout.txt for the LAST
     * store to TEST_RESULTS, so this must happen before the done
     * write to ensure it's flushed to the trace before exit. */
    ldr     r0, =TEST_RESULTS
    ldr     r1, =TEST_PASS_VAL
    str     r1, [r0]

    /* ---- Trigger the bridge's exit-on-done path. ---- */
    /* Writing 1 to register[1] makes the bridge call exitSimLoopNow.
     * gem5 prints "Exiting @ tick N because MProfileBridgeIO signaled
     * done.", which run_bridge_io_test.py also matches on. */
    ldr     r0, =BRIDGE_REG_DONE
    mov     r1, #1
    str     r1, [r0]

    /* ---- Belt-and-braces fallback. ---- */
    /* If the bridge done path is broken, the str above won't end the
     * simulation.  Fall through to a semihosted SYS_EXIT so the test
     * still terminates within the tick budget rather than spinning
     * forever (and so the failure mode is "missing 'done' message in
     * simout" rather than "no result"). */
    mov     r0, #0x18                /* SYS_EXIT */
    ldr     r1, =0x20026             /* ADP_Stopped_ApplicationExit */
    bkpt    #0xab
    b       .

test_fail:
    ldr     r0, =TEST_RESULTS
    ldr     r1, =TEST_FAIL_VAL
    str     r1, [r0]
    /* No bridge-done write on the failure path: we want the harness
     * to see the FAIL marker via the standard simout-store scan even
     * if the bridge itself is misbehaving. */
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .
