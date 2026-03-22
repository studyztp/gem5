/*
 * test_scb_byte_access.S — SCB sub-word (byte/halfword) access (BUG-7)
 *
 * Verifies that SCB registers support byte-level reads and writes.
 * DDI0403E B3.2.11-13 says SHPR1/2/3 are byte-accessible.  CMSIS
 * NVIC_SetPriority() for system exceptions uses byte writes:
 *   NVIC_SetPriority(PendSV_IRQn, pri)  → STRB to SHPR3+2 (0xE000ED22)
 *   NVIC_SetPriority(SysTick_IRQn, pri) → STRB to SHPR3+3 (0xE000ED23)
 *
 * Before the fix:
 *   read():  pkt->setLE<uint32_t>(data) always writes 4 bytes regardless
 *            of packet size — buffer overrun for LDRB/LDRH.
 *   write(): pkt->getLE<uint32_t>() always reads 4 bytes — reads past
 *            buffer for STRB/STRH.
 *
 * =========================================================================
 * Test design — uses SHPR3 at 0xE000ED20:
 *   byte 0: Debug Monitor priority (exc 12)
 *   byte 1: reserved
 *   byte 2: PendSV priority (exc 14)
 *   byte 3: SysTick priority (exc 15)
 *
 *   Subtest 1: Write 0x00000000 to SHPR3 (word), read back as word
 *              Expected: 0x00000000
 *
 *   Subtest 2: STRB 0x80 to 0xE000ED22 (PendSV byte)
 *   Subtest 3: Read SHPR3 as word
 *              Expected: 0x00800000 (only byte 2 = 0x80)
 *
 *   Subtest 4: LDRB from 0xE000ED22
 *              Expected: 0x80
 *
 *   Subtest 5: LDRB from 0xE000ED23 (SysTick byte, untouched)
 *              Expected: 0x00
 *
 *   Subtest 6: STRB 0x40 to 0xE000ED23 (SysTick byte)
 *   Subtest 7: Read SHPR3 as word
 *              Expected: 0x40800000
 *
 *   Subtest 8: Clean up — write 0x00000000 to SHPR3
 *
 * =========================================================================
 * Memory map:
 *   0x20000100  TEST_RESULT
 *   0x20000104  TEST_SUBTEST
 *   0x20000108  TEST_DATA
 */

    .syntax unified
    .cpu cortex-m4
    .thumb

    .equ TEST_RESULT,   0x20000100
    .equ TEST_SUBTEST,  0x20000104
    .equ TEST_DATA,     0x20000108
    .equ PASS_VAL,      0xCAFECAFE
    .equ FAIL_VAL,      0xDEADDEAD

    .equ SHPR3_ADDR,    0xE000ED20
    .equ PENDSV_BYTE,   0xE000ED22   /* SHPR3 + 2 */
    .equ SYSTICK_BYTE,  0xE000ED23   /* SHPR3 + 3 */

/* =========================================================================
 * Vector table
 * ========================================================================= */
    .section .isr_vector, "a", %progbits
    .align 2
    .globl _vector_table
_vector_table:
    .word _estack
    .word Reset_Handler

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
 * Reset Handler
 * ========================================================================= */
    .text
    .thumb_func
    .globl Reset_Handler
Reset_Handler:

/* -- Subtest 1: Word write to SHPR3, word read back -- */
    ldr r0, =SHPR3_ADDR
    movs r1, #0
    str r1, [r0]               /* clear SHPR3 */
    ldr r2, [r0]               /* read back as word */
    cmp r2, #0
    bne 1f
    b 2f
1:  fail_test 1, r2
2:

/* -- Subtest 2-3: Byte write to PendSV priority, word read back -- */
    ldr r0, =PENDSV_BYTE
    movs r1, #0x80
    strb r1, [r0]              /* STRB 0x80 to SHPR3+2 */

    ldr r0, =SHPR3_ADDR
    ldr r2, [r0]               /* read SHPR3 as word */
    ldr r3, =0x00800000        /* expected: byte 2 = 0x80, rest = 0 */
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 3, r2
2:

/* -- Subtest 4: Byte read of PendSV priority -- */
    ldr r0, =PENDSV_BYTE
    ldrb r2, [r0]              /* LDRB from SHPR3+2 */
    cmp r2, #0x80
    bne 1f
    b 2f
1:  fail_test 4, r2
2:

/* -- Subtest 5: Byte read of SysTick priority (untouched) -- */
    ldr r0, =SYSTICK_BYTE
    ldrb r2, [r0]              /* LDRB from SHPR3+3 */
    cmp r2, #0
    bne 1f
    b 2f
1:  fail_test 5, r2
2:

/* -- Subtest 6-7: Byte write to SysTick priority, word read back -- */
    ldr r0, =SYSTICK_BYTE
    movs r1, #0x40
    strb r1, [r0]              /* STRB 0x40 to SHPR3+3 */

    ldr r0, =SHPR3_ADDR
    ldr r2, [r0]               /* read SHPR3 as word */
    ldr r3, =0x40800000        /* expected: byte 3 = 0x40, byte 2 = 0x80 */
    cmp r2, r3
    bne 1f
    b 2f
1:  fail_test 7, r2
2:

/* -- Subtest 8: Clean up — restore SHPR3 to 0 -- */
    ldr r0, =SHPR3_ADDR
    movs r1, #0
    str r1, [r0]
    ldr r2, [r0]
    cmp r2, #0
    bne 1f
    b 2f
1:  fail_test 8, r2
2:

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
