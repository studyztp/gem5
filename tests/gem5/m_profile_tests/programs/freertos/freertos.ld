/*
 * Linker script for FreeRTOS test on gem5 M-profile (STM32F405).
 *
 * Extended from cortexm4.ld with proper .data/.bss symbols for C startup.
 * Flash at 0x08000000, SRAM at 0x20000000 (128KB).
 */

ENTRY(Reset_Handler)

_estack = 0x20020000;   /* Top of SRAM1 (128KB) — initial MSP */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 1M
    SRAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
}

SECTIONS
{
    /* Vector table — must be first in flash */
    .isr_vector :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))
        . = ALIGN(4);
    } > FLASH

    /* Code */
    .text :
    {
        . = ALIGN(4);
        *(.text)
        *(.text*)
        *(.glue_7)
        *(.glue_7t)
        KEEP(*(.init))
        KEEP(*(.fini))
        . = ALIGN(4);
    } > FLASH

    /* Constants */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* ARM exception unwind tables — needed by gcc for C++ but harmless */
    .ARM.extab :
    {
        *(.ARM.extab*)
    } > FLASH

    .ARM.exidx :
    {
        *(.ARM.exidx*)
    } > FLASH

    /* Initialized data — stored in flash, copied to SRAM at startup */
    _sidata = LOADADDR(.data);

    .data :
    {
        . = ALIGN(4);
        _sdata = .;
        *(.data)
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > SRAM AT> FLASH

    /* Zero-initialized data */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;
        __bss_start__ = _sbss;
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
        __bss_end__ = _ebss;
    } > SRAM

    /* Heap — used by FreeRTOS heap_1.c (ucHeap array) */
    ._user_heap_stack :
    {
        . = ALIGN(8);
        _end = .;
        PROVIDE(end = .);
        . = . + 0x200;   /* minimal C stack (main uses MSP before scheduler) */
        . = ALIGN(8);
    } > SRAM
}
