/*
 * FreeRTOSConfig.h
 *
 * Project-specific kernel configuration for the Timeline Scheduler.
 * Target: QEMU Cortex-M3 (lm3s811evb / lm3s6965evb machine).
 * Kernel: FreeRTOS 11.x
 *
 * Every non-obvious value is explained inline. Read the comments before
 * changing anything — several settings are load-bearing for the timeline
 * scheduler and must not be altered without understanding the impact.
 *
 * FreeRTOS Kernel V11.x
 * Copyright (C) 2024 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/*-----------------------------------------------------------
 * IMPORTANT: this guard prevents the assembler from seeing
 * C-only declarations (e.g. extern) that it cannot parse.
 *----------------------------------------------------------*/
#ifdef __GNUC__
    #include <stdint.h>
    extern uint32_t SystemCoreClock;
#endif

/*===========================================================================
 * 1. TIMELINE SCHEDULER SWITCH
 *
 * Set to 1 to compile in the timeline scheduler and the enriched TCB fields.
 * Set to 0 to revert to a fully stock FreeRTOS build with zero overhead.
 * This macro guards every kernel modification made by this project.
 *=========================================================================*/
#define configUSE_TIMELINE_SCHEDULER            1

/*===========================================================================
 * 2. CORE SCHEDULING BEHAVIOUR
 *=========================================================================*/

/* Must be 1. The port layer uses preemption to trigger context switches via
 * PendSV. Our timeline engine sets xSwitchRequired inside xTaskIncrementTick()
 * and relies on PendSV being asserted — that only happens when preemption is
 * enabled. Disabling this would break the entire switch mechanism.          */
#define configUSE_PREEMPTION                    1

/* Must be 0. Tickless idle suspends the SysTick interrupt for multiple ticks
 * at a time to save power. This would cause the timeline engine to lose track
 * of frame position entirely, skipping task slots and missing deadlines.
 * Never enable this for a time-triggered scheduler.                          */
#define configUSE_TICKLESS_IDLE                 0

/* 1 ms per tick. At 1000 Hz the TIMELINE_MS_TO_TICKS() macro is a no-op
 * (ms == ticks), which eliminates rounding error in slot boundaries.
 * This also satisfies the ≤1-tick jitter requirement in the spec: 1 tick = 1ms
 * is already the finest resolution a tick-driven system can achieve.         */
#define configTICK_RATE_HZ                      ( ( TickType_t ) 1000 )

/* QEMU lm3s811evb / lm3s6965evb runs at 12 MHz. If you switch QEMU machines
 * (e.g. mps2-an385 at 25 MHz) update this accordingly. The SysTick reload
 * value is computed from this by the port layer.                             */
#define configCPU_CLOCK_HZ                      ( ( uint32_t ) 12000000 )

/* Priority levels:
 *   0            — Idle task (FreeRTOS internal, do not use in user code)
 *   1            — Reserved (spare)
 *   2            — TIMELINE_SRT_PRIORITY  (defined in timeline_scheduler.h)
 *   3            — TIMELINE_HRT_PRIORITY  (defined in timeline_scheduler.h)
 *   4            — Reserved for future use / configTIMER_TASK_PRIORITY
 *
 * configMAX_PRIORITIES must be strictly greater than the highest priority
 * level used, so 5 is the minimum safe value for this layout.               */
#define configMAX_PRIORITIES                    ( 5 )

/* Idle task stack. The idle task does nothing except run SRT tasks via the
 * timeline engine — the minimal size is sufficient.                          */
#define configMINIMAL_STACK_SIZE                ( ( uint16_t ) 128 )

/* Total heap for dynamic allocation. Covers:
 *   - Up to 16 task stacks (16 × 2 × configMINIMAL_STACK_SIZE × 4 bytes)
 *   - TCBs (each ≈ 100–150 bytes after our extensions)
 * 32 KB is generous for QEMU and leaves room for the trace ring buffer.      */
#define configTOTAL_HEAP_SIZE                   ( ( size_t ) ( 32 * 1024 ) )

/* Maximum length of a task name string, including the null terminator.
 * 16 characters is enough for descriptive names like "HRT_Sensor_A".        */
#define configMAX_TASK_NAME_LEN                 ( 16 )

/* Use 32-bit tick counters. 16-bit ticks overflow after only 65 seconds at
 * 1000 Hz, which would corrupt the frame clock inside xTimelineTickHandler().
 * Always keep this 0 for any real-time application with long uptimes.        */
#define configUSE_16_BIT_TICKS                  0

/* When 1, the idle task yields immediately if any other task at idle priority
 * is ready. Irrelevant here since no user task runs at idle priority, but
 * keeping it 1 is the safe default.                                          */
#define configIDLE_SHOULD_YIELD                 1

/* Task preemption disable is not needed — our scheduler controls preemption
 * through direct list manipulation, not the per-task disable flag.           */
#define configUSE_TASK_PREEMPTION_DISABLE       0

/*===========================================================================
 * 3. HOOK FUNCTIONS
 *
 * IMPORTANT: all hooks are DISABLED.
 *
 * We do NOT use vApplicationTickHook() because our timeline logic lives
 * directly inside the modified xTaskIncrementTick() in tasks.c. Using a tick
 * hook as well would mean our code runs twice per tick — once inside the
 * kernel function and once in the hook — and the hook still has the ISR
 * restriction on API calls that your professor flagged.
 *
 * vApplicationIdleHook() is similarly unused: SRT scheduling is handled
 * inside xTimelineTickHandler() via direct list manipulation, not from the
 * idle task hook.
 *=========================================================================*/
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            0   /* assertions in vConfigureScheduler() catch alloc failures */
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0

/*===========================================================================
 * 4. SOFTWARE TIMERS
 *
 * Disabled. The software timer task is an additional FreeRTOS task that runs
 * at a configurable priority and processes timer callbacks. It would consume
 * CPU time outside the timeline engine's control, violating the ≤10% overhead
 * requirement and introducing non-deterministic scheduling behaviour.
 *=========================================================================*/
#define configUSE_TIMERS                        0
#define configTIMER_TASK_PRIORITY               ( 4 )       /* unused, kept for build */
#define configTIMER_QUEUE_LENGTH                ( 5 )       /* unused, kept for build */
#define configTIMER_TASK_STACK_DEPTH            ( configMINIMAL_STACK_SIZE * 2 )

/*===========================================================================
 * 5. TRACE AND MONITORING
 *
 * configUSE_TRACE_FACILITY = 1 adds uxTCBNumber and uxTaskNumber to the TCB,
 * which are used by kernel-aware debuggers (QEMU + GDB) and by our own trace
 * module to uniquely identify tasks in log output.
 *
 * configGENERATE_RUN_TIME_STATS = 1 adds ulRunTimeCounter to the TCB and
 * enables vTaskGetRunTimeStats(). This is how you measure whether scheduler
 * overhead stays within the ≤10% CPU budget stated in the spec. Without this
 * you are guessing at CPU usage rather than measuring it.
 *
 * To use run-time stats you must also provide two macros that the kernel
 * calls to read a high-resolution counter. On QEMU Cortex-M the SysTick
 * current-value register gives sub-tick resolution:
 *   portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() — initialisation (empty here,
 *     SysTick is already running as the tick source)
 *   portGET_RUN_TIME_COUNTER_VALUE()          — reads the counter
 *=========================================================================*/
#define configUSE_TRACE_FACILITY                1
#define configGENERATE_RUN_TIME_STATS           1

/* SysTick counts DOWN from reload to 0. To get a monotonically increasing
 * counter we subtract the current value from the reload value, then add the
 * tick count scaled to sub-tick units. This gives tick-level + sub-tick
 * resolution without needing a separate hardware timer.
 *
 * The SysTick current-value register is read via its fixed Cortex-M
 * architectural address (0xE000E018) rather than through the CMSIS SysTick
 * struct. This avoids a dependency on device-specific headers inside tasks.c,
 * where the CMSIS headers are not included. The address is architecturally
 * fixed on all Cortex-M targets so this is fully portable.                  */
#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() /* SysTick already running  */
#define portGET_RUN_TIME_COUNTER_VALUE()                                      \
    ( xTaskGetTickCount() * ( configCPU_CLOCK_HZ / configTICK_RATE_HZ )      \
      + ( ( configCPU_CLOCK_HZ / configTICK_RATE_HZ )                        \
          - ( *( ( volatile uint32_t * ) 0xE000E018UL ) ) ) )

/*===========================================================================
 * 6. STACK OVERFLOW DETECTION
 *
 * Disabled to keep the project minimal — no hook function required.
 * Re-enable with configCHECK_FOR_STACK_OVERFLOW 2 during debugging if
 * you observe unexplained memory corruption or erratic task behaviour.
 *=========================================================================*/
#define configCHECK_FOR_STACK_OVERFLOW          0

/*===========================================================================
 * 7. MUTEXES, SEMAPHORES, QUEUES
 *
 * Mutexes and semaphores are disabled. The assignment states that inter-task
 * communication uses polling, and the timeline architecture makes mutual
 * exclusion unnecessary (tasks are non-overlapping by design for HRT).
 * Disabling these features removes the associated TCB fields (uxBasePriority,
 * uxMutexesHeld) and reduces RAM usage.
 *
 * Queues are left enabled at the kernel level because FreeRTOS uses them
 * internally for certain housekeeping paths even if your application does not.
 *=========================================================================*/
#define configUSE_MUTEXES                       0
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configQUEUE_REGISTRY_SIZE               0   /* disable queue registry  */

/*===========================================================================
 * 8. MEMORY ALLOCATION
 *
 * Both static and dynamic allocation are enabled. Tasks are created
 * dynamically inside vConfigureScheduler() using xTaskCreate(). Static
 * allocation is left available so the idle task can be given a statically
 * allocated stack, which avoids heap fragmentation on startup.
 *=========================================================================*/
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configSUPPORT_STATIC_ALLOCATION         0

/*===========================================================================
 * 9. CORTEX-M INTERRUPT PRIORITY CONFIGURATION
 *
 * The LM3S (Cortex-M3) NVIC implements 3 priority bits → 8 priority levels
 * (0 = highest, 7 = lowest).
 *
 * configKERNEL_INTERRUPT_PRIORITY:
 *   The SysTick and PendSV interrupts run at the LOWEST hardware priority
 *   (7, encoded as 0xE0 in the 8-bit NVIC register with 3 bits). This ensures
 *   that any application ISR can preempt the kernel tick without risk.
 *
 * configMAX_SYSCALL_INTERRUPT_PRIORITY:
 *   ISRs at or below this priority (numerically ≥ this value) may call
 *   FreeRTOS "FromISR" API functions safely. ISRs above this priority
 *   (numerically < this value) must NEVER call any FreeRTOS API.
 *   Set to priority level 5 (encoded as 0xA0) — leaves levels 0–4 free for
 *   safety-critical ISRs that must never be delayed by FreeRTOS critical
 *   sections.
 *=========================================================================*/
#define configPRIO_BITS                         3   /* LM3S Cortex-M3: 3 bits */

#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         ( ( 1U << configPRIO_BITS ) - 1U )  /* 7  */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5

#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )          /* 0xE0 */
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )     /* 0xA0 */

/*===========================================================================
 * 10. ASSERTION HANDLER
 *
 * configASSERT() is called throughout the FreeRTOS kernel to catch
 * programming errors (NULL pointers, invalid parameters, etc.). On QEMU with
 * semihosting, an infinite loop is the simplest handler — it will halt the
 * simulator and you can inspect state in GDB. In production you would replace
 * this with a safe-state entry routine.
 *=========================================================================*/
#define configASSERT( x )                                                     \
    if( ( x ) == 0 )                                                          \
    {                                                                         \
        taskDISABLE_INTERRUPTS();                                             \
        for( ; ; );                                                           \
    }

/*===========================================================================
 * 11. OPTIONAL / UNUSED FEATURES — explicitly disabled for clarity
 *=========================================================================*/
#define configUSE_CO_ROUTINES                   0   /* deprecated, never use  */
#define configUSE_TASK_NOTIFICATIONS            1   /* keep: used internally  */
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configUSE_APPLICATION_TASK_TAG          0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_POSIX_ERRNO                   0
#define configUSE_C_RUNTIME_TLS_SUPPORT         0
#define configUSE_CORE_AFFINITY                 0   /* single-core target     */
#define configNUMBER_OF_CORES                   1

/*===========================================================================
 * 12. INCLUDE / EXCLUDE OPTIONAL API FUNCTIONS
 *
 * Only include what the project actually needs. Excluding unused functions
 * reduces code size. Functions required by the timeline scheduler internally
 * are marked with a comment.
 *=========================================================================*/
#define INCLUDE_vTaskPrioritySet                0   /* timeline controls priority */
#define INCLUDE_uxTaskPriorityGet               0
#define INCLUDE_vTaskDelete                     1   /* used in frame reset        */
#define INCLUDE_vTaskSuspend                    1   /* required by list internals  */
#define INCLUDE_xResumeFromISR                  0   /* not used (no FromISR calls) */
#define INCLUDE_vTaskDelayUntil                 0   /* tasks don't self-schedule   */
#define INCLUDE_vTaskDelay                      0   /* tasks don't self-schedule   */
#define INCLUDE_xTaskGetSchedulerState          1   /* used in vConfigureScheduler */
#define INCLUDE_xTaskGetCurrentTaskHandle       1   /* used in tick handler        */
#define INCLUDE_uxTaskGetStackHighWaterMark     1   /* used in test suite          */
#define INCLUDE_xTaskGetIdleTaskHandle          1   /* used to identify idle task  */
#define INCLUDE_eTaskGetState                   1   /* used in trace module        */
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_xTaskAbortDelay                 0

/*===========================================================================
 * 13. MAP FREERTOS HANDLER NAMES TO CMSIS NAMES
 *
 * The Cortex-M port expects these three interrupt handlers. CMSIS and most
 * startup files define the CMSIS names, so we remap here. If your startup
 * file already uses the FreeRTOS names, remove these defines.
 *=========================================================================*/
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */