/*
 * main_freertos_test.c — Two-task FreeRTOS test for gem5 M-profile.
 *
 * Creates two tasks (Task A at priority 2, Task B at priority 1) that
 * each increment a counter on every iteration with a vTaskDelay(1) to
 * yield.  After enough ticks, main checks both counters and reports
 * PASS/FAIL via semihosting.
 *
 * This exercises:
 *   - Bugs 1-4: PSP/MSP switching (tasks run in Thread/PSP, handlers in MSP)
 *   - Bug 16:   Priority-based preemption (SysTick, PendSV, SVCall)
 *   - SysTick:  Periodic tick interrupt driving the scheduler
 *   - PendSV:   Context switch mechanism
 *   - SVCall:   vPortSVCHandler for starting the first task
 *
 * Result protocol:
 *   0x20000100 = TEST_RESULT  (0xCAFECAFE = pass, 0xDEADDEAD = fail)
 *   0x20000104 = TEST_SUBTEST (failing subtest number)
 *   0x20000108 = TEST_DATA    (debug data)
 */

#include "FreeRTOS.h"
#include "task.h"

/* Result addresses in SRAM — same protocol as all other M-profile tests */
#define TEST_RESULT   ( *( (volatile uint32_t *) 0x20000100 ) )
#define TEST_SUBTEST  ( *( (volatile uint32_t *) 0x20000104 ) )
#define TEST_DATA     ( *( (volatile uint32_t *) 0x20000108 ) )

#define PASS_VAL  0xCAFECAFEUL
#define FAIL_VAL  0xDEADDEADUL

/* How many ticks each task should run before we check results.
 * With 1000 Hz tick rate, 10 ticks = 10ms.  Each task does
 * vTaskDelay(1) per iteration, so ~10 iterations expected. */
/* 3 iterations × 1ms SysTick period ≈ 3B gem5 ticks.  Enough to prove
 * context switches work without generating a multi-GB Exec trace. */
#define TARGET_TICKS  3

/* Shared counters — incremented by each task */
static volatile uint32_t counterA = 0;
static volatile uint32_t counterB = 0;

/* Flag: set by Task A when it has run enough */
static volatile uint32_t doneA = 0;
static volatile uint32_t doneB = 0;

/*
 * Semihosting exit — BKPT #0xAB with SYS_EXIT.
 * This cleanly stops the gem5 simulation.
 */
static void semihosting_exit(void)
{
    /* r0 = SYS_EXIT (0x18), r1 = ADP_Stopped_ApplicationExit (0x20026) */
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
 * Task A — higher priority (2).
 * Increments counterA, delays 1 tick each iteration.
 */
static void vTaskA(void *pvParameters)
{
    (void) pvParameters;

    for (;;) {
        counterA++;
        if (counterA >= TARGET_TICKS) {
            doneA = 1;
            /* Suspend self — let Task B and idle run */
            vTaskDelay(portMAX_DELAY);
        } else {
            vTaskDelay(1);
        }
    }
}

/*
 * Task B — lower priority (1).
 * Increments counterB, delays 1 tick each iteration.
 */
static void vTaskB(void *pvParameters)
{
    (void) pvParameters;

    for (;;) {
        counterB++;
        if (counterB >= TARGET_TICKS) {
            doneB = 1;
            vTaskDelay(portMAX_DELAY);
        } else {
            vTaskDelay(1);
        }
    }
}

/*
 * Main — create tasks, start scheduler, verify results.
 *
 * Note: vTaskStartScheduler() never returns if the scheduler starts
 * successfully.  We use the idle hook or a monitor task to check
 * results.  But since we disabled hooks, we use a third "monitor"
 * task at the lowest priority.
 */
static void vMonitorTask(void *pvParameters)
{
    (void) pvParameters;

    /* Wait for both tasks to finish their iterations */
    while (!doneA || !doneB) {
        vTaskDelay(1);
    }

    /* Subtest 1: Task A counter reached target */
    if (counterA < TARGET_TICKS) {
        fail(1, counterA);
    }

    /* Subtest 2: Task B counter reached target */
    if (counterB < TARGET_TICKS) {
        fail(2, counterB);
    }

    /* Subtest 3: Both tasks ran (neither starved) */
    if (counterA == 0 || counterB == 0) {
        fail(3, (counterA << 16) | counterB);
    }

    /* All checks passed */
    pass();
}

int main(void)
{
    BaseType_t ret;

    /* Create Task A (priority 2 — higher) */
    ret = xTaskCreate(vTaskA, "TaskA", configMINIMAL_STACK_SIZE,
                      NULL, 2, NULL);
    if (ret != pdPASS) {
        fail(10, ret);
    }

    /* Create Task B (priority 1 — lower) */
    ret = xTaskCreate(vTaskB, "TaskB", configMINIMAL_STACK_SIZE,
                      NULL, 1, NULL);
    if (ret != pdPASS) {
        fail(11, ret);
    }

    /* Create Monitor task (priority 1 — same as Task B, lowest among
     * user tasks).  Monitor only needs to run after both A and B have
     * finished their iterations and are sleeping with portMAX_DELAY.
     * If Monitor had the highest priority, it would monopolize every
     * SysTick wakeup slot and starve A and B. */
    ret = xTaskCreate(vMonitorTask, "Mon", configMINIMAL_STACK_SIZE,
                      NULL, 1, NULL);
    if (ret != pdPASS) {
        fail(12, ret);
    }

    /* Start the scheduler — this never returns on success.
     * FreeRTOS calls vPortSVCHandler (SVCall) to start the first task,
     * then PendSV for context switches, and SysTick for tick interrupts. */
    vTaskStartScheduler();

    /* If we get here, scheduler failed to start */
    fail(13, 0);

    return 0;
}
