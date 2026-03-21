/*
 * test_systick_active_pending.S — SysTick "active and pending" (Bug 17 check)
 *
 * Verifies the ARM-architectural "active and pending" exception state:
 *   - SysTick CAN be pending while its handler is already active
 *   - The pending instance must NOT re-enter the handler (no self-preemption)
 *   - After the handler returns, the pending SysTick runs via tail-chaining
 *
 * This is NOT a bug — DDI0403E B1.5.4 defines preemption as IP < EP
 * (strictly less than), and the active exception's priority contributes
 * to execution priority, so same-priority pending cannot preempt.
 *
 * -------------------------------------------------------------------------
 * Test design:
 *
 *   1. Configure SysTick with a short reload value (100 cycles)
 *   2. Set SysTick priority to 0x80 via SHPR3
 *   3. Enable SysTick (ENABLE | TICKINT | CLKSOURCE)
 *   4. SysTick fires → SysTick_Handler runs
 *
 *   In SysTick_Handler (first invocation):
 *     a. Increment invocation_count to 1
 *     b. Record marker 0x10 (first entry)
 *     c. Spin in a delay loop (~300 iterations) so that SysTick fires
 *        again while the handler is still running.  This puts SysTick
 *        into the "active AND pending" state.
 *     d. Record marker 0x11 (first exit)
 *     e. Return — the pending SysTick should tail-chain
 *
 *   In SysTick_Handler (second invocation, via tail-chain):
 *     a. Increment invocation_count to 2
 *     b. Record marker 0x20 (second entry)
 *     c. Disable SysTick (prevent further interrupts)
 *     d. Record marker 0x21 (second exit)
 *     e. Return
 *
 *   In Reset_Handler (after both invocations):
 *     Verify markers and nesting depth
 *
 * -------------------------------------------------------------------------
 * Expected correct behavior:
 *   Markers: [0x10, 0x11, 0x20, 0x21]
 *     → Handler ran twice sequentially (tail-chained), never nested
 *
 * If Bug 17 existed (self-preemption):
 *   Markers: [0x10, 0x20, 0x21, 0x11]
 *     → Second invocation nested inside first (re-entrant)
 *
 * -------------------------------------------------------------------------
 * SysTick registers:
 *   0xE000E010  SYST_CSR   — Control and Status (ENABLE[0], TICKINT[1],
 *                             CLKSOURCE[2], COUNTFLAG[16])
 *   0xE000E014  SYST_RVR   — Reload Value (24-bit)
 *   0xE000E018  SYST_CVR   — Current Value (write any to clear)
 *
 * SysTick priority:
 *   0xE000ED20  SHPR3[31:24] — SysTick priority byte
 *
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000200  execution_order[0..3]  — 4 marker words
 *   0x20000220  order_index
 *   0x20000224  invocation_count
 *   0x20000228  nesting_depth          — current depth (inc on entry, dec on exit)
 *   0x2000022C  max_nesting_depth      — high-water mark
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,       0x20000100
    .equ TEST_SUBTEST,      0x20000104
    .equ EXEC_ORDER,        0x20000200
    .equ ORDER_INDEX,       0x20000220
    .equ INVOCATION_COUNT,  0x20000224
    .equ NESTING_DEPTH,     0x20000228
    .equ MAX_NESTING_DEPTH, 0x2000022C
    .equ PASS_VAL,          0xCAFECAFE
    .equ FAIL_VAL,          0xDEADDEAD

    /* SysTick registers */
    .equ SYST_CSR,          0xE000E010
    .equ SYST_RVR,          0xE000E014
    .equ SYST_CVR,          0xE000E018
    .equ SHPR3,             0xE000ED20

    /* SysTick CSR bits */
    .equ CSR_ENABLE,        (1 << 0)
    .equ CSR_TICKINT,       (1 << 1)
    .equ CSR_CLKSOURCE,     (1 << 2)
    .equ CSR_ALL,           (CSR_ENABLE | CSR_TICKINT | CSR_CLKSOURCE)

    /* Markers */
    .equ MARKER_FIRST_ENTRY,   0x10
    .equ MARKER_FIRST_EXIT,    0x11
    .equ MARKER_SECOND_ENTRY,  0x20
    .equ MARKER_SECOND_EXIT,   0x21

    /* Delay loop iteration count — must be large enough so that SysTick
     * fires at least once during the handler.  With reload=100 cycles
     * at 168 MHz and ~2 instructions per loop iteration, 300 iterations
     * gives ~600 instructions ≈ 600+ cycles >> 100 cycle reload.  */
    .equ DELAY_ITERATIONS,  300

/* =========================================================================
 * Vector table
 *
 * SysTick is exception 15.
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
    .word   SysTick_Handler     /* Entry 15: SysTick */

/* =========================================================================
 * Reset_Handler — main test body
 * ========================================================================= */
    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* ------------------------------------------------------------------
     * Phase 1: Initialize all state variables to zero
     * ------------------------------------------------------------------ */
    ldr     r4, =ORDER_INDEX
    mov     r0, #0
    str     r0, [r4]

    ldr     r4, =INVOCATION_COUNT
    str     r0, [r4]

    ldr     r4, =NESTING_DEPTH
    str     r0, [r4]

    ldr     r4, =MAX_NESTING_DEPTH
    str     r0, [r4]

    /* Zero execution_order[0..3] */
    ldr     r4, =EXEC_ORDER
    mov     r1, #0
    mov     r2, #4
init_loop:
    str     r1, [r4], #4
    subs    r2, r2, #1
    bne     init_loop

    /* ------------------------------------------------------------------
     * Phase 2: Configure SysTick
     * ------------------------------------------------------------------ */

    /* Set SysTick priority to 0x80 via SHPR3[31:24].
     * SHPR3 is at 0xE000ED20.  We do a read-modify-write to avoid
     * clobbering PendSV priority in bits[23:16]. */
    ldr     r5, =SHPR3
    ldr     r0, [r5]
    ldr     r1, =0x00FFFFFF     /* clear bits[31:24] */
    and     r0, r0, r1
    ldr     r1, =0x80000000     /* SysTick priority = 0x80 */
    orr     r0, r0, r1
    str     r0, [r5]

    /* Set reload value to 100 cycles.
     * At 168 MHz, SysTick fires every 101 CPU cycles (~101 instructions). */
    ldr     r5, =SYST_RVR
    mov     r0, #100
    str     r0, [r5]

    /* Clear current value (writing any value clears it and COUNTFLAG) */
    ldr     r5, =SYST_CVR
    mov     r0, #0
    str     r0, [r5]

    /* Enable SysTick: ENABLE | TICKINT | CLKSOURCE */
    ldr     r5, =SYST_CSR
    mov     r0, #CSR_ALL
    str     r0, [r5]

    /* ------------------------------------------------------------------
     * Phase 3: Wait for SysTick handler to run twice
     *
     * Busy-wait polling invocation_count.  SysTick events fire between
     * instruction boundaries in the AtomicSimpleCPU model.
     * ------------------------------------------------------------------ */
    ldr     r4, =INVOCATION_COUNT
wait_loop:
    ldr     r0, [r4]
    cmp     r0, #2
    blt     wait_loop

    /* ------------------------------------------------------------------
     * Phase 4: Verify results
     * ------------------------------------------------------------------ */
    ldr     r4, =EXEC_ORDER
    ldr     r6, =TEST_RESULT
    ldr     r7, =TEST_SUBTEST

    /* Subtest 1: order_index == 4 (exactly 4 markers recorded) */
    ldr     r5, =ORDER_INDEX
    ldr     r0, [r5]
    cmp     r0, #4
    bne     fail_1

    /* Subtest 2: marker[0] == 0x10 (first SysTick entry) */
    ldr     r0, [r4, #0]
    cmp     r0, #MARKER_FIRST_ENTRY
    bne     fail_2

    /* Subtest 3: marker[1] == 0x11 (first SysTick exit — NOT nested)
     * If self-preemption occurred, marker[1] would be 0x20 instead. */
    ldr     r0, [r4, #4]
    cmp     r0, #MARKER_FIRST_EXIT
    bne     fail_3

    /* Subtest 4: marker[2] == 0x20 (second SysTick entry, tail-chained) */
    ldr     r0, [r4, #8]
    cmp     r0, #MARKER_SECOND_ENTRY
    bne     fail_4

    /* Subtest 5: marker[3] == 0x21 (second SysTick exit) */
    ldr     r0, [r4, #12]
    cmp     r0, #MARKER_SECOND_EXIT
    bne     fail_5

    /* Subtest 6: max_nesting_depth == 1 (never nested — depth was
     * always 1 during handler execution, never 2+) */
    ldr     r5, =MAX_NESTING_DEPTH
    ldr     r0, [r5]
    cmp     r0, #1
    bne     fail_6

    /* Subtest 7: invocation_count == 2 (handler ran exactly twice) */
    ldr     r5, =INVOCATION_COUNT
    ldr     r0, [r5]
    cmp     r0, #2
    bne     fail_7

    /* ------------------------------------------------------------------
     * All subtests passed
     * ------------------------------------------------------------------ */
    ldr     r0, =PASS_VAL
    str     r0, [r6]
    b       test_exit

    /* Failure paths — record which subtest failed */
fail_1: mov r0, #1
    b       fail_common
fail_2: mov r0, #2
    b       fail_common
fail_3: mov r0, #3
    b       fail_common
fail_4: mov r0, #4
    b       fail_common
fail_5: mov r0, #5
    b       fail_common
fail_6: mov r0, #6
    b       fail_common
fail_7: mov r0, #7

fail_common:
    str     r0, [r7]            /* TEST_SUBTEST = failing subtest */
    ldr     r0, =FAIL_VAL
    str     r0, [r6]            /* TEST_RESULT = FAIL */

test_exit:
    /* Semihosting exit */
    mov     r0, #0x18           /* SYS_EXIT */
    ldr     r1, =0x20026        /* ADP_Stopped_ApplicationExit */
    bkpt    #0xAB

/* =========================================================================
 * SysTick_Handler — runs when SysTick counter reaches zero
 *
 * First invocation: spins in a delay loop so SysTick fires again
 *   (creating the "active AND pending" state), then returns.
 * Second invocation: disables SysTick and returns.
 * ========================================================================= */
    .thumb_func
SysTick_Handler:
    push    {r4-r7, lr}

    /* ---- Track nesting depth ---- */
    ldr     r4, =NESTING_DEPTH
    ldr     r0, [r4]
    add     r0, r0, #1          /* nesting_depth++ */
    str     r0, [r4]

    /* Update max_nesting_depth if current depth is higher */
    ldr     r5, =MAX_NESTING_DEPTH
    ldr     r1, [r5]
    cmp     r0, r1
    ble     depth_ok
    str     r0, [r5]            /* max_nesting_depth = nesting_depth */
depth_ok:

    /* ---- Increment invocation_count ---- */
    ldr     r4, =INVOCATION_COUNT
    ldr     r0, [r4]
    add     r0, r0, #1
    str     r0, [r4]

    /* ---- Branch on invocation number ---- */
    cmp     r0, #1
    beq     first_invocation
    cmp     r0, #2
    beq     second_invocation

    /* Third+ invocation: just disable SysTick and return.
     * This is a safety net — should not happen if test is correct. */
    ldr     r5, =SYST_CSR
    mov     r0, #0
    str     r0, [r5]
    b       handler_exit

first_invocation:
    /* Record first-entry marker */
    mov     r0, #MARKER_FIRST_ENTRY
    bl      record_marker

    /* Spin in a delay loop.  With reload=100 cycles and ~2 cycles per
     * loop iteration, 300 iterations ≈ 600 cycles.  SysTick will fire
     * multiple times during this loop, setting PENDSTSET each time.
     * But the handler must NOT be re-entered (same priority = no preempt).
     * Only one pending instance survives (bit, not counter). */
    mov     r2, #0
    ldr     r3, =DELAY_ITERATIONS
delay_loop:
    add     r2, r2, #1
    cmp     r2, r3
    blt     delay_loop

    /* Record first-exit marker.
     * If no self-preemption occurred, this is marker[1]. */
    mov     r0, #MARKER_FIRST_EXIT
    bl      record_marker
    b       handler_exit

second_invocation:
    /* Disable SysTick FIRST to prevent further timer events */
    ldr     r5, =SYST_CSR
    mov     r0, #0
    str     r0, [r5]

    /* Record second-entry marker */
    mov     r0, #MARKER_SECOND_ENTRY
    bl      record_marker

    /* Record second-exit marker */
    mov     r0, #MARKER_SECOND_EXIT
    bl      record_marker

handler_exit:
    /* Decrement nesting depth */
    ldr     r4, =NESTING_DEPTH
    ldr     r0, [r4]
    sub     r0, r0, #1
    str     r0, [r4]

    pop     {r4-r7, pc}

/* =========================================================================
 * record_marker — append a marker value to execution_order[]
 *
 * Input: r0 = marker value
 * Clobbers: r1, r2, r3
 * ========================================================================= */
    .thumb_func
record_marker:
    ldr     r1, =ORDER_INDEX
    ldr     r2, [r1]            /* r2 = current index */
    ldr     r3, =EXEC_ORDER
    str     r0, [r3, r2, lsl #2]   /* execution_order[index] = marker */
    add     r2, r2, #1
    str     r2, [r1]            /* order_index++ */
    bx      lr

/* =========================================================================
 * default_handler — catch unexpected exceptions
 * ========================================================================= */
    .thumb_func
default_handler:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    mov     r0, #99
    ldr     r1, =TEST_SUBTEST
    str     r0, [r1]
    /* Semihosting exit */
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xAB

    .end
