/*
 * timeline_scheduler.h
 *
 * Timeline-based deterministic scheduler for FreeRTOS 11.x
 * Replaces the default priority-based scheduling model with a
 * time-triggered, major-frame/sub-frame architecture.
 *
 * Design note — TCB enrichment:
 *   Per-task runtime state and scheduling parameters are stored directly
 *   inside the FreeRTOS Task Control Block (TCB, tskTaskControlBlock in
 *   tasks.c) rather than in a parallel external structure. This keeps all
 *   task state in one place and allows the tick handler to access it
 *   directly via pxCurrentTCB without any table lookup.
 *
 *   The following fields are added to tskTaskControlBlock in tasks.c:
 *
 *       TaskType_t      xTimelineType;         // HARD_RT or SOFT_RT
 *       TickType_t      xTimelineStartTick;    // slot start (frame-relative)
 *       TickType_t      xTimelineEndTick;      // slot end / deadline
 *       UBaseType_t     uxTimelineSubframeId;  // sub-frame membership
 *       UBaseType_t     uxTimelineIndex;       // index in xSchedule.xHandles[]
 *                                              // TIMELINE_NO_TASK if unmanaged
 *       BaseType_t      xTimelineIsRunning;    // pdTRUE while executing
 *       BaseType_t      xTimelineHasCompleted; // pdTRUE if returned before DL
 *       BaseType_t      xTimelineWasKilled;    // pdTRUE if terminated at DL
 *
 *   All fields are initialised to safe defaults in prvInitialiseNewTask()
 *   so that unmanaged tasks (idle task, timer task) are never affected by
 *   the timeline tick handler.
 *
 *   The global schedule table (TimelineSchedule_t, defined in
 *   timeline_scheduler.c) holds only frame-level data and the ordered
 *   array of TaskHandles — it does NOT duplicate per-task runtime state.
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

#ifndef TIMELINE_SCHEDULER_H
#define TIMELINE_SCHEDULER_H

#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Constants
 * ========================================================================= */

/* Maximum number of tasks the scheduler can manage (HRT + SRT combined).   */
/* The assignment requires support for up to 8 HRT tasks.                   */
#define TIMELINE_MAX_TASKS          ( 16U )

/* Priority levels. These are passed to xTaskCreate() and must be below     */
/* configMAX_PRIORITIES defined in FreeRTOSConfig.h.                        */
#define TIMELINE_HRT_PRIORITY       ( 3U )
#define TIMELINE_SRT_PRIORITY       ( 2U )
/* Priority 1 is left for the FreeRTOS idle task. Do not use it here.       */

/* Stack depth for timeline-managed tasks. Adjust per application.          */
#define TIMELINE_TASK_STACK_DEPTH   ( configMINIMAL_STACK_SIZE * 2U )

/* Sentinel for uxTimelineIndex inside the TCB. A task whose index equals   */
/* this value is NOT managed by the timeline scheduler (e.g. idle task).    */
/* The tick handler checks this before acting on any TCB.                   */
#define TIMELINE_NO_TASK            ( 0xFFFFFFFFU )

/* Utility macro: convert milliseconds to ticks.                            */
#define TIMELINE_MS_TO_TICKS( ms ) \
    ( ( TickType_t ) ( ( uint32_t )(ms) * configTICK_RATE_HZ / 1000U ) )

/* =========================================================================
 * Public Types
 * ========================================================================= */

/**
 * @brief Task category: hard real-time or soft real-time.
 *
 * Stored in the TCB field xTimelineType for every timeline-managed task.
 */
typedef enum
{
    TASK_TYPE_HARD_RT = 0, /**< Deadline-enforced, non-preemptable by SRT.  */
    TASK_TYPE_SOFT_RT = 1  /**< Best-effort, runs in idle gaps, preemptable. */
} TaskType_t;

/**
 * @brief Scheduler event types logged by the trace module.
 *
 * Defined here (in the kernel header) rather than in trace_module.h so
 * that timeline_scheduler.c can reference them via the extern vTraceRecord()
 * declaration without pulling any application-level header into the kernel.
 */
typedef enum
{
    TRACE_EVENT_TASK_START    = 0, /**< HRT task dispatched into ready list.  */
    TRACE_EVENT_TASK_COMPLETE = 1, /**< HRT task returned before deadline.    */
    TRACE_EVENT_DEADLINE_MISS = 2, /**< HRT task killed at its deadline.      */
    TRACE_EVENT_FRAME_START   = 3, /**< New major frame began.               */
    TRACE_EVENT_SRT_START     = 4, /**< SRT task dispatched into ready list.  */
    TRACE_EVENT_SRT_COMPLETE  = 5, /**< SRT task returned.                   */
} TraceEventType_t;

/**
 * @brief User-facing task configuration.
 *
 * The application fills an array of these and passes it to
 * vConfigureScheduler(). All times are in milliseconds; they are
 * converted to ticks and written into the TCB by vConfigureScheduler().
 *
 * For SOFT_RT tasks, ulStart_ms and ulEnd_ms are ignored — the scheduler
 * runs them in idle gaps in the fixed order they appear in the array.
 */
typedef struct
{
    const char *    pcTaskName;  /**< Human-readable name (for trace).       */
    TaskFunction_t  pvFunction;  /**< Task function pointer.                 */
    TaskType_t      xType;       /**< TASK_TYPE_HARD_RT or SOFT_RT.          */
    uint32_t        ulStart_ms;  /**< HRT slot start time in ms.             */
    uint32_t        ulEnd_ms;    /**< HRT slot end time (deadline) in ms.    */
    UBaseType_t     uxSubframeId;/**< Sub-frame this task belongs to.        */
} TimelineTaskConfig_t;

/**
 * @brief Top-level schedule configuration passed to vConfigureScheduler().
 *
 * The global schedule table (TimelineSchedule_t) inside timeline_scheduler.c
 * is populated from this struct. It stores TaskHandles (and thus TCB
 * pointers) together with frame-level parameters only — all per-task
 * runtime state lives in the TCB fields listed in the file header above.
 */
typedef struct
{
    const TimelineTaskConfig_t * pxTasks;        /**< Array of task configs. */
    UBaseType_t                  uxTaskCount;    /**< Length of pxTasks[].   */
    uint32_t                     ulMajorFrame_ms;/**< Major frame in ms.     */
} TimelineConfig_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Configure and initialise the timeline scheduler.
 *
 * Must be called once from main(), before vTaskStartScheduler().
 * For each task in pxConfig:
 *   - Creates the FreeRTOS task in the suspended state.
 *   - Converts ms times to ticks and writes them into the task's TCB
 *     (xTimelineStartTick, xTimelineEndTick, xTimelineType, etc.).
 *   - Stores the TaskHandle in the global schedule table.
 *
 * @param pxConfig  Pointer to a fully populated TimelineConfig_t.
 *                  Must not be NULL. Must remain valid for the lifetime
 *                  of the application (keep it as a static or global).
 */
void vConfigureScheduler( const TimelineConfig_t * pxConfig );

/**
 * @brief Timeline tick handler — called from xTaskIncrementTick().
 *
 * Implements the frame clock, HRT dispatch, deadline enforcement, and
 * SRT idle-gap scheduling. Reads and writes timeline fields directly on
 * the TCBs of managed tasks (via pxCurrentTCB and the handle table).
 * Returns pdTRUE if pxCurrentTCB has been changed and a context switch
 * is required.
 *
 * Do NOT call this from application code.
 *
 * @param xCurrentTick  The new tick count (post-increment value).
 * @return pdTRUE if a context switch is needed, pdFALSE otherwise.
 */
BaseType_t xTimelineTickHandler( TickType_t xCurrentTick );

/**
 * @brief Called by a task's shim wrapper when the task function returns.
 *
 * Sets xTimelineHasCompleted = pdTRUE directly on the calling task's TCB,
 * then suspends the task by moving it off the ready list — without using
 * the public vTaskSuspend() API (which is unsafe from this context).
 *
 * Tasks must never call this directly. It is invoked automatically by the
 * internal shim wrapper that surrounds every timeline-managed task function.
 *
 * @param xTask  Handle of the completing task, returned by xTaskCreate().
 *               Internally cast to TCB_t * inside timeline_scheduler.c,
 *               where that private type is visible. TCB_t is not exposed
 *               in this header because it is private to tasks.c.
 */
void vTimelineTaskCompleted( TaskHandle_t xTask );

/**
 * @brief Return the TaskHandle_t for a given schedule index.
 *
 * Used by the trace module to retrieve a handle by index so it can call
 * pcTaskGetName() without accessing private kernel types.
 * Returns NULL if uxIndex is out of range.
 *
 * @param uxIndex  Index into the internal schedule handle table.
 * @return TaskHandle_t of the task, or NULL if index is invalid.
 */
TaskHandle_t xTraceGetHandle( UBaseType_t uxIndex );

/**
 * @brief Return the number of major frames completed since scheduler start.
 *
 * Used by the test suite to synchronise with frame boundaries.
 * Incremented inside prvTimelineFrameReset() on every frame tick 0.
 */
UBaseType_t uxTimelineGetFrameCount( void );

/**
 * @brief Snapshot of a task's TCB timeline fields for test inspection.
 */
typedef struct
{
    BaseType_t xIsRunning;    /**< pdTRUE while task is executing.           */
    BaseType_t xHasCompleted; /**< pdTRUE if returned before deadline.       */
    BaseType_t xWasKilled;    /**< pdTRUE if terminated at deadline.         */
    TickType_t xStartTick;    /**< Slot start tick (frame-relative).         */
    TickType_t xEndTick;      /**< Slot end tick / deadline (frame-relative).*/
    TaskType_t xType;         /**< HARD_RT or SOFT_RT.                       */
} TimelineTaskState_t;

/**
 * @brief Fill pxState with a snapshot of a task's TCB timeline fields.
 *
 * Safe to call from task context. Used by the test suite to inspect
 * task outcomes after a frame completes.
 *
 * @param uxIndex   Index into the internal schedule handle table.
 * @param pxState   Pointer to a TimelineTaskState_t to fill.
 * @return pdTRUE on success, pdFALSE if uxIndex is out of range.
 */
BaseType_t xTimelineGetTaskState( UBaseType_t          uxIndex,
                                  TimelineTaskState_t * pxState );

#ifdef __cplusplus
}
#endif

#endif /* TIMELINE_SCHEDULER_H */