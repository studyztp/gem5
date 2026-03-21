/*
 * test_deliberate_fail.S — Firmware that ALWAYS reports failure.
 *
 * Purpose (Bug 21 verification):
 *   This firmware is a "poison test".  It intentionally writes the FAIL
 *   magic value (0xDEADDEAD) to the TEST_RESULT address in SRAM and then
 *   spins in an infinite loop.
 *
 *   When run against the UNFIXED test verifier (run_m4_test.py), this test
 *   will "pass" because the verifier only checks that the simulation hit the
 *   tick limit — it never reads SRAM.  This proves Bug 21 is real.
 *
 *   After Bug 21 is fixed (verifier reads SRAM), this test will correctly
 *   report FAILURE, and it becomes a permanent regression guard: if the
 *   verifier ever regresses back to a no-op, this test will start "passing"
 *   again (which is wrong) and will catch the regression.
 *
 * Test result protocol (same as all other M-profile tests):
 *   0x20000100 (TEST_RESULT)  = 0xDEADDEAD  (FAIL — written by this firmware)
 *   0x20000104 (TEST_SUBTEST) = 99           (sentinel: "always-fail" firmware)
 *
 * Expected behavior:
 *   With unfixed verifier:  test PASSES  (false positive — proves Bug 21)
 *   With fixed verifier:    test FAILS   (correct — verifier reads 0xDEADDEAD)
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    /* ------------------------------------------------------------------ */
    /* Test result addresses — same protocol as all other M-profile tests  */
    /* ------------------------------------------------------------------ */
    .equ TEST_RESULT,   0x20000100   /* SRAM1 — write PASS_VAL or FAIL_VAL */
    .equ TEST_SUBTEST,  0x20000104   /* SRAM1 — which subtest failed (1-based) */
    .equ FAIL_VAL,      0xDEADDEAD  /* failure magic number */

    /* ------------------------------------------------------------------ */
    /* Vector table — required by Cortex-M: CPU reads initial SP and       */
    /* Reset_Handler address from the first two words of this section      */
    /* on reset.  All other vectors point to default_handler.              */
    /* ------------------------------------------------------------------ */
    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000          /* word 0: initial MSP (top of SRAM1) */
    .word   Reset_Handler       /* word 1: Reset vector — first code to run */
    .word   default_handler     /* NMI (exc 2) */
    .word   default_handler     /* HardFault (exc 3) */
    .word   default_handler     /* MemManage (exc 4) */
    .word   default_handler     /* BusFault (exc 5) */
    .word   default_handler     /* UsageFault (exc 6) */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   default_handler     /* SVCall (exc 11) */
    .word   0                   /* Debug Monitor */
    .word   0                   /* Reserved */
    .word   default_handler     /* PendSV (exc 14) */
    .word   default_handler     /* SysTick (exc 15) */

    /* ------------------------------------------------------------------ */
    /* Code section                                                         */
    /* ------------------------------------------------------------------ */
    .section .text

    /*
     * Reset_Handler — entry point after CPU reset.
     *
     * Deliberately writes FAIL_VAL to SRAM and loops forever.
     * The simulation exits when the tick limit is reached.
     *
     * If the test verifier is working correctly, it will read
     * 0xDEADDEAD from 0x20000100 and report test failure.
     * If the verifier is broken (Bug 21), it will report success.
     */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* Write FAIL_VAL to TEST_RESULT.
     * ldr rx, =CONST loads a 32-bit constant via the literal pool.
     * str writes it to the given address. */
    ldr     r0, =FAIL_VAL         /* r0 = 0xDEADDEAD */
    ldr     r1, =TEST_RESULT      /* r1 = 0x20000100 */
    str     r0, [r1]              /* mem[0x20000100] = 0xDEADDEAD */

    /* Write subtest sentinel 99 to TEST_SUBTEST.
     * 99 = "this firmware always fails; not a specific subtest" */
    mov     r0, #99
    ldr     r1, =TEST_SUBTEST     /* r1 = 0x20000104 */
    str     r0, [r1]              /* mem[0x20000104] = 99 */

    /* Exit via ARM semihosting SYS_EXIT.
     * A working verifier must read SRAM to determine the outcome;
     * it cannot rely solely on the exit event. */
    mov     r0, #0x18           /* SYS_EXIT */
    ldr     r1, =0x20026        /* ADP_Stopped_ApplicationExit */
    bkpt    #0xab               /* M-profile semihosting trigger */
    b       .                   /* unreachable */

    /*
     * default_handler — catches all unexpected exceptions.
     * If any interrupt fires, we end up here and spin.
     */
    .thumb_func
default_handler:
    b       .
