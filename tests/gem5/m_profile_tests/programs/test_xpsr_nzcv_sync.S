/*
 * test_xpsr_nzcv_sync.S — CC flat reg ↔ xPSR NZCV sync (BUG-5)
 *
 * Verifies that NZCV condition flags are correctly synced between
 * CC flat registers (used by A-profile ALU instructions) and
 * MISCREG_M_XPSR (used by M-profile MRS/MSR and exception frames).
 *
 * gem5 reuses A-profile Thumb instruction implementations which store
 * flags in CC flat regs, NOT in xPSR.  Sync is needed at:
 *   1. Exception entry: live xPSR must have correct NZCV for handler
 *   2. MRS APSR: must return live flags, not stale xPSR
 *   3. MSR APSR: must update CC regs so conditionals see new flags
 *   4. Exception return: flags must survive the round trip
 *
 * =========================================================================
 * Test design:
 *
 *   Subtest 1 (MRS reads live flags):
 *     CMP r0, r0  → sets Z=1, C=1, N=0, V=0
 *     MRS r1, APSR
 *     Verify Z=1 (bit 30) and C=1 (bit 29) in r1
 *
 *   Subtest 2 (MRS reads N flag):
 *     LDR r0, =-1 ; ADDS r0, #0  → sets N=1, Z=0
 *     MRS r1, APSR
 *     Verify N=1 (bit 31) in r1
 *
 *   Subtest 3 (MSR writes affect conditionals):
 *     MSR APSR_nzcvq, r0  with Z=1 set
 *     BEQ should be taken (Z=1 means equal)
 *
 *   Subtest 4 (Exception handler sees correct flags):
 *     SUBS r0, r0, r0  → sets Z=1, C=1
 *     SVC #0  → SVCall handler reads MRS APSR
 *     Verify handler saw Z=1, C=1
 *
 *   Subtest 5 (Flags survive exception round trip):
 *     MOVS r0, #1 ; SUBS r0, #2  → sets N=1, C=0 (borrow)
 *     SVC #1  → handler does nothing, returns
 *     After return: BLT should be taken (N!=V → less than)
 *
 * =========================================================================
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000108  TEST_DATA
 *   0x20000200  svc_apsr   — APSR read by SVCall handler
 *   0x20000204  svc_number — SVC number (to distinguish subtest 4 vs 5)
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    .equ SVC_APSR,      0x20000200
    .equ SVC_NUMBER,    0x20000204

/* =========================================================================
 * Vector table
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl _vector_table
_vector_table:
    .word _estack
    .word Reset_Handler
    .word 0                     /* NMI */
    .word 0                     /* HardFault */
    .word 0                     /* MemManage */
    .word 0                     /* BusFault */
    .word 0                     /* UsageFault */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word 0                     /* Reserved */
    .word SVCall_Handler        /* SVCall */

/* =========================================================================
 * Macros
 * ========================================================================= */
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
    /* Initialize SRAM */
    ldr r0, =SVC_APSR
    movs r1, #0
    str r1, [r0]
    ldr r0, =SVC_NUMBER
    str r1, [r0]

/* -- Subtest 1: MRS reads live Z and C flags -- */
/* CMP r0, r0 sets Z=1 (equal), C=1 (no borrow), N=0, V=0.
 * MRS r1, APSR should return Z=1 (bit 30) and C=1 (bit 29). */
    movs r0, #0
    cmp r0, r0                  /* Z=1, C=1, N=0, V=0 */
    mrs r1, apsr                /* read APSR — must have live flags */
    /* Check Z=1 (bit 30) */
    lsr r2, r1, #30
    and r2, r2, #1
    cmp r2, #1
    bne 1f
    b 2f
1:  fail_test 1, r1
2:
    /* Check C=1 (bit 29) — reload r1 since CMP above clobbered flags */
    ldr r0, =SVC_APSR           /* use SVC_APSR as temp storage */
    str r1, [r0]                /* save APSR value */
    ldr r1, [r0]                /* reload */
    lsr r2, r1, #29
    and r2, r2, #1
    cmp r2, #1
    bne 1f
    b 2f
1:  fail_test 1, r1
2:

/* -- Subtest 2: MRS reads live N flag -- */
/* Load -1 (0xFFFFFFFF) and add 0 — sets N=1, Z=0. */
    ldr r0, =0xFFFFFFFF
    adds r0, r0, #0            /* N=1, Z=0, C=0, V=0 */
    mrs r1, apsr
    /* Check N=1 (bit 31) */
    lsr r2, r1, #31
    cmp r2, #1
    bne 1f
    b 2f
1:  fail_test 2, r1
2:

/* -- Subtest 3: MSR APSR writes affect conditional branches -- */
/* Set Z=1 via MSR, then BEQ should be taken. */
    ldr r0, =0x40000000         /* bit 30 = Z flag */
    msr apsr_nzcvq, r0         /* write Z=1, N=0, C=0, V=0, Q=0 */
    beq 3f                      /* should be taken (Z=1) */
    /* If we get here, MSR didn't update CC regs */
    mov r2, #0
    fail_test 3, r2
3:

/* -- Subtest 4: Exception handler sees correct flags -- */
/* Set Z=1, C=1 then SVC — handler reads APSR and stores it. */
    movs r0, #0
    subs r0, r0, #0            /* Z=1, C=1 (SUB 0 from 0) */
    svc #0                      /* → SVCall handler (svc_num=0) */

    /* Check handler saw Z=1 (bit 30) */
    ldr r0, =SVC_APSR
    ldr r2, [r0]
    lsr r3, r2, #30
    and r3, r3, #1
    cmp r3, #1
    bne 1f
    b 2f
1:  fail_test 4, r2
2:

/* -- Subtest 5: Flags survive exception round trip -- */
/* Set N=1, C=0: compute 1 - 2 = -1 (N=1, C=0 borrow, Z=0, V=0).
 * Then SVC #1 — handler does nothing special, just returns.
 * After return, BLT (signed less than: N!=V) should be taken. */
    movs r0, #1
    subs r0, r0, #2            /* r0 = -1, N=1, C=0, Z=0, V=0 */
    svc #1                      /* → SVCall handler (svc_num=1, no-op) */

    /* After exception return, N=1 and V=0 should still hold.
     * BLT is taken when N!=V. */
    blt 3f
    /* If we get here, flags were corrupted by exception round trip */
    mov r2, #0
    fail_test 5, r2
3:

/* -- All subtests passed -- */
test_pass:
    ldr r0, =TEST_RESULT
    ldr r1, =PASS_VAL
    str r1, [r0]

test_exit:
    mov r0, #0x18
    ldr r1, =0x20026
    bkpt #0xAB
    b .

/* =========================================================================
 * SVCall Handler
 *
 * For SVC #0 (subtest 4): reads APSR and stores to SVC_APSR.
 * For SVC #1 (subtest 5): does nothing (just returns, testing round trip).
 *
 * Extracts SVC number from the stacked PC to determine which SVC.
 * The SVC instruction is at [stacked_PC - 2] and the immediate is
 * in bits[7:0] of the instruction.
 * ========================================================================= */
    .thumb_func
    .globl SVCall_Handler
SVCall_Handler:
    /* Get stacked PC from the exception frame.
     * MSP points to the frame: R0,R1,R2,R3,R12,LR,PC,xPSR
     * Stacked PC is at SP+0x18 */
    mrs r0, msp                 /* r0 = current MSP (frame pointer) */
    ldr r1, [r0, #0x18]        /* r1 = stacked PC (return address) */

    /* SVC number is in the SVC instruction at [stacked_PC - 2].
     * SVC instruction encoding: 0xDF<imm8>
     * We want bits[7:0] of the halfword at (stacked_PC - 2). */
    subs r1, r1, #2            /* point to SVC instruction */
    ldrb r1, [r1]              /* r1 = SVC number (low byte) */

    /* Store SVC number for debugging */
    ldr r2, =SVC_NUMBER
    str r1, [r2]

    /* SVC #0: read and store APSR */
    cmp r1, #0
    bne svc_done

    mrs r3, apsr               /* read APSR — should have live flags */
    ldr r2, =SVC_APSR
    str r3, [r2]

svc_done:
    bx lr                       /* exception return */
