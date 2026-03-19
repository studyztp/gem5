/*
 * Test: Nested exceptions and SP integrity
 *
 * Tests:
 *   1. SVC from Thread mode → SVCall handler → return to Thread
 *   2. SVC handler calls a function that modifies SP (stack usage)
 *   3. Verify SP is correct after each exception return
 *   4. Multiple SVCs in sequence (verifies frame push/pop balance)
 *
 * This tests that exception stacking/unstacking correctly uses
 * the architectural SP (R13) and that the stack frames don't
 * corrupt each other.
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,  0x20000100
    .equ TEST_SUBTEST, 0x20000104
    .equ COUNTER,      0x20000108
    .equ PASS_VAL,     0xCAFECAFE
    .equ FAIL_VAL,     0xDEADDEAD

    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000
    .word   Reset_Handler
    .word   default_handler
    .word   default_handler
    .fill   7, 4, 0
    .word   SVCall_Handler
    .fill   4, 4, 0

    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Initialize counter */
    ldr     r4, =COUNTER
    mov     r0, #0
    str     r0, [r4]

    /* --- Test 1: Single SVC, verify SP before/after --- */
    mov     r5, sp              /* save SP before */
    svc     #0
    mov     r6, sp              /* SP after */
    cmp     r5, r6
    bne     fail_1
    /* Check counter incremented */
    ldr     r0, [r4]
    cmp     r0, #1
    bne     fail_1

    /* --- Test 2: Multiple SVCs in sequence --- */
    mov     r5, sp
    svc     #0
    svc     #0
    svc     #0
    mov     r6, sp
    cmp     r5, r6              /* SP should be same after 3 SVCs */
    bne     fail_2
    ldr     r0, [r4]
    cmp     r0, #4              /* counter should be 4 (1 + 3) */
    bne     fail_2

    /* --- Test 3: SVC with stack-heavy code between --- */
    sub     sp, sp, #128        /* allocate 128 bytes on stack */
    mov     r0, #0x42
    str     r0, [sp]            /* write to allocated space */
    mov     r5, sp              /* save modified SP */
    svc     #0
    mov     r6, sp
    cmp     r5, r6              /* SP should still be the modified one */
    bne     fail_3
    ldr     r0, [sp]            /* verify stack data intact */
    cmp     r0, #0x42
    bne     fail_3
    add     sp, sp, #128        /* clean up */

    /* --- Test 4: Register preservation across SVC --- */
    /* R0-R3, R12 are caller-saved (pushed by hardware).
     * They should be restored after exception return. */
    ldr     r0, =0x11111111
    ldr     r1, =0x22222222
    ldr     r2, =0x33333333
    ldr     r3, =0x44444444
    svc     #0                  /* handler clobbers r0-r3 */
    ldr     r4, =0x11111111
    cmp     r0, r4
    bne     fail_4
    ldr     r4, =0x22222222
    cmp     r1, r4
    bne     fail_4
    ldr     r4, =0x33333333
    cmp     r2, r4
    bne     fail_4
    ldr     r4, =0x44444444
    cmp     r3, r4
    bne     fail_4

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
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    b       done

    .thumb_func
SVCall_Handler:
    /* Increment counter */
    ldr     r0, =COUNTER
    ldr     r1, [r0]
    add     r1, r1, #1
    str     r1, [r0]
    /* Clobber r0-r3 (caller-saved, restored by hardware on return) */
    mov     r0, #0xDE
    mov     r1, #0xAD
    mov     r2, #0xBE
    mov     r3, #0xEF
    bx      lr

    .thumb_func
default_handler:
    b       .
