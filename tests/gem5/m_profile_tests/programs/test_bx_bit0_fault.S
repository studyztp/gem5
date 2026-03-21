/*
 * Test: BX with bit[0]=0 must UsageFault (Bug 7)
 *
 * Bug 7: BxMProfile::execute() allowed BX with bit[0]=0, which
 * would clear the Thumb bit.  M-profile is always Thumb — clearing
 * the Thumb bit is an INVSTATE UsageFault.
 *
 * After the fix, BX with bit[0]=0 generates MPEXC_USAGEFAULT.
 *
 * Test plan:
 *   Subtest 1: Normal BX with bit[0]=1 works (sanity check)
 *   Subtest 2: BX with bit[0]=0 → UsageFault handler runs
 *   Subtest 3: Normal BX still works after the fault (recovery)
 *
 * Result at 0x20000100:
 *   0xCAFECAFE = all tests passed
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,  0x20000100
    .equ TEST_SUBTEST, 0x20000104
    .equ TEST_SCRATCH,  0x20000108
    .equ PASS_VAL,     0xCAFECAFE
    .equ FAIL_VAL,     0xDEADDEAD

    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000          /* Initial SP */
    .word   Reset_Handler       /* Reset */
    .word   default_handler     /* NMI */
    .word   HardFault_Handler   /* HardFault */
    .word   default_handler     /* MemManage */
    .word   default_handler     /* BusFault */
    .word   UsageFault_Handler  /* UsageFault */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   0                   /* Reserved */
    .word   default_handler     /* SVCall */
    .word   0                   /* Debug Monitor */
    .word   0                   /* Reserved */
    .word   default_handler     /* PendSV */
    .word   default_handler     /* SysTick */

    .section .text

    /* --- Simple function for normal BX test --- */
    .thumb_func
add_five:
    add     r0, r0, #5
    bx      lr

    /* --- Reset Handler --- */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* Enable UsageFault in SHCSR (System Handler Control and State Register)
     * Address: 0xE000ED24, bit 18 = USGFAULTENA
     * Without this, UsageFault escalates to HardFault. */
    ldr     r0, =0xE000ED24
    ldr     r1, [r0]
    orr     r1, r1, #(1 << 18)
    str     r1, [r0]

    /* --- Subtest 1: Normal BX with bit[0]=1 (should work) --- */
    mov     r0, #7
    ldr     r1, =add_five       /* Thumb address: bit[0]=1 */
    blx     r1                   /* call add_five */
    cmp     r0, #12              /* 7+5=12 */
    bne     fail_1

    /* --- Subtest 2: BX with bit[0]=0 → must trigger UsageFault --- */
    /* Clear the scratch marker first */
    ldr     r4, =TEST_SCRATCH
    mov     r0, #0
    str     r0, [r4]

    /* Create an address with bit[0]=0.
     * We take the address of add_five and clear bit[0]. */
    ldr     r1, =add_five
    bic     r1, r1, #1          /* clear bit[0] → even address */

    /* BX r1 should trigger UsageFault (not execute add_five) */
    bx      r1

    /* If we get here, the UsageFault handler returned to the next
     * instruction (it adjusts the stacked PC to skip past the BX).
     * This is the expected path after the fault is handled. */
after_fault_bx:
    ldr     r0, [r4]
    cmp     r0, #0x55           /* marker set by UsageFault handler */
    bne     fail_2

    /* --- Subtest 3: Normal BX still works after fault recovery --- */
    mov     r0, #20
    ldr     r1, =add_five
    blx     r1
    cmp     r0, #25             /* 20+5=25 */
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

    /* --- UsageFault Handler ---
     *
     * When BX with bit[0]=0 triggers UsageFault, the hardware pushes
     * an exception frame.  The stacked PC points to the faulting BX
     * instruction.  We need to advance it past the BX (2 bytes for
     * Thumb-16 BX) so that on return we land at after_fault_bx.
     *
     * Frame layout on stack (from SP):
     *   SP+0x00: R0
     *   SP+0x04: R1
     *   SP+0x08: R2
     *   SP+0x0C: R3
     *   SP+0x10: R12
     *   SP+0x14: LR
     *   SP+0x18: ReturnAddress (stacked PC)
     *   SP+0x1C: xPSR
     */
    .thumb_func
UsageFault_Handler:
    /* Write marker to prove handler ran */
    ldr     r0, =TEST_SCRATCH
    mov     r1, #0x55
    str     r1, [r0]

    /* Advance stacked PC past the faulting BX instruction (2 bytes).
     * This makes exception return go to after_fault_bx. */
    ldr     r0, [sp, #0x18]     /* read stacked PC */
    add     r0, r0, #2          /* skip past BX (Thumb-16 = 2 bytes) */
    str     r0, [sp, #0x18]     /* write back adjusted PC */

    /* Return from exception via BX LR (EXC_RETURN) */
    bx      lr

    /* --- HardFault Handler ---
     * If UsageFault is not enabled, the fault escalates to HardFault.
     * Write a distinctive marker so we can diagnose. */
    .thumb_func
HardFault_Handler:
    ldr     r0, =TEST_SCRATCH
    mov     r1, #0xEE
    str     r1, [r0]
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    mov     r2, #2
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
    /* Exit — don't loop, we want the test framework to see FAIL */
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .

    .thumb_func
default_handler:
    b       .
