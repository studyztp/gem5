/*
 * Test: Exclusive monitor cache-line granularity (Bug 13)
 *
 * Bug 13: MISA::handleLockedWrite() compares exact addresses
 * (lockedAddr == req->getPaddr()) instead of cache-line-masked
 * addresses ((lockedAddr & mask) == (req->getPaddr() & mask)).
 *
 * Per DDI0403E A3.4.5: "The size of the [exclusive monitor]
 * marked block is IMPLEMENTATION DEFINED, between one word
 * and 2^10 words."  gem5 uses cache-line granularity.
 *
 * Test plan:
 *   Subtest 1: LDREX/STREX same address → succeeds (baseline)
 *   Subtest 2: LDREX offset 0, STREX offset 4 (same cache line) → succeeds
 *   Subtest 3: LDREX offset 0, STREX offset 64 (different cache line) → fails
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

    /* 128-byte aligned buffer in SRAM for exclusive access tests.
     * Must be aligned to at least the cache line size (64 bytes). */
    .equ EXCL_BUF,     0x20000200  /* 0x200 is 128-byte aligned */

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
    .word   default_handler     /* SVCall */
    .word   0                   /* Debug Monitor */
    .word   0                   /* Reserved */
    .word   default_handler     /* PendSV */
    .word   default_handler     /* SysTick */

    .section .text

    /* --- Reset Handler --- */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* Initialize buffer words to known values */
    ldr     r4, =EXCL_BUF
    mov     r0, #0xAA
    str     r0, [r4, #0]        /* offset 0 */
    mov     r0, #0xBB
    str     r0, [r4, #4]        /* offset 4 */
    mov     r0, #0xCC
    str     r0, [r4, #64]       /* offset 64 (next cache line) */

    /* --- Subtest 1: Same-address LDREX/STREX (baseline) ---
     * LDREX r0, [r4]    ; load exclusive from offset 0
     * STREX r1, r0, [r4] ; store exclusive to offset 0
     * r1 should be 0 (success)
     */
    ldrex   r0, [r4]
    strex   r1, r0, [r4]
    cmp     r1, #0
    bne     fail_1

    /* --- Subtest 2: Same cache line, different offset ---
     * LDREX from offset 0, STREX to offset 4.
     * Both are within the same 64-byte cache line.
     * With Bug 13: STREX fails (r1=1) because exact addresses differ.
     * After fix: STREX succeeds (r1=0) because cache lines match.
     */
    ldrex   r0, [r4]            /* exclusive load from offset 0 */
    mov     r0, #0xDD           /* new value to store */
    strex   r1, r0, [r4, #4]   /* exclusive store to offset 4 */
    cmp     r1, #0              /* should succeed (same cache line) */
    bne     fail_2

    /* Verify the value was actually written */
    ldr     r0, [r4, #4]
    cmp     r0, #0xDD
    bne     fail_2

    /* --- Subtest 3: Different cache line → STREX must fail ---
     * LDREX from offset 0 (cache line 0), STREX to offset 64 (cache line 1).
     * STREX should fail (r1=1) because different cache lines.
     */
    ldrex   r0, [r4]            /* exclusive load from offset 0 */
    mov     r0, #0xEE           /* value to try to store */
    strex   r1, r0, [r4, #64]  /* exclusive store to offset 64 */
    cmp     r1, #1              /* should fail (different cache line) */
    bne     fail_3

    /* Verify offset 64 was NOT modified (STREX failed) */
    ldr     r0, [r4, #64]
    cmp     r0, #0xCC           /* should still be original value */
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
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .

    .thumb_func
default_handler:
    b       .
