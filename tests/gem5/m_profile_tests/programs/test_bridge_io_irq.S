/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved.
 *
 * test_bridge_io_irq.S — IRQ-driven echo firmware for the
 * MProfileBridgeIO ping-pong test.
 *
 * Lifecycle:
 *   1. Reset_Handler enables IRQ 101 in the NVIC, writes the 4-byte
 *      sentinel "INIT" into the bridge's output buffer, sets out_size,
 *      and writes registers[1] = 1.  The bridge calls exitSimLoopNow,
 *      handing control back to the Python harness.
 *   2. After signalling, the firmware enables interrupts (cpsie i)
 *      and parks in a wfi loop.
 *   3. The Python harness pushes input via updateInputData() and
 *      calls raiseInterrupt() — this sends IRQ 101 through the SCS,
 *      waking the CPU.
 *   4. Bridge_IRQ_Handler echoes (in_size, in_addr) -> (out_addr),
 *      sets out_size, writes done = 1.  The bridge exits the sim
 *      loop again, handing control back to Python.
 *   5. Steps 3-4 repeat for as many iterations as Python wants.
 *
 * Pass/fail is determined entirely by the Python harness (via output
 * data comparison); this firmware never writes to TEST_RESULTS, so
 * if the firmware crashes mid-iteration the harness will time out
 * waiting for the next "MProfileBridgeIO signaled done." and report
 * the failure with a useful message.
 *
 * Build with the existing Makefile; uses the same cortexm4.ld
 * linker script as the rest of m_profile_tests/programs/.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

/* ================================================================
 * Bridge MMIO layout (defaults: pio_addr=0x90000000, in/out=1024 B).
 * ================================================================ */
    .equ BRIDGE_BASE,     0x90000000
    .equ BRIDGE_REG_GO,   BRIDGE_BASE + 0x00
    .equ BRIDGE_REG_DONE, BRIDGE_BASE + 0x04
    .equ BRIDGE_REG_IN,   BRIDGE_BASE + 0x08
    .equ BRIDGE_REG_INSZ, BRIDGE_BASE + 0x0C
    .equ BRIDGE_REG_OUT,  BRIDGE_BASE + 0x10
    .equ BRIDGE_REG_OUTSZ,BRIDGE_BASE + 0x14
    .equ EXPECTED_OUT_ADDR, 0x90000418

/* ================================================================
 * NVIC bits we use (ARMv7-M B3.4 / DDI0403E).
 *   ISER[3] at NVIC_BASE + 0x10C controls IRQs 96..127.
 *   Bit (101 % 32) = 5 enables IRQ 101.  Default priority is 0
 *   (highest configurable), which preempts thread mode (priority 0xFF).
 *   No IPR write needed.
 * ================================================================ */
    .equ NVIC_ISER_3,     0xE000E10C
    .equ IRQ101_BIT,      0x20

/* ================================================================
 * "INIT" sentinel — 4 bytes, little-endian: 'I','N','I','T'.
 * ================================================================ */
    .equ INIT_WORD,       0x54494E49

/* ================================================================
 * Vector table — 16 system + 102 external IRQ slots.
 * IRQ 101 occupies offset 0x40 + 4*101 = 0x1D4 (exception number 117).
 * ================================================================ */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl __isr_vector
__isr_vector:
    .word   _estack
    .word   Reset_Handler
    .word   Default_Handler   /* NMI */
    .word   Default_Handler   /* HardFault */
    .word   Default_Handler   /* MemManage */
    .word   Default_Handler   /* BusFault */
    .word   Default_Handler   /* UsageFault */
    .word   0
    .word   0
    .word   0
    .word   0
    .word   Default_Handler   /* SVCall */
    .word   0
    .word   0
    .word   Default_Handler   /* PendSV */
    .word   Default_Handler   /* SysTick */

    /* External IRQs 0..100 — never enabled by this firmware, but the
     * vector table still has to be physically big enough to reach
     * IRQ 101's slot.  Fill with Default_Handler so any spurious
     * vector dispatch lands in a defined infinite loop. */
    .rept 101
    .word   Default_Handler
    .endr
    /* IRQ 101 — the bridge IRQ. */
    .word   Bridge_IRQ_Handler

/* ================================================================
 * Default handler — infinite loop.  Spurious vectors land here.
 * ================================================================ */
    .section .text
    .align 2
    .thumb_func
Default_Handler:
    b       .

/* ================================================================
 * Reset Handler — initial setup + first "ready" signal.
 * ================================================================ */
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Enable IRQ 101 in NVIC.  Writing 1 to ISER bit 5 of word 3
     * sets-enabled (W1S); writing 0 has no effect, so the rest of
     * the register is unaffected. */
    ldr     r0, =NVIC_ISER_3
    movs    r1, #IRQ101_BIT
    str     r1, [r0]

    /* Initial output: "INIT" at out_addr, out_size = 4. */
    ldr     r0, =EXPECTED_OUT_ADDR
    ldr     r1, =INIT_WORD
    str     r1, [r0]

    ldr     r0, =BRIDGE_REG_OUTSZ
    movs    r1, #4
    str     r1, [r0]

    /* Signal "ready" to Python.  This is the first sim-exit; the
     * harness will read getOutputData() and verify it == "INIT". */
    ldr     r0, =BRIDGE_REG_DONE
    movs    r1, #1
    str     r1, [r0]

    /* Make sure interrupts are enabled before parking.  PRIMASK was
     * left in its reset state (0 = enabled) but be explicit so this
     * code is robust against firmware-author edits above. */
    cpsie   i

main_loop:
    /* WFI — return when ANY interrupt is pending+enabled and PRIMASK=0.
     * The SCS's IRQ 101 pend (set by Python's raiseInterrupt()) wakes
     * us; the IRQ then preempts and runs Bridge_IRQ_Handler.  After
     * the handler returns, control resumes here and we wait again. */
    wfi
    b       main_loop

/* ================================================================
 * Bridge IRQ Handler — echo input bytes to output buffer.
 *
 * The Cortex-M hardware auto-stacks r0-r3, r12, lr, pc, xPSR on
 * exception entry, so r0-r3 / r12 are free to clobber.  r4-r11 are
 * caller-saved per AAPCS — push the ones we use.
 * ================================================================ */
    .thumb_func
Bridge_IRQ_Handler:
    push    {r4, r5}

    /* Snapshot the bridge register block once. */
    ldr     r0, =BRIDGE_BASE
    ldr     r1, [r0, #0x0C]    /* in_size  = registers[3] */
    ldr     r2, [r0, #0x08]    /* in_addr  = registers[2] */
    ldr     r3, [r0, #0x10]    /* out_addr = registers[4] */

    /* Byte-copy in_size bytes from in_addr to out_addr.  Byte loop
     * keeps the test independent of input alignment / size. */
    movs    r4, #0
copy_loop:
    cmp     r4, r1
    bge     copy_done
    ldrb    r5, [r2, r4]
    strb    r5, [r3, r4]
    adds    r4, r4, #1
    b       copy_loop

copy_done:
    /* out_size = in_size — Python reads this via getOutputDataSize(). */
    str     r1, [r0, #0x14]

    /* Tell Python "iteration done".  This is the per-iteration
     * sim-exit; the harness reads the output, verifies it, then
     * calls updateInputData()/raiseInterrupt() for the next
     * iteration. */
    movs    r5, #1
    str     r5, [r0, #0x04]

    pop     {r4, r5}
    bx      lr     /* EXC_RETURN — unstacks frame, back to main_loop */
