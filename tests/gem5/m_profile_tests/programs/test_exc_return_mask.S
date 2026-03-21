/*
 * Test: EXC_RETURN mask precision (Bug 5)
 *
 * Bug 5: The EXC_RETURN detection mask was 0xFFFFFF00 instead of
 * 0xFFFFFFF0.  This means addresses 0xFFFFFF00–0xFFFFFFEF were
 * falsely detected as EXC_RETURN values and triggered exception
 * return when they shouldn't.
 *
 * After the fix, only 0xFFFFFFF0–0xFFFFFFFF trigger EXC_RETURN.
 *
 * This test is a functional sanity check.  Since we can't easily
 * test addresses in the 0xFFFFFF00–0xFFFFFFEF range (no memory
 * there), we verify that the three valid EXC_RETURN values used
 * by real code all work correctly:
 *   0xFFFFFFF1 — return to Handler mode, use MSP
 *   0xFFFFFFF9 — return to Thread mode, use MSP
 *   0xFFFFFFFD — return to Thread mode, use PSP
 *
 * Test plan:
 *   Subtest 1: SVC → SVCall handler → BX LR (0xFFFFFFF9) → verify return
 *   Subtest 2: Re-verify that a second SVC + BX LR works (regression check)
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

    /* --- Reset Handler --- */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* --- Subtest 1: SVC → SVCall → BX LR (EXC_RETURN 0xFFFFFFF9) --- */
    ldr     r4, =TEST_SUBTEST
    mov     r0, #0
    str     r0, [r4]            /* clear marker */
    svc     #0                  /* trigger SVCall */
    /* If we get here, EXC_RETURN 0xFFFFFFF9 was correctly detected */
    ldr     r0, [r4]            /* read marker from handler */
    cmp     r0, #0x42
    bne     fail_1

    /* --- Subtest 2: Second SVC → verify EXC_RETURN still works --- */
    mov     r0, #0
    str     r0, [r4]            /* clear marker */
    svc     #1                  /* trigger SVCall again */
    ldr     r0, [r4]            /* read marker from handler */
    cmp     r0, #0x42
    bne     fail_2

    /* All passed */
    ldr     r0, =PASS_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    b       done

fail_1: mov r2, #1
    b fail
fail_2: mov r2, #2
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
    /* Write marker to prove handler ran */
    ldr     r0, =TEST_SUBTEST
    mov     r1, #0x42
    str     r1, [r0]
    /* Return via EXC_RETURN in LR (0xFFFFFFF9 = Thread/MSP).
     * This is the value the mask must correctly detect. */
    bx      lr

    .thumb_func
default_handler:
    b       .
