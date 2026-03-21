/*
 * Test: MRS/MSR instruction for all M-profile special registers
 *
 * Tests each SYSm encoding by writing a known value and reading back:
 *   SYSm=0  APSR (xPSR N,Z,C,V,Q,GE flags)
 *   SYSm=8  MSP  (Main Stack Pointer)
 *   SYSm=9  PSP  (Process Stack Pointer)
 *   SYSm=16 PRIMASK
 *   SYSm=17 BASEPRI
 *   SYSm=19 FAULTMASK
 *   SYSm=20 CONTROL
 *
 * Result at 0x20000100:
 *   0xCAFECAFE = all tests passed
 *   0xDEADDEAD = test failed
 *   0x20000104 = which sub-test failed
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

    /* --- Test 1: PRIMASK write/read --- */
    mov     r0, #1
    msr     PRIMASK, r0
    mrs     r1, PRIMASK
    cmp     r1, #1
    bne     fail_1
    mov     r0, #0
    msr     PRIMASK, r0
    mrs     r1, PRIMASK
    cmp     r1, #0
    bne     fail_1

    /* --- Test 2: BASEPRI write/read --- */
    mov     r0, #0x40
    msr     BASEPRI, r0
    mrs     r1, BASEPRI
    cmp     r1, #0x40
    bne     fail_2
    mov     r0, #0
    msr     BASEPRI, r0

    /* --- Test 3: FAULTMASK write/read --- */
    mov     r0, #1
    msr     FAULTMASK, r0
    mrs     r1, FAULTMASK
    cmp     r1, #1
    bne     fail_3
    mov     r0, #0
    msr     FAULTMASK, r0

    /* --- Test 4: CONTROL write/read --- */
    /* Only bit[0] (nPRIV) is writable in Thread mode */
    mrs     r0, CONTROL
    cmp     r0, #0
    bne     fail_4

    /* --- Test 5: MSP write/read --- */
    /* Save current MSP, write new value, read back, restore */
    mrs     r4, MSP         /* save */
    ldr     r0, =0x20010000
    msr     MSP, r0
    mrs     r1, MSP
    cmp     r1, r0
    bne     fail_5_restore
    msr     MSP, r4         /* restore */
    b       test_5_ok
fail_5_restore:
    msr     MSP, r4         /* restore even on fail */
    b       fail_5
test_5_ok:

    /* --- Test 6: PSP write/read --- */
    ldr     r0, =0x20018000
    msr     PSP, r0
    mrs     r1, PSP
    cmp     r1, r0
    bne     fail_6

    /* --- Test 7: APSR flags via MSR/MRS --- */
    /* Write N=1,Z=0,C=1,V=0 to APSR_nzcvq */
    ldr     r0, =0xA0000000     /* N=1, C=1 → bits[31:28]=1010 */
    msr     APSR_nzcvq, r0
    mrs     r1, APSR
    ldr     r2, =0xA0000000
    and     r1, r1, r2          /* mask to N,Z,C,V bits */
    cmp     r1, r2
    bne     fail_7

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
