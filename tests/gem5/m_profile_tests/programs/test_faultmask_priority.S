/*
 * test_faultmask_priority.S — C-1: executionPriority() uint8_t bug
 *
 * Bug C-1: executionPriority() returns uint8_t.  Thread-mode default
 *   priority is 0xFF (255) instead of the spec's 256 (DDI0403E B1.5.4).
 *   With 8 priority bits (priorityMask = 0xFF), the lowest configurable
 *   priority is 0xFF.  An IRQ at priority 0xFF cannot preempt Thread
 *   mode because 0xFF < 0xFF = false (unsigned).
 *
 *   After fix (int16_t return, thread default = 256):
 *     0xFF < 256 = true → IRQ delivered correctly.
 *
 *   Additionally, FAULTMASK maps to priority 0 instead of -1.  This
 *   matters for HardFault blocking (tested once fault escalation M-10
 *   is implemented).  The int16_t return type enables the -1 value.
 *
 * -------------------------------------------------------------------------
 * Platform configuration:
 *   This test MUST run with priority_bits=8 (--priority-bits 8) so that
 *   the lowest configurable priority is 0xFF and the bug manifests.
 *   With the default 4 bits, lowest priority = 0xF0 < 0xFF → works fine.
 *
 * -------------------------------------------------------------------------
 * Interrupt assignment (all use 8-bit priorities):
 *
 *   IRQ0 = A, priority 0xFF (lowest configurable with 8 bits)
 *   IRQ1 = B, priority 0xFE (one step higher than A)
 *
 * -------------------------------------------------------------------------
 * Test algorithm:
 *
 *   Subtest 1: IRQ at priority 0xFF delivered from Thread mode
 *     1. Set IRQ0 priority to 0xFF
 *     2. Enable IRQ0 via ISER0
 *     3. Pend IRQ0 via ISPR0
 *     4. DSB + ISB (give delivery opportunity)
 *     5. Check irq0_ran flag
 *     → With bug:  0xFF < 0xFF = false → NOT delivered → FAIL
 *     → With fix:  0xFF < 256  = true  → delivered     → PASS
 *
 *   Subtest 2: Nested preemption with 8-bit priorities
 *     1. Reset flags
 *     2. Pend IRQ0 (priority 0xFF) again
 *     3. Inside A_Handler: pend IRQ1 (priority 0xFE)
 *        → 0xFE < 0xFF = true → IRQ1 should preempt IRQ0
 *     4. Verify execution order: A_START, B_START, B_END, A_END
 *     → This subtest depends on subtest 1 passing (IRQ0 must fire first).
 *
 *   Subtest 3: FAULTMASK blocks all configurable interrupts
 *     1. Reset flags
 *     2. Set FAULTMASK = 1 (execution priority → -1 after fix, 0 before)
 *     3. Pend IRQ0 (priority 0xFF)
 *     4. DSB + ISB
 *     5. Verify IRQ0 did NOT fire (blocked by FAULTMASK)
 *     6. Clear FAULTMASK
 *     7. DSB + ISB
 *     8. Verify IRQ0 now fired
 *     → Passes both before and after fix (0 and -1 both block priority 0xFF).
 *       Included for completeness and to validate FAULTMASK basic behavior.
 *
 * -------------------------------------------------------------------------
 * Expected results:
 *
 *   Before fix (uint8_t, thread default 0xFF):
 *     Subtest 1 FAILS: IRQ0 at priority 0xFF not delivered
 *
 *   After fix (int16_t, thread default 256):
 *     All 3 subtests PASS
 *
 * -------------------------------------------------------------------------
 * Memory map:
 *   0x20000100  TEST_RESULT     — 0xCAFECAFE or 0xDEADDEAD
 *   0x20000104  TEST_SUBTEST    — failing subtest number (1-based)
 *   0x20000200  irq0_ran        — set to 1 by A_Handler
 *   0x20000204  irq1_ran        — set to 1 by B_Handler
 *   0x20000210  execution_order[0..3] — 4 marker words for nesting test
 *   0x20000220  order_index     — current index (0..3)
 *
 * NVIC register addresses:
 *   0xE000E100  ISER0
 *   0xE000E200  ISPR0
 *   0xE000E400  IPR0  (byte 0 = IRQ0, byte 1 = IRQ1)
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,       0x20000100
    .equ TEST_SUBTEST,      0x20000104
    .equ IRQ0_RAN,          0x20000200
    .equ IRQ1_RAN,          0x20000204
    .equ EXEC_ORDER,        0x20000210  /* 4 words: marker values */
    .equ ORDER_INDEX,       0x20000220  /* current index (0..3) */
    .equ PASS_VAL,          0xCAFECAFE
    .equ FAIL_VAL,          0xDEADDEAD

    /* NVIC registers */
    .equ NVIC_ISER0,        0xE000E100
    .equ NVIC_ISPR0,        0xE000E200
    .equ NVIC_IPR0,         0xE000E400

    /* Execution order markers for subtest 2:
     *   A = IRQ0 (priority 0xFF): 0xA0 start, 0xA1 end
     *   B = IRQ1 (priority 0xFE): 0xB0 start, 0xB1 end */
    .equ MARKER_A_START,    0xA0
    .equ MARKER_A_END,      0xA1
    .equ MARKER_B_START,    0xB0
    .equ MARKER_B_END,      0xB1

    /* Phase control: tells A_Handler whether to pend B (subtest 2 only) */
    .equ PHASE,             0x20000230

/* =========================================================================
 * Vector table
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000          /* Entry 0:  Initial MSP (top of SRAM1) */
    .word   Reset_Handler       /* Entry 1:  Reset */
    .word   default_handler     /* Entry 2:  NMI */
    .word   default_handler     /* Entry 3:  HardFault */
    .word   default_handler     /* Entry 4:  MemManage */
    .word   default_handler     /* Entry 5:  BusFault */
    .word   default_handler     /* Entry 6:  UsageFault */
    .word   0, 0, 0, 0          /* Entries 7-10: Reserved */
    .word   default_handler     /* Entry 11: SVCall */
    .word   0                   /* Entry 12: DebugMonitor */
    .word   0                   /* Entry 13: Reserved */
    .word   default_handler     /* Entry 14: PendSV */
    .word   default_handler     /* Entry 15: SysTick */
    .word   A_Handler           /* Entry 16: IRQ0 = A (priority 0xFF) */
    .word   B_Handler           /* Entry 17: IRQ1 = B (priority 0xFE) */

/* =========================================================================
 * Reset_Handler — main test body
 * ========================================================================= */
    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* ------------------------------------------------------------------
     * Common setup: zero all state variables
     * ------------------------------------------------------------------ */
    mov     r0, #0
    ldr     r1, =IRQ0_RAN
    str     r0, [r1]
    ldr     r1, =IRQ1_RAN
    str     r0, [r1]
    ldr     r1, =ORDER_INDEX
    str     r0, [r1]
    ldr     r1, =PHASE
    str     r0, [r1]

    /* Zero execution_order[0..3] */
    ldr     r4, =EXEC_ORDER
    mov     r1, #0
    str     r1, [r4, #0]
    str     r1, [r4, #4]
    str     r1, [r4, #8]
    str     r1, [r4, #12]

    /* ------------------------------------------------------------------
     * Configure NVIC priorities via IPR0 word write.
     *
     * With 8 priority bits, every bit matters.
     * IPR0 layout (little-endian):
     *   [7:0]   = IRQ0 priority = 0xFF (lowest with 8 bits)
     *   [15:8]  = IRQ1 priority = 0xFE (one step higher)
     *   [23:16] = IRQ2 priority = 0x00 (unused)
     *   [31:24] = IRQ3 priority = 0x00 (unused)
     *
     * Word value = 0x0000FEFF
     * ------------------------------------------------------------------ */
    ldr     r5, =NVIC_IPR0
    ldr     r0, =0x0000FEFF
    str     r0, [r5]

    /* Enable IRQ0 and IRQ1 via ISER0: bits [1:0] = 0x03 */
    ldr     r5, =NVIC_ISER0
    mov     r0, #0x03
    str     r0, [r5]

    /* ==================================================================
     * Subtest 1: IRQ at priority 0xFF delivered from Thread mode
     *
     * This is the core C-1 test.  With uint8_t executionPriority()
     * returning 0xFF for Thread mode, an IRQ at priority 0xFF cannot
     * be delivered because 0xFF < 0xFF = false.
     * ================================================================== */

    /* Phase 0: A_Handler should NOT pend B (just set irq0_ran) */
    mov     r0, #0
    ldr     r1, =PHASE
    str     r0, [r1]

    /* Pend IRQ0 via ISPR0 bit 0 */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #1              /* bit 0 = IRQ0 */
    str     r0, [r5]
    dsb
    isb                         /* delivery opportunity */

    /* Check: did IRQ0 fire? */
    ldr     r1, =IRQ0_RAN
    ldr     r0, [r1]
    cmp     r0, #1
    bne     fail_1              /* FAIL: IRQ0 not delivered (C-1 bug) */

    /* ==================================================================
     * Subtest 2: Nested preemption with 8-bit priorities
     *
     * IRQ0 (0xFF) runs.  Inside A_Handler, pend IRQ1 (0xFE).
     * Since 0xFE < 0xFF, IRQ1 should preempt IRQ0.
     *
     * Expected order: A_START, B_START, B_END, A_END
     * ================================================================== */

    /* Reset state */
    mov     r0, #0
    ldr     r1, =IRQ0_RAN
    str     r0, [r1]
    ldr     r1, =IRQ1_RAN
    str     r0, [r1]
    ldr     r1, =ORDER_INDEX
    str     r0, [r1]

    /* Phase 1: tell A_Handler to pend B */
    mov     r0, #1
    ldr     r1, =PHASE
    str     r0, [r1]

    /* Pend IRQ0 */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #1
    str     r0, [r5]
    dsb
    isb                         /* A_Handler runs, B preempts inside */

    /* Verify order_index == 4 (all 4 markers recorded) */
    ldr     r1, =ORDER_INDEX
    ldr     r0, [r1]
    cmp     r0, #4
    bne     fail_2

    /* Verify execution_order[0] == A_START (0xA0) */
    ldr     r4, =EXEC_ORDER
    ldr     r0, [r4, #0]
    cmp     r0, #MARKER_A_START
    bne     fail_3

    /* Verify execution_order[1] == B_START (0xB0) — B preempted A */
    ldr     r0, [r4, #4]
    cmp     r0, #MARKER_B_START
    bne     fail_4

    /* Verify execution_order[2] == B_END (0xB1) */
    ldr     r0, [r4, #8]
    cmp     r0, #MARKER_B_END
    bne     fail_5

    /* Verify execution_order[3] == A_END (0xA1) — A resumes */
    ldr     r0, [r4, #12]
    cmp     r0, #MARKER_A_END
    bne     fail_6

    /* ==================================================================
     * Subtest 3: FAULTMASK blocks all configurable interrupts
     *
     * Set FAULTMASK = 1, pend IRQ0 (priority 0xFF), verify NOT delivered.
     * Clear FAULTMASK, verify IRQ0 then fires.
     *
     * This passes both before and after fix (both 0 and -1 block 0xFF).
     * Included for FAULTMASK behavioral validation.
     * ================================================================== */

    /* Reset state */
    mov     r0, #0
    ldr     r1, =IRQ0_RAN
    str     r0, [r1]

    /* Phase 0: A_Handler should NOT pend B */
    ldr     r1, =PHASE
    str     r0, [r1]

    /* Set FAULTMASK = 1 via MSR */
    mov     r0, #1
    msr     faultmask, r0

    /* Pend IRQ0 */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #1
    str     r0, [r5]
    dsb
    isb                         /* delivery opportunity (should be blocked) */

    /* Verify IRQ0 did NOT fire */
    ldr     r1, =IRQ0_RAN
    ldr     r0, [r1]
    cmp     r0, #0
    bne     fail_7              /* FAIL: IRQ fired despite FAULTMASK */

    /* Clear FAULTMASK */
    mov     r0, #0
    msr     faultmask, r0
    dsb
    isb                         /* delivery opportunity (should fire now) */

    /* Verify IRQ0 now fired */
    ldr     r1, =IRQ0_RAN
    ldr     r0, [r1]
    cmp     r0, #1
    bne     fail_8              /* FAIL: IRQ not delivered after clearing */

    /* ==================================================================
     * All subtests passed
     * ================================================================== */
    b       pass

/* =========================================================================
 * A_Handler — IRQ0, priority 0xFF (lowest with 8 bits)
 *
 * Phase 0: just set irq0_ran flag (subtests 1 and 3).
 * Phase 1: record markers and pend IRQ1 for nesting (subtest 2).
 * ========================================================================= */
    .thumb_func
    .globl A_Handler
A_Handler:
    push    {r4-r7, lr}

    /* Set irq0_ran = 1 */
    ldr     r1, =IRQ0_RAN
    mov     r0, #1
    str     r0, [r1]

    /* Check phase: if phase == 1, do nesting test */
    ldr     r1, =PHASE
    ldr     r0, [r1]
    cmp     r0, #1
    bne     a_handler_done

    /* Phase 1: nesting test — record A_START, pend B, record A_END */

    /* Record A_START */
    mov     r0, #MARKER_A_START
    bl      record_marker

    /* Pend IRQ1 (priority 0xFE) — should preempt since 0xFE < 0xFF */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #2              /* bit 1 = IRQ1 = B */
    str     r0, [r5]
    dsb
    isb                         /* B preempts here */

    /* B has returned, record A_END */
    mov     r0, #MARKER_A_END
    bl      record_marker

a_handler_done:
    pop     {r4-r7, pc}

/* =========================================================================
 * B_Handler — IRQ1, priority 0xFE
 *
 * Records B_START and B_END markers for the nesting test.
 * ========================================================================= */
    .thumb_func
    .globl B_Handler
B_Handler:
    push    {r4-r7, lr}

    /* Set irq1_ran = 1 */
    ldr     r1, =IRQ1_RAN
    mov     r0, #1
    str     r0, [r1]

    /* Record B_START */
    mov     r0, #MARKER_B_START
    bl      record_marker

    /* Record B_END */
    mov     r0, #MARKER_B_END
    bl      record_marker

    pop     {r4-r7, pc}

/* =========================================================================
 * record_marker — append a marker value to execution_order[]
 *
 * Input:  r0 = marker value
 * Clobbers: r1, r2, r3
 * ========================================================================= */
    .thumb_func
record_marker:
    ldr     r1, =ORDER_INDEX
    ldr     r2, [r1]            /* r2 = current index */
    ldr     r3, =EXEC_ORDER
    str     r0, [r3, r2, lsl #2]  /* execution_order[index] = marker */
    add     r2, r2, #1
    str     r2, [r1]            /* order_index++ */
    bx      lr

/* =========================================================================
 * Result reporting
 * ========================================================================= */
    .thumb_func
pass:
    ldr     r0, =PASS_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    /* Semihosting exit */
    mov     r0, #0x18           /* SYS_EXIT */
    ldr     r1, =0x20026        /* ADP_Stopped_ApplicationExit */
    bkpt    #0xab
    b       .

    .thumb_func
fail_1:
    mov     r0, #1
    b       fail_common
    .thumb_func
fail_2:
    mov     r0, #2
    b       fail_common
    .thumb_func
fail_3:
    mov     r0, #3
    b       fail_common
    .thumb_func
fail_4:
    mov     r0, #4
    b       fail_common
    .thumb_func
fail_5:
    mov     r0, #5
    b       fail_common
    .thumb_func
fail_6:
    mov     r0, #6
    b       fail_common
    .thumb_func
fail_7:
    mov     r0, #7
    b       fail_common
    .thumb_func
fail_8:
    mov     r0, #8
    b       fail_common

    .thumb_func
fail_common:
    ldr     r1, =TEST_SUBTEST
    str     r0, [r1]
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    /* Semihosting exit */
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .

/* =========================================================================
 * default_handler — catch unexpected exceptions
 * ========================================================================= */
    .thumb_func
default_handler:
    b       .                   /* spin forever — tick limit will catch */

    .end
