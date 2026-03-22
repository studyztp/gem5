/*
 * test_nvic_oob.S — NVIC array out-of-bounds test (BUG-1)
 *
 * Proves that NVIC bit-vector arrays (nvicEnabled, nvicPending, nvicActive)
 * are correctly sized to cover IRQs 224-239 (word index 7).
 *
 * The bug: MAX_IRQS / 32 = 240 / 32 = 7 (integer division), so the arrays
 * have only 7 elements (indices 0-6).  IRQs 224-239 need word index 7,
 * which is out-of-bounds.  Writing to ISER[7] (0xE000E11C) corrupts
 * adjacent memory — specifically nvicPending[0], since it sits right
 * after nvicEnabled[6] in the struct layout.
 *
 * =========================================================================
 * Phase A — Corruption detection (works with any num_irqs config)
 *
 *   Subtest 1: Set IRQ 0 pending via ISPR[0], read back to confirm
 *   Subtest 2: Write 0xFFFFFFFF to ISER[7] — the OOB write
 *   Subtest 3: Read ISPR[0] — verify IRQ 0 still pending (bit 0 = 1)
 *              Before fix: nvicEnabled[7] overlaps nvicPending[0],
 *              so 0xFFFFFFFF overwrites the pending state.
 *   Subtest 4: Clean up — clear IRQ 0 pending via ICPR[0]
 *
 * Phase B — High-IRQ register read/write (meaningful with num_irqs=240)
 *
 *   Subtest 5:  Write 0x1 to ISER[7] — enable IRQ 224
 *   Subtest 6:  Read ISER[7] — verify bit 0 set
 *   Subtest 7:  Write 0x1 to ISPR[7] — pend IRQ 224
 *   Subtest 8:  Read ISPR[7] — verify bit 0 set
 *   Subtest 9:  Clear via ICPR[7], read ISPR[7] — verify cleared
 *   Subtest 10: Read IABR[7] — verify 0 (not active)
 *
 * Phase C — Nested interrupt delivery for high IRQs (num_irqs=240)
 *
 *   Uses IRQ 224 (exception 240) and IRQ 239 (exception 255).
 *   IRQ 224 priority = 0x80 (lower priority, runs first).
 *   IRQ 239 priority = 0x40 (higher priority, preempts).
 *
 *   Flow:
 *     1. Enable both IRQs, pend IRQ 224
 *     2. IRQ 224 handler runs, records "started", pends IRQ 239
 *     3. IRQ 239 preempts (0x40 < 0x80), records "nested ran", returns
 *     4. IRQ 224 resumes, records "completed", returns
 *     5. Main code verifies execution order:
 *        seq[0]=1 (IRQ224 start), seq[1]=2 (IRQ239), seq[2]=3 (IRQ224 end)
 *
 *   Subtest 11: Verify seq[0] == 1 (IRQ 224 started)
 *   Subtest 12: Verify seq[1] == 2 (IRQ 239 preempted)
 *   Subtest 13: Verify seq[2] == 3 (IRQ 224 completed)
 *
 * =========================================================================
 * Memory map:
 *   0x20000100  TEST_RESULT   — 0xCAFECAFE (pass) or 0xDEADDEAD (fail)
 *   0x20000104  TEST_SUBTEST  — subtest number of first failure
 *   0x20000108  TEST_DATA     — observed value for debugging
 *   0x20000200  seq[0..2]     — execution sequence log for Phase C
 *   0x20000210  seq_idx       — next index into seq[]
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    /* NVIC register addresses */
    .equ NVIC_ISER7,    0xE000E11C   /* ISER word 7: IRQs 224-255 */
    .equ NVIC_ICER7,    0xE000E19C   /* ICER word 7 */
    .equ NVIC_ISPR0,    0xE000E200   /* ISPR word 0: IRQs 0-31 */
    .equ NVIC_ICPR0,    0xE000E280   /* ICPR word 0 */
    .equ NVIC_ISPR7,    0xE000E21C   /* ISPR word 7: IRQs 224-255 */
    .equ NVIC_ICPR7,    0xE000E29C   /* ICPR word 7 */
    .equ NVIC_IABR7,    0xE000E31C   /* IABR word 7 (read-only active) */
    .equ NVIC_IPR56,    0xE000E4E0   /* IPR word 56: priorities for IRQ 224-227 */
    .equ NVIC_IPR59,    0xE000E4EC   /* IPR word 59: priorities for IRQ 236-239 */

    /* Phase C execution sequence tracking (in SRAM) */
    .equ SEQ_BASE,      0x20000200   /* seq[0], seq[1], seq[2] */
    .equ SEQ_IDX,       0x20000210   /* next write index */

/* =========================================================================
 * Vector table — 256 entries to cover up to exception 255 (IRQ 239)
 *
 * Most entries are 0 (unused).  Only the entries we need have valid
 * handler addresses:
 *   [0]   = initial MSP
 *   [1]   = Reset_Handler
 *   [240] = IRQ 224 handler (exception 240 = 16 + 224)
 *   [255] = IRQ 239 handler (exception 255 = 16 + 239)
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl _vector_table
_vector_table:
    .word _estack               /* [0]   Initial MSP */
    .word Reset_Handler         /* [1]   Reset */
    .word 0                     /* [2]   NMI */
    .word 0                     /* [3]   HardFault */
    .word 0                     /* [4]   MemManage */
    .word 0                     /* [5]   BusFault */
    .word 0                     /* [6]   UsageFault */
    .word 0                     /* [7]   Reserved */
    .word 0                     /* [8]   Reserved */
    .word 0                     /* [9]   Reserved */
    .word 0                     /* [10]  Reserved */
    .word 0                     /* [11]  SVCall */
    .word 0                     /* [12]  Debug Monitor */
    .word 0                     /* [13]  Reserved */
    .word 0                     /* [14]  PendSV */
    .word 0                     /* [15]  SysTick */

    /* Entries [16]..[239] — external IRQs 0-223: unused, fill with 0 */
    .rept 224
    .word 0
    .endr

    /* Entry [240] = IRQ 224 handler */
    .word irq224_handler        /* [240] */

    /* Entries [241]..[254] — IRQs 225-238: unused */
    .rept 14
    .word 0
    .endr

    /* Entry [255] = IRQ 239 handler */
    .word irq239_handler        /* [255] */

/* =========================================================================
 * Macros
 * ========================================================================= */

/*
 * fail_test subtest_num, observed_val_reg
 *   Store FAIL result with subtest number and observed data, then exit.
 */
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
    /* ---- Initialize sequence tracking for Phase C ---- */
    ldr r0, =SEQ_BASE
    movs r1, #0
    str r1, [r0, #0]           /* seq[0] = 0 */
    str r1, [r0, #4]           /* seq[1] = 0 */
    str r1, [r0, #8]           /* seq[2] = 0 */
    ldr r0, =SEQ_IDX
    str r1, [r0]               /* seq_idx = 0 */

/* =====================================================================
 * Phase A — Corruption detection
 *
 * Writing to ISER[7] should NOT corrupt ISPR[0].  Before the fix,
 * nvicEnabled[7] is out-of-bounds and overlaps nvicPending[0] in
 * the struct layout, so a write to ISER[7] overwrites pending state.
 * ===================================================================== */

phase_a:
    /* -- Subtest 1: Set IRQ 0 pending, verify -- */
    ldr r0, =NVIC_ISPR0
    movs r1, #1                /* bit 0 = IRQ 0 */
    str r1, [r0]               /* write ISPR[0]: pend IRQ 0 */
    ldr r2, [r0]               /* read back ISPR[0] */
    tst r2, r1                 /* bit 0 still set? */
    beq 1f                     /* branch to fail if not set */
    b 2f
1:  fail_test 1, r2
2:

    /* -- Subtest 2: Write 0xFFFFFFFF to ISER[7] — the OOB trigger -- */
    ldr r0, =NVIC_ISER7
    ldr r1, =0xFFFFFFFF
    str r1, [r0]               /* write ISER[7] — OOB if arrays too small */

    /* -- Subtest 3: Read ISPR[0] — must be exactly 0x00000001 -- */
    /* Before fix: OOB write to nvicEnabled[7] corrupts nvicPending[0],
     * changing it from 0x00000001 to 0xFFFFFFFF.  A simple bit-test
     * (TST) would miss this because bit 0 is still set within 0xFFFFFFFF.
     * Use exact CMP instead to detect any corruption. */
    ldr r0, =NVIC_ISPR0
    ldr r2, [r0]               /* read ISPR[0] */
    cmp r2, #1                 /* must be exactly 0x00000001 */
    bne 1f
    b 2f
1:  fail_test 3, r2            /* corruption detected: nvicPending[0] was overwritten */
2:

    /* -- Subtest 4: Clean up — clear IRQ 0 pending -- */
    ldr r0, =NVIC_ICPR0
    movs r1, #1
    str r1, [r0]               /* write ICPR[0]: clear IRQ 0 pending */
    ldr r0, =NVIC_ISPR0
    ldr r2, [r0]               /* read back ISPR[0] */
    movs r3, #1
    tst r2, r3                 /* bit 0 should be clear now */
    bne 1f
    b 2f
1:  fail_test 4, r2
2:

    /* Also clean up ISER[7] from subtest 2 */
    ldr r0, =NVIC_ICER7
    ldr r1, =0xFFFFFFFF
    str r1, [r0]               /* disable all IRQs in word 7 */

/* =====================================================================
 * Phase B — High-IRQ register read/write correctness
 *
 * Verifies that ISER[7]/ISPR[7]/ICPR[7]/IABR[7] correctly read/write
 * bits for IRQs 224-255.  Meaningful with num_irqs=240.
 *
 * IMPORTANT: Each register type is tested independently to avoid
 * triggering actual interrupt delivery.  On a correctly working system,
 * enabling an IRQ (ISER) AND pending it (ISPR) simultaneously causes
 * immediate delivery — the pending bit transitions to active and the
 * handler runs.  So we:
 *   - Test ISER (enable/disable) without pending
 *   - Test ISPR/ICPR (pend/clear) without enabling
 *   - Test IABR (active) as read-only check
 * ===================================================================== */

phase_b:
    /* -- Subtest 5: Enable IRQ 224 via ISER[7], read back -- */
    ldr r0, =NVIC_ISER7
    movs r1, #1
    str r1, [r0]               /* write ISER[7]: enable IRQ 224 (not pending, no delivery) */

    /* -- Subtest 6: Read back ISER[7] — verify bit 0 set -- */
    ldr r2, [r0]               /* read ISER[7] */
    tst r2, r1                 /* bit 0 set? */
    beq 1f
    b 2f
1:  fail_test 6, r2
2:

    /* -- Subtest 7: Disable IRQ 224 via ICER[7], read back ISER[7] -- */
    ldr r0, =NVIC_ICER7
    movs r1, #1
    str r1, [r0]               /* write ICER[7]: disable IRQ 224 */
    ldr r0, =NVIC_ISER7
    ldr r2, [r0]               /* read ISER[7] */
    movs r3, #1
    tst r2, r3                 /* bit 0 should be clear */
    bne 1f
    b 2f
1:  fail_test 7, r2
2:

    /* -- Subtest 8: Pend IRQ 224 via ISPR[7] (NOT enabled, no delivery) -- */
    ldr r0, =NVIC_ISPR7
    movs r1, #1
    str r1, [r0]               /* write ISPR[7]: pend IRQ 224 (not enabled) */
    ldr r2, [r0]               /* read back ISPR[7] */
    movs r3, #1
    tst r2, r3                 /* bit 0 set? */
    beq 1f
    b 2f
1:  fail_test 8, r2
2:

    /* -- Subtest 9: Clear IRQ 224 pending via ICPR[7], verify cleared -- */
    ldr r0, =NVIC_ICPR7
    movs r1, #1
    str r1, [r0]               /* write ICPR[7]: clear IRQ 224 pending */
    ldr r0, =NVIC_ISPR7
    ldr r2, [r0]               /* read ISPR[7] */
    movs r3, #1
    tst r2, r3                 /* bit 0 should be clear */
    bne 1f
    b 2f
1:  fail_test 9, r2
2:

    /* -- Subtest 10: Read IABR[7] — should be 0 (nothing active) -- */
    ldr r0, =NVIC_IABR7
    ldr r2, [r0]
    cmp r2, #0
    bne 1f
    b 2f
1:  fail_test 10, r2
2:

/* =====================================================================
 * Phase C — Nested interrupt delivery for high IRQs
 *
 * Tests the full interrupt path: writeNVIC → updatePending →
 * activateIRQ → vector fetch → handler entry → preemption →
 * deactivateIRQ → exception return.
 *
 * IRQ 224 (exc 240): priority 0x80 (lower priority, runs first)
 * IRQ 239 (exc 255): priority 0x40 (higher priority, preempts)
 *
 * Expected sequence:
 *   seq[0] = 1  (IRQ 224 handler started)
 *   seq[1] = 2  (IRQ 239 handler ran — preempted IRQ 224)
 *   seq[2] = 3  (IRQ 224 handler completed after return from IRQ 239)
 * ===================================================================== */

phase_c:
    /* -- Set IRQ 224 priority to 0x80 -- */
    /* IPR56 at 0xE000E4E0 holds priorities for IRQs 224-227.
     * IRQ 224 is byte 0 of that word. */
    ldr r0, =NVIC_IPR56
    ldr r1, [r0]               /* read current IPR word */
    bic r1, r1, #0xFF          /* clear byte 0 (IRQ 224) */
    orr r1, r1, #0x80          /* set IRQ 224 priority = 0x80 */
    str r1, [r0]

    /* -- Set IRQ 239 priority to 0x40 -- */
    /* IPR59 at 0xE000E4EC holds priorities for IRQs 236-239.
     * IRQ 239 is byte 3 of that word. */
    ldr r0, =NVIC_IPR59
    ldr r1, [r0]               /* read current IPR word */
    ldr r2, =0xFF000000
    bic r1, r1, r2             /* clear byte 3 (IRQ 239) */
    ldr r2, =0x40000000
    orr r1, r1, r2             /* set IRQ 239 priority = 0x40 */
    str r1, [r0]

    /* -- Enable both IRQs (bits 0 and 15 of ISER[7]) -- */
    /* IRQ 224 = bit 0, IRQ 239 = bit 15 */
    ldr r0, =NVIC_ISER7
    ldr r1, =(1 << 0) | (1 << 15)
    str r1, [r0]               /* enable IRQ 224 and IRQ 239 */

    /* -- Pend IRQ 224 to start the chain -- */
    /* IRQ 239 will be pended from inside the IRQ 224 handler */
    ldr r0, =NVIC_ISPR7
    movs r1, #1                /* bit 0 = IRQ 224 */
    str r1, [r0]

    /* DSB + ISB to ensure the pend takes effect immediately */
    dsb
    isb

    /* -- After both handlers return, check the sequence -- */

    /* -- Subtest 11: seq[0] should be 1 (IRQ 224 started) -- */
    ldr r0, =SEQ_BASE
    ldr r2, [r0, #0]           /* seq[0] */
    cmp r2, #1
    bne 1f
    b 2f
1:  fail_test 11, r2
2:

    /* -- Subtest 12: seq[1] should be 2 (IRQ 239 preempted) -- */
    ldr r0, =SEQ_BASE
    ldr r2, [r0, #4]           /* seq[1] */
    cmp r2, #2
    bne 1f
    b 2f
1:  fail_test 12, r2
2:

    /* -- Subtest 13: seq[2] should be 3 (IRQ 224 completed) -- */
    ldr r0, =SEQ_BASE
    ldr r2, [r0, #8]           /* seq[2] */
    cmp r2, #3
    bne 1f
    b 2f
1:  fail_test 13, r2
2:

    /* -- Clean up: disable both IRQs -- */
    ldr r0, =NVIC_ICER7
    ldr r1, =(1 << 0) | (1 << 15)
    str r1, [r0]

/* =====================================================================
 * All subtests passed
 * ===================================================================== */
test_pass:
    ldr r0, =TEST_RESULT
    ldr r1, =PASS_VAL
    str r1, [r0]

test_exit:
    /* Exit via semihosting SYS_EXIT (BKPT #0xAB) */
    mov r0, #0x18              /* SYS_EXIT */
    ldr r1, =0x20026          /* ADP_Stopped_ApplicationExit */
    bkpt #0xAB

    /* Should not reach here; infinite loop as safety net */
    b .

/* =========================================================================
 * IRQ 224 handler (exception 240)
 *
 * Records seq[idx++] = 1 (started), then pends IRQ 239 to trigger
 * preemption, then after returning from the nested IRQ 239 handler,
 * records seq[idx++] = 3 (completed).
 * ========================================================================= */
    .thumb_func
    .globl irq224_handler
irq224_handler:
    push {r4-r7, lr}

    /* Record: seq[seq_idx] = 1 (IRQ 224 started) */
    ldr r4, =SEQ_BASE
    ldr r5, =SEQ_IDX
    ldr r6, [r5]               /* r6 = current seq_idx */
    movs r7, #1
    str r7, [r4, r6, lsl #2]   /* seq[seq_idx] = 1 */
    adds r6, r6, #1
    str r6, [r5]               /* seq_idx++ */

    /* Pend IRQ 239 (bit 15 of ISPR[7]) — should preempt us */
    ldr r0, =NVIC_ISPR7
    mov r1, #(1 << 15)
    str r1, [r0]
    dsb
    isb
    /* --- IRQ 239 handler runs here (preemption) --- */
    /* --- We resume here after IRQ 239 returns    --- */

    /* Record: seq[seq_idx] = 3 (IRQ 224 completed after preemption) */
    ldr r6, [r5]               /* re-read seq_idx (IRQ 239 incremented it) */
    movs r7, #3
    str r7, [r4, r6, lsl #2]   /* seq[seq_idx] = 3 */
    adds r6, r6, #1
    str r6, [r5]               /* seq_idx++ */

    pop {r4-r7, pc}            /* exception return via POP {PC} */

/* =========================================================================
 * IRQ 239 handler (exception 255)
 *
 * Records seq[idx++] = 2 (nested handler ran).
 * This is the highest-numbered IRQ in the architecture (last entry
 * in all NVIC arrays and the last slot in the vector table).
 * ========================================================================= */
    .thumb_func
    .globl irq239_handler
irq239_handler:
    push {r4-r7, lr}

    /* Record: seq[seq_idx] = 2 (IRQ 239 ran, preempting IRQ 224) */
    ldr r4, =SEQ_BASE
    ldr r5, =SEQ_IDX
    ldr r6, [r5]               /* r6 = current seq_idx */
    movs r7, #2
    str r7, [r4, r6, lsl #2]   /* seq[seq_idx] = 2 */
    adds r6, r6, #1
    str r6, [r5]               /* seq_idx++ */

    pop {r4-r7, pc}            /* exception return via POP {PC} */
