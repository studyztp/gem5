/*
 * Linker script for Cortex-M4 test programs on gem5.
 *
 * Memory map matches STM32F405 (used by the gem5 M-profile platform).
 * Flash at 0x08000000, SRAM at 0x20000000.
 *
 * The vector table (.isr_vector) must be at the start of flash.
 * VTOR should be set to 0x08000000 (the default for STM32).
 */

ENTRY(Reset_Handler)

_estack = 0x20020000;  /* Top of SRAM1 (128KB from 0x20000000) */

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
        . = ALIGN(4);
    } > FLASH

    /* Read-only data */
    .rodata :
    {
        . = ALIGN(4);
        *(.rodata)
        *(.rodata*)
        . = ALIGN(4);
    } > FLASH

    /* Initialized data (copied from flash to SRAM at startup) */
    .data :
    {
        . = ALIGN(4);
        *(.data)
        *(.data*)
        . = ALIGN(4);
    } > SRAM AT> FLASH

    /* Zero-initialized data */
    .bss :
    {
        . = ALIGN(4);
        *(.bss)
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
    } > SRAM
}
