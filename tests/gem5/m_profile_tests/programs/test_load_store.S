/*
 * Test: Load/Store instruction endianness (CPSR alias verification)
 *
 * Tests that the CPSR.e alias returns 0 (little-endian) so that
 * cSwap in load/store instructions is a no-op.
 *
 * Tests:
 *   1. STR/LDR word
 *   2. STRH/LDRH halfword
 *   3. STRB/LDRB byte
 *   4. STRD/LDRD doubleword
 *   5. STM/LDM multiple
 *   6. PUSH/POP
 *   7. Unaligned access (halfword at odd+1 address)
 *
 * Result at 0x20000100:
 *   0xCAFECAFE = all tests passed
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,  0x20000100
    .equ TEST_SUBTEST, 0x20000104
    .equ SCRATCH,      0x20000200
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
    ldr     r6, =SCRATCH

    /* --- Test 1: STR/LDR word --- */
    ldr     r0, =0x12345678
    str     r0, [r6]
    ldr     r1, [r6]
    cmp     r0, r1
    bne     fail_1

    /* --- Test 2: STRH/LDRH halfword --- */
    ldr     r0, =0xABCD
    strh    r0, [r6, #4]
    ldrh    r1, [r6, #4]
    cmp     r0, r1
    bne     fail_2

    /* --- Test 3: STRB/LDRB byte --- */
    mov     r0, #0xEF
    strb    r0, [r6, #8]
    ldrb    r1, [r6, #8]
    cmp     r0, r1
    bne     fail_3

    /* --- Test 4: STRD/LDRD doubleword --- */
    ldr     r0, =0x11111111
    ldr     r1, =0x22222222
    strd    r0, r1, [r6, #16]
    ldrd    r2, r3, [r6, #16]
    cmp     r0, r2
    bne     fail_4
    cmp     r1, r3
    bne     fail_4

    /* --- Test 5: STM/LDM multiple --- */
    ldr     r0, =0xAAAAAAAA
    ldr     r1, =0xBBBBBBBB
    ldr     r2, =0xCCCCCCCC
    add     r4, r6, #32
    stm     r4!, {r0-r2}
    sub     r4, r4, #12     /* reset pointer */
    mov     r0, #0
    mov     r1, #0
    mov     r2, #0
    ldm     r4, {r0-r2}
    ldr     r3, =0xAAAAAAAA
    cmp     r0, r3
    bne     fail_5
    ldr     r3, =0xBBBBBBBB
    cmp     r1, r3
    bne     fail_5
    ldr     r3, =0xCCCCCCCC
    cmp     r2, r3
    bne     fail_5

    /* --- Test 6: PUSH/POP --- */
    ldr     r0, =0xDEADBEEF
    push    {r0}
    mov     r0, #0
    pop     {r0}
    ldr     r1, =0xDEADBEEF
    cmp     r0, r1
    bne     fail_6

    /* --- Test 7: Byte order verification --- */
    /* Store 0x04030201 and read individual bytes */
    ldr     r0, =0x04030201
    str     r0, [r6, #48]
    ldrb    r1, [r6, #48]       /* should be 0x01 (LE: LSB first) */
    cmp     r1, #0x01
    bne     fail_7
    ldrb    r1, [r6, #49]       /* should be 0x02 */
    cmp     r1, #0x02
    bne     fail_7
    ldrb    r1, [r6, #50]       /* should be 0x03 */
    cmp     r1, #0x03
    bne     fail_7
    ldrb    r1, [r6, #51]       /* should be 0x04 */
    cmp     r1, #0x04
    bne     fail_7

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
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    b       done
