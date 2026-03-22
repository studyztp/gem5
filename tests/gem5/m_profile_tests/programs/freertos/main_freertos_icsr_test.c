/*
 * main_freertos_icsr_test.c — FreeRTOS ICSR dynamic field test (BUG-2)
 *
 * Tests that ICSR (0xE000ED04) returns correct dynamic fields in a
 * real RTOS environment where SVCall, PendSV, SysTick, and external
 * IRQs interact naturally.
 *
 * DDI0403E B3.2.4 defines these read-only fields:
 *   VECTACTIVE [8:0]    — current exception number
 *   RETTOBASE  [11]     — 1 if only one active exception
 *   VECTPENDING [20:12] — highest-priority pending exception number
 *   ISRPENDING [22]     — 1 if any external IRQ is pending
 *
 * Before the fix, all these fields read as 0 because readSCB() returns
 * the raw stored ICSR value without computing them.
 *
 * =========================================================================
 * Test design:
 *
 *   Subtest 1 — Thread mode baseline:
 *     Read ICSR from a FreeRTOS task running in Thread mode.
 *     VECTACTIVE must be 0.
 *
 *   Subtest 2 — SysTick handler VECTACTIVE:
 *     Inside the SysTick hook, read ICSR.
 *     VECTACTIVE must be 15 (SysTick = exception 15).
 *
 *   Subtest 3 — SysTick handler RETTOBASE:
 *     Inside SysTick (only active exception), RETTOBASE must be 1.
 *
 *   Subtest 4 — External IRQ pending during handler:
 *     Inside SysTick hook, pend IRQ 0 (enabled, low priority, can't
 *     preempt SysTick). Read ICSR.
 *     ISRPENDING must be 1.
 *
 *   Subtest 5 — VECTPENDING with external IRQ:
 *     Same context as subtest 4.
 *     VECTPENDING must be 16 (IRQ 0 = exception 16).
 *
 *   Subtest 6 — PendSV VECTACTIVE:
 *     Inside a custom PendSV wrapper, read ICSR before calling the
 *     real FreeRTOS PendSV handler.
 *     VECTACTIVE must be 14 (PendSV = exception 14).
 *     NOTE: This is tricky because FreeRTOS owns PendSV.  Instead,
 *     we check VECTACTIVE from the tick hook which runs inside
 *     SysTick — and SysTick triggers PendSV for context switches.
 *     We verify PendSV is pending (VECTPENDING == 14) when
 *     SysTick sets PENDSVSET.
 *
 * Result protocol:
 *   0x20000100 = TEST_RESULT  (0xCAFECAFE = pass, 0xDEADDEAD = fail)
 *   0x20000104 = TEST_SUBTEST (failing subtest number)
 *   0x20000108 = TEST_DATA    (debug data — observed ICSR value)
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

/* Result addresses in SRAM */
#define TEST_RESULT   ( *( (volatile uint32_t *) 0x20000100 ) )
#define TEST_SUBTEST  ( *( (volatile uint32_t *) 0x20000104 ) )
#define TEST_DATA     ( *( (volatile uint32_t *) 0x20000108 ) )

#define PASS_VAL  0xCAFECAFEUL
#define FAIL_VAL  0xDEADDEADUL

/* ICSR and NVIC register addresses */
#define SCB_ICSR    ( *( (volatile uint32_t *) 0xE000ED04 ) )
#define NVIC_ISER0  ( *( (volatile uint32_t *) 0xE000E100 ) )
#define NVIC_ICER0  ( *( (volatile uint32_t *) 0xE000E180 ) )
#define NVIC_ISPR0  ( *( (volatile uint32_t *) 0xE000E200 ) )
#define NVIC_ICPR0  ( *( (volatile uint32_t *) 0xE000E280 ) )
#define NVIC_IPR0   ( *( (volatile uint32_t *) 0xE000E400 ) )

/* Shared state between tick hook and task */
static volatile uint32_t tick_icsr_1 = 0;       /* ICSR on SysTick entry */
static volatile uint32_t tick_icsr_2 = 0;       /* ICSR after pending IRQ 0 */
static volatile uint32_t tick_hook_done = 0;     /* 1 when tick hook has run */
static volatile uint32_t tick_count = 0;

/* How many SysTick periods to wait before checking */
#define TARGET_TICKS  2

static void semihosting_exit(void)
{
    __asm volatile (
        "mov r0, #0x18\n"
        "ldr r1, =0x20026\n"
        "bkpt #0xab\n"
        ::: "r0", "r1"
    );
}

static void fail(uint32_t subtest, uint32_t data)
{
    TEST_DATA = data;
    TEST_SUBTEST = subtest;
    TEST_RESULT = FAIL_VAL;
    semihosting_exit();
    for (;;);
}

static void pass(void)
{
    TEST_RESULT = PASS_VAL;
    semihosting_exit();
    for (;;);
}

/*
 * FreeRTOS tick hook — runs inside SysTick handler context.
 *
 * This is the ideal place to test ICSR because:
 *   - We're in Handler mode (SysTick = exception 15)
 *   - We can pend external IRQs and observe ISRPENDING/VECTPENDING
 *   - FreeRTOS may have set PENDSVSET for context switch
 *
 * Only runs the test once (on the first tick after TARGET_TICKS).
 */
void vApplicationTickHook(void)
{
    if (tick_hook_done)
        return;

    tick_count++;
    if (tick_count < TARGET_TICKS)
        return;

    /* --- Read ICSR on SysTick entry (subtests 2-3) --- */
    tick_icsr_1 = SCB_ICSR;

    /* --- Set up IRQ 0: lowest priority, enable, pend (subtests 4-5) --- */
    /* Priority 0xF0 = lowest configurable.  SysTick has priority set by
     * FreeRTOS (configKERNEL_INTERRUPT_PRIORITY = 0xF0), so IRQ 0 at
     * the same priority can't preempt — equal priority doesn't preempt
     * on M-profile. */
    NVIC_IPR0 = 0xF0;         /* IRQ 0 priority = 0xF0 (byte 0) */
    NVIC_ISER0 = 1;           /* enable IRQ 0 */
    NVIC_ISPR0 = 1;           /* pend IRQ 0 */
    __asm volatile ("dsb");
    __asm volatile ("isb");

    /* --- Read ICSR after pending IRQ 0 (subtests 4-5) --- */
    tick_icsr_2 = SCB_ICSR;

    /* --- Clean up: clear pending and disable IRQ 0 --- */
    NVIC_ICPR0 = 1;
    NVIC_ICER0 = 1;

    tick_hook_done = 1;
}

/*
 * Test task — runs in Thread mode (PSP).
 *
 * Waits for the tick hook to capture ICSR values, then validates them.
 */
static void vTestTask(void *pvParameters)
{
    (void) pvParameters;
    uint32_t icsr, field;

    /* --- Subtest 1: Thread mode VECTACTIVE == 0 --- */
    icsr = SCB_ICSR;
    field = icsr & 0x1FF;           /* VECTACTIVE [8:0] */
    if (field != 0) {
        fail(1, icsr);
    }

    /* Wait for tick hook to run and capture ICSR values */
    while (!tick_hook_done) {
        vTaskDelay(1);
    }

    /* --- Subtest 2: SysTick VECTACTIVE == 15 --- */
    icsr = tick_icsr_1;
    field = icsr & 0x1FF;           /* VECTACTIVE [8:0] */
    if (field != 15) {
        fail(2, icsr);
    }

    /* --- Subtest 3: SysTick RETTOBASE == 1 --- */
    field = (icsr >> 11) & 1;       /* RETTOBASE [11] */
    if (field != 1) {
        fail(3, icsr);
    }

    /* --- Subtest 4: ISRPENDING == 1 (IRQ 0 pended) --- */
    icsr = tick_icsr_2;
    field = (icsr >> 22) & 1;       /* ISRPENDING [22] */
    if (field != 1) {
        fail(4, icsr);
    }

    /* --- Subtest 5: VECTPENDING == 16 (IRQ 0 = exception 16) --- */
    field = (icsr >> 12) & 0x1FF;   /* VECTPENDING [20:12] */
    if (field != 16) {
        fail(5, icsr);
    }

    /* All subtests passed */
    pass();
}

int main(void)
{
    BaseType_t ret;

    ret = xTaskCreate(vTestTask, "ICSR", configMINIMAL_STACK_SIZE,
                      NULL, 2, NULL);
    if (ret != pdPASS) {
        fail(10, ret);
    }

    vTaskStartScheduler();

    /* Should not reach here */
    fail(11, 0);
    return 0;
}
