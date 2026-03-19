/*
 * Test: BX with EXC_RETURN detection
 *
 * Tests:
 *   1. Normal BX to a function (should branch normally)
 *   2. SVC triggers SVCall exception
 *   3. SVCall handler returns via BX LR (EXC_RETURN 0xFFFFFFF9)
 *   4. Verify execution continues after SVC
 *
 * This tests the critical EXC_RETURN detection in BxMProfile.
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
    .word   SVCall_Handler      /* SVCall */
    .word   0                   /* Debug Monitor */
    .word   0                   /* Reserved */
    .word   default_handler     /* PendSV */
    .word   default_handler     /* SysTick */

    .section .text

    /* --- Simple function for testing normal BX --- */
    .thumb_func
add_one:
    add     r0, r0, #1
    bx      lr

    /* --- Reset Handler --- */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* --- Test 1: Normal BX (not EXC_RETURN) --- */
    mov     r0, #10
    ldr     r1, =add_one
    blx     r1              /* call add_one via BLX reg */
    cmp     r0, #11         /* should be 10+1=11 */
    bne     fail_1

    /* --- Test 2: SVC → SVCall handler → BX LR (EXC_RETURN) --- */
    /* The SVCall handler will:
     *   1. Write 0x42 to 0x20000104 (proof it ran)
     *   2. BX LR (EXC_RETURN 0xFFFFFFF9 → return to Thread/MSP)
     * After return, execution continues here. */
    mov     r0, #0
    ldr     r4, =TEST_SUBTEST
    str     r0, [r4]        /* clear marker */
    svc     #0              /* trigger SVCall */
    /* If we reach here, EXC_RETURN worked */
    ldr     r0, [r4]        /* read marker from handler */
    cmp     r0, #0x42
    bne     fail_2

    /* --- Test 3: Normal BX after exception return --- */
    /* Verify BX still works normally after an EXC_RETURN */
    mov     r0, #20
    ldr     r1, =add_one
    blx     r1
    cmp     r0, #21
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
fail:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
done:
    b       done

    /* --- SVCall Handler --- */
    .thumb_func
SVCall_Handler:
    /* Write marker to prove handler ran */
    ldr     r0, =TEST_SUBTEST
    mov     r1, #0x42
    str     r1, [r0]
    /* Return via EXC_RETURN in LR.
     * LR was set to 0xFFFFFFF9 by hardware on exception entry
     * (Thread mode, MSP). BX LR triggers mProfileExcReturn(). */
    bx      lr

    .thumb_func
default_handler:
    b       .
