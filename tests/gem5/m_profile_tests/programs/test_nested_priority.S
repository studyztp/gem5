/*
 * test_nested_priority.S — NVIC priority-based preemption (Bug 16)
 *
 * Bug 16: executionPriority() in m_profile_scs.cc always returns 0xFF
 *   when no masks (FAULTMASK/PRIMASK/BASEPRI) are set, ignoring the
 *   priorities of currently active exceptions.  This allows any pending
 *   interrupt to preempt any handler regardless of priority.
 *
 * Additionally, deactivateIRQ() is never called from mProfileExcReturn(),
 *   so active bits are never cleared on exception return.  Both issues
 *   must be fixed for correct priority-based preemption.
 *
 * -------------------------------------------------------------------------
 * Interrupt assignment:
 *
 *   IRQ0 = A, priority 0x00 (highest configurable)
 *   IRQ1 = B, priority 0x40
 *   IRQ2 = C, priority 0x80
 *   IRQ3 = D, priority 0x80 (same as C — equal, not higher)
 *
 *   Priority ordering: A > B > C = D
 *   (lower number = higher priority on Cortex-M)
 *
 * -------------------------------------------------------------------------
 * Test algorithm:
 *
 *   Phase 1: Setup
 *     1. Configure priorities via IPR0 word write (avoids Bug 18):
 *        IPR0 = 0x80804000 → IRQ0=0x00, IRQ1=0x40, IRQ2=0x80, IRQ3=0x80
 *     2. Enable IRQ0-3 via ISER0: write 0x0F
 *     3. Initialize execution_order[0..7] and order_index = 0
 *
 *   Phase 2: Trigger C (priority 0x80)
 *     4. Pend IRQ2 via ISPR0 bit 2 → CPU delivers C (exception 18)
 *
 *   In C_Handler (IRQ2, priority 0x80):
 *     5. Record C_START (0xC0)
 *     6. Pend D (IRQ3) via ISPR0 bit 3
 *        → D has equal priority (0x80).  Equal is NOT strictly less than
 *          the execution priority, so D must NOT preempt C.
 *        → With Bug 16: executionPriority() returns 0xFF,
 *          0x80 < 0xFF → D wrongly preempts C.
 *     7. DSB + ISB (give preemption opportunity)
 *     8. Pend A (IRQ0) via ISPR0 bit 0
 *        → A has higher priority (0x00 < 0x80), so A SHOULD preempt C.
 *     9. DSB + ISB → A preempts here
 *     10. (After A, B return) Record C_END (0xC1)
 *
 *   In A_Handler (IRQ0, priority 0x00):
 *     11. Record A_START (0xA0)
 *     12. Pend B (IRQ1) via ISPR0 bit 1
 *         → B priority 0x40 >= A priority 0x00, so B must NOT preempt A.
 *         → With Bug 16: 0x40 < 0xFF → B wrongly preempts A.
 *     13. DSB + ISB (give preemption opportunity)
 *     14. Record A_END (0xA1)
 *     15. Return from handler
 *         → Pending: B(0x40), D(0x80).  Active: C(0x80).
 *           executionPriority = 0x80 (C is active).
 *           B(0x40) < 0x80 → B preempts C.
 *
 *   In B_Handler (IRQ1, priority 0x40):
 *     16. Record B_START (0xB0)
 *     17. Record B_END (0xB1)
 *     18. Return from handler
 *         → Pending: D(0x80).  Active: C(0x80).
 *           executionPriority = 0x80 (C is active).
 *           D(0x80) NOT < 0x80 (equal) → D cannot preempt.
 *           Resume C.
 *
 *   Back in C_Handler:
 *     19. Record C_END (0xC1)
 *     20. Return from handler
 *         → Pending: D(0x80).  Active: none.
 *           executionPriority = 0xFF (Thread mode).
 *           D(0x80) < 0xFF → D runs.
 *
 *   In D_Handler (IRQ3, priority 0x80):
 *     21. Record D_START (0xD0)
 *     22. Record D_END (0xD1)
 *     23. Return → back to Thread mode
 *
 *   Phase 3: Verify execution order (8 markers)
 *     Index 0: C_START (0xC0)
 *     Index 1: A_START (0xA0)  ← key check: NOT D_START
 *     Index 2: A_END   (0xA1)
 *     Index 3: B_START (0xB0)  ← B preempts C after A returns
 *     Index 4: B_END   (0xB1)
 *     Index 5: C_END   (0xC1)  ← C resumes after B returns
 *     Index 6: D_START (0xD0)  ← D runs last (after C deactivates)
 *     Index 7: D_END   (0xD1)
 *
 * -------------------------------------------------------------------------
 * Expected results:
 *
 *   With Bug 16 (executionPriority returns 0xFF):
 *     Buggy order: [C0, D0, D1, A0, B0, B1, A1, C1]
 *     D wrongly preempts C (equal priority), B wrongly preempts A.
 *     → FAIL at subtest 2 (index 1: expected 0xA0, got 0xD0)
 *
 *   After fix:
 *     Correct order: [C0, A0, A1, B0, B1, C1, D0, D1]
 *     → PASS
 *
 * -------------------------------------------------------------------------
 * NVIC register addresses:
 *   0xE000E100  ISER0  — Interrupt Set-Enable Register 0
 *   0xE000E200  ISPR0  — Interrupt Set-Pending Register 0
 *   0xE000E400  IPR0   — Interrupt Priority Register 0 (4 bytes: IRQ0-3)
 *
 * Memory map:
 *   0x20000100  TEST_RESULT        — 0xCAFECAFE or 0xDEADDEAD
 *   0x20000104  TEST_SUBTEST       — failing subtest number (1-based)
 *   0x20000200  execution_order[0..7]  — 8 marker words
 *   0x20000220  order_index        — current index (0..7)
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,       0x20000100
    .equ TEST_SUBTEST,      0x20000104
    .equ EXEC_ORDER,        0x20000200  /* 8 words: marker values */
    .equ ORDER_INDEX,       0x20000220  /* current index (0..7) */
    .equ PASS_VAL,          0xCAFECAFE
    .equ FAIL_VAL,          0xDEADDEAD

    /* NVIC registers */
    .equ NVIC_ISER0,        0xE000E100
    .equ NVIC_ISPR0,        0xE000E200
    .equ NVIC_IPR0,         0xE000E400

    /* Execution order markers:
     *   A = IRQ0 (priority 0x00): 0xA0 start, 0xA1 end
     *   B = IRQ1 (priority 0x40): 0xB0 start, 0xB1 end
     *   C = IRQ2 (priority 0x80): 0xC0 start, 0xC1 end
     *   D = IRQ3 (priority 0x80): 0xD0 start, 0xD1 end */
    .equ MARKER_A_START,    0xA0
    .equ MARKER_A_END,      0xA1
    .equ MARKER_B_START,    0xB0
    .equ MARKER_B_END,      0xB1
    .equ MARKER_C_START,    0xC0
    .equ MARKER_C_END,      0xC1
    .equ MARKER_D_START,    0xD0
    .equ MARKER_D_END,      0xD1

/* =========================================================================
 * Vector table
 *
 * Exceptions 16-19 = IRQ0-3 (A, B, C, D).
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
    .word   A_Handler           /* Entry 16: IRQ0 = A (priority 0x00) */
    .word   B_Handler           /* Entry 17: IRQ1 = B (priority 0x40) */
    .word   C_Handler           /* Entry 18: IRQ2 = C (priority 0x80) */
    .word   D_Handler           /* Entry 19: IRQ3 = D (priority 0x80) */

/* =========================================================================
 * Reset_Handler — main test body
 * ========================================================================= */
    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* ------------------------------------------------------------------
     * Phase 1: Initialize state
     * ------------------------------------------------------------------ */

    /* Zero the order_index */
    ldr     r4, =ORDER_INDEX
    mov     r0, #0
    str     r0, [r4]

    /* Zero execution_order[0..7] */
    ldr     r4, =EXEC_ORDER
    mov     r1, #0
    mov     r2, #8
init_loop:
    str     r1, [r4], #4
    subs    r2, r2, #1
    bne     init_loop

    /* ------------------------------------------------------------------
     * Configure NVIC priorities via IPR0 word write.
     *
     * IPR0 at 0xE000E400 is a 32-bit register with 4 packed priority
     * bytes (little-endian):
     *   [7:0]   = IRQ0 priority = 0x00 (A, highest)
     *   [15:8]  = IRQ1 priority = 0x40 (B)
     *   [23:16] = IRQ2 priority = 0x80 (C)
     *   [31:24] = IRQ3 priority = 0x80 (D, same as C)
     *
     * Word value = 0x80804000
     *
     * We use a word write to avoid Bug 18 (byte access not yet fixed).
     * ------------------------------------------------------------------ */
    ldr     r5, =NVIC_IPR0
    ldr     r0, =0x80804000
    str     r0, [r5]

    /* Enable IRQ0-3 via ISER0: bits [3:0] = 0x0F */
    ldr     r5, =NVIC_ISER0
    mov     r0, #0x0F
    str     r0, [r5]

    /* ------------------------------------------------------------------
     * Phase 2: Trigger C (IRQ2) via software pending.
     *
     * Writing bit 2 to ISPR0 pends IRQ2 (C).  C is the only pending
     * interrupt and is enabled, so the NVIC delivers it at the next
     * instruction boundary after DSB+ISB.
     * ------------------------------------------------------------------ */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #4              /* pend IRQ2 = C (bit 2) */
    str     r0, [r5]
    dsb
    isb                         /* C handler runs here */

    /* ------------------------------------------------------------------
     * Phase 3: All handlers have completed.  Verify execution order.
     *
     * Expected correct order (8 markers):
     *   [0] = 0xC0  C_START
     *   [1] = 0xA0  A_START  (A preempts C)
     *   [2] = 0xA1  A_END
     *   [3] = 0xB0  B_START  (B preempts C after A returns)
     *   [4] = 0xB1  B_END
     *   [5] = 0xC1  C_END    (C resumes)
     *   [6] = 0xD0  D_START  (D runs after C deactivates)
     *   [7] = 0xD1  D_END
     * ------------------------------------------------------------------ */

    /* Subtest 1: order_index == 8 (all 8 markers recorded) */
    ldr     r4, =ORDER_INDEX
    ldr     r0, [r4]
    cmp     r0, #8
    bne     fail_1

    ldr     r4, =EXEC_ORDER

    /* Subtest 2: [0] == C_START (0xC0) — C started first */
    ldr     r0, [r4, #0]
    cmp     r0, #MARKER_C_START
    bne     fail_2

    /* Subtest 3: [1] == A_START (0xA0) — A preempted C
     * KEY CHECK: with Bug 16 this is 0xD0 (D wrongly preempts C) */
    ldr     r0, [r4, #4]
    cmp     r0, #MARKER_A_START
    bne     fail_3

    /* Subtest 4: [2] == A_END (0xA1) — A finished without B preempting */
    ldr     r0, [r4, #8]
    cmp     r0, #MARKER_A_END
    bne     fail_4

    /* Subtest 5: [3] == B_START (0xB0) — B preempts C after A returns */
    ldr     r0, [r4, #12]
    cmp     r0, #MARKER_B_START
    bne     fail_5

    /* Subtest 6: [4] == B_END (0xB1) */
    ldr     r0, [r4, #16]
    cmp     r0, #MARKER_B_END
    bne     fail_6

    /* Subtest 7: [5] == C_END (0xC1) — C resumes after B returns */
    ldr     r0, [r4, #20]
    cmp     r0, #MARKER_C_END
    bne     fail_7

    /* Subtest 8: [6] == D_START (0xD0) — D runs last */
    ldr     r0, [r4, #24]
    cmp     r0, #MARKER_D_START
    bne     fail_8

    /* Subtest 9: [7] == D_END (0xD1) */
    ldr     r0, [r4, #28]
    cmp     r0, #MARKER_D_END
    bne     fail_9

    /* All subtests passed */
    b       pass

/* =========================================================================
 * C_Handler — IRQ2, priority 0x80
 *
 * C is the first interrupt to fire.  Inside C:
 *   1. Record C_START
 *   2. Pend D (equal priority 0x80) → must NOT preempt
 *   3. Pend A (higher priority 0x00) → MUST preempt
 *   4. (After A and B return) Record C_END
 * ========================================================================= */
    .thumb_func
    .globl C_Handler
C_Handler:
    push    {r4-r7, lr}

    /* Record C_START */
    mov     r0, #MARKER_C_START
    bl      record_marker

    /* Pend D (IRQ3, bit 3) — equal priority, must NOT preempt C.
     * With Bug 16: executionPriority() returns 0xFF, 0x80 < 0xFF → D
     * wrongly preempts here.
     * Correct: executionPriority() returns 0x80 (C active), 0x80 is
     * NOT < 0x80 → D stays pending. */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #8              /* bit 3 = IRQ3 = D */
    str     r0, [r5]
    dsb
    isb                         /* preemption opportunity */

    /* If Bug 16 is present, D ran and returned here.
     * If fixed, D is still pending — it will run after C returns. */

    /* Pend A (IRQ0, bit 0) — higher priority, MUST preempt C.
     * Both buggy and correct: A preempts C (0x00 < 0x80 and 0x00 < 0xFF). */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #1              /* bit 0 = IRQ0 = A */
    str     r0, [r5]
    dsb
    isb                         /* A preempts here */

    /* A has returned.  With correct fix, B also ran (B preempted C
     * after A returned, since B priority 0x40 < C priority 0x80). */

    /* Record C_END */
    mov     r0, #MARKER_C_END
    bl      record_marker

    pop     {r4-r7, pc}

/* =========================================================================
 * A_Handler — IRQ0, priority 0x00 (highest)
 *
 * A preempts C.  Inside A:
 *   1. Record A_START
 *   2. Pend B (priority 0x40) → must NOT preempt A (0x40 >= 0x00)
 *   3. Record A_END
 *
 * After A returns:
 *   Pending: B(0x40), D(0x80).  Active: C(0x80).
 *   B(0x40) < 0x80 → B preempts C next.
 * ========================================================================= */
    .thumb_func
    .globl A_Handler
A_Handler:
    push    {r4-r7, lr}

    /* Record A_START */
    mov     r0, #MARKER_A_START
    bl      record_marker

    /* Pend B (IRQ1, bit 1) — lower priority than A, must NOT preempt.
     * With Bug 16: executionPriority() returns 0xFF, 0x40 < 0xFF → B
     * wrongly preempts A.
     * Correct: executionPriority() returns 0x00 (A active), 0x40 is
     * NOT < 0x00 → B stays pending until A returns. */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #2              /* bit 1 = IRQ1 = B */
    str     r0, [r5]
    dsb
    isb                         /* preemption opportunity */

    /* If Bug 16 is present, B ran and returned here.
     * If fixed, B is still pending — it will preempt C after A returns. */

    /* Record A_END */
    mov     r0, #MARKER_A_END
    bl      record_marker

    pop     {r4-r7, pc}

/* =========================================================================
 * B_Handler — IRQ1, priority 0x40
 *
 * B runs after A returns (B preempts C since 0x40 < 0x80).
 * Simply records start and end markers.
 * ========================================================================= */
    .thumb_func
    .globl B_Handler
B_Handler:
    push    {r4-r7, lr}

    mov     r0, #MARKER_B_START
    bl      record_marker

    mov     r0, #MARKER_B_END
    bl      record_marker

    pop     {r4-r7, pc}

/* =========================================================================
 * D_Handler — IRQ3, priority 0x80 (same as C)
 *
 * D runs last — after C returns to Thread mode, D finally gets
 * delivered since its priority 0x80 < 0xFF (Thread mode).
 * ========================================================================= */
    .thumb_func
    .globl D_Handler
D_Handler:
    push    {r4-r7, lr}

    mov     r0, #MARKER_D_START
    bl      record_marker

    mov     r0, #MARKER_D_END
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
fail_9:
    mov     r0, #9
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
    b       .                   /* spin forever — tick limit will catch this */

    .end
