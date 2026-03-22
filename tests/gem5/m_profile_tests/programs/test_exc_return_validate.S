/*
 * test_exc_return_validate.S — EXC_RETURN validation (MISSING-1)
 *
 * Verifies that valid EXC_RETURN values work correctly.  Invalid
 * EXC_RETURN values are caught by fatal_if in mProfileExcReturnUnstack()
 * and halt the simulation with a clear error message — they cannot be
 * tested as pass/fail within the firmware.
 *
 * This test exercises all three valid EXC_RETURN encodings:
 *   0xFFFFFFF9 — return to Thread mode, use MSP (normal SVC return)
 *   0xFFFFFFFD — return to Thread mode, use PSP
 *   0xFFFFFFF1 — return to Handler mode, use MSP (nested exception)
 *
 * =========================================================================
 * Test design:
 *
 *   Subtest 1 (EXC_RETURN 0xFFFFFFF9 — Thread/MSP):
 *     SVC #0 from Thread mode with SPSEL=0 (MSP)
 *     Handler returns via BX LR → EXC_RETURN = 0xFFFFFFF9
 *     Verify return to Thread mode, R0 preserved
 *
 *   Subtest 2 (EXC_RETURN 0xFFFFFFFD — Thread/PSP):
 *     Set PSP, switch to PSP via CONTROL.SPSEL=1
 *     SVC #0 from Thread mode with SPSEL=1 (PSP)
 *     Handler returns via BX LR → EXC_RETURN = 0xFFFFFFFD
 *     Verify return to Thread mode, SP = PSP
 *
 *   Subtest 3 (EXC_RETURN 0xFFFFFFF1 — Handler/MSP, nested):
 *     SVC #1 → SVCall handler pends IRQ 0 (higher priority)
 *     IRQ 0 preempts SVCall → nested exception
 *     IRQ 0 handler returns → EXC_RETURN = 0xFFFFFFF1
 *     SVCall resumes and returns → EXC_RETURN = 0xFFFFFFF9
 *     Verify both handlers ran
 *
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000108  TEST_DATA
 *   0x20000200  handler_count — incremented by handlers
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    .equ HANDLER_COUNT, 0x20000200
    .equ PSP_VALUE,     0x20010000

    .equ NVIC_ISER0,    0xE000E100
    .equ NVIC_ICER0,    0xE000E180
    .equ NVIC_ISPR0,    0xE000E200
    .equ NVIC_ICPR0,    0xE000E280
    .equ NVIC_IPR0,     0xE000E400

/* =========================================================================
 * Vector table
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl _vector_table
_vector_table:
    .word _estack               /* [0]  Initial MSP */
    .word Reset_Handler         /* [1]  Reset */
    .word 0                     /* [2]  NMI */
    .word 0                     /* [3]  HardFault */
    .word 0                     /* [4]  MemManage */
    .word 0                     /* [5]  BusFault */
    .word 0                     /* [6]  UsageFault */
    .word 0                     /* [7]  Reserved */
    .word 0                     /* [8]  Reserved */
    .word 0                     /* [9]  Reserved */
    .word 0                     /* [10] Reserved */
    .word SVCall_Handler        /* [11] SVCall */
    .word 0                     /* [12] Debug Monitor */
    .word 0                     /* [13] Reserved */
    .word 0                     /* [14] PendSV */
    .word 0                     /* [15] SysTick */
    .word IRQ0_Handler          /* [16] IRQ 0 */

/* =========================================================================
 * Macros
 * ========================================================================= */
.macro fail_test subtest_num, obs_reg
    /* Ensure we're on MSP for fail path */
    movs r5, #0
    msr control, r5
    isb
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
    ldr r0, =HANDLER_COUNT
    movs r1, #0
    str r1, [r0]

/* -- Subtest 1: Valid EXC_RETURN 0xFFFFFFF9 (Thread/MSP) -- */
/* Normal SVC from Thread mode with MSP active. */
    ldr r4, =0x12345678         /* marker to verify preservation */
    svc #0                      /* → SVCall, returns with 0xFFFFFFF9 */
    /* Verify r4 preserved (not corrupted by unstack) */
    ldr r3, =0x12345678
    cmp r4, r3
    bne 1f
    b 2f
1:  fail_test 1, r4
2:

/* -- Subtest 2: Valid EXC_RETURN 0xFFFFFFFD (Thread/PSP) -- */
/* Switch to PSP, then SVC. Handler returns with 0xFFFFFFFD. */
    ldr r0, =PSP_VALUE
    msr psp, r0
    movs r0, #2                 /* CONTROL.SPSEL = 1 */
    msr control, r0
    isb

    svc #0                      /* → SVCall, returns with 0xFFFFFFFD */

    /* Verify SP is still PSP after return */
    mov r2, sp
    ldr r3, =PSP_VALUE
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 2, r2
2:

    /* Switch back to MSP */
    movs r0, #0
    msr control, r0
    isb

/* -- Subtest 3: Valid EXC_RETURN 0xFFFFFFF1 (Handler/MSP, nested) -- */
/* SVC #1 → SVCall handler enables and pends IRQ 0 (priority 0x00,
 * higher than SVCall's default 0x00 — equal priority won't preempt,
 * so set SVCall priority to 0x80 first). IRQ 0 preempts SVCall,
 * runs, returns with 0xFFFFFFF1 (Handler/MSP). Then SVCall resumes
 * and returns with 0xFFFFFFF9. */

    /* Set SVCall priority to 0x80 (lower) via SHPR2 byte 3 */
    ldr r0, =0xE000ED1C         /* SHPR2 */
    ldr r1, [r0]
    orr r1, r1, #0x80000000     /* byte 3 = SVCall priority = 0x80 */
    str r1, [r0]

    /* Set IRQ 0 priority to 0x00 (higher) */
    ldr r0, =NVIC_IPR0
    movs r1, #0
    str r1, [r0]

    /* Reset handler count */
    ldr r0, =HANDLER_COUNT
    movs r1, #0
    str r1, [r0]

    svc #1                      /* → SVCall pends IRQ 0, IRQ 0 preempts */

    /* Verify both handlers ran: handler_count should be 2
     * (SVCall incremented once, IRQ 0 incremented once) */
    ldr r0, =HANDLER_COUNT
    ldr r2, [r0]
    cmp r2, #2
    bne 1f
    b 2f
1:  fail_test 3, r2
2:

    /* Clean up: disable IRQ 0, restore SVCall priority */
    ldr r0, =NVIC_ICER0
    movs r1, #1
    str r1, [r0]
    ldr r0, =0xE000ED1C
    movs r1, #0
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

/* =========================================================================
 * SVCall Handler
 *
 * SVC #0: increment handler_count, return normally
 * SVC #1: increment handler_count, enable + pend IRQ 0, return
 * ========================================================================= */
    .thumb_func
    .globl SVCall_Handler
SVCall_Handler:
    push {r4-r5, lr}

    /* Increment handler_count */
    ldr r4, =HANDLER_COUNT
    ldr r5, [r4]
    adds r5, r5, #1
    str r5, [r4]

    /* Extract SVC number from stacked PC.
     * The hardware exception frame may be on MSP or PSP depending
     * on EXC_RETURN bit[2].  The saved LR (EXC_RETURN) is at
     * [current MSP + 8] (below the 3-reg PUSH {r4,r5,lr}).
     * If bit[2]=0: frame on MSP at [current MSP + 12].
     * If bit[2]=1: frame on PSP (read via MRS PSP). */
    mrs r0, msp
    ldr r1, [r0, #8]           /* saved LR = EXC_RETURN */
    tst r1, #4                  /* bit[2]: 0=MSP frame, 1=PSP frame */
    bne 1f
    /* MSP frame: hardware frame at MSP + 12 (past our PUSH) */
    add r0, r0, #12            /* r0 = hardware frame base on MSP */
    b 2f
1:  /* PSP frame: hardware frame on PSP */
    mrs r0, psp                 /* r0 = hardware frame base on PSP */
2:
    ldr r1, [r0, #0x18]        /* stacked PC */
    subs r1, r1, #2
    ldrb r1, [r1]               /* SVC number */

    cmp r1, #1
    bne svc_done

    /* SVC #1: enable and pend IRQ 0 to trigger nested exception */
    ldr r0, =NVIC_ISER0
    movs r1, #1
    str r1, [r0]                /* enable IRQ 0 */
    ldr r0, =NVIC_ISPR0
    str r1, [r0]                /* pend IRQ 0 — should preempt */
    dsb
    isb

svc_done:
    pop {r4-r5, pc}             /* exception return via POP {PC} */

/* =========================================================================
 * IRQ 0 Handler — increments handler_count, returns
 * Uses EXC_RETURN 0xFFFFFFF1 (return to Handler mode, MSP)
 * ========================================================================= */
    .thumb_func
    .globl IRQ0_Handler
IRQ0_Handler:
    ldr r0, =HANDLER_COUNT
    ldr r1, [r0]
    adds r1, r1, #1
    str r1, [r0]
    bx lr                       /* EXC_RETURN 0xFFFFFFF1 */
