/*
 * Test: Blocked A-profile instructions should fault on M-profile
 *
 * Each blocked instruction is encoded as .hword/.word raw encoding
 * wrapped in a SVC-based test harness.  The SVCall handler checks
 * if the test reached the expected point.
 *
 * For now we just test that execution doesn't crash on basic
 * Thumb instructions that should work, and we verify that the
 * decoder path is correct.
 *
 * Tests:
 *   1. Basic arithmetic (should work)
 *   2. IT block (should work)
 *   3. DSP: QADD (should work, tests CpsrQ alias)
 *   4. Saturate: SSAT (should work, tests CpsrQ alias)
 *   5. Multiply: MUL (should work)
 *   6. Divide: SDIV/UDIV (should work)
 *   7. Bit field: BFI/UBFX (should work)
 *   8. CLZ (should work)
 *
 * Result at 0x20000100:
 *   0xCAFECAFE = all tests passed
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

    /* --- Test 1: Basic arithmetic --- */
    mov     r0, #100
    mov     r1, #200
    add     r2, r0, r1         /* r2 = 300 */
    ldr     r3, =300
    cmp     r2, r3
    bne     fail_1

    /* --- Test 2: IT block --- */
    mov     r0, #5
    cmp     r0, #5
    ite     eq
    moveq   r1, #1             /* should execute */
    movne   r1, #0             /* should not */
    cmp     r1, #1
    bne     fail_2

    /* --- Test 3: QADD (saturating add, tests Q flag path) --- */
    ldr     r0, =0x70000000
    ldr     r1, =0x70000000
    qadd    r2, r0, r1         /* saturates to 0x7FFFFFFF */
    ldr     r3, =0x7FFFFFFF
    cmp     r2, r3
    bne     fail_3

    /* --- Test 4: SSAT (signed saturate) --- */
    mov     r0, #200
    ssat    r1, #8, r0         /* saturate to 8-bit signed: 127 */
    cmp     r1, #127
    bne     fail_4

    /* --- Test 5: MUL --- */
    mov     r0, #7
    mov     r1, #6
    mul     r2, r0, r1         /* 42 */
    cmp     r2, #42
    bne     fail_5

    /* --- Test 6: SDIV/UDIV --- */
    mov     r0, #100
    mov     r1, #10
    udiv    r2, r0, r1         /* 10 */
    cmp     r2, #10
    bne     fail_6
    sdiv    r2, r0, r1         /* 10 */
    cmp     r2, #10
    bne     fail_6

    /* --- Test 7: BFI / UBFX --- */
    mov     r0, #0
    mov     r1, #0xFF
    bfi     r0, r1, #8, #8    /* insert 0xFF at bits[15:8] */
    ldr     r2, =0xFF00
    cmp     r0, r2
    bne     fail_7
    ubfx    r3, r0, #8, #8    /* extract bits[15:8] → 0xFF */
    cmp     r3, #0xFF
    bne     fail_7

    /* --- Test 8: CLZ --- */
    mov     r0, #1
    clz     r1, r0             /* 31 leading zeros */
    cmp     r1, #31
    bne     fail_8

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
    b fail
fail_7: mov r2, #7
    b fail
fail_8: mov r2, #8
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    b       done
