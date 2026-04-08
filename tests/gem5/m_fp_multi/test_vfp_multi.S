/**
 * @file test_vfp_multi.S
 * Microbenchmarks to test M-profile VLDM/VSTM/VPUSH/VPOP.
 *
 * Each test stores known values, does a push/pop or ldm/stm,
 * then checks the result by comparing against expected values.
 * Writes 0 to 0xDEAD0000 on failure (triggers BadAddr fault in gem5).
 * Reaches the end (infinite loop) on success.
 *
 * Build:
 *   arm-none-eabi-gcc -mcpu=cortex-m4 -mfpu=fpv4-sp-d16 \
 *     -mfloat-abi=hard -mthumb -nostdlib -T link.ld test_vfp_multi.S \
 *     -o test_vfp_multi.elf
 *
 * Run in gem5:
 *   gem5.opt run_m5op_bench.py --firmware test_vfp_multi.elf --no-art
 */

    .syntax unified
    .cpu    cortex-m4
    .fpu    fpv4-sp-d16
    .thumb

/* ================================================================
 * Vector table (minimal)
 * ================================================================ */
    .section .isr_vector, "a", %progbits
    .word   _stack_top          @ Initial SP
    .word   _start + 1          @ Reset handler (thumb)

/* ================================================================
 * Test code
 * ================================================================ */
    .text
    .thumb_func
    .global _start
_start:
    /* Enable FPU: set CP10/CP11 full access in CPACR */
    ldr     r0, =0xE000ED88
    ldr     r1, [r0]
    orr     r1, r1, #(0xF << 20)
    str     r1, [r0]
    dsb
    isb

    /* Set FPSCR to known state */
    mov     r0, #0
    vmsr    fpscr, r0

    /* ============================================================
     * TEST 1: VPUSH/VPOP single-precision (1 register)
     * vpush {s0}  /  vpop {s0}
     * ============================================================ */
test1:
    /* Load known value into s0 */
    ldr     r0, =0x3F800000     @ 1.0f
    vmov    s0, r0

    /* Save SP before push */
    mov     r4, sp

    /* VPUSH {s0} — should decrement SP by 4 and store s0 */
    vpush   {s0}

    /* Check SP decremented by 4 */
    mov     r5, sp
    sub     r6, r4, #4
    cmp     r5, r6
    bne     fail

    /* Corrupt s0 */
    mov     r0, #0
    vmov    s0, r0

    /* VPOP {s0} — should load s0 and increment SP by 4 */
    vpop    {s0}

    /* Check SP restored */
    cmp     sp, r4
    bne     fail

    /* Check s0 value restored */
    vmov    r0, s0
    ldr     r1, =0x3F800000
    cmp     r0, r1
    bne     fail

    /* ============================================================
     * TEST 2: VPUSH/VPOP single-precision (4 registers)
     * vpush {s0-s3}  /  vpop {s0-s3}
     * ============================================================ */
test2:
    ldr     r0, =0x41200000     @ 10.0f
    vmov    s0, r0
    ldr     r0, =0x41A00000     @ 20.0f
    vmov    s1, r0
    ldr     r0, =0x41F00000     @ 30.0f
    vmov    s2, r0
    ldr     r0, =0x42200000     @ 40.0f
    vmov    s3, r0

    mov     r4, sp

    vpush   {s0-s3}

    /* Check SP decremented by 16 */
    mov     r5, sp
    sub     r6, r4, #16
    cmp     r5, r6
    bne     fail

    /* Corrupt all */
    mov     r0, #0
    vmov    s0, r0
    vmov    s1, r0
    vmov    s2, r0
    vmov    s3, r0

    vpop    {s0-s3}

    /* Check SP restored */
    cmp     sp, r4
    bne     fail

    /* Check s0 = 10.0f */
    vmov    r0, s0
    ldr     r1, =0x41200000
    cmp     r0, r1
    bne     fail

    /* Check s3 = 40.0f */
    vmov    r0, s3
    ldr     r1, =0x42200000
    cmp     r0, r1
    bne     fail

    /* ============================================================
     * TEST 3: VPUSH/VPOP double-precision (1 register)
     * vpush {d0}  /  vpop {d0}
     * ============================================================ */
test3:
    ldr     r0, =0x00000000     @ low word of 1.0 double
    ldr     r1, =0x3FF00000     @ high word of 1.0 double
    vmov    d0, r0, r1

    mov     r4, sp

    vpush   {d0}

    /* Check SP decremented by 8 */
    mov     r5, sp
    sub     r6, r4, #8
    cmp     r5, r6
    bne     fail

    /* Corrupt d0 */
    mov     r0, #0
    mov     r1, #0
    vmov    d0, r0, r1

    vpop    {d0}

    /* Check SP restored */
    cmp     sp, r4
    bne     fail

    /* Check d0 value restored */
    vmov    r0, r1, d0
    ldr     r2, =0x00000000
    cmp     r0, r2
    bne     fail
    ldr     r2, =0x3FF00000
    cmp     r1, r2
    bne     fail

    /* ============================================================
     * TEST 4: VSTM/VLDM increment after (single)
     * vstmia r0!, {s4-s7}  /  vldmia r0!, {s4-s7}
     * ============================================================ */
test4:
    /* Use stack area as scratch buffer */
    sub     sp, sp, #32
    mov     r7, sp              @ save buffer base

    ldr     r0, =0x41200000     @ 10.0f
    vmov    s4, r0
    ldr     r0, =0x41A00000     @ 20.0f
    vmov    s5, r0
    ldr     r0, =0x41F00000     @ 30.0f
    vmov    s6, r0
    ldr     r0, =0x42200000     @ 40.0f
    vmov    s7, r0

    /* VSTM: store s4-s7 at [sp], sp += 16 */
    mov     r0, r7
    vstmia  r0!, {s4-s7}

    /* Check r0 advanced by 16 */
    add     r1, r7, #16
    cmp     r0, r1
    bne     fail

    /* Corrupt registers */
    mov     r0, #0
    vmov    s4, r0
    vmov    s5, r0
    vmov    s6, r0
    vmov    s7, r0

    /* VLDM: load s4-s7 from [buffer], ptr += 16 */
    mov     r0, r7
    vldmia  r0!, {s4-s7}

    /* Check r0 advanced by 16 */
    add     r1, r7, #16
    cmp     r0, r1
    bne     fail

    /* Check s4 = 10.0f */
    vmov    r0, s4
    ldr     r1, =0x41200000
    cmp     r0, r1
    bne     fail

    /* Check s7 = 40.0f */
    vmov    r0, s7
    ldr     r1, =0x42200000
    cmp     r0, r1
    bne     fail

    add     sp, sp, #32         @ restore stack

    /* ============================================================
     * TEST 5: VPUSH/VPOP double (2 registers)
     * vpush {d4-d5}  /  vpop {d4-d5}
     * ============================================================ */
test5:
    ldr     r0, =0xAAAAAAAA
    ldr     r1, =0xBBBBBBBB
    vmov    d4, r0, r1
    ldr     r0, =0xCCCCCCCC
    ldr     r1, =0xDDDDDDDD
    vmov    d5, r0, r1

    mov     r4, sp

    vpush   {d4-d5}

    /* Check SP decremented by 16 */
    mov     r5, sp
    sub     r6, r4, #16
    cmp     r5, r6
    bne     fail

    /* Corrupt */
    mov     r0, #0
    mov     r1, #0
    vmov    d4, r0, r1
    vmov    d5, r0, r1

    vpop    {d4-d5}

    cmp     sp, r4
    bne     fail

    /* Check d4 low */
    vmov    r0, r1, d4
    ldr     r2, =0xAAAAAAAA
    cmp     r0, r2
    bne     fail
    ldr     r2, =0xBBBBBBBB
    cmp     r1, r2
    bne     fail

    /* Check d5 high */
    vmov    r0, r1, d5
    ldr     r2, =0xCCCCCCCC
    cmp     r0, r2
    bne     fail
    ldr     r2, =0xDDDDDDDD
    cmp     r1, r2
    bne     fail

    /* ============================================================
     * ALL TESTS PASSED
     * ============================================================ */
pass:
    /* Signal success: write 0x50415353 ("PASS") to semihosting */
    mov     r0, #0x18           @ SYS_EXIT
    ldr     r1, =0x20026
    bkpt    #0xAB

    /* Should not reach here */
    b       .

    /* ============================================================
     * FAILURE
     * ============================================================ */
fail:
    /* Signal failure: write to unmapped address to trigger fault */
    mov     r0, #0x18           @ SYS_EXIT
    ldr     r1, =0x20000        @ ADP_Stopped_InternalError
    bkpt    #0xAB
    b       .

/* ================================================================
 * Linker symbols
 * ================================================================ */
    .section .stack, "aw", %nobits
    .align  3
    .space  4096
_stack_top:
