/*
 * Test: SP synchronization between R13 and MSP/PSP misc regs
 *
 * Verifies that R13 (architectural SP) stays in sync with
 * MISCREG_M_MSP/PSP across:
 *   1. Direct SP modification (SUB SP, #N) then SVC exception
 *   2. Exception entry pushes frame using correct (modified) SP
 *   3. Exception return restores correct SP
 *   4. SP after exception return matches pre-exception value
 *
 * This catches the bug where exception stacking reads from
 * MISCREG_M_MSP (stale) instead of R13 (current).
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
    .word   default_handler     /* NMI */
    .word   default_handler     /* HardFault */
    .fill   7, 4, 0
    .word   SVCall_Handler      /* SVCall */
    .fill   4, 4, 0

    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* --- Test 1: Modify SP, trigger SVC, verify SP after return --- */
    /* Save initial SP */
    mov     r4, sp              /* r4 = initial SP (0x20020000) */

    /* Modify SP by subtracting (simulates local variable allocation) */
    sub     sp, sp, #64         /* SP = SP - 64 */
    mov     r5, sp              /* r5 = modified SP */

    /* Store a marker on the stack to verify frame location */
    ldr     r0, =0xAA55AA55
    str     r0, [sp]            /* write marker at current SP */

    /* Trigger SVC — exception entry should use current SP (r5),
     * NOT the initial MSP (r4) */
    svc     #0

    /* After return, SP should be restored to r5 (pre-exception) */
    mov     r6, sp
    cmp     r6, r5
    bne     fail_1

    /* Verify the marker is still there */
    ldr     r0, [sp]
    ldr     r1, =0xAA55AA55
    cmp     r0, r1
    bne     fail_1

    /* Restore SP */
    add     sp, sp, #64

    /* --- Test 2: Nested SP modification in handler --- */
    /* The SVCall handler itself uses the stack (PUSH/POP).
     * Verify SP is correct after handler returns. */
    mov     r4, sp              /* save current SP */
    svc     #1                  /* SVCall with imm=1, handler does PUSH/POP */
    mov     r5, sp
    cmp     r5, r4              /* SP should be unchanged after return */
    bne     fail_2

    /* --- Test 3: MSR MSP should update R13 --- */
    mrs     r0, MSP             /* read current MSP */
    mov     r4, r0              /* save it */
    ldr     r1, =0x20018000
    msr     MSP, r1             /* write new MSP */
    mov     r2, sp              /* read R13 — should be 0x20018000 */
    cmp     r2, r1
    bne     fail_3_restore
    msr     MSP, r4             /* restore original MSP */
    mov     r0, sp              /* verify R13 restored */
    cmp     r0, r4
    bne     fail_3
    b       test_3_ok
fail_3_restore:
    msr     MSP, r4
    b       fail_3
test_3_ok:

    /* --- Test 4: MRS MSP should read from R13 --- */
    sub     sp, sp, #32         /* modify R13 */
    mrs     r0, MSP             /* should read current R13 */
    mov     r1, sp
    cmp     r0, r1
    bne     fail_4
    add     sp, sp, #32         /* restore */

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
    /* This handler uses the stack to verify it works correctly.
     * Uses BX LR for return (not POP {PC}) because POP {PC}
     * with EXC_RETURN is not yet supported — tested separately
     * in test_pop_pc_exc_return.S. */
    push    {r0-r3, lr}
    /* Write marker to prove handler ran */
    ldr     r0, =TEST_SUBTEST
    mov     r1, #0x99
    str     r1, [r0]
    pop     {r0-r3, lr}
    bx      lr                  /* return via BX LR (EXC_RETURN) */

    .thumb_func
default_handler:
    b       .
