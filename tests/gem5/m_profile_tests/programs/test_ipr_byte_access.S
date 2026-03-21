/*
 * test_ipr_byte_access.S — NVIC IPR byte-level access (Bug 18 check)
 *
 * Verifies that NVIC Interrupt Priority Registers (IPR) support
 * byte-level reads and writes, as required by DDI0403E B3.4.9.
 *
 * Each byte at 0xE000E400 + n holds the priority for external IRQ n.
 * CMSIS HAL code commonly does:
 *   NVIC->IP[irq] = priority;   // STRB to 0xE000E400 + irq
 *
 * With Bug 18, the SCS device always treats every access as 32-bit:
 *   - read()  calls pkt->setLE<uint32_t>() regardless of packet size
 *   - write() calls pkt->getLE<uint32_t>() regardless of packet size
 *   - readNVIC()/writeNVIC() round offset down to 4-byte boundary
 *
 * A STRB to 0xE000E401 (IRQ1) is misinterpreted as a 32-bit write
 * to 0xE000E400, corrupting all four IRQs (0-3) in that word.
 *
 * -------------------------------------------------------------------------
 * Test design:
 *
 *   Phase 1: Baseline — 32-bit word write to IPR0 (should always work)
 *     Write 0x00000000 to 0xE000E400 (clear all 4 priorities)
 *     Read back as word — verify == 0x00000000
 *
 *   Phase 2: Byte write to IRQ1 priority
 *     STRB 0x80 to 0xE000E401 (set IRQ1 priority to 0x80)
 *     Read back as word from 0xE000E400
 *     Expected correct: 0x00008000 (only byte[1] = 0x80)
 *     Expected buggy:   something else (all bytes corrupted)
 *
 *   Phase 3: Verify adjacent priorities unchanged
 *     LDRB from 0xE000E400 (IRQ0) — should be 0x00
 *     LDRB from 0xE000E402 (IRQ2) — should be 0x00
 *     LDRB from 0xE000E403 (IRQ3) — should be 0x00
 *
 *   Phase 4: Second byte write to IRQ3 priority
 *     STRB 0x40 to 0xE000E403
 *     Read back as word — expected 0x40008000
 *
 *   Phase 5: Byte read-back of individual priorities
 *     LDRB from 0xE000E400 (IRQ0) — expected 0x00
 *     LDRB from 0xE000E401 (IRQ1) — expected 0x80
 *     LDRB from 0xE000E402 (IRQ2) — expected 0x00
 *     LDRB from 0xE000E403 (IRQ3) — expected 0x40
 *
 *   Phase 6: Word write then byte read-back
 *     Write 0x20406080 to 0xE000E400
 *     LDRB at +0 — expected 0x80 (IRQ0)
 *     LDRB at +1 — expected 0x60 (IRQ1)
 *     LDRB at +2 — expected 0x40 (IRQ2)
 *     LDRB at +3 — expected 0x20 (IRQ3)
 *
 * -------------------------------------------------------------------------
 * NVIC IPR layout (DDI0403E B3.4.9):
 *   0xE000E400  IPR0:  [IRQ3_PRI | IRQ2_PRI | IRQ1_PRI | IRQ0_PRI]
 *                       byte[3]    byte[2]    byte[1]    byte[0]
 *
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000108  TEST_DATA    — stores observed IPR word value for debugging
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    /* NVIC IPR base */
    .equ NVIC_IPR_BASE, 0xE000E400

/* =========================================================================
 * Vector table
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000          /* Initial MSP */
    .word   Reset_Handler       /* Reset */
    .fill   14, 4, 0            /* Entries 2-15: unused */

/* =========================================================================
 * Reset_Handler — main test body
 * ========================================================================= */
    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* r6 = TEST_RESULT address, r7 = TEST_SUBTEST address */
    ldr     r6, =TEST_RESULT
    ldr     r7, =TEST_SUBTEST

    /* r4 = NVIC IPR base address */
    ldr     r4, =NVIC_IPR_BASE

    /* ------------------------------------------------------------------
     * Phase 1: Baseline — clear all 4 priorities with a 32-bit word write
     * ------------------------------------------------------------------ */
    mov     r0, #0
    str     r0, [r4]            /* IPR0 word = 0x00000000 */

    /* Read back as word to confirm baseline */
    ldr     r1, [r4]
    cmp     r1, #0
    bne     fail_1

    /* ------------------------------------------------------------------
     * Phase 2: Byte write to IRQ1 priority
     *
     * STRB 0x80 to 0xE000E401  (only IRQ1's priority byte)
     *
     * With Bug 18: the SCS device reads pkt->getLE<uint32_t>() from
     * a 1-byte packet (UB/garbage), then writes all 4 IRQ priorities
     * from that garbage value.  The word read-back will NOT be 0x00008000.
     * ------------------------------------------------------------------ */
    mov     r0, #0x80
    strb    r0, [r4, #1]        /* byte write to IRQ1 */

    /* Read back as word.
     * Expected correct: 0x00008000 (only byte[1] set to 0x80)
     * Save to TEST_DATA for debugging regardless of pass/fail */
    ldr     r1, [r4]
    ldr     r5, =TEST_DATA
    str     r1, [r5]            /* save observed value */

    ldr     r2, =0x00008000
    cmp     r1, r2
    bne     fail_2

    /* ------------------------------------------------------------------
     * Phase 3: Verify adjacent priorities unchanged
     * ------------------------------------------------------------------ */

    /* Subtest 3: IRQ0 priority should still be 0 */
    ldrb    r0, [r4, #0]
    cmp     r0, #0
    bne     fail_3

    /* Subtest 4: IRQ2 priority should still be 0 */
    ldrb    r0, [r4, #2]
    cmp     r0, #0
    bne     fail_4

    /* Subtest 5: IRQ3 priority should still be 0 */
    ldrb    r0, [r4, #3]
    cmp     r0, #0
    bne     fail_5

    /* ------------------------------------------------------------------
     * Phase 4: Second byte write — set IRQ3 priority to 0x40
     * ------------------------------------------------------------------ */
    mov     r0, #0x40
    strb    r0, [r4, #3]        /* byte write to IRQ3 */

    /* Read back as word.
     * Expected: 0x40008000 (IRQ0=0, IRQ1=0x80, IRQ2=0, IRQ3=0x40) */
    ldr     r1, [r4]
    ldr     r2, =0x40008000
    cmp     r1, r2
    bne     fail_6

    /* ------------------------------------------------------------------
     * Phase 5: Byte read-back of each individual priority
     * ------------------------------------------------------------------ */

    /* Subtest 7: IRQ0 = 0x00 */
    ldrb    r0, [r4, #0]
    cmp     r0, #0x00
    bne     fail_7

    /* Subtest 8: IRQ1 = 0x80 */
    ldrb    r0, [r4, #1]
    cmp     r0, #0x80
    bne     fail_8

    /* Subtest 9: IRQ2 = 0x00 */
    ldrb    r0, [r4, #2]
    cmp     r0, #0x00
    bne     fail_9

    /* Subtest 10: IRQ3 = 0x40 */
    ldrb    r0, [r4, #3]
    cmp     r0, #0x40
    bne     fail_10

    /* ------------------------------------------------------------------
     * Phase 6: Word write then byte read-back
     *
     * Write 0x20406080 as a 32-bit word:
     *   byte[0] = 0x80 (IRQ0), byte[1] = 0x60 (IRQ1),
     *   byte[2] = 0x40 (IRQ2), byte[3] = 0x20 (IRQ3)
     * Then read each back individually with LDRB.
     * ------------------------------------------------------------------ */
    ldr     r0, =0x20406080
    str     r0, [r4]

    /* Subtest 11: IRQ0 byte read = 0x80 */
    ldrb    r0, [r4, #0]
    cmp     r0, #0x80
    bne     fail_11

    /* Subtest 12: IRQ1 byte read = 0x60 */
    ldrb    r0, [r4, #1]
    cmp     r0, #0x60
    bne     fail_12

    /* Subtest 13: IRQ2 byte read = 0x40 */
    ldrb    r0, [r4, #2]
    cmp     r0, #0x40
    bne     fail_13

    /* Subtest 14: IRQ3 byte read = 0x20 */
    ldrb    r0, [r4, #3]
    cmp     r0, #0x20
    bne     fail_14

    /* ------------------------------------------------------------------
     * All subtests passed
     * ------------------------------------------------------------------ */
    ldr     r0, =PASS_VAL
    str     r0, [r6]
    b       test_exit

    /* Failure paths */
fail_1:  mov r0, #1
    b       fail_common
fail_2:  mov r0, #2
    b       fail_common
fail_3:  mov r0, #3
    b       fail_common
fail_4:  mov r0, #4
    b       fail_common
fail_5:  mov r0, #5
    b       fail_common
fail_6:  mov r0, #6
    b       fail_common
fail_7:  mov r0, #7
    b       fail_common
fail_8:  mov r0, #8
    b       fail_common
fail_9:  mov r0, #9
    b       fail_common
fail_10: mov r0, #10
    b       fail_common
fail_11: mov r0, #11
    b       fail_common
fail_12: mov r0, #12
    b       fail_common
fail_13: mov r0, #13
    b       fail_common
fail_14: mov r0, #14

fail_common:
    str     r0, [r7]            /* TEST_SUBTEST = failing subtest */
    ldr     r0, =FAIL_VAL
    str     r0, [r6]            /* TEST_RESULT = FAIL */

test_exit:
    /* Semihosting exit */
    mov     r0, #0x18           /* SYS_EXIT */
    ldr     r1, =0x20026        /* ADP_Stopped_ApplicationExit */
    bkpt    #0xAB

    .end
