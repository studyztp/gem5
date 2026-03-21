/*
 * FreeRTOSConfig.h — Minimal config for gem5 M-profile (STM32F405).
 *
 * Targets the AtomicSimpleCPU with no FPU, no MPU, no TrustZone.
 * Uses the ARM_CM3 port (no FPU context save — gem5 doesn't model FPU).
 * Heap uses heap_1.c (simple, no free — sufficient for test).
 * SysTick at 1000 Hz (1ms tick) with 168 MHz CPU clock.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* Scheduler */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_TICKLESS_IDLE                  0
#define configCPU_CLOCK_HZ                      ( 168000000UL )
#define configTICK_RATE_HZ                      ( 1000 )
#define configMAX_PRIORITIES                    ( 5 )
#define configMINIMAL_STACK_SIZE                ( 128 )
#define configMAX_TASK_NAME_LEN                 ( 16 )
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES    1

/* Memory */
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 16 * 1024 ) )
#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1

/* Hook functions — disabled for minimal test */
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configCHECK_FOR_STACK_OVERFLOW          0

/* No software timers, mutexes, semaphores, or co-routines needed */
#define configUSE_TIMERS                        0
#define configUSE_MUTEXES                       0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_CO_ROUTINES                   0

/* INCLUDE_ functions — only what we need */
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_vTaskPrioritySet                0
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    0
#define INCLUDE_vTaskDelayUntil                 0
#define INCLUDE_xTaskGetSchedulerState          0

/* Cortex-M4 interrupt priority configuration.
 * STM32F405 implements 4 priority bits (16 levels).
 * FreeRTOS needs the lowest-priority for PendSV/SysTick. */
#define configPRIO_BITS                         4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

/* Map FreeRTOS exception handlers to CMSIS vector names.
 * The startup code places these in the vector table. */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/* configASSERT — spin forever on failure (gem5 will hit tick limit) */
#define configASSERT(x) if ((x) == 0) { for (;;); }

#endif /* FREERTOS_CONFIG_H */
