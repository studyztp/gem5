/*
 * Copyright (c) 2026 University of California, Davis and Cornell University
 * All rights reserved.
 *
 * Basic Cortex-M4 test program for gem5 M-profile validation.
 *
 * Tests:
 *   1. Vector table setup (initial SP and Reset_Handler address)
 *   2. Basic data processing (MOV, ADD, SUB)
 *   3. Load/store to SRAM
 *   4. MRS/MSR for M-profile special registers (PRIMASK, CONTROL)
 *   5. CPS (CPSID i / CPSIE i)
 *   6. SVC (supervisor call → SVCall exception)
 *   7. SysTick interrupt
 *
 * Memory map (matches STM32F405):
 *   Flash:  0x08000000 (vector table + code)
 *   SRAM1:  0x20000000 (data + stack)
 *   SCS:    0xE000E000 (NVIC + SysTick + SCB)
 *
 * Build with:
 *   arm-none-eabi-as -mcpu=cortex-m4 -mthumb cortexm4_basic.S -o test.o
 *   arm-none-eabi-ld -T cortexm4.ld test.o -o cortexm4_basic.elf
 *
 * Run with gem5:
 *   ./build/ARM/gem5.opt tests/gem5/m_profile_tests/configs/run_m4_test.py \
 *       --firmware tests/gem5/m_profile_tests/programs/cortexm4_basic.elf
 *
 * Validate by checking gem5 trace output:
 *   --debug-flags=Exec,Decode,MProfileCPSR
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

/* ================================================================
 * SCS register addresses (DDI0403E B3.2)
 * ================================================================ */
    .equ SCS_BASE,      0xE000E000
    .equ SYSTICK_CSR,   SCS_BASE + 0x010  /* SysTick Control and Status */
    .equ SYSTICK_RVR,   SCS_BASE + 0x014  /* SysTick Reload Value */
    .equ SYSTICK_CVR,   SCS_BASE + 0x018  /* SysTick Current Value */
    .equ SCB_VTOR,      SCS_BASE + 0xD08  /* Vector Table Offset */
    .equ SCB_SHPR3,     SCS_BASE + 0xD20  /* System Handler Priority 3 */

/* ================================================================
 * Test result addresses in SRAM
 * ================================================================ */
    .equ TEST_RESULTS,  0x20000100
    .equ TEST_PASS_VAL, 0xCAFECAFE
    .equ TEST_FAIL_VAL, 0xDEADDEAD

/* ================================================================
 * Vector table — must be at the start of flash
 * ================================================================
 *
 * DDI0403E B1.5.2: The vector table contains:
 *   Offset 0x00: Initial Main Stack Pointer value
 *   Offset 0x04: Reset handler address (bit[0] must be 1 for Thumb)
 *   Offset 0x08-0x3C: Exception handlers (NMI, HardFault, ...)
 *   Offset 0x40+: External interrupt handlers
 */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl __isr_vector
__isr_vector:
    .word   _estack             /* 0x00: Initial SP (top of SRAM) */
    .word   Reset_Handler       /* 0x04: Reset */
    .word   NMI_Handler         /* 0x08: NMI */
    .word   HardFault_Handler   /* 0x0C: HardFault */
    .word   MemManage_Handler   /* 0x10: MemManage */
    .word   BusFault_Handler    /* 0x14: BusFault */
    .word   UsageFault_Handler  /* 0x18: UsageFault */
    .word   0                   /* 0x1C: Reserved */
    .word   0                   /* 0x20: Reserved */
    .word   0                   /* 0x24: Reserved */
    .word   0                   /* 0x28: Reserved */
    .word   SVCall_Handler      /* 0x2C: SVCall */
    .word   0                   /* 0x30: Debug Monitor */
    .word   0                   /* 0x34: Reserved */
    .word   PendSV_Handler      /* 0x38: PendSV */
    .word   SysTick_Handler     /* 0x3C: SysTick */

/* ================================================================
 * Reset Handler — main test entry point
 * ================================================================ */
    .section .text
    .align 2
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

    /* ---- Test 1: Basic data processing ---- */
    /* These instructions should decode via standard Thumb decoder
     * (fall-through path).  Verify they execute correctly. */
    mov     r0, #0
    mov     r1, #42
    add     r2, r0, r1          /* r2 = 42 */
    sub     r3, r2, #10         /* r3 = 32 */
    ldr     r4, =0x20000000     /* SRAM base (loaded from literal pool) */

    /* ---- Test 2: Load/store to SRAM ---- */
    /* Tests that load/store instructions work correctly through
     * the CPSR alias (Cpsr.e read returns 0 = LE). */
    str     r2, [r4]            /* store 42 to SRAM[0] */
    ldr     r5, [r4]            /* load back */
    cmp     r5, r2              /* should be equal */
    bne     test_fail

    /* ---- Test 3: MRS/MSR for M-profile special registers ---- */
    /* These are intercepted by MDecoder → MrsMProfile/MsrMProfile.
     * DDI0403E A7.7.41 (MRS), A7.7.42 (MSR) */

    /* Read PRIMASK (SYSm=16) — should be 0 (interrupts enabled) */
    mrs     r0, PRIMASK
    cmp     r0, #0
    bne     test_fail

    /* Write PRIMASK = 1 (disable interrupts) */
    mov     r0, #1
    msr     PRIMASK, r0

    /* Read back — should be 1 */
    mrs     r1, PRIMASK
    cmp     r1, #1
    bne     test_fail

    /* Re-enable interrupts */
    mov     r0, #0
    msr     PRIMASK, r0

    /* Read CONTROL (SYSm=20) — should be 0 (privileged, MSP) */
    mrs     r0, CONTROL
    cmp     r0, #0
    bne     test_fail

    /* ---- Test 4: CPS (change processor state) ---- */
    /* Intercepted by MDecoder → CpsMProfile.
     * DDI0403E A7.7.17 */

    cpsid   i                   /* disable interrupts (PRIMASK=1) */
    mrs     r0, PRIMASK
    cmp     r0, #1
    bne     test_fail

    cpsie   i                   /* enable interrupts (PRIMASK=0) */
    mrs     r0, PRIMASK
    cmp     r0, #0
    bne     test_fail

    /* ---- Test 5: SVC (supervisor call) ---- */
    /* Intercepted by MDecoder → SvcMProfile.
     * Should trigger SVCall exception (exception number 11).
     * The SVCall handler sets a flag in SRAM. */
    ldr     r4, =TEST_RESULTS
    mov     r0, #0
    str     r0, [r4]            /* clear result */
    svc     #0                  /* trigger SVCall */
    ldr     r0, [r4]            /* read result from handler */
    ldr     r1, =TEST_PASS_VAL
    cmp     r0, r1
    bne     test_fail

    /* ---- Test 6: SysTick interrupt ---- */
    /* Configure SysTick to fire after 100 ticks.
     * The handler sets a flag in SRAM. */
    ldr     r4, =TEST_RESULTS
    mov     r0, #0
    str     r0, [r4, #4]        /* clear SysTick result */

    /* Set reload value = 99 (counts 100 ticks: 99 down to 0) */
    ldr     r0, =SYSTICK_RVR
    mov     r1, #99
    str     r1, [r0]

    /* Clear current value (write any value) */
    ldr     r0, =SYSTICK_CVR
    mov     r1, #0
    str     r1, [r0]

    /* Enable SysTick with TICKINT and processor clock:
     * CSR = ENABLE(1) | TICKINT(1) | CLKSOURCE(1) = 0x7 */
    ldr     r0, =SYSTICK_CSR
    mov     r1, #7
    str     r1, [r0]

    /* Wait for SysTick to fire (busy-wait).
     * The handler will set TEST_RESULTS+4 = TEST_PASS_VAL. */
    mov     r6, #0
    ldr     r7, =1000           /* timeout counter (must use ldr for large imm) */
systick_wait:
    ldr     r0, [r4, #4]
    ldr     r1, =TEST_PASS_VAL
    cmp     r0, r1
    beq     systick_done
    add     r6, r6, #1
    cmp     r6, r7              /* timeout after 1000 iterations */
    bge     test_fail
    b       systick_wait

systick_done:
    /* Disable SysTick */
    ldr     r0, =SYSTICK_CSR
    mov     r1, #0
    str     r1, [r0]

    /* ---- All tests passed ---- */
test_pass:
    ldr     r4, =TEST_RESULTS
    ldr     r0, =TEST_PASS_VAL
    str     r0, [r4, #8]        /* final result = PASS */
    b       test_done

test_fail:
    ldr     r4, =TEST_RESULTS
    ldr     r0, =TEST_FAIL_VAL
    str     r0, [r4, #8]        /* final result = FAIL */

test_done:
    /* Infinite loop — gem5 will terminate via tick limit */
    b       test_done

/* ================================================================
 * Exception Handlers
 * ================================================================ */

    .thumb_func
SVCall_Handler:
    /* SVCall handler: write TEST_PASS_VAL to TEST_RESULTS[0] */
    ldr     r0, =TEST_RESULTS
    ldr     r1, =TEST_PASS_VAL
    str     r1, [r0]
    bx      lr                  /* return from exception (EXC_RETURN) */

    .thumb_func
SysTick_Handler:
    /* SysTick handler: write TEST_PASS_VAL to TEST_RESULTS[4] */
    ldr     r0, =TEST_RESULTS
    ldr     r1, =TEST_PASS_VAL
    str     r1, [r0, #4]
    bx      lr                  /* return from exception (EXC_RETURN) */

/* Default handlers — infinite loop (will trigger timeout) */
    .thumb_func
NMI_Handler:
    .thumb_func
HardFault_Handler:
    .thumb_func
MemManage_Handler:
    .thumb_func
BusFault_Handler:
    .thumb_func
UsageFault_Handler:
    .thumb_func
PendSV_Handler:
    b       .
