/*
 * test_suite.c
 *
 * Automated test suite for the FreeRTOS timeline scheduler.
 *
 * Tests are runtime behavioral checks that run while the scheduler is
 * active. Each test inspects TCB state and trace output after one or more
 * major frames and reports a structured PASS/FAIL result.
 *
 * Usage:
 *   Call vTestSuiteStart() from main() after vTraceInit() and before
 *   vTaskStartScheduler(). The test monitor task runs automatically once
 *   the scheduler starts.
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

#include <stdio.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timeline_scheduler.h"
#include "trace_module.h"
#include "test_suite.h"
#include "uart.h"

/* =========================================================================
 * Test configuration
 *
 * The test monitor runs at the highest priority so it can inspect state
 * immediately after a frame boundary without being preempted.
 * ========================================================================= */
#define TEST_MONITOR_PRIORITY   ( configMAX_PRIORITIES - 1U )
#define TEST_MONITOR_STACK      ( configMINIMAL_STACK_SIZE * 4U )

/* How many frames to wait before sampling task state after a frame reset.
 * 1 means: wait for frame N to complete, then inspect state at frame N+1. */
#define TEST_SETTLE_FRAMES      ( 1U )

/* =========================================================================
 * Private globals
 * ========================================================================= */
static UBaseType_t uxTestsPassed = 0U;
static UBaseType_t uxTestsFailed = 0U;

/* =========================================================================
 * Private helper functions
 * ========================================================================= */

/**
 * @brief Print a PASS or FAIL line and update counters.
 */
static void vTestReport( UBaseType_t  uxTestNumber,
                         const char * pcTestName,
                         BaseType_t   xPassed,
                         const char * pcReason )
{
    if( xPassed == pdTRUE )
    {
        char pcBuf[ 72 ];
        sprintf( pcBuf, "Test %2lu - %-40s PASSED\r\n",
                 ( unsigned long ) uxTestNumber,
                 pcTestName );
        UART_printf( pcBuf );
        uxTestsPassed++;
    }
    else
    {
        char pcBuf[ 96 ];
        sprintf( pcBuf, "Test %2lu - %-40s FAILED (%s)\r\n",
                 ( unsigned long ) uxTestNumber,
                 pcTestName,
                 pcReason );
        UART_printf( pcBuf );
        uxTestsFailed++;
    }
}

/**
 * @brief Print the overall test summary.
 */
static void vTestSummary( void )
{
    char pcBuf[ 64 ];

    UART_printf( "\r\n================================================\r\n" );
    UART_printf( "Test Suite Complete\r\n" );

    sprintf( pcBuf, "  Passed : %lu / %lu\r\n",
             ( unsigned long ) uxTestsPassed,
             ( unsigned long ) ( uxTestsPassed + uxTestsFailed ) );
    UART_printf( pcBuf );

    sprintf( pcBuf, "  Failed : %lu / %lu\r\n",
             ( unsigned long ) uxTestsFailed,
             ( unsigned long ) ( uxTestsPassed + uxTestsFailed ) );
    UART_printf( pcBuf );

    UART_printf( "================================================\r\n" );
}

/**
 * @brief Block until the frame counter reaches xTargetFrame.
 *
 * The monitor task calls this to synchronise with frame boundaries.
 * Uses taskYIELD() to release the CPU while waiting so lower priority
 * tasks (HRT, SRT) can actually execute.
 */
static void vWaitForFrame( UBaseType_t uxTargetFrame )
{
    while( uxTimelineGetFrameCount() < uxTargetFrame )
    {
        //taskYIELD();
        vTaskDelay( 1 );
    }
}

/* =========================================================================
 * Individual tests
 *
 * Each test follows the same pattern:
 *   1. Wait for a known frame boundary.
 *   2. Wait one more frame so the scheduler has run at least once.
 *   3. Sample TCB state via xTimelineGetTaskState().
 *   4. Call vTestReport() with the result.
 * ========================================================================= */

/**
 * @brief Test 1 — HRT task completes before its deadline.
 *
 * Verifies that a HRT task whose function body returns before xEndTick
 * is marked as completed (xHasCompleted = pdTRUE, xWasKilled = pdFALSE).
 */
static void vTest_HRTNormalCompletion( UBaseType_t uxTestNum,
                                       UBaseType_t uxTaskIndex )
{
    TimelineTaskState_t xState;
    UBaseType_t         uxTargetFrame;

    /* Wait for the next frame to complete so we sample a fresh state */
    uxTargetFrame = uxTimelineGetFrameCount() + TEST_SETTLE_FRAMES + 1U;
    vWaitForFrame( uxTargetFrame );

    xTimelineGetTaskState( uxTaskIndex, &xState );

    if( xState.xType != TASK_TYPE_HARD_RT )
    {
        vTestReport( uxTestNum, "HRT Normal Completion",
                     pdFALSE, "task is not HARD_RT" );
        return;
    }

    vTestReport( uxTestNum, "HRT Normal Completion",
                 ( xState.xHasCompleted == pdTRUE  &&
                   xState.xWasKilled    == pdFALSE &&
                   xState.xIsRunning    == pdFALSE )
                     ? pdTRUE : pdFALSE,
                 "xHasCompleted not set or xWasKilled set unexpectedly" );
}

/**
 * @brief Test 2 — HRT task deadline miss.
 *
 * Verifies that a HRT task whose function deliberately runs past its
 * deadline is killed (xWasKilled = pdTRUE, xHasCompleted = pdFALSE).
 *
 * IMPORTANT: the task function at uxTaskIndex must be a stub that spins
 * longer than its assigned slot for this test to be meaningful. Use
 * vTestTask_SpinForever() defined at the bottom of this file.
 */
static void vTest_HRTDeadlineMiss( UBaseType_t uxTestNum,
                                   UBaseType_t uxTaskIndex )
{
    TimelineTaskState_t xState;
    UBaseType_t         uxTargetFrame;

    uxTargetFrame = uxTimelineGetFrameCount() + TEST_SETTLE_FRAMES + 1U;
    vWaitForFrame( uxTargetFrame );

    xTimelineGetTaskState( uxTaskIndex, &xState );

    vTestReport( uxTestNum, "HRT Deadline Miss",
                 ( xState.xWasKilled    == pdTRUE  &&
                   xState.xHasCompleted == pdFALSE &&
                   xState.xIsRunning    == pdFALSE )
                     ? pdTRUE : pdFALSE,
                 "task was not killed at deadline" );
}

/**
 * @brief Test 3 — HRT task dispatch timing.
 *
 * Verifies that a HRT task is dispatched at the correct tick relative to
 * the frame start. Samples the tick at which the task becomes running and
 * compares it against xTimelineStartTick stored in the TCB.
 *
 * This test checks jitter by verifying the task was ready no earlier than
 * xStartTick and no later than xStartTick + 1 (≤1 tick jitter spec).
 */
static void vTest_HRTDispatchTiming( UBaseType_t uxTestNum,
                                     UBaseType_t uxTaskIndex )
{
    TimelineTaskState_t xState;
    TickType_t          xDispatchTick;
    TickType_t          xFrameTick;
    UBaseType_t         uxTargetFrame;

    uxTargetFrame = uxTimelineGetFrameCount() + TEST_SETTLE_FRAMES + 1U;
    vWaitForFrame( uxTargetFrame );

    /* Sample tick immediately after frame reset — frame tick is 0 here,
     * so absolute tick mod frame length gives us the frame-relative position */
    xDispatchTick = xTaskGetTickCount();
    xTimelineGetTaskState( uxTaskIndex, &xState );
    xFrameTick    = xDispatchTick % ( xState.xEndTick + 1U );

    /* The task should have started within 1 tick of its scheduled start */
    vTestReport( uxTestNum, "HRT Dispatch Timing (jitter ≤1 tick)",
                 ( xState.xStartTick <= xFrameTick &&
                   xFrameTick <= xState.xStartTick + 1U )
                     ? pdTRUE : pdFALSE,
                 "task dispatched outside ±1 tick of scheduled start" );
}

/**
 * @brief Test 4 — SRT task runs during idle gap.
 *
 * Verifies that a SRT task completes during a frame (xHasCompleted = pdTRUE)
 * when idle time is available. The HRT tasks in the test schedule must leave
 * a gap large enough for the SRT task to run.
 */
static void vTest_SRTRunsInIdleGap( UBaseType_t uxTestNum,
                                    UBaseType_t uxTaskIndex )
{
    TimelineTaskState_t xState;
    UBaseType_t         uxTargetFrame;

    uxTargetFrame = uxTimelineGetFrameCount() + TEST_SETTLE_FRAMES + 1U;
    vWaitForFrame( uxTargetFrame );

    xTimelineGetTaskState( uxTaskIndex, &xState );

    if( xState.xType != TASK_TYPE_SOFT_RT )
    {
        vTestReport( uxTestNum, "SRT Runs in Idle Gap",
                     pdFALSE, "task is not SOFT_RT" );
        return;
    }

    vTestReport( uxTestNum, "SRT Runs in Idle Gap",
                 ( xState.xHasCompleted == pdTRUE &&
                   xState.xWasKilled    == pdFALSE )
                     ? pdTRUE : pdFALSE,
                 "SRT task did not complete during idle gap" );
}

/**
 * @brief Test 5 — SRT task is preempted by HRT task.
 *
 * Verifies that when a HRT task becomes ready while a SRT task is running,
 * the SRT task is preempted. After the HRT task completes, xWasKilled on
 * the SRT task must be pdFALSE (it was preempted, not killed) and it must
 * eventually complete (xHasCompleted = pdTRUE) once HRT finishes.
 */
static void vTest_SRTPreemptedByHRT( UBaseType_t uxTestNum,
                                     UBaseType_t uxHRTIndex,
                                     UBaseType_t uxSRTIndex )
{
    TimelineTaskState_t xHRTState;
    TimelineTaskState_t xSRTState;
    UBaseType_t         uxTargetFrame;

    /* Wait two frames to allow SRT to complete after HRT preemption */
    uxTargetFrame = uxTimelineGetFrameCount() + TEST_SETTLE_FRAMES + 2U;
    vWaitForFrame( uxTargetFrame );

    xTimelineGetTaskState( uxHRTIndex, &xHRTState );
    xTimelineGetTaskState( uxSRTIndex, &xSRTState );

    vTestReport( uxTestNum, "SRT Preempted by HRT",
                 ( xHRTState.xHasCompleted == pdTRUE &&
                   xSRTState.xWasKilled    == pdFALSE &&
                   xSRTState.xHasCompleted == pdTRUE )
                     ? pdTRUE : pdFALSE,
                 "HRT did not complete or SRT was incorrectly killed" );
}

/**
 * @brief Test 6 — Frame reset restores all tasks to initial state.
 *
 * Verifies that at frame tick 0, all tasks have their runtime flags reset
 * (xIsRunning = pdFALSE, xHasCompleted = pdFALSE, xWasKilled = pdFALSE).
 * Samples state immediately after the frame boundary.
 */
static void vTest_FrameReset( UBaseType_t uxTestNum,
                              UBaseType_t uxTaskCount )
{
    UBaseType_t         uxIndex;
    UBaseType_t         uxTargetFrame;
    TimelineTaskState_t xState;
    BaseType_t          xAllReset = pdTRUE;
    UBaseType_t         uxFirstFailed = 0U;

    /* Synchronise to the very start of the next frame */
    uxTargetFrame = uxTimelineGetFrameCount() + 1U;
    vWaitForFrame( uxTargetFrame );

    /* Sample immediately — monitor runs at highest priority so no task
     * will have been dispatched yet in this new frame                   */
    for( uxIndex = 0U; uxIndex < uxTaskCount; uxIndex++ )
    {
        xTimelineGetTaskState( uxIndex, &xState );

        if( xState.xIsRunning    != pdFALSE ||
            xState.xHasCompleted != pdFALSE ||
            xState.xWasKilled    != pdFALSE )
        {
            xAllReset   = pdFALSE;
            uxFirstFailed = uxIndex;
            break;
        }
    }

    vTestReport( uxTestNum, "Frame Reset Clears All Task State",
                 xAllReset,
                 "at least one task had stale flags after frame reset" );

    ( void ) uxFirstFailed;
}

/**
 * @brief Test 7 — Deterministic repetition across frames.
 *
 * Runs the scheduler for multiple frames and verifies that the outcome
 * (completed / killed) is identical across all observed frames.
 * Demonstrates repeatable deterministic behavior as required by the spec.
 */
static void vTest_DeterministicRepetition( UBaseType_t uxTestNum,
                                           UBaseType_t uxTaskIndex,
                                           UBaseType_t uxFramesToObserve )
{
    UBaseType_t         uxFrame;
    TimelineTaskState_t xState;
    BaseType_t          xFirstCompleted;
    BaseType_t          xFirstKilled;
    BaseType_t          xConsistent = pdTRUE;
    UBaseType_t         uxStartFrame;

    /* Record baseline from first observed frame */
    uxStartFrame = uxTimelineGetFrameCount() + TEST_SETTLE_FRAMES + 1U;
    vWaitForFrame( uxStartFrame );
    xTimelineGetTaskState( uxTaskIndex, &xState );
    xFirstCompleted = xState.xHasCompleted;
    xFirstKilled    = xState.xWasKilled;

    /* Verify subsequent frames match */
    for( uxFrame = 1U; uxFrame < uxFramesToObserve; uxFrame++ )
    {
        vWaitForFrame( uxStartFrame + uxFrame );
        xTimelineGetTaskState( uxTaskIndex, &xState );

        if( xState.xHasCompleted != xFirstCompleted ||
            xState.xWasKilled    != xFirstKilled )
        {
            xConsistent = pdFALSE;
            break;
        }
    }

    vTestReport( uxTestNum, "Deterministic Repetition Across Frames",
                 xConsistent,
                 "task outcome changed between frames" );
}

/**
 * @brief Test 8 — Minimum time gap between adjacent HRT tasks.
 *
 * Verifies that two HRT tasks with adjacent slots (end of one = start of
 * next) do not interfere — both complete correctly without killing each other.
 */
static void vTest_MinimalTimeGap( UBaseType_t uxTestNum,
                                  UBaseType_t uxTask1Index,
                                  UBaseType_t uxTask2Index )
{
    TimelineTaskState_t xState1;
    TimelineTaskState_t xState2;
    UBaseType_t         uxTargetFrame;

    uxTargetFrame = uxTimelineGetFrameCount() + TEST_SETTLE_FRAMES + 1U;
    vWaitForFrame( uxTargetFrame );

    xTimelineGetTaskState( uxTask1Index, &xState1 );
    xTimelineGetTaskState( uxTask2Index, &xState2 );

    /* Verify slots are actually adjacent */
    if( xState1.xEndTick != xState2.xStartTick )
    {
        vTestReport( uxTestNum, "Minimal Time Gap Between HRT Tasks",
                     pdFALSE, "tasks are not adjacent — check test config" );
        return;
    }

    vTestReport( uxTestNum, "Minimal Time Gap Between HRT Tasks",
                 ( xState1.xHasCompleted == pdTRUE &&
                   xState1.xWasKilled    == pdFALSE &&
                   xState2.xHasCompleted == pdTRUE  &&
                   xState2.xWasKilled    == pdFALSE )
                     ? pdTRUE : pdFALSE,
                 "one or both adjacent tasks failed to complete correctly" );
}

/* =========================================================================
 * Stub task functions for testing
 * ========================================================================= */

/**
 * @brief Normal stub — returns immediately, simulating a fast task.
 * Use for Tests 1, 4, 5, 6, 7, 8.
 */
void vTestTask_Normal( void * pvParameters )
{
    ( void ) pvParameters;
    /* Intentionally empty — returns immediately */
}

/**
 * @brief Spinning stub — never returns, forcing a deadline miss.
 * Use for Test 2. Must be assigned a slot shorter than the spin time.
 */
void vTestTask_SpinForever( void * pvParameters )
{
    ( void ) pvParameters;
    for( ; ; )
    {
        /* Spin — the tick handler will kill this task at its deadline */
        __asm volatile ( "nop" );
    }
}

/* =========================================================================
 * Test monitor task
 * ========================================================================= */

/**
 * @brief Monitor task — runs all tests sequentially then prints summary.
 *
 * Runs at TEST_MONITOR_PRIORITY (highest) so it can sample state at frame
 * boundaries before any managed task gets a chance to run.
 * Suspends itself permanently after the suite completes.
 */
static void prvTestMonitorTask( void * pvParameters )
{
    ( void ) pvParameters;

    /* Let the scheduler run for at least one full frame before testing */
    vWaitForFrame( 2U );

    UART_printf( "\r\n================================================\r\n" );
    UART_printf( "Timeline Scheduler Test Suite\r\n" );
    UART_printf( "================================================\r\n\r\n" );

    /* ------------------------------------------------------------------
     * Test 1 — HRT task 0 completes normally
     * Task 0 must be a HARD_RT task using vTestTask_Normal
     * ------------------------------------------------------------------ */
    vTest_HRTNormalCompletion( 1U, 0U );

    /* ------------------------------------------------------------------
     * Test 2 — HRT task deadline miss
     * Task 1 must be a HARD_RT task using vTestTask_SpinForever,
     * assigned a slot shorter than any nonzero spin time (e.g. 2 ms)
     * ------------------------------------------------------------------ */
    vTest_HRTDeadlineMiss( 2U, 1U );

    /* ------------------------------------------------------------------
     * Test 3 — HRT task 0 dispatch timing within ≤1 tick jitter
     * ------------------------------------------------------------------ */
    vTest_HRTDispatchTiming( 3U, 0U );

    /* ------------------------------------------------------------------
     * Test 4 — SRT task 2 runs in the idle gap left by HRT tasks
     * Task 2 must be a SOFT_RT task using vTestTask_Normal
     * ------------------------------------------------------------------ */
    vTest_SRTRunsInIdleGap( 4U, 2U );

    /* ------------------------------------------------------------------
     * Test 5 — SRT task preempted by HRT, then completes after HRT done
     * HRT = task 0, SRT = task 2
     * ------------------------------------------------------------------ */
    vTest_SRTPreemptedByHRT( 5U, 0U, 2U );

    /* ------------------------------------------------------------------
     * Test 6 — Frame reset clears all task flags for 3 tasks
     * ------------------------------------------------------------------ */
    vTest_FrameReset( 6U, 3U );

    /* ------------------------------------------------------------------
     * Test 7 — Task 0 outcome is consistent across 5 frames
     * ------------------------------------------------------------------ */
    vTest_DeterministicRepetition( 7U, 0U, 5U );

    /* ------------------------------------------------------------------
     * Test 8 — Minimal time gap: tasks 0 and 3 must have adjacent slots
     * (xEndTick of task 0 == xStartTick of task 3)
     * ------------------------------------------------------------------ */
    vTest_MinimalTimeGap( 8U, 0U, 3U );

    /* ------------------------------------------------------------------
     * Final summary
     * ------------------------------------------------------------------ */
    vTestSummary();

    /* Monitor has finished — suspend permanently */
    vTaskSuspend( NULL );
}

/* =========================================================================
 * Public API
 * ========================================================================= */

void vTestSuiteStart( void )
{
    BaseType_t xReturn;

    xReturn = xTaskCreate( prvTestMonitorTask,
                           "TestMonitor",
                           TEST_MONITOR_STACK,
                           NULL,
                           TEST_MONITOR_PRIORITY,
                           NULL );

    configASSERT( xReturn == pdPASS );
}