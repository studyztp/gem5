/*
 * Test: CPS instruction encoding verification
 *
 * Tests all CPS variants to verify MDecoder bit extraction:
 *   CPSID i  → PRIMASK = 1
 *   CPSIE i  → PRIMASK = 0
 *   CPSID f  → FAULTMASK = 1
 *   CPSIE f  → FAULTMASK = 0
 *   CPSID if → both = 1
 *   CPSIE if → both = 0
 *
 * Result at 0x20000100:
 *   0xCAFECAFE = all tests passed
 *   0xDEADDEAD = test failed
 *   0x20000104 = which sub-test failed (1-6)
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,  0x20000100
    .equ TEST_SUBTEST, 0x20000104
    .equ PASS_VAL,     0xCAFECAFE
    .equ FAIL_VAL,     0xDEADDEAD

    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000
    .word   Reset_Handler
    .fill   14, 4, 0

    .section .text
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Ensure clean state */
    mov     r0, #0
    msr     PRIMASK, r0
    msr     FAULTMASK, r0

    /* --- Test 1: CPSID i should set PRIMASK=1 --- */
    cpsid   i
    mrs     r0, PRIMASK
    cmp     r0, #1
    bne     fail_1

    /* --- Test 2: CPSIE i should clear PRIMASK=0 --- */
    cpsie   i
    mrs     r0, PRIMASK
    cmp     r0, #0
    bne     fail_2

    /* --- Test 3: CPSID f should set FAULTMASK=1 --- */
    cpsid   f
    mrs     r0, FAULTMASK
    cmp     r0, #1
    bne     fail_3

    /* --- Test 4: CPSIE f should clear FAULTMASK=0 --- */
    cpsie   f
    mrs     r0, FAULTMASK
    cmp     r0, #0
    bne     fail_4

    /* --- Test 5: CPSID if should set both --- */
    cpsid   if
    mrs     r0, PRIMASK
    cmp     r0, #1
    bne     fail_5
    mrs     r0, FAULTMASK
    cmp     r0, #1
    bne     fail_5

    /* --- Test 6: CPSIE if should clear both --- */
    cpsie   if
    mrs     r0, PRIMASK
    cmp     r0, #0
    bne     fail_6
    mrs     r0, FAULTMASK
    cmp     r0, #0
    bne     fail_6

    /* All passed */
    ldr     r0, =PASS_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    b       done

fail_1: mov r2, #1
    b fail
fail_2: mov r2, #2
    b fail
fail_3: mov r2, #3
    b fail
fail_4: mov r2, #4
    b fail
fail_5: mov r2, #5
    b fail
fail_6: mov r2, #6
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    b       done
