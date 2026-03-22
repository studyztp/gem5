/*
 * test_icsr_read.S — ICSR dynamic field read test (BUG-2 basic check)
 *
 * Verifies that reading ICSR (0xE000ED04) returns correct values for
 * the dynamic read-only fields that the spec (DDI0403E B3.2.4) defines:
 *
 *   VECTACTIVE [8:0]   — current exception number from xPSR.IPSR
 *   RETTOBASE  [11]    — 1 if only one active exception
 *   VECTPENDING [20:12] — highest-priority pending exception number
 *   ISRPENDING [22]    — 1 if any external IRQ is pending
 *
 * Before the fix, readSCB() returns the raw stored ICSR value which
 * has zeros in all these fields.  CMSIS __get_IPSR() and FreeRTOS
 * xPortIsInsideInterrupt() depend on VECTACTIVE being correct.
 *
 * =========================================================================
 * Test design:
 *
 *   Subtest 1 (Thread mode):
 *     Read ICSR, verify VECTACTIVE == 0 (correct — Thread mode)
 *
 *   Subtest 2 (SVCall handler):
 *     Trigger SVC #0, inside handler read ICSR
 *     Verify VECTACTIVE == 11 (SVCall = exception 11)
 *
 *   Subtest 3 (SVCall handler):
 *     Verify RETTOBASE == 1 (only one active exception)
 *
 *   Subtest 4 (SVCall handler, with external IRQ pending):
 *     Pend IRQ 0 (without enabling — won't fire, just sets pending)
 *     Read ICSR, verify ISRPENDING == 1
 *
 *   Subtest 5 (SVCall handler, with external IRQ pending):
 *     Verify VECTPENDING shows IRQ 0 = exception 16
 *     (Only valid if IRQ 0 is enabled; if not enabled, VECTPENDING
 *      may show 0.  We enable IRQ 0 with lower priority than SVCall
 *      so it can't preempt, but it becomes the pending exception.)
 *
 *   Subtest 6 (back in Thread mode):
 *     Clean up: clear IRQ 0 pending, disable IRQ 0
 *     Read ICSR, verify VECTACTIVE == 0 again
 *
 * =========================================================================
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000108  TEST_DATA
 *   0x20000200  svc_icsr_val  — ICSR value read inside SVCall handler
 *   0x20000204  svc_icsr_val2 — ICSR value after pending IRQ 0
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    .equ ICSR_ADDR,     0xE000ED04
    .equ NVIC_ISER0,    0xE000E100
    .equ NVIC_ICER0,    0xE000E180
    .equ NVIC_ISPR0,    0xE000E200
    .equ NVIC_ICPR0,    0xE000E280
    .equ NVIC_IPR0,     0xE000E400

    /* SRAM locations for passing data out of SVCall handler */
    .equ SVC_ICSR_VAL,  0x20000200
    .equ SVC_ICSR_VAL2, 0x20000204

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
    /* IRQ 0 — needed for subtest 5 (pending detection) */
    .word 0                     /* [16] IRQ0 (no handler needed — won't fire) */

/* =========================================================================
 * Macros
 * ========================================================================= */
.macro fail_test subtest_num, obs_reg
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
 * Reset Handler — main test code
 * ========================================================================= */
    .text
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Initialize SRAM data locations */
    ldr r0, =SVC_ICSR_VAL
    movs r1, #0
    str r1, [r0]
    ldr r0, =SVC_ICSR_VAL2
    str r1, [r0]

/* -- Subtest 1: Thread mode — VECTACTIVE should be 0 -- */
    ldr r0, =ICSR_ADDR
    ldr r2, [r0]               /* read ICSR */
    ubfx r3, r2, #0, #9        /* extract VECTACTIVE [8:0] */
    cmp r3, #0                 /* should be 0 in Thread mode */
    bne 1f
    b 2f
1:  fail_test 1, r2
2:

/* -- Trigger SVCall to test ICSR inside Handler mode -- */
/* The SVCall handler will:
 *   1. Read ICSR and store to SVC_ICSR_VAL (for subtests 2-3)
 *   2. Set IRQ 0 priority to 0xF0 (lowest), enable it, pend it
 *   3. Read ICSR again and store to SVC_ICSR_VAL2 (for subtests 4-5)
 *   4. Return to Thread mode */
    svc #0

/* -- Subtest 2: VECTACTIVE == 11 (SVCall) inside handler -- */
    ldr r0, =SVC_ICSR_VAL
    ldr r2, [r0]               /* ICSR value from inside SVCall */
    ubfx r3, r2, #0, #9        /* extract VECTACTIVE [8:0] */
    cmp r3, #11                /* SVCall = exception 11 */
    bne 1f
    b 2f
1:  fail_test 2, r2
2:

/* -- Subtest 3: RETTOBASE == 1 (only one active exception) -- */
    ldr r0, =SVC_ICSR_VAL
    ldr r2, [r0]
    lsr r3, r2, #11
    and r3, r3, #1             /* extract RETTOBASE [11] */
    cmp r3, #1
    bne 1f
    b 2f
1:  fail_test 3, r2
2:

/* -- Subtest 4: ISRPENDING == 1 (IRQ 0 was pended in handler) -- */
    ldr r0, =SVC_ICSR_VAL2
    ldr r2, [r0]               /* ICSR value after pending IRQ 0 */
    lsr r3, r2, #22
    and r3, r3, #1             /* extract ISRPENDING [22] */
    cmp r3, #1
    bne 1f
    b 2f
1:  fail_test 4, r2
2:

/* -- Subtest 5: VECTPENDING == 16 (IRQ 0 = exception 16) -- */
    ldr r0, =SVC_ICSR_VAL2
    ldr r2, [r0]
    ubfx r3, r2, #12, #9       /* extract VECTPENDING [20:12] */
    cmp r3, #16                /* IRQ 0 = exception 16 */
    bne 1f
    b 2f
1:  fail_test 5, r2
2:

/* -- Subtest 6: Back in Thread mode — verify VECTACTIVE == 0 -- */
    /* IRQ 0 was already cleaned up inside SVCall handler. */
    ldr r0, =ICSR_ADDR
    ldr r2, [r0]               /* read ICSR */
    ubfx r3, r2, #0, #9        /* extract VECTACTIVE [8:0] */
    cmp r3, #0                 /* back in Thread mode */
    bne 1f
    b 2f
1:  fail_test 6, r2
2:

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
 * Reads ICSR twice:
 *   1. Immediately on entry → SVC_ICSR_VAL (VECTACTIVE, RETTOBASE)
 *   2. After pending IRQ 0  → SVC_ICSR_VAL2 (ISRPENDING, VECTPENDING)
 * ========================================================================= */
    .thumb_func
    .globl SVCall_Handler
SVCall_Handler:
    /* -- Read ICSR on entry (VECTACTIVE should be 11) -- */
    ldr r0, =ICSR_ADDR
    ldr r1, [r0]
    ldr r2, =SVC_ICSR_VAL
    str r1, [r2]               /* save first ICSR read */

    /* -- Set IRQ 0 priority to 0xF0 (lowest configurable) -- */
    /* This ensures IRQ 0 can't preempt SVCall (priority 0x00).
     * We need IRQ 0 enabled for VECTPENDING to report it. */
    ldr r0, =NVIC_IPR0
    ldr r1, [r0]
    orr r1, r1, #0xF0          /* byte 0 = IRQ 0 priority = 0xF0 */
    str r1, [r0]

    /* -- Enable IRQ 0 -- */
    ldr r0, =NVIC_ISER0
    movs r1, #1
    str r1, [r0]

    /* -- Pend IRQ 0 (enabled but lower priority, won't preempt) -- */
    ldr r0, =NVIC_ISPR0
    movs r1, #1
    str r1, [r0]

    /* -- Read ICSR again (ISRPENDING and VECTPENDING should be set) -- */
    ldr r0, =ICSR_ADDR
    ldr r1, [r0]
    ldr r2, =SVC_ICSR_VAL2
    str r1, [r2]               /* save second ICSR read */

    /* -- Clean up IRQ 0 before returning -- */
    /* Must clear pending and disable here, not in Thread mode.
     * After SVCall returns, IRQ 0 (enabled+pending, priority 0xF0)
     * becomes immediately deliverable in Thread mode (exec priority 0xFF).
     * Without a handler at vector[16], the CPU would fetch address 0
     * and crash — this is correct hardware behavior, not a system bug. */
    ldr r0, =NVIC_ICPR0
    movs r1, #1
    str r1, [r0]               /* clear IRQ 0 pending */
    ldr r0, =NVIC_ICER0
    str r1, [r0]               /* disable IRQ 0 */

    bx lr
