/*
 * test_cpuid_readonly.S — CPUID read-only enforcement (BUG-3)
 *
 * Verifies that CPUID at 0xE000ED00 is read-only (DDI0403E B3.2.3).
 * Before the fix, writes to CPUID go through writeSCB() →
 * tc->setMiscReg(MISCREG_M_CPUID, data) → default case in setMiscReg()
 * which overwrites miscRegs[MISCREG_M_CPUID].
 *
 * The test is variant-agnostic: it reads the current CPUID value
 * (whatever the platform configured), writes a different value,
 * and verifies the original value is preserved.
 *
 * =========================================================================
 * Test design:
 *
 *   Subtest 1: Read CPUID, save as 'original' (must be nonzero)
 *   Subtest 2: Write ~original (bitwise NOT) to CPUID
 *   Subtest 3: Read CPUID — must equal original (write discarded)
 *   Subtest 4: Write 0x00000000 to CPUID
 *   Subtest 5: Read CPUID — must equal original (write discarded)
 *
 * =========================================================================
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000108  TEST_DATA
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    .equ CPUID_ADDR,    0xE000ED00

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

/* -- Subtest 1: Read CPUID — save as original, must be nonzero -- */
    ldr r0, =CPUID_ADDR
    ldr r4, [r0]               /* r4 = original CPUID (kept for all subtests) */
    cmp r4, #0                 /* sanity check: CPUID should never be 0 */
    beq 1f
    b 2f
1:  fail_test 1, r4
2:

/* -- Subtest 2: Write ~original to CPUID -- */
    mvn r1, r4                 /* r1 = bitwise NOT of original */
    str r1, [r0]               /* write — should be silently discarded */

/* -- Subtest 3: Read CPUID — must still equal original -- */
    ldr r2, [r0]
    cmp r2, r4
    bne 1f
    b 2f
1:  fail_test 3, r2            /* CPUID was overwritten — bug confirmed */
2:

/* -- Subtest 4: Write 0x00000000 to CPUID -- */
    movs r1, #0
    str r1, [r0]               /* write 0 — should be silently discarded */

/* -- Subtest 5: Read CPUID — must still equal original -- */
    ldr r2, [r0]
    cmp r2, r4
    bne 1f
    b 2f
1:  fail_test 5, r2            /* CPUID was overwritten — bug confirmed */
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
