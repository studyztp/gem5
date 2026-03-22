/*
 * test_control_spsel.S — CONTROL.SPSEL R13 swap test (BUG-6)
 *
 * Verifies that writing CONTROL.SPSEL via MSR switches R13 between
 * MSP and PSP (DDI0403E B1.4.4).
 *
 * Before the fix, setMiscReg() has no case for MISCREG_M_CONTROL,
 * so the CONTROL register value is stored but R13 is never swapped.
 * After MSR CONTROL with SPSEL=1, SP still points to MSP instead of PSP.
 *
 * =========================================================================
 * Test design:
 *
 *   Setup: Save initial MSP value for later comparison.
 *
 *   Subtest 1: Set PSP to known value 0x20010000 via MSR PSP
 *              Read back via MRS PSP — verify == 0x20010000
 *
 *   Subtest 2: MSR CONTROL, #2 (SPSEL=1 → switch to PSP) + ISB
 *              Read SP (R13) — must equal 0x20010000 (PSP value)
 *              Before fix: SP still has MSP value
 *
 *   Subtest 3: Write a marker to [SP] to prove we're using PSP memory
 *              (SP should be in PSP region 0x20010000, not MSP region)
 *
 *   Subtest 4: MSR CONTROL, #0 (SPSEL=0 → switch back to MSP) + ISB
 *              Read SP — must equal original MSP value
 *
 *   Subtest 5: Verify PSP still holds 0x20010000 via MRS PSP
 *
 * =========================================================================
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000108  TEST_DATA
 *   0x20000200  saved_msp  — initial MSP value for comparison
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    .equ SAVED_MSP,     0x20000200
    .equ PSP_VALUE,     0x20010000

/* =========================================================================
 * Vector table
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl _vector_table
_vector_table:
    .word _estack
    .word Reset_Handler

/* =========================================================================
 * Macros
 * ========================================================================= */
.macro fail_test subtest_num, obs_reg
    /* Before failing, switch back to MSP so the fail code has a valid
     * stack.  CONTROL=0 → SPSEL=0 → MSP. */
    movs r5, #0
    msr control, r5
    isb
    ldr r6, =TEST_SUBTEST
    movs r7, #\subtest_num
    str r7, [r6]
    ldr r6, =TEST_DATA
    str \obs_reg, [r6]
    ldr r6, =TEST_RESULT
    ldr r7, =FAIL_VAL
    str r7, [r6]
    b test_exit
.endm

/* =========================================================================
 * Reset Handler
 * ========================================================================= */
    .text
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Save initial MSP for later comparison */
    mrs r4, msp                 /* r4 = initial MSP (kept for all subtests) */
    ldr r0, =SAVED_MSP
    str r4, [r0]

/* -- Subtest 1: Set PSP to known value, verify via MRS -- */
    ldr r0, =PSP_VALUE          /* r0 = 0x20010000 */
    msr psp, r0                 /* write PSP */
    mrs r2, psp                 /* read back PSP */
    cmp r2, r0
    bne 1f
    b 2f
1:  fail_test 1, r2
2:

/* -- Subtest 2: Switch to PSP via CONTROL.SPSEL=1 -- */
/* After this, R13 should reflect PSP (0x20010000), not MSP. */
    movs r0, #2                 /* CONTROL = SPSEL=1, nPRIV=0 */
    msr control, r0
    isb                         /* ISB required after CONTROL write */

    /* Read SP — should be PSP value */
    mov r2, sp                  /* r2 = current SP (should be PSP) */
    ldr r3, =PSP_VALUE
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 2, r2
2:

/* -- Subtest 3: Verify we can use PSP memory region -- */
/* Store a marker at [SP-4] to prove SP is in the PSP region.
 * This would crash or corrupt data if SP were still MSP. */
    ldr r1, =0xA5A5A5A5
    str r1, [sp, #-4]!         /* push marker to PSP stack */
    ldr r2, [sp]               /* read back */
    adds sp, sp, #4            /* restore SP */
    ldr r3, =0xA5A5A5A5
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 3, r2
2:

/* -- Subtest 4: Switch back to MSP via CONTROL.SPSEL=0 -- */
    movs r0, #0                 /* CONTROL = SPSEL=0, nPRIV=0 */
    msr control, r0
    isb

    /* Read SP — should be original MSP value */
    mov r2, sp
    cmp r2, r4                  /* r4 = saved initial MSP */
    bne 1f
    b 2f
1:  fail_test 4, r2
2:

/* -- Subtest 5: Verify PSP still holds its value via MRS -- */
    mrs r2, psp
    ldr r3, =PSP_VALUE
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 5, r2
2:

/* -- All subtests passed -- */
test_pass:
    ldr r0, =TEST_RESULT
    ldr r1, =PASS_VAL
    str r1, [r0]

test_exit:
    mov r0, #0x18
    ldr r1, =0x20026
    bkpt #0xAB
    b .
