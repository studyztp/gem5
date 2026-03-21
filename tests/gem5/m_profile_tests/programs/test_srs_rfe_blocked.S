/*
 * Test: SRS/RFE must be blocked on M-profile (Bugs 8–10)
 *
 * Bugs 8-10: The M-profile decoder extracted wrong bit positions for
 * HTOPCODE_10_9 (bits(inst,25,24) instead of bits(inst,26,25)),
 * HTOPCODE_6 (bits(inst,21) instead of bits(inst,22)), and
 * HTOPCODE_8_7 (bits(inst,23,22) instead of bits(inst,24,23)).
 * This caused SRS/RFE instructions to not be intercepted.
 *
 * DDI0403D Table A5-9: op1=01, op2=00xx0xx → Load/Store Multiple.
 * SRS and RFE fall in this space with HTOPCODE_8_7={0,3} and
 * HTOPCODE_6=0.  They are UNDEFINED on M-profile.
 *
 * Test plan:
 *   Subtest 1: SRS DB sp! (raw encoding) → UsageFault
 *   Subtest 2: RFE-like (raw encoding, HTOPCODE_8_7=3) → UsageFault
 *
 * Result at 0x20000100:
 *   0xCAFECAFE = all tests passed
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,  0x20000100
    .equ TEST_SUBTEST, 0x20000104
    .equ TEST_SCRATCH, 0x20000108
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

    /* --- Reset Handler --- */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* Enable UsageFault in SHCSR */
    ldr     r0, =0xE000ED24
    ldr     r1, [r0]
    orr     r1, r1, #(1 << 18)
    str     r1, [r0]

    /* Initialize fault counter to 0 */
    ldr     r4, =TEST_SCRATCH
    mov     r0, #0
    str     r0, [r4]

    /* --- Subtest 1: SRS DB sp! (A-profile only → UsageFault) ---
     *
     * SRS DB encoding (HTOPCODE_8_7=0):
     *   hw1 = 0xE80D: 1110_1000_0000_1101
     *     hw1[12:11]=01, hw1[10:9]=00, hw1[8:7]=00, hw1[6]=0, Rn=SP
     *   hw2 = 0xC000: 1100_0000_0000_0000
     *
     * Matches decode path: HTOPCODE_12_11=1, HTOPCODE_10_9=0,
     * HTOPCODE_6=0, HTOPCODE_8_7=0 → SRS/RFE
     */
    .short 0xE80D, 0xC000      /* SRS DB sp!, #0 (raw Thumb-32) */
after_srs:

    /* --- Subtest 2: RFE-like (HTOPCODE_8_7=3 → also SRS/RFE) ---
     *
     * RFE-like encoding (HTOPCODE_8_7=3):
     *   hw1 = 0xE980: 1110_1001_1000_0000
     *     hw1[12:11]=01, hw1[10:9]=00, hw1[8:7]=11, hw1[6]=0, Rn=R0
     *   hw2 = 0xC000
     */
    .short 0xE980, 0xC000      /* RFE-like (raw Thumb-32) */
after_rfe:

    /* --- Verify: fault counter should be 2 --- */
    ldr     r0, [r4]
    cmp     r0, #2
    bne     fail

    /* All passed */
    ldr     r0, =PASS_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    b       done

fail:
    /* r0 has the actual fault count */
    mov     r2, r0
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]            /* store fault count as subtest ID */
done:
    /* Exit via ARM semihosting SYS_EXIT */
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .

    /* --- UsageFault Handler ---
     * Increment fault counter and skip the 4-byte faulting instruction.
     */
    .thumb_func
UsageFault_Handler:
    /* Increment fault counter */
    ldr     r0, =TEST_SCRATCH
    ldr     r1, [r0]
    add     r1, r1, #1
    str     r1, [r0]

    /* Advance stacked PC past the 4-byte instruction */
    ldr     r0, [sp, #0x18]     /* stacked PC */
    add     r0, r0, #4          /* skip 4-byte Thumb-32 */
    str     r0, [sp, #0x18]
    bx      lr

    /* --- HardFault Handler --- */
    .thumb_func
HardFault_Handler:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    mov     r2, #99
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .

    .thumb_func
default_handler:
    b       .
