/*
 * timeline_scheduler.c
 *
 * Timeline-based deterministic scheduler for FreeRTOS 11.x.
 *
 * COMPILATION NOTE:
 *   This file is NOT compiled as a standalone translation unit.
 *   It is #included at the bottom of tasks.c so that it shares the same
 *   translation unit and has access to private kernel types and variables:
 *       TCB_t, pxReadyTasksLists[], xSuspendedTaskList, pxCurrentTCB.
 *
 *   Do NOT add this file to CMakeLists.txt, Makefile, or any IDE project
 *   as a direct compilation target — doing so will cause "TCB_t undefined"
 *   and similar errors because those types are private to tasks.c.
 *
 *   Required edits in tasks.c:
 *
 *   1) Near the top, with the other includes:
 *          #if ( configUSE_TIMELINE_SCHEDULER == 1 )
 *              #include "timeline_scheduler.h"
 *          #endif
 *
 *   2) At the very bottom of the file:
 *          #if ( configUSE_TIMELINE_SCHEDULER == 1 )
 *              #include "timeline_scheduler.c"
 *          #endif
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

/* This file has no #include directives of its own. All types, macros, and
 * headers it depends on (TCB_t, pxReadyTasksLists, FreeRTOS.h, task.h,
 * timeline_scheduler.h) are already in scope because this file is compiled
 * as part of tasks.c's translation unit. See the compilation note above.   */

#if ( configUSE_TIMELINE_SCHEDULER == 1 )

/* Forward declaration — implementation is in trace_module.c (application
 * layer). Declared here rather than via #include to avoid pulling an
 * application-level header into the kernel's compilation unit.
 * The linker resolves this reference at link time.                          */
extern void vTraceRecord( TickType_t       xTick,
                          TraceEventType_t xEvent,
                          UBaseType_t      uxTaskIndex );

/* =========================================================================
 * Internal types
 * ========================================================================= */

/**
 * @brief Global schedule table.
 *
 * Holds frame-level configuration and the ordered array of task handles.
 * Per-task runtime state (xTimelineIsRunning, xTimelineHasCompleted, etc.)
 * lives directly in each task's TCB — not here.
 */
typedef struct
{
    TaskHandle_t   xHandles[ TIMELINE_MAX_TASKS ];    /**< One per managed task.        */
    TaskFunction_t pvFunctions[ TIMELINE_MAX_TASKS ]; /**< Real function pointer.        */
    UBaseType_t    uxTaskCount;                       /**< Total tasks (HRT + SRT).      */
    UBaseType_t    uxHRTCount;                        /**< Number of HRT tasks.          */
    UBaseType_t    uxSRTCount;                        /**< Number of SRT tasks.          */
    TickType_t     xMajorFrameTicks;                  /**< Frame length in ticks.        */
    UBaseType_t    uxCurrentSRTIndex;                 /**< Next SRT task to dispatch.    */
} TimelineSchedule_t;

/* =========================================================================
 * Private global
 * ========================================================================= */

PRIVILEGED_DATA static TimelineSchedule_t xSchedule;
PRIVILEGED_DATA static volatile UBaseType_t uxFrameCount    = 0U;
PRIVILEGED_DATA static          TickType_t  xFrameBaseTick  = 0U;
PRIVILEGED_DATA static          BaseType_t  xSchedulerReady = pdFALSE;

/* =========================================================================
 * Private function prototypes
 * ========================================================================= */

static void prvTimelineShimTask( void * pvParameters );
static BaseType_t prvTimelineDispatchSRT( void );

/* =========================================================================
 * Private functions
 * ========================================================================= */

/**
 * @brief Shim wrapper executed by every timeline-managed FreeRTOS task.
 *
 * xTaskCreate() receives this function instead of the real task function.
 * The shim calls the real function, handles post-completion bookkeeping,
 * then suspends itself to wait for the next frame reset.
 *
 * The for(;;) loop ensures the task replays correctly on every frame without
 * needing to be recreated.
 *
 * @param pvParameters  The task's index in xSchedule, cast to void *.
 */
static void prvTimelineShimTask( void * pvParameters )
{
    /* Recover the schedule index from the void * parameter.
     * uintptr_t intermediate cast silences pointer-to-integer warnings
     * on 32-bit and 64-bit targets alike.                                   */
    UBaseType_t uxIndex = ( UBaseType_t ) ( uintptr_t ) pvParameters;

    for( ; ; )
    {
        /* -----------------------------------------------------------------
         * Execute the real task function.
         * The function is expected to run to completion and return.
         * It must NOT call vTaskDelay(), vTaskSuspend(), or any blocking
         * FreeRTOS API — the timeline scheduler controls all timing.
         * ----------------------------------------------------------------- */
        xSchedule.pvFunctions[ uxIndex ]( NULL );

        /* -----------------------------------------------------------------
         * The real function has returned. Notify the scheduler that this
         * task completed before its deadline and suspend until frame reset.
         *
         * vTimelineTaskCompleted() is called from task context here, so
         * vTaskSuspend(NULL) inside it is entirely safe — there is no ISR
         * restriction on vTaskSuspend() from a running task.
         * ----------------------------------------------------------------- */
        vTimelineTaskCompleted( xSchedule.xHandles[ uxIndex ] );

        /* Execution does not continue past vTimelineTaskCompleted() until
         * the frame reset in xTimelineTickHandler() resumes this task.
         * The for(;;) then loops back to call the real function again on
         * the next major frame.                                              */
    }
}

/* =========================================================================
 * Public API — implementation
 * ========================================================================= */

/**
 * @brief Mark a task as completed and suspend it until the next frame.
 *
 * Called by prvTimelineShimTask() after the real task function returns.
 * Updates the TCB completion flag then suspends the calling task.
 *
 * Must be called from task context only (not from an ISR).
 */
void vTimelineTaskCompleted( TaskHandle_t xTask )
{
    /* Cast the opaque handle to a TCB pointer. Valid here because this
     * file is compiled inside tasks.c's translation unit where TCB_t
     * is defined.                                                            */
    TCB_t * pxTCB = ( TCB_t * ) xTask;

    configASSERT( pxTCB != NULL );
    configASSERT( pxTCB->uxTimelineIndex != TIMELINE_NO_TASK );

    /* Update runtime state in the TCB */
    pxTCB->xTimelineHasCompleted = pdTRUE;
    pxTCB->xTimelineIsRunning    = pdFALSE;

    /* Log completion before suspending */
    vTraceRecord( xTaskGetTickCount(),
                  ( pxTCB->xTimelineType == TASK_TYPE_HARD_RT )
                      ? TRACE_EVENT_TASK_COMPLETE
                      : TRACE_EVENT_SRT_COMPLETE,
                  pxTCB->uxTimelineIndex );

    /* Suspend self. The frame reset inside xTimelineTickHandler() will
     * resume this task at the start of the next major frame.
     * vTaskSuspend(NULL) is safe here — we are in task context.             */
    vTaskSuspend( NULL );
}

/**
 * @brief Configure and initialise the timeline scheduler.
 *
 * Parses the user-provided TimelineConfig_t, converts times to ticks,
 * creates all managed tasks (initially suspended), and populates both the
 * global schedule table and the TCB timeline fields for each task.
 *
 * Must be called once from main(), before vTaskStartScheduler().
 */
void vConfigureScheduler( const TimelineConfig_t * pxConfig )
{
    UBaseType_t uxIndex;
    UBaseType_t uxHRTCount = 0U;
    UBaseType_t uxSRTCount = 0U;
    BaseType_t  xReturn;

    /* ------------------------------------------------------------------
     * 1. Validate top-level config
     * ------------------------------------------------------------------ */
    configASSERT( pxConfig != NULL );
    configASSERT( pxConfig->pxTasks != NULL );
    configASSERT( pxConfig->uxTaskCount > 0U );
    configASSERT( pxConfig->uxTaskCount <= TIMELINE_MAX_TASKS );
    configASSERT( pxConfig->ulMajorFrame_ms > 0U );

    /* ------------------------------------------------------------------
     * 2. Initialise frame-level fields in the global schedule table
     * ------------------------------------------------------------------ */
    xSchedule.uxTaskCount       = pxConfig->uxTaskCount;
    xSchedule.xMajorFrameTicks  = TIMELINE_MS_TO_TICKS( pxConfig->ulMajorFrame_ms );
    xSchedule.uxCurrentSRTIndex = 0U;

    /* Sanity check: frame must be at least 1 tick */
    configASSERT( xSchedule.xMajorFrameTicks > 0U );

    /* ------------------------------------------------------------------
     * 3. Create each task and populate its TCB timeline fields
     * ------------------------------------------------------------------ */
    for( uxIndex = 0U; uxIndex < pxConfig->uxTaskCount; uxIndex++ )
    {
        const TimelineTaskConfig_t * pxCfg = &pxConfig->pxTasks[ uxIndex ];
        TaskHandle_t xHandle  = NULL;
        TCB_t *      pxTCB;
        UBaseType_t  uxPriority;

        /* Validate individual task entry */
        configASSERT( pxCfg->pvFunction != NULL );
        configASSERT( pxCfg->pcTaskName != NULL );

        if( pxCfg->xType == TASK_TYPE_HARD_RT )
        {
            /* HRT: start must be strictly before end, both within frame */
            configASSERT( pxCfg->ulStart_ms < pxCfg->ulEnd_ms );
            configASSERT( TIMELINE_MS_TO_TICKS( pxCfg->ulEnd_ms )
                          <= xSchedule.xMajorFrameTicks );
            uxPriority = TIMELINE_HRT_PRIORITY;
            uxHRTCount++;
        }
        else
        {
            /* SRT tasks have no fixed slot — timing fields unused */
            uxPriority = TIMELINE_SRT_PRIORITY;
            uxSRTCount++;
        }

        /* Store the real function pointer in the schedule table.
         * The shim reads it from here on every frame iteration.             */
        xSchedule.pvFunctions[ uxIndex ] = pxCfg->pvFunction;

        /* Create the FreeRTOS task. The shim is passed as the task function.
         * The task's index in xSchedule is passed as pvParameters so the
         * shim can identify itself without a global lookup.                  */
        xReturn = xTaskCreate(
            prvTimelineShimTask,                       /* shim wrapper        */
            pxCfg->pcTaskName,
            TIMELINE_TASK_STACK_DEPTH,
            ( void * ) ( uintptr_t ) uxIndex,          /* index as parameter  */
            uxPriority,
            &xHandle
        );

        configASSERT( xReturn == pdPASS );
        configASSERT( xHandle != NULL );

        /* Store handle in the schedule table */
        xSchedule.xHandles[ uxIndex ] = xHandle;

        /* ------------------------------------------------------------------
         * Write timeline parameters directly into the TCB.
         *
         * This cast is valid because:
         *   a) TaskHandle_t is typedef'd as void* pointing to a TCB_t.
         *   b) This file is compiled inside tasks.c's translation unit,
         *      so TCB_t is fully defined and visible here.
         *   c) We are before vTaskStartScheduler() — no concurrency yet.
         * ------------------------------------------------------------------ */
        pxTCB = ( TCB_t * ) xHandle;

        pxTCB->xTimelineType         = pxCfg->xType;
        pxTCB->uxTimelineSubframeId  = pxCfg->uxSubframeId;
        pxTCB->uxTimelineIndex       = uxIndex;
        pxTCB->xTimelineIsRunning    = pdFALSE;
        pxTCB->xTimelineHasCompleted = pdFALSE;
        pxTCB->xTimelineWasKilled    = pdFALSE;

        if( pxCfg->xType == TASK_TYPE_HARD_RT )
        {
            pxTCB->xTimelineStartTick = TIMELINE_MS_TO_TICKS( pxCfg->ulStart_ms );
            pxTCB->xTimelineEndTick   = TIMELINE_MS_TO_TICKS( pxCfg->ulEnd_ms );
        }
        else
        {
            /* SRT: no fixed slot, sentinel values */
            pxTCB->xTimelineStartTick = 0U;
            pxTCB->xTimelineEndTick   = 0U;
        }

        /* Suspend the task immediately. The tick handler will resume it
         * when its slot arrives in the first frame.                         */
        vTaskSuspend( xHandle );
    }

    /* ------------------------------------------------------------------
     * 4. Store final counts
     * ------------------------------------------------------------------ */
    xSchedule.uxHRTCount = uxHRTCount;
    xSchedule.uxSRTCount = uxSRTCount;
}

/* =========================================================================
 * Private helper functions — Step 5
 * ========================================================================= */

/**
 * @brief Move a task off the ready list into the suspended list.
 *
 * Kernel-level equivalent of vTaskSuspend() — safe from tick context.
 * Clears the priority bitmap entry if no other task remains at this
 * priority, so taskSELECT_HIGHEST_PRIORITY_TASK() sees correct state.
 */
static void prvTimelineRemoveTaskFromReadyList( TCB_t * pxTCB )
{
    if( uxListRemove( &( pxTCB->xStateListItem ) ) == ( UBaseType_t ) 0 )
    {
        /* Last task at this priority level — clear the bitmap entry     */
        taskRESET_READY_PRIORITY( pxTCB->uxPriority );
    }

    vListInsertEnd( &xSuspendedTaskList, &( pxTCB->xStateListItem ) );
}

/**
 * @brief Reset all task runtime state at the start of a new major frame.
 *
 * Clears TCB flags and re-suspends all tasks so they start the frame
 * in a known state, ready to be dispatched when their slot arrives.
 * Also resets the SRT round-robin index.
 */
static void prvTimelineFrameReset( void )
{
    UBaseType_t uxIndex;

    /* Log the frame boundary first, before any state is modified */
    vTraceRecord( xTaskGetTickCount(), TRACE_EVENT_FRAME_START, 0U );
    uxFrameCount++;

    for( uxIndex = 0U; uxIndex < xSchedule.uxTaskCount; uxIndex++ )
    {
        TCB_t * pxTCB = ( TCB_t * ) xSchedule.xHandles[ uxIndex ];

        /* Do not touch the currently running task — its shim is still
         * on-CPU and will call vTimelineTaskCompleted() by itself.      */
        if( pxTCB == pxCurrentTCB )
        {
            continue;
        }

        pxTCB->xTimelineIsRunning    = pdFALSE;
        pxTCB->xTimelineHasCompleted = pdFALSE;
        pxTCB->xTimelineWasKilled    = pdFALSE;

        /* Park every task in the suspended list. The tick handler will
         * move each one to the ready list when its slot arrives.         */
        prvTimelineRemoveTaskFromReadyList( pxTCB );
    }

    /* SRT tasks replay in the same fixed order every frame */
    xSchedule.uxCurrentSRTIndex = 0U;
}

/**
 * @brief Returns pdTRUE if any HRT task is currently marked as running.
 */
static BaseType_t prvTimelineHRTActive( void )
{
    UBaseType_t uxIndex;

    for( uxIndex = 0U; uxIndex < xSchedule.uxTaskCount; uxIndex++ )
    {
        TCB_t * pxTCB = ( TCB_t * ) xSchedule.xHandles[ uxIndex ];

        if( ( pxTCB->xTimelineType      == TASK_TYPE_HARD_RT ) &&
            ( pxTCB->xTimelineIsRunning == pdTRUE ) )
        {
            return pdTRUE;
        }
    }

    return pdFALSE;
}

/**
 * @brief Dispatch and enforce deadlines for HRT tasks at xFrameTick.
 *
 * On start tick  → moves the task into the ready list.
 * On end tick    → if still running, removes it and marks deadline miss.
 *
 * @return pdTRUE if a context switch is required.
 */
static BaseType_t prvTimelineDispatchHRT( TickType_t xFrameTick )
{
    UBaseType_t uxIndex;
    BaseType_t  xSwitchRequired = pdFALSE;

    for( uxIndex = 0U; uxIndex < xSchedule.uxTaskCount; uxIndex++ )
    {
        TCB_t * pxTCB = ( TCB_t * ) xSchedule.xHandles[ uxIndex ];

        if( pxTCB->xTimelineType != TASK_TYPE_HARD_RT )
        {
            continue;
        }

        /* --- Start of slot: dispatch the task ------------------------- */
        if( xFrameTick == pxTCB->xTimelineStartTick )
        {
            if( ( pxTCB->xTimelineHasCompleted == pdFALSE ) &&
                ( pxTCB->xTimelineWasKilled    == pdFALSE ) )
            {
                pxTCB->xTimelineIsRunning = pdTRUE;
                ( void ) uxListRemove( &( pxTCB->xStateListItem ) );
                prvAddTaskToReadyList( pxTCB );
                vTraceRecord( xTaskGetTickCount(), TRACE_EVENT_TASK_START, uxIndex );
                xSwitchRequired = pdTRUE;
            }
        }

        /* --- End of slot: enforce deadline ---------------------------- */
        if( xFrameTick == pxTCB->xTimelineEndTick )
        {
            if( pxTCB->xTimelineIsRunning == pdTRUE )
            {
                pxTCB->xTimelineIsRunning = pdFALSE;
                pxTCB->xTimelineWasKilled = pdTRUE;
                prvTimelineRemoveTaskFromReadyList( pxTCB );
                vTraceRecord( xTaskGetTickCount(), TRACE_EVENT_DEADLINE_MISS, uxIndex );
                xSwitchRequired = pdTRUE;
            }
        }
    }

    return xSwitchRequired;
}

/**
 * @brief Dispatch the next pending SRT task in fixed compile-time order.
 *
 * Scans from uxCurrentSRTIndex forward. Skips tasks that have already
 * completed this frame. Called only when no HRT task is active.
 *
 * @return pdTRUE if an SRT task was moved to the ready list.
 */
static BaseType_t prvTimelineDispatchSRT( void )
{
    UBaseType_t uxIndex;
    UBaseType_t uxSRTSeen = 0U;

    for( uxIndex = 0U; uxIndex < xSchedule.uxTaskCount; uxIndex++ )
    {
        TCB_t * pxTCB = ( TCB_t * ) xSchedule.xHandles[ uxIndex ];

        if( pxTCB->xTimelineType != TASK_TYPE_SOFT_RT )
        {
            continue;
        }

        if( uxSRTSeen < xSchedule.uxCurrentSRTIndex )
        {
            /* Not yet at the current SRT position — keep scanning */
            uxSRTSeen++;
            continue;
        }

        /* This is the current candidate */
        if( ( pxTCB->xTimelineHasCompleted == pdFALSE ) &&
            ( pxTCB->xTimelineIsRunning    == pdFALSE ) )
        {
            pxTCB->xTimelineIsRunning = pdTRUE;
            ( void ) uxListRemove( &( pxTCB->xStateListItem ) );
            prvAddTaskToReadyList( pxTCB );
            vTraceRecord( xTaskGetTickCount(), TRACE_EVENT_SRT_START, uxIndex );
            return pdTRUE;
        }
        else
        {
            /* Already done this frame — advance and try next */
            xSchedule.uxCurrentSRTIndex++;
            uxSRTSeen++;
        }
    }

    /* No SRT task available — idle will run */
    return pdFALSE;
}

/**
 * @brief Timeline tick handler — called from xTaskIncrementTick().
 *
 * Implements the frame clock, HRT dispatch, deadline enforcement, and
 * SRT idle-gap scheduling via direct kernel list manipulation.
 *
 * On the very first tick, records xFrameBaseTick so that frame position
 * is computed relative to scheduler start rather than absolute tick 0.
 * This ensures frame 0 is properly initialised regardless of how many
 * ticks elapsed before vTaskStartScheduler() was called.
 *
 * @param xCurrentTick  Post-increment tick count from xTaskIncrementTick().
 * @return pdTRUE if pxCurrentTCB should be updated (context switch needed).
 */
BaseType_t xTimelineTickHandler( TickType_t xCurrentTick )
{
    BaseType_t xSwitchRequired = pdFALSE;
    TickType_t xFrameTick;

    /* ------------------------------------------------------------------ */
    /* 0. First-tick initialisation                                        */
    /*                                                                     */
    /* Record the base tick and perform the frame 0 reset immediately so  */
    /* all tasks start suspended with clean flags before any dispatch.    */
    /* ------------------------------------------------------------------ */
    if( xSchedulerReady == pdFALSE )
    {
        xFrameBaseTick  = 0U;
        xSchedulerReady = pdTRUE;
        prvTimelineFrameReset();
        xSwitchRequired = pdTRUE;
    }

    /* ------------------------------------------------------------------ */
    /* 1. Compute position within the current major frame                 */
    /*    Relative to xFrameBaseTick so frame boundaries are independent  */
    /*    of the absolute tick value at scheduler start.                  */
    /* ------------------------------------------------------------------ */
    xFrameTick = ( xCurrentTick - xFrameBaseTick )
                 % xSchedule.xMajorFrameTicks;

    /* ------------------------------------------------------------------ */
    /* 2. Frame boundary: reset all task state for the new frame.         */
    /*    Guard against re-triggering on xFrameBaseTick itself (already   */
    /*    handled above).                                                  */
    /* ------------------------------------------------------------------ */
    if( ( xFrameTick == 0U ) && ( xCurrentTick != 0U ) )
    {
        prvTimelineFrameReset();
        xSwitchRequired = pdTRUE;
    }

    /* ------------------------------------------------------------------ */
    /* 3. HRT dispatch and deadline enforcement                           */
    /* ------------------------------------------------------------------ */
    if( prvTimelineDispatchHRT( xFrameTick ) == pdTRUE )
    {
        xSwitchRequired = pdTRUE;
    }

    /* ------------------------------------------------------------------ */
    /* 4. SRT dispatch — only when no HRT task is active                  */
    /* ------------------------------------------------------------------ */
    if( prvTimelineHRTActive() == pdFALSE )
    {
        if( prvTimelineDispatchSRT() == pdTRUE )
        {
            xSwitchRequired = pdTRUE;
        }
    }

    /* ------------------------------------------------------------------ */
    /* 5. Update pxCurrentTCB to the highest priority ready task          */
    /*                                                                    */
    /* We reuse the standard macro — our helpers have already placed the  */
    /* correct task into the ready list at the right priority level, so   */
    /* the priority scan will naturally select the task we want.          */
    /* ------------------------------------------------------------------ */
    if( xSwitchRequired == pdTRUE )
    {
        taskSELECT_HIGHEST_PRIORITY_TASK();
    }

    return xSwitchRequired;
}

/**
 * @brief Return the TaskHandle_t for a given schedule index.
 */
TaskHandle_t xTraceGetHandle( UBaseType_t uxIndex )
{
    if( uxIndex < xSchedule.uxTaskCount )
    {
        return xSchedule.xHandles[ uxIndex ];
    }

    return NULL;
}

/**
 * @brief Return the number of major frames completed since scheduler start.
 */
UBaseType_t uxTimelineGetFrameCount( void )
{
    return uxFrameCount;
}

/**
 * @brief Fill pxState with a snapshot of a task's TCB timeline fields.
 */
BaseType_t xTimelineGetTaskState( UBaseType_t          uxIndex,
                                  TimelineTaskState_t * pxState )
{
    TCB_t * pxTCB;

    if( ( uxIndex >= xSchedule.uxTaskCount ) || ( pxState == NULL ) )
    {
        return pdFALSE;
    }

    pxTCB = ( TCB_t * ) xSchedule.xHandles[ uxIndex ];

    pxState->xIsRunning    = pxTCB->xTimelineIsRunning;
    pxState->xHasCompleted = pxTCB->xTimelineHasCompleted;
    pxState->xWasKilled    = pxTCB->xTimelineWasKilled;
    pxState->xStartTick    = pxTCB->xTimelineStartTick;
    pxState->xEndTick      = pxTCB->xTimelineEndTick;
    pxState->xType         = pxTCB->xTimelineType;

    return pdTRUE;
}

#endif /* configUSE_TIMELINE_SCHEDULER */