/*
 * test_psp_exception.S — PSP/MSP switching during exceptions
 *
 * Tests Bugs 1–4, all in the M-profile exception entry/return path:
 *
 *   Bug 1 (ArmMFault::invoke): Exception entry from Thread/PSP does not
 *           switch R13 to MSP.  Handler starts with R13 = PSP-frame address
 *           instead of MSP.
 *
 *   Bug 2 (ArmMFault::invoke): CONTROL.SPSEL not cleared to 0 on exception
 *           entry.  Handler sees SPSEL=1 (PSP) instead of SPSEL=0 (MSP).
 *
 *   Bug 3 (mProfileExcReturn): Exception return reads the exception frame
 *           from R13 (which is MSP in the handler when bugs 1+2 are fixed)
 *           instead of PSP.  Restores garbage values.
 *
 *   Bug 4 (mProfileExcReturn): CONTROL.SPSEL not restored from EXC_RETURN
 *           bit[2] after exception return.  Thread mode stays on MSP.
 *
 * -------------------------------------------------------------------------
 * Test algorithm:
 *
 *   Phase 1: Setup
 *     1. MSR PSP = PSP_BASE (0x20010000) with SPSEL=0 — updates misc reg only
 *     2. MSR CONTROL = 0b10 (SPSEL=1), ISB
 *     3. MSR PSP = PSP_BASE again — now SPSEL=1 so MSR PSP also updates R13
 *     4. Subtest 1: verify R13 == PSP_BASE
 *
 *   Phase 2: SVC (exception entry, Tests Bugs 1 and 2)
 *     5. SVC #0
 *     In SVCall_Handler (no PUSH/POP — handler records state to TEST_DATA):
 *       a. STR SP  → TEST_DATA[0]   (raw handler SP)
 *       b. MRS CONTROL → TEST_DATA[4]
 *       c. STR LR  → TEST_DATA[8]   (EXC_RETURN value set by hardware)
 *       d. BX LR
 *
 *   Phase 3: Check handler observations (Tests Bugs 1 and 2)
 *     Subtest 2: TEST_DATA[0] == MSP  (Bug 1: equals PSP-0x20 instead)
 *     Subtest 3: TEST_DATA[4].SPSEL == 0  (Bug 2: equals 1 instead)
 *     Subtest 4: TEST_DATA[8] == 0xFFFFFFFD  (sanity: EXC_RETURN correct)
 *
 *   Phase 4: Check post-return state (Tests Bugs 3 and 4)
 *     Subtest 5: CONTROL.SPSEL == 1  (Bug 4: stays 0 after return)
 *     Subtest 6: SP (R13) == PSP_BASE  (Bug 3: restored from MSP → garbage)
 *
 * -------------------------------------------------------------------------
 * Expected failure progression as bugs are fixed:
 *
 *   All 4 bugs present:
 *     → FAIL subtest 2 (Bug 1: handler SP = PSP-0x20 ≠ MSP)
 *
 *   Bugs 1+2 fixed (ArmMFault::invoke fixed):
 *     → FAIL subtest 5 or 6 (Bug 3 reads MSP area as PSP frame → garbage)
 *       May also crash (HardFault) if the garbage retAddr is unmapped.
 *
 *   All 4 bugs fixed:
 *     → PASS (exit 0, TEST_RESULT = 0xCAFECAFE)
 *
 * -------------------------------------------------------------------------
 * Memory map used:
 *   0x20000100  TEST_RESULT  — 0xCAFECAFE (pass) or 0xDEADDEAD (fail)
 *   0x20000104  TEST_SUBTEST — failing subtest number
 *   0x20000108  TEST_DATA[0] — handler R13 (SP) value
 *   0x2000010C  TEST_DATA[4] — handler CONTROL value
 *   0x20000110  TEST_DATA[8] — handler LR (EXC_RETURN) value
 *   0x20010000  PSP_BASE     — Thread-mode PSP (middle of SRAM1)
 *   0x20020000  MSP          — initial MSP from vector table (top of SRAM1)
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108  /* 12 bytes: handler sp / control / lr */
    .equ PSP_BASE,      0x20010000  /* Thread PSP — safe area in SRAM1 */
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

/* =========================================================================
 * Vector table
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
__isr_vector:
    .word   0x20020000          /* Entry 0:  Initial MSP = top of SRAM1 */
    .word   Reset_Handler       /* Entry 1:  Reset */
    .word   default_handler     /* Entry 2:  NMI */
    .word   HardFault_Handler   /* Entry 3:  HardFault — catches bad returns */
    .word   default_handler     /* Entry 4:  MemManage */
    .word   default_handler     /* Entry 5:  BusFault */
    .word   default_handler     /* Entry 6:  UsageFault */
    .word   0, 0, 0, 0          /* Entries 7–10: Reserved */
    .word   SVCall_Handler      /* Entry 11: SVCall */
    .word   0                   /* Entry 12: DebugMonitor */
    .word   0                   /* Entry 13: Reserved */
    .word   default_handler     /* Entry 14: PendSV */
    .word   default_handler     /* Entry 15: SysTick */

/* =========================================================================
 * Reset_Handler — main test body
 * ========================================================================= */
    .section .text

    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* ------------------------------------------------------------------
     * Phase 1: Switch Thread mode to PSP
     * ------------------------------------------------------------------
     *
     * Step 1: Write PSP = PSP_BASE.
     *   CONTROL.SPSEL is still 0 (MSP active), so MSR PSP only updates
     *   the PSP misc register — R13 is unchanged (still MSP).
     */
    ldr     r0, =PSP_BASE
    msr     PSP, r0

    /*
     * Step 2: Switch to PSP by writing CONTROL.SPSEL=1.
     *   ISB required after CONTROL write (DDI0403E B5.2.3).
     *   NOTE: Our gem5 MSR CONTROL handler does not update R13 when
     *   SPSEL changes (a known gem5 limitation separate from Bugs 1-4).
     *   After this, CONTROL.SPSEL=1 but R13 still holds the MSP value.
     */
    movs    r1, #2              /* CONTROL[1]=SPSEL=1, CONTROL[0]=nPRIV=0 */
    msr     CONTROL, r1
    isb                         /* Spec: ISB after CONTROL write */

    /*
     * Step 3: Write PSP = PSP_BASE again, now that SPSEL=1.
     *   MSR PSP checks SPSEL; when SPSEL=1 it also updates R13.
     *   After this: R13 == PSP_BASE == 0x20010000.
     */
    ldr     r0, =PSP_BASE
    msr     PSP, r0

    /* Subtest 1: R13 (SP) must equal PSP_BASE after the SPSEL=1 + PSP write */
    ldr     r0, =PSP_BASE
    cmp     sp, r0
    bne     fail_1

    /* ------------------------------------------------------------------
     * Phase 2: Fire SVC from Thread/PSP mode
     * ------------------------------------------------------------------
     *
     * What the spec says should happen on SVC entry:
     *   • Hardware pushes {r0-r3,r12,lr,pc,xpsr} onto PSP.
     *   • R13 (SP) switches to MSP for the handler.
     *   • CONTROL.SPSEL is cleared to 0.
     *   • LR is set to 0xFFFFFFFD (Thread/PSP EXC_RETURN).
     *
     * What happens with Bugs 1+2:
     *   • Frame IS pushed onto PSP (correct).
     *   • R13 stays at PSP-0x20 (Bug 1: not switched to MSP).
     *   • CONTROL.SPSEL stays at 1 (Bug 2: not cleared).
     *   • LR = 0xFFFFFFFD (computeExcReturn reads SPSEL=1 → correct).
     *
     * SVCall_Handler stores its raw observations to TEST_DATA,
     * then returns via BX LR (EXC_RETURN).
     */
    svc     #0                  /* Trigger SVCall exception */

    /* ------------------------------------------------------------------
     * Phase 3: Check handler observations
     * ------------------------------------------------------------------
     *
     * After SVC return, r4-r11 are caller-saved across exception
     * boundaries (only r0-r3,r12,lr,pc,xpsr are in the HW frame).
     * We use r5 as a base pointer; it was not modified before the SVC
     * and the HW restore doesn't touch it.
     */
    ldr     r5, =TEST_DATA

    /* Subtest 2: Handler SP should equal MSP
     *   Bug 1: handler SP = PSP_BASE - 0x20 = 0x2000FFE0 (PSP frame ptr)
     *   Fixed:  handler SP = 0x20020000 (MSP, unchanged during SVC entry) */
    ldr     r0, [r5, #0]        /* handler_sp */
    mrs     r1, MSP             /* current MSP (unchanged across SVC) */
    cmp     r0, r1
    bne     fail_2

    /* Subtest 3: Handler CONTROL.SPSEL must be 0 (handler uses MSP)
     *   Bug 2: SPSEL = 1 inside handler (was not cleared on entry)
     *   Fixed:  SPSEL = 0 */
    ldr     r0, [r5, #4]        /* handler_control */
    tst     r0, #2              /* bit[1] = SPSEL */
    bne     fail_3

    /* Subtest 4: Handler LR must be 0xFFFFFFFD (Thread/PSP EXC_RETURN)
     *   This verifies that computeExcReturn() correctly detected PSP mode.
     *   Expected: 0xFFFFFFFD on all runs (not a bug, sanity check). */
    ldr     r0, [r5, #8]        /* handler_lr */
    ldr     r1, =0xFFFFFFFD
    cmp     r0, r1
    bne     fail_4

    /* ------------------------------------------------------------------
     * Phase 4: Check post-return state (Bugs 3 and 4)
     * ------------------------------------------------------------------
     *
     * With Bugs 3+4 present (after Bugs 1+2 are fixed):
     *   • mProfileExcReturn reads frameptr from R13 (= MSP in handler)
     *     instead of from the PSP misc register.  Restores r0-r3 etc.
     *     from the MSP area (garbage), including the return address.
     *   • May cause HardFault if the garbage retAddr is unmapped.
     *   • If we reach here at all, CONTROL.SPSEL was not restored (Bug 4).
     *
     * Subtest 5: CONTROL.SPSEL must be 1 after return to Thread/PSP
     *   Bug 4: SPSEL = 0 (was cleared by entry fix, never restored) */
    mrs     r0, CONTROL
    tst     r0, #2              /* bit[1] = SPSEL */
    beq     fail_5              /* Bug 4: SPSEL not restored to 1 */

    /* Subtest 6: R13 must equal PSP_BASE after unstack from PSP frame
     *   The exception frame was pushed at PSP_BASE - 0x20 = 0x2000FFE0.
     *   After unstacking, SP should return to PSP_BASE = 0x20010000.
     *   Bug 3: reads frame from MSP area → restores wrong SP value */
    ldr     r0, =PSP_BASE
    cmp     sp, r0
    bne     fail_6

    /* All subtests passed */
    ldr     r0, =PASS_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    b       done

fail_1: movs    r2, #1
    b       fail
fail_2: movs    r2, #2
    b       fail
fail_3: movs    r2, #3
    b       fail
fail_4: movs    r2, #4
    b       fail
fail_5: movs    r2, #5
    b       fail
fail_6: movs    r2, #6
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

/* =========================================================================
 * SVCall_Handler
 *
 * Stores raw handler-mode observations to TEST_DATA, then returns
 * via EXC_RETURN in LR.  Deliberately avoids PUSH/POP because:
 *   • With Bug 1 active, R13 points into the PSP exception frame —
 *     any PUSH would corrupt the saved register state.
 *   • We want to capture the raw SP/CONTROL/LR without modification.
 * ========================================================================= */
    .thumb_func
SVCall_Handler:
    /* Record raw handler observations to TEST_DATA */
    ldr     r0, =TEST_DATA
    str     sp, [r0, #0]        /* TEST_DATA[0]: handler SP
                                 *   Correct: MSP = 0x20020000
                                 *   Bug 1:   PSP - 0x20 = 0x2000FFE0 */
    mrs     r1, CONTROL
    str     r1, [r0, #4]        /* TEST_DATA[4]: handler CONTROL
                                 *   Correct: SPSEL=0 → value = 0b00
                                 *   Bug 2:   SPSEL=1 → value = 0b10 */
    str     lr, [r0, #8]        /* TEST_DATA[8]: EXC_RETURN in LR
                                 *   Should always be 0xFFFFFFFD
                                 *   (Thread mode + was using PSP) */

    /* Return via EXC_RETURN.
     * With LR=0xFFFFFFFD: hardware should unstack from PSP, set
     * CONTROL.SPSEL=1, and resume Thread mode.
     * Bug 3: unstacks from R13 (MSP area) → garbage restore.
     * Bug 4: does not set CONTROL.SPSEL=1 → Thread stays on MSP. */
    bx      lr

/* =========================================================================
 * HardFault_Handler
 *
 * Catches crashes caused by Bug 3's garbage exception return address.
 * When mProfileExcReturn reads retAddr from the MSP area (garbage) and
 * that address is unmapped, a HardFault fires.  We record a distinctive
 * subtest=99 to make this visible in the trace.
 * ========================================================================= */
    .thumb_func
HardFault_Handler:
    ldr     r0, =FAIL_VAL
    ldr     r1, =TEST_RESULT
    str     r0, [r1]
    movs    r2, #99             /* 99 = crashed due to corrupt EXC_RETURN */
    ldr     r1, =TEST_SUBTEST
    str     r2, [r1]
    b       .                   /* spin */

/* =========================================================================
 * default_handler — catches unexpected exceptions
 * ========================================================================= */
    .thumb_func
default_handler:
    b       .
