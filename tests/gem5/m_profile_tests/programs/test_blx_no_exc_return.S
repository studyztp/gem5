/*
 * Test: BLX must NOT trigger EXC_RETURN (Bug 6)
 *
 * Bug 6: BlxRegMProfile::execute() had an EXC_RETURN check identical
 * to BxMProfile.  Per DDI0403E B1.5.8, only BX, POP {PC}, LDM {PC},
 * and LDR Rd=PC can trigger exception return.  BLX is explicitly
 * excluded.
 *
 * After the fix, BLX with an EXC_RETURN-like value in the register
 * is treated as a normal branch (which will fault on unmapped memory).
 *
 * Test plan:
 *   We can't easily test "BLX with 0xFFFFFFF9" without crashing,
 *   since that address is unmapped (it would UsageFault / HardFault).
 *   Instead, we verify:
 *
 *   Subtest 1: Normal BLX Rm works (function call via register)
 *   Subtest 2: BLX Rm sets LR correctly (return address | 1)
 *   Subtest 3: SVC → BX LR (EXC_RETURN) works (BX, not BLX)
 *   Subtest 4: After exception return, BLX Rm still works normally
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
    .word   0x20020000          /* Initial SP */
    .word   Reset_Handler       /* Reset */
    .word   default_handler     /* NMI */
    .word   default_handler     /* HardFault */
    .word   default_handler     /* MemManage */
    .word   default_handler     /* BusFault */
    .word   default_handler     /* UsageFault */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   SVCall_Handler      /* SVCall */
    .word   0                   /* Debug Monitor */
    .word   0                   /* Reserved */
    .word   default_handler     /* PendSV */
    .word   default_handler     /* SysTick */

    .section .text

    /* --- Simple function for BLX tests --- */
    .thumb_func
add_ten:
    add     r0, r0, #10
    bx      lr

    /* --- Reset Handler --- */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* --- Subtest 1: Normal BLX Rm --- */
    mov     r0, #5
    ldr     r1, =add_ten
    blx     r1                  /* call add_ten via BLX reg */
    cmp     r0, #15             /* should be 5+10=15 */
    bne     fail_1

    /* --- Subtest 2: BLX sets LR = (return_addr) | 1 --- */
    /* After the BLX above, LR should point to the instruction
     * after BLX (the cmp), with bit[0]=1 (Thumb state). */
    mov     r0, lr
    tst     r0, #1              /* bit[0] must be 1 */
    beq     fail_2

    /* --- Subtest 3: SVC → SVCall → BX LR (EXC_RETURN via BX) --- */
    ldr     r4, =TEST_SUBTEST
    mov     r0, #0
    str     r0, [r4]
    svc     #0                  /* SVCall handler uses BX LR to return */
    ldr     r0, [r4]
    cmp     r0, #0x42
    bne     fail_3

    /* --- Subtest 4: BLX Rm still works after exception return --- */
    mov     r0, #20
    ldr     r1, =add_ten
    blx     r1
    cmp     r0, #30             /* should be 20+10=30 */
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
    b fail
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    /* Exit via ARM semihosting SYS_EXIT */
    mov     r0, #0x18           /* SYS_EXIT */
    ldr     r1, =0x20026        /* ADP_Stopped_ApplicationExit */
    bkpt    #0xab               /* M-profile semihosting trigger */
    b       .                   /* unreachable */

    /* --- SVCall Handler --- */
    .thumb_func
SVCall_Handler:
    ldr     r0, =TEST_SUBTEST
    mov     r1, #0x42
    str     r1, [r0]
    bx      lr                  /* BX LR → EXC_RETURN (NOT BLX!) */

    .thumb_func
default_handler:
    b       .
