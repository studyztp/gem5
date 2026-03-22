/*
 * test_nvic_irq_mask.S — NVIC unimplemented IRQ bit masking (BUG-4)
 *
 * Verifies that NVIC registers return RAZ/WI (Read-As-Zero, Write-Ignored)
 * for bits corresponding to unimplemented IRQs (DDI0403E B3.4.3).
 *
 * Uses the default STM32F405 platform with numIRQs=82.
 *   Word 2 covers IRQs 64-95:
 *     bits 0-17  = IRQs 64-81 (implemented)
 *     bits 18-31 = IRQs 82-95 (NOT implemented, must RAZ/WI)
 *   Word 3 covers IRQs 96-127: entirely unimplemented (all RAZ/WI)
 *
 * Before the fix, readNVIC() returns raw array values without masking,
 * and writeNVIC() stores bits for unimplemented IRQs.  Firmware that
 * probes for implemented interrupts by writing 0xFFFFFFFF and reading
 * back would incorrectly see non-existent IRQs as implemented.
 *
 * =========================================================================
 * Test design:
 *
 *   Subtest 1: Write 0xFFFFFFFF to ISER[2], read back
 *              Expected: 0x0003FFFF (bits 0-17 only)
 *
 *   Subtest 2: Clean up — write 0xFFFFFFFF to ICER[2], read ISER[2]
 *              Expected: 0x00000000
 *
 *   Subtest 3: Write 0xFFFFFFFF to ISPR[2], read back
 *              Expected: 0x0003FFFF (bits 0-17 only)
 *
 *   Subtest 4: Clean up — write 0xFFFFFFFF to ICPR[2], read ISPR[2]
 *              Expected: 0x00000000
 *
 *   Subtest 5: Read IABR[2] — should be 0, with no stale bits
 *              Expected: 0x00000000
 *
 *   Subtest 6: Write 0xFFFFFFFF to ISER[3] (entirely beyond numIRQs),
 *              read back
 *              Expected: 0x00000000 (all RAZ)
 *
 *   Subtest 7: Clean up ISER[3] via ICER[3]
 *
 * =========================================================================
 * Key constant: 0x0003FFFF = 18 bits set = IRQs 64-81 (82 - 64 = 18)
 *
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

    /* NVIC register addresses for word 2 (IRQs 64-95) */
    .equ NVIC_ISER2,    0xE000E108
    .equ NVIC_ICER2,    0xE000E188
    .equ NVIC_ISPR2,    0xE000E208
    .equ NVIC_ICPR2,    0xE000E288
    .equ NVIC_IABR2,    0xE000E308

    /* NVIC register addresses for word 3 (IRQs 96-127, all unimplemented) */
    .equ NVIC_ISER3,    0xE000E10C
    .equ NVIC_ICER3,    0xE000E18C

    /* Expected mask for word 2: numIRQs=82, word 2 starts at IRQ 64,
     * 82-64=18 implemented bits → mask = (1<<18)-1 = 0x0003FFFF */
    .equ WORD2_MASK,    0x0003FFFF

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

/* -- Subtest 1: Write 0xFFFFFFFF to ISER[2], read back -- */
/* Only bits 0-17 should stick (IRQs 64-81). Bits 18-31 are
 * unimplemented and must read as zero (RAZ/WI). */
    ldr r0, =NVIC_ISER2
    ldr r1, =0xFFFFFFFF
    str r1, [r0]               /* write all-ones to ISER[2] */
    ldr r2, [r0]               /* read back */
    ldr r3, =WORD2_MASK        /* expected: 0x0003FFFF */
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 1, r2
2:

/* -- Subtest 2: Clean up ISER[2] via ICER[2], verify zero -- */
    ldr r0, =NVIC_ICER2
    ldr r1, =0xFFFFFFFF
    str r1, [r0]               /* clear all enable bits */
    ldr r0, =NVIC_ISER2
    ldr r2, [r0]               /* read back ISER[2] */
    cmp r2, #0
    bne 1f
    b 2f
1:  fail_test 2, r2
2:

/* -- Subtest 3: Write 0xFFFFFFFF to ISPR[2], read back -- */
/* Same mask applies to pending bits. */
    ldr r0, =NVIC_ISPR2
    ldr r1, =0xFFFFFFFF
    str r1, [r0]               /* pend all IRQs in word 2 */
    ldr r2, [r0]               /* read back */
    ldr r3, =WORD2_MASK
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 3, r2
2:

/* -- Subtest 4: Clean up ISPR[2] via ICPR[2], verify zero -- */
    ldr r0, =NVIC_ICPR2
    ldr r1, =0xFFFFFFFF
    str r1, [r0]
    ldr r0, =NVIC_ISPR2
    ldr r2, [r0]
    cmp r2, #0
    bne 1f
    b 2f
1:  fail_test 4, r2
2:

/* -- Subtest 5: Read IABR[2] — should be 0 -- */
    ldr r0, =NVIC_IABR2
    ldr r2, [r0]
    cmp r2, #0
    bne 1f
    b 2f
1:  fail_test 5, r2
2:

/* -- Subtest 6: ISER[3] entirely beyond numIRQs — all RAZ -- */
/* Word 3 = IRQs 96-127.  With numIRQs=82, none are implemented. */
    ldr r0, =NVIC_ISER3
    ldr r1, =0xFFFFFFFF
    str r1, [r0]               /* write all-ones — should be ignored */
    ldr r2, [r0]               /* read back */
    cmp r2, #0                 /* expect all zeros (RAZ) */
    bne 1f
    b 2f
1:  fail_test 6, r2
2:

/* -- Subtest 7: Clean up ISER[3] -- */
    ldr r0, =NVIC_ICER3
    ldr r1, =0xFFFFFFFF
    str r1, [r0]

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
