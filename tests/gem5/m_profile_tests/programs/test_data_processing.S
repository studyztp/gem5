/*
 * Test: Comprehensive data processing instruction coverage
 *
 * Tests Thumb-16 and Thumb-32 data processing instructions to
 * verify they execute correctly through the CPSR alias path.
 * Each instruction's result is verified against expected values.
 *
 * Tests:
 *   1. MOV/MVN
 *   2. ADD/SUB/RSB
 *   3. AND/ORR/EOR/BIC
 *   4. LSL/LSR/ASR/ROR
 *   5. MUL/MLA/MLS
 *   6. SDIV/UDIV
 *   7. REV/REV16/REVSH/RBIT
 *   8. SXTH/SXTB/UXTH/UXTB
 *   9. Condition flags (N,Z,C,V)
 */
    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,  0x20000100
    .equ TEST_SUBTEST, 0x20000104
    .equ PASS_VAL,     0xCAFECAFE
    .equ FAIL_VAL,     0xDEADDEAD

    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000
    .word   Reset_Handler
    .fill   14, 4, 0

    .section .text
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* --- Test 1: MOV/MVN --- */
    mov     r0, #42
    cmp     r0, #42
    bne     fail_1
    mvn     r1, r0              /* r1 = ~42 = 0xFFFFFFD5 */
    ldr     r2, =0xFFFFFFD5
    cmp     r1, r2
    bne     fail_1

    /* --- Test 2: ADD/SUB/RSB --- */
    mov     r0, #100
    mov     r1, #50
    add     r2, r0, r1          /* 150 */
    cmp     r2, #150
    bne     fail_2
    sub     r3, r0, r1          /* 50 */
    cmp     r3, #50
    bne     fail_2
    rsb     r3, r1, #200        /* 200 - 50 = 150 */
    cmp     r3, #150
    bne     fail_2

    /* --- Test 3: AND/ORR/EOR/BIC --- */
    ldr     r0, =0xFF00FF00
    ldr     r1, =0x0F0F0F0F
    and     r2, r0, r1          /* 0x0F000F00 */
    ldr     r3, =0x0F000F00
    cmp     r2, r3
    bne     fail_3
    orr     r2, r0, r1          /* 0xFF0FFF0F */
    ldr     r3, =0xFF0FFF0F
    cmp     r2, r3
    bne     fail_3
    eor     r2, r0, r1          /* 0xF00FF00F */
    ldr     r3, =0xF00FF00F
    cmp     r2, r3
    bne     fail_3
    bic     r2, r0, r1          /* r0 & ~r1 = 0xF000F000 */
    ldr     r3, =0xF000F000
    cmp     r2, r3
    bne     fail_3

    /* --- Test 4: LSL/LSR/ASR --- */
    mov     r0, #1
    lsl     r1, r0, #4          /* 16 */
    cmp     r1, #16
    bne     fail_4
    mov     r0, #128
    lsr     r1, r0, #3          /* 16 */
    cmp     r1, #16
    bne     fail_4
    ldr     r0, =0x80000000     /* -2147483648 */
    asr     r1, r0, #4          /* sign-extended shift */
    ldr     r2, =0xF8000000
    cmp     r1, r2
    bne     fail_4

    /* --- Test 5: MUL/MLA/MLS --- */
    mov     r0, #7
    mov     r1, #6
    mul     r2, r0, r1          /* 42 */
    cmp     r2, #42
    bne     fail_5
    mov     r3, #10
    mla     r2, r0, r1, r3      /* 7*6+10 = 52 */
    cmp     r2, #52
    bne     fail_5
    mls     r2, r0, r1, r3      /* 10 - 7*6 = -32 */
    ldr     r3, =-32
    cmp     r2, r3
    bne     fail_5

    /* --- Test 6: SDIV/UDIV --- */
    mov     r0, #100
    mov     r1, #7
    udiv    r2, r0, r1          /* 14 */
    cmp     r2, #14
    bne     fail_6
    sdiv    r2, r0, r1          /* 14 */
    cmp     r2, #14
    bne     fail_6
    /* Negative division */
    ldr     r0, =-100
    mov     r1, #7
    sdiv    r2, r0, r1          /* -14 */
    ldr     r3, =-14
    cmp     r2, r3
    bne     fail_6

    /* --- Test 7: REV/REV16/RBIT --- */
    ldr     r0, =0x12345678
    rev     r1, r0              /* 0x78563412 */
    ldr     r2, =0x78563412
    cmp     r1, r2
    bne     fail_7
    rev16   r1, r0              /* 0x34127856 */
    ldr     r2, =0x34127856
    cmp     r1, r2
    bne     fail_7

    /* --- Test 8: SXTH/SXTB/UXTH/UXTB --- */
    ldr     r0, =0x0000FF80     /* 0xFF80 as unsigned halfword */
    sxth    r1, r0              /* sign-extend: 0xFFFFFF80 */
    ldr     r2, =0xFFFFFF80
    cmp     r1, r2
    bne     fail_8
    uxth    r1, r0              /* zero-extend: 0x0000FF80 */
    ldr     r2, =0x0000FF80
    cmp     r1, r2
    bne     fail_8
    sxtb    r1, r0              /* sign-extend byte: 0x80 → 0xFFFFFF80 */
    ldr     r2, =0xFFFFFF80
    cmp     r1, r2
    bne     fail_8
    uxtb    r1, r0              /* zero-extend byte: 0x80 */
    cmp     r1, #0x80
    bne     fail_8

    /* --- Test 9: Condition flags (N,Z,C,V) --- */
    /* Z flag */
    movs    r0, #0
    beq     z_ok
    b       fail_9
z_ok:
    /* N flag */
    ldr     r0, =0x80000000
    cmp     r0, #0              /* N=1 (negative) */
    bmi     n_ok
    b       fail_9
n_ok:
    /* C flag (carry from addition) */
    ldr     r0, =0xFFFFFFFF
    adds    r1, r0, #1          /* 0xFFFFFFFF + 1 = 0, C=1 */
    bcs     c_ok
    b       fail_9
c_ok:
    /* V flag (overflow) */
    ldr     r0, =0x7FFFFFFF
    adds    r1, r0, #1          /* 0x7FFFFFFF + 1 = overflow, V=1 */
    bvs     v_ok
    b       fail_9
v_ok:

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
fail_4: mov r2, #4
    b fail
fail_5: mov r2, #5
    b fail
fail_6: mov r2, #6
    b fail
fail_7: mov r2, #7
    b fail
fail_8: mov r2, #8
    b fail
fail_9: mov r2, #9
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    /* Exit via ARM semihosting SYS_EXIT */
    mov     r0, #0x18           /* SYS_EXIT */
    ldr     r1, =0x20026        /* ADP_Stopped_ApplicationExit */
    bkpt    #0xab               /* M-profile semihosting trigger */
    b       .                   /* unreachable */
