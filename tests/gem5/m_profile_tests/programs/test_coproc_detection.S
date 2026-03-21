/*
 * Test: Coprocessor detection accuracy (Bugs 11–12)
 *
 * Bug 11: Coprocessor detection too broad for HTOPCODE_12_11==3.
 *   The old code falsely classified non-coprocessor instructions
 *   (with hw1[9:8] in {0,1}) as coprocessor due to wrong bit
 *   extraction and an extra overly-broad check.
 *
 * Bug 12: Coprocessor detection missed CP15 for HTOPCODE_12_11==1.
 *   Reading hw1[9:8] instead of hw1[10:9] caused MRC/MCR CP15
 *   instructions to slip through undetected.
 *
 * DDI0403D Table A5-9: Coprocessor instructions require op2[6]=1
 * (hw1[10]=1, HTOPCODE_10_9 >= 2) for both op1=01 and op1=11.
 *
 * Test plan:
 *   Subtest 1: STR.W with ltcoproc=0xF NOT falsely blocked (Bug 11)
 *   Subtest 2: MRC p15 IS blocked as UsageFault (Bug 12)
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

    /* --- Subtest 1: STR.W with ltcoproc=0xF must NOT be blocked ---
     *
     * str.w r0, [r1, #0xF00] has:
     *   hw1 = 0xF8C1: HTOPCODE_12_11=3, HTOPCODE_10_9=0
     *   hw2 = 0x0F00: bits[11:8]=0xF (ltcoproc field)
     *
     * With Bug 11: hw1[9:8]=3 → is_coproc=true → ltcoproc=0xF
     *   → MProfileUndefined("MRC/MCR CP15") → UsageFault!
     * After fix: HTOPCODE_10_9=0 < 2 → not coprocessor → executes OK
     *
     * We set R1 to point to a scratch buffer so the store is valid.
     */
    ldr     r1, =0x20000200     /* scratch buffer in SRAM */
    mov     r0, #0x42
    str.w   r0, [r1, #0xF00]   /* this should execute normally */
    /* If we got here without a fault, subtest 1 passed.
     * Verify the store actually wrote the value. */
    ldr     r2, =0x20001100     /* 0x20000200 + 0xF00 = 0x20001100 */
    ldr     r0, [r2]
    cmp     r0, #0x42
    bne     fail_1

    /* --- Subtest 2: MRC p15 IS blocked → UsageFault ---
     *
     * MRC p15, 0, R0, c0, c0, 0 (read MIDR — A-profile only):
     *   hw1 = 0xEE10: HTOPCODE_12_11=1, HTOPCODE_10_9=3
     *   hw2 = 0x0F10: ltcoproc=0xF (CP15)
     *
     * With Bug 12: old code read hw1[9:8]=0x1 → not >= 2
     *   → is_coproc=false → MRC slips through → undefined behavior
     * After fix: HTOPCODE_10_9=3 >= 2 → is_coproc=true
     *   → ltcoproc=0xF → MProfileUndefined("MRC/MCR CP15") → UsageFault
     */
    .short 0xEE10, 0x0F10      /* MRC p15, 0, R0, c0, c0, 0 */
after_mrc:

    /* Verify fault counter = 1 (only MRC faulted, not STR.W) */
    ldr     r0, [r4]
    cmp     r0, #1
    bne     fail_2

    /* All passed */
    ldr     r0, =PASS_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    b       done

fail_1: mov r2, #1
    b fail
fail_2: mov r2, #2
    b fail
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    mov     r0, #0x18
    ldr     r1, =0x20026
    bkpt    #0xab
    b       .

    /* --- UsageFault Handler ---
     * Increment fault counter and skip the 4-byte instruction.
     */
    .thumb_func
UsageFault_Handler:
    ldr     r0, =TEST_SCRATCH
    ldr     r1, [r0]
    add     r1, r1, #1
    str     r1, [r0]
    ldr     r0, [sp, #0x18]
    add     r0, r0, #4
    str     r0, [sp, #0x18]
    bx      lr

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
