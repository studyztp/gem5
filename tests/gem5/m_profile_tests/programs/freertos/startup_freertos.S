/*
 * startup_freertos.S — Minimal Cortex-M4 startup for FreeRTOS on gem5.
 *
 * Provides:
 *   - Vector table with all 16 system exceptions + IRQ0-15
 *   - Reset_Handler: copies .data, zeros .bss, calls main()
 *   - Default_Handler: infinite loop for unhandled exceptions
 *
 * FreeRTOS handlers (SVC_Handler, PendSV_Handler, SysTick_Handler)
 * are defined in port.c via FreeRTOSConfig.h #defines.
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

/* =========================================================================
 * Vector table
 *
 * FreeRTOS needs: SVCall (11), PendSV (14), SysTick (15).
 * All others point to Default_Handler.
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl __isr_vector
__isr_vector:
    .word   _estack              /* 0:  Initial MSP */
    .word   Reset_Handler        /* 1:  Reset */
    .word   NMI_Handler          /* 2:  NMI */
    .word   HardFault_Handler    /* 3:  HardFault */
    .word   MemManage_Handler    /* 4:  MemManage */
    .word   BusFault_Handler     /* 5:  BusFault */
    .word   UsageFault_Handler   /* 6:  UsageFault */
    .word   0                    /* 7:  Reserved */
    .word   0                    /* 8:  Reserved */
    .word   0                    /* 9:  Reserved */
    .word   0                    /* 10: Reserved */
    .word   SVC_Handler          /* 11: SVCall (FreeRTOS vPortSVCHandler) */
    .word   DebugMon_Handler     /* 12: DebugMonitor */
    .word   0                    /* 13: Reserved */
    .word   PendSV_Handler       /* 14: PendSV (FreeRTOS xPortPendSVHandler) */
    .word   SysTick_Handler      /* 15: SysTick (FreeRTOS xPortSysTickHandler) */
    /* External IRQs 0-15 (all default) */
    .word   Default_Handler      /* 16: IRQ0 */
    .word   Default_Handler      /* 17: IRQ1 */
    .word   Default_Handler      /* 18: IRQ2 */
    .word   Default_Handler      /* 19: IRQ3 */
    .word   Default_Handler      /* 20: IRQ4 */
    .word   Default_Handler      /* 21: IRQ5 */
    .word   Default_Handler      /* 22: IRQ6 */
    .word   Default_Handler      /* 23: IRQ7 */
    .word   Default_Handler      /* 24: IRQ8 */
    .word   Default_Handler      /* 25: IRQ9 */
    .word   Default_Handler      /* 26: IRQ10 */
    .word   Default_Handler      /* 27: IRQ11 */
    .word   Default_Handler      /* 28: IRQ12 */
    .word   Default_Handler      /* 29: IRQ13 */
    .word   Default_Handler      /* 30: IRQ14 */
    .word   Default_Handler      /* 31: IRQ15 */

/* =========================================================================
 * Reset_Handler — C runtime init, then call main()
 * ========================================================================= */
    .section .text
    .thumb_func
    .globl Reset_Handler
Reset_Handler:
    /* Copy .data from flash to SRAM */
    ldr     r0, =_sdata          /* dest start (SRAM) */
    ldr     r1, =_edata          /* dest end */
    ldr     r2, =_sidata         /* source (flash LMA) */
copy_data:
    cmp     r0, r1
    bge     zero_bss
    ldr     r3, [r2], #4
    str     r3, [r0], #4
    b       copy_data

    /* Zero .bss */
zero_bss:
    ldr     r0, =_sbss
    ldr     r1, =_ebss
    mov     r2, #0
clear_bss:
    cmp     r0, r1
    bge     call_main
    str     r2, [r0], #4
    b       clear_bss

call_main:
    bl      main
    /* If main returns (shouldn't), spin */
    b       .

/* =========================================================================
 * Default exception handlers — weak symbols, can be overridden
 * ========================================================================= */
    .thumb_func
    .weak NMI_Handler
NMI_Handler:
    b       .

    .thumb_func
    .weak HardFault_Handler
HardFault_Handler:
    b       .

    .thumb_func
    .weak MemManage_Handler
MemManage_Handler:
    b       .

    .thumb_func
    .weak BusFault_Handler
BusFault_Handler:
    b       .

    .thumb_func
    .weak UsageFault_Handler
UsageFault_Handler:
    b       .

    .thumb_func
    .weak DebugMon_Handler
DebugMon_Handler:
    b       .

    .thumb_func
    .weak Default_Handler
Default_Handler:
    b       .

    .end
