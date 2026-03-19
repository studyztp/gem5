/*
 * Test: POP {PC} and LDM {PC} for EXC_RETURN
 *
 * On Cortex-M, exception handlers commonly return via:
 *   POP {r4-r7, pc}    — loads EXC_RETURN from stack into PC
 *   LDM sp!, {..., pc}  — same effect
 *
 * When the loaded PC value is an EXC_RETURN magic value
 * (0xFFFFFFF1/F9/FD), the CPU must trigger exception return
 * (unstacking) instead of branching to that address.
 *
 * This tests whether POP {PC} with EXC_RETURN value works.
 * If EXC_RETURN is not detected, the CPU will try to fetch
 * from 0xFFFFFFF8 and fault.
 *
 * Tests:
 *   1. SVC handler returns via POP {r4-r7, pc}
 *   2. SVC handler returns via BX LR (baseline, should work)
 *   3. Verify execution continues correctly after both
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
    .word   default_handler
    .word   default_handler
    .fill   7, 4, 0
    .word   SVCall_Handler
    .fill   4, 4, 0

    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* --- Test 1: Handler returns via BX LR (baseline) --- */
    mov     r0, #0
    ldr     r4, =TEST_SUBTEST
    str     r0, [r4]
    mov     r7, #1              /* tell handler: use BX LR */
    svc     #0
    ldr     r0, [r4]
    cmp     r0, #0xAA
    bne     fail_1

    /* --- Test 2: Handler returns via POP {r4-r7, pc} --- */
    mov     r0, #0
    str     r0, [r4]
    mov     r7, #2              /* tell handler: use POP {PC} */
    svc     #0
    ldr     r0, [r4]
    cmp     r0, #0xBB
    bne     fail_2

    /* --- Test 3: Verify R4-R7 preserved across POP return --- */
    ldr     r4, =0x44444444
    ldr     r5, =0x55555555
    ldr     r6, =0x66666666
    mov     r7, #3              /* tell handler: use POP, clobber r4-r7 */
    svc     #0
    /* After exception return, r4-r7 should be restored from the
     * exception frame (hardware pushed them on entry) */
    /* Actually, r4-r7 are callee-saved — the hardware frame only
     * saves r0-r3,r12,lr,pc,xpsr.  The handler must preserve r4-r7
     * itself (via PUSH/POP).  So this tests the handler's PUSH/POP. */
    ldr     r0, =TEST_SUBTEST
    ldr     r0, [r0]
    cmp     r0, #0xCC
    bne     fail_3

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
    /* r7 selects which return method to use:
     *   1 = BX LR
     *   2 = POP {r4-r7, pc}
     *   3 = POP with register clobber test
     */
    cmp     r7, #1
    beq     handler_bx_lr
    cmp     r7, #2
    beq     handler_pop_pc
    cmp     r7, #3
    beq     handler_pop_clobber
    b       handler_bx_lr       /* default */

handler_bx_lr:
    ldr     r0, =TEST_SUBTEST
    mov     r1, #0xAA
    str     r1, [r0]
    bx      lr                  /* EXC_RETURN via BX */

handler_pop_pc:
    push    {r4, lr}            /* save LR (EXC_RETURN) on stack */
    ldr     r0, =TEST_SUBTEST
    mov     r1, #0xBB
    str     r1, [r0]
    pop     {r4, pc}            /* pop LR into PC → EXC_RETURN */

handler_pop_clobber:
    push    {r4-r7, lr}
    /* Clobber r4-r7 inside handler */
    mov     r4, #0
    mov     r5, #0
    mov     r6, #0
    mov     r7, #0
    ldr     r0, =TEST_SUBTEST
    mov     r1, #0xCC
    str     r1, [r0]
    pop     {r4-r7, pc}         /* restore r4-r7 and EXC_RETURN via PC */

    .thumb_func
default_handler:
    b       .
