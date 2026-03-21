/*
 * test_scs_checkpoint.S — SCS checkpoint/restore via nested exceptions (Bug 19)
 *
 * Verifies that all NVIC exception state survives gem5 checkpoint/restore
 * by checkpointing mid-exception-handler and verifying the full nesting
 * and unwinding chain works correctly after restore.
 *
 * -------------------------------------------------------------------------
 * Design (same priority setup as test_nested_priority.S):
 *
 *   IRQ0 = A, priority 0x00 (highest)
 *   IRQ1 = B, priority 0x40
 *   IRQ2 = C, priority 0x80
 *   IRQ3 = D, priority 0x80 (same as C)
 *
 * Execution flow:
 *   1. Reset_Handler: configure priorities, enable IRQ0-3, pend C
 *   2. C_Handler: record C_START, pend D (equal priority, stays pending),
 *      pend A (higher priority)
 *
 *   *** Config script checkpoints at tick 523776 ***
 *   At checkpoint: C is active, D and A are pending.
 *   (From nested_priority Exec trace: tick 523776 = C_Handler just wrote
 *    ISPR0 to pend A, before DSB+ISB triggers A's preemption.)
 *
 *   After restore:
 *   3. C_Handler continues: DSB+ISB → A preempts C
 *   4. A_Handler: record A_START, pend B, record A_END, return
 *   5. B preempts C (0x40 < 0x80): record B_START, B_END, return
 *   6. C_Handler resumes: record C_END, return
 *   7. D runs (0x80 < 0xFF): record D_START, D_END, return
 *   8. Thread mode: verify order = [C0, A0, A1, B0, B1, C1, D0, D1]
 *
 * -------------------------------------------------------------------------
 * If Bug 19 exists (no serialize/unserialize in MProfileSCS):
 *   After restore, nvicActive[], nvicPending[], nvicPriority[] are zeroed.
 *   - A pending bit lost → A never preempts C
 *   - D pending bit lost → D never fires
 *   - C active bit lost → priority tracking broken
 *   Result: only [C0, C1] recorded → FAIL at subtest 1 (order_index != 8)
 *
 * After fix:
 *   All NVIC state survives → correct order → PASS
 *
 * -------------------------------------------------------------------------
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000200  execution_order[0..7]
 *   0x20000220  order_index
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,       0x20000100
    .equ TEST_SUBTEST,      0x20000104
    .equ EXEC_ORDER,        0x20000200
    .equ ORDER_INDEX,       0x20000220
    .equ PASS_VAL,          0xCAFECAFE
    .equ FAIL_VAL,          0xDEADDEAD

    /* NVIC registers */
    .equ NVIC_ISER0,        0xE000E100
    .equ NVIC_ISPR0,        0xE000E200
    .equ NVIC_IPR0,         0xE000E400

    /* Execution order markers */
    .equ MARKER_A_START,    0xA0
    .equ MARKER_A_END,      0xA1
    .equ MARKER_B_START,    0xB0
    .equ MARKER_B_END,      0xB1
    .equ MARKER_C_START,    0xC0
    .equ MARKER_C_END,      0xC1
    .equ MARKER_D_START,    0xD0
    .equ MARKER_D_END,      0xD1

/* =========================================================================
 * Vector table — IRQ0-3 = A, B, C, D handlers
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000          /* Entry 0:  Initial MSP */
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
 * Reset_Handler — setup, trigger exceptions, verify after unwinding
 * ========================================================================= */
    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* Zero order_index */
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

    /* Configure priorities: IPR0 = 0x80804000
     *   IRQ0=0x00 (A), IRQ1=0x40 (B), IRQ2=0x80 (C), IRQ3=0x80 (D) */
    ldr     r5, =NVIC_IPR0
    ldr     r0, =0x80804000
    str     r0, [r5]

    /* Enable IRQ0-3: ISER0 bits [3:0] = 0x0F */
    ldr     r5, =NVIC_ISER0
    mov     r0, #0x0F
    str     r0, [r5]

    /* Pend C (IRQ2, bit 2) → CPU delivers C */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #4
    str     r0, [r5]
    dsb
    isb                         /* C_Handler runs here, full chain follows */

    /* ==================================================================
     * All handlers have returned.  Verify execution order.
     *
     * Expected: [C0, A0, A1, B0, B1, C1, D0, D1]
     * ================================================================== */

    /* Subtest 1: order_index == 8 */
    ldr     r4, =ORDER_INDEX
    ldr     r0, [r4]
    cmp     r0, #8
    bne     fail_1

    ldr     r4, =EXEC_ORDER

    /* Subtest 2: [0] == C_START (0xC0) */
    ldr     r0, [r4, #0]
    cmp     r0, #MARKER_C_START
    bne     fail_2

    /* Subtest 3: [1] == A_START (0xA0) */
    ldr     r0, [r4, #4]
    cmp     r0, #MARKER_A_START
    bne     fail_3

    /* Subtest 4: [2] == A_END (0xA1) */
    ldr     r0, [r4, #8]
    cmp     r0, #MARKER_A_END
    bne     fail_4

    /* Subtest 5: [3] == B_START (0xB0) */
    ldr     r0, [r4, #12]
    cmp     r0, #MARKER_B_START
    bne     fail_5

    /* Subtest 6: [4] == B_END (0xB1) */
    ldr     r0, [r4, #16]
    cmp     r0, #MARKER_B_END
    bne     fail_6

    /* Subtest 7: [5] == C_END (0xC1) */
    ldr     r0, [r4, #20]
    cmp     r0, #MARKER_C_END
    bne     fail_7

    /* Subtest 8: [6] == D_START (0xD0) */
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
 * First interrupt to fire.  Records C_START, pends D (equal priority,
 * stays pending), pends A (higher priority, preempts C).
 * After A→B chain returns, records C_END.
 * ========================================================================= */
    .thumb_func
    .globl C_Handler
C_Handler:
    push    {r4-r7, lr}

    /* Record C_START */
    mov     r0, #MARKER_C_START
    bl      record_marker

    /* Pend D (IRQ3, bit 3) — equal priority 0x80, must NOT preempt C */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #8
    str     r0, [r5]
    dsb
    isb

    /* Pend A (IRQ0, bit 0) — higher priority 0x00, MUST preempt C.
     * *** Config script checkpoints at tick 523776 right after this
     *     ISPR0 write, before DSB+ISB triggers A's preemption. ***
     * At checkpoint: C active, A+D pending. */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #1
    str     r0, [r5]
    dsb
    isb                         /* A preempts here (after restore if checkpointed) */

    /* A (and B) have returned.  Record C_END. */
    mov     r0, #MARKER_C_END
    bl      record_marker

    pop     {r4-r7, pc}

/* =========================================================================
 * A_Handler — IRQ0, priority 0x00 (highest)
 *
 * Preempts C.  Records A_START, pends B (lower priority, stays pending),
 * records A_END, returns.
 * On return: B preempts C (0x40 < 0x80).
 * ========================================================================= */
    .thumb_func
    .globl A_Handler
A_Handler:
    push    {r4-r7, lr}

    /* Record A_START */
    mov     r0, #MARKER_A_START
    bl      record_marker

    /* Pend B (IRQ1, bit 1) — priority 0x40 >= A's 0x00, stays pending */
    ldr     r5, =NVIC_ISPR0
    mov     r0, #2
    str     r0, [r5]
    dsb
    isb

    /* Record A_END */
    mov     r0, #MARKER_A_END
    bl      record_marker

    pop     {r4-r7, pc}

/* =========================================================================
 * B_Handler — IRQ1, priority 0x40
 *
 * Runs after A returns (B preempts C since 0x40 < 0x80).
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
 * Runs last — after C returns to Thread mode, D finally delivers.
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
 * record_marker — append a marker to execution_order[]
 *
 * Input:  r0 = marker value
 * Clobbers: r1, r2, r3
 * ========================================================================= */
    .thumb_func
record_marker:
    ldr     r1, =ORDER_INDEX
    ldr     r2, [r1]
    ldr     r3, =EXEC_ORDER
    str     r0, [r3, r2, lsl #2]
    add     r2, r2, #1
    str     r2, [r1]
    bx      lr

/* =========================================================================
 * Result reporting
 * ========================================================================= */
    .thumb_func
pass:
    ldr     r0, =PASS_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    mov     r0, #0x18
    ldr     r1, =0x20026
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
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .

/* =========================================================================
 * default_handler — catch unexpected exceptions
 * ========================================================================= */
    .thumb_func
default_handler:
    b       .

    .end
