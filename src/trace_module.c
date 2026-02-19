/*
 * trace_module.c
 *
 * Tick-level trace and monitoring module for the FreeRTOS timeline scheduler.
 *
 * This file IS compiled as a standalone translation unit (unlike
 * timeline_scheduler.c). It only uses the public FreeRTOS API and the
 * public timeline_scheduler.h interface — no private kernel types needed.
 *
 * Add it to your build system (CMakeLists.txt / Makefile) as a normal
 * source file alongside main.c.
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
#include "trace_module.h"
#include "timeline_scheduler.h"
#include "uart.h"

#if ( configUSE_TIMELINE_SCHEDULER == 1 )

/* =========================================================================
 * Private types
 * ========================================================================= */

/**
 * @brief A single trace record stored in the ring buffer.
 *
 * Kept intentionally small (12 bytes) so that writing one from ISR context
 * is fast and does not stress the ring buffer capacity.
 */
typedef struct
{
    TickType_t       xTick;        /**< Absolute tick when event occurred.   */
    TraceEventType_t xEvent;       /**< Event classification.                */
    UBaseType_t      uxTaskIndex;  /**< Index in the schedule handle table.  */
} TraceRecord_t;

/* =========================================================================
 * Private globals
 * ========================================================================= */

PRIVILEGED_DATA static TraceRecord_t xTraceBuffer[ TRACE_BUFFER_SIZE ];

/* Head: written by vTraceRecord() in ISR/tick context.
 * Tail: read and advanced by the drain task in task context.
 * On single-core Cortex-M, 32-bit reads and writes are atomic, so no
 * mutex is required — volatile is sufficient for visibility.               */
static volatile UBaseType_t uxTraceHead = 0U;
static volatile UBaseType_t uxTraceTail = 0U;

/* Human-readable names for each event type. Index matches TraceEventType_t. */
static const char * const pcEventNames[] =
{
    "START",         /* TRACE_EVENT_TASK_START    */
    "COMPLETE",      /* TRACE_EVENT_TASK_COMPLETE */
    "DEADLINE_MISS", /* TRACE_EVENT_DEADLINE_MISS */
    "FRAME_START",   /* TRACE_EVENT_FRAME_START   */
    "SRT_START",     /* TRACE_EVENT_SRT_START     */
    "SRT_COMPLETE",  /* TRACE_EVENT_SRT_COMPLETE  */
};

/* =========================================================================
 * Private functions
 * ========================================================================= */

/**
 * @brief Drain task — reads the ring buffer and prints each record.
 *
 * Runs at TRACE_DRAIN_PRIORITY (1), below all timeline-managed tasks.
 * Calls printf() only from task context, never from ISR context.
 * Yields after each drain pass to avoid starving the idle task.
 */
static void prvTraceDrainTask( void * pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        /* Drain all pending records in one pass */
        while( uxTraceTail != uxTraceHead )
        {
            /* Read the record at the current tail */
            TraceRecord_t xRecord = xTraceBuffer[ uxTraceTail ];

            /* Advance tail before printing to minimise the window during
             * which the ISR could overwrite this slot.                   */
            uxTraceTail = ( uxTraceTail + 1U )
                          & ( ( UBaseType_t ) TRACE_BUFFER_SIZE - 1U );

            /* Resolve task name from the public handle API.
             * pcTaskGetName() is safe from task context.                 */
            const char * pcName;

            if( xRecord.xEvent == TRACE_EVENT_FRAME_START )
            {
                pcName = "---";
            }
            else
            {
                TaskHandle_t xHandle = xTraceGetHandle( xRecord.uxTaskIndex );

                if( xHandle != NULL )
                {
                    pcName = pcTaskGetName( xHandle );
                }
                else
                {
                    pcName = "UNKNOWN";
                }
            }

            {
                char pcBuf[ 64 ];
                sprintf( pcBuf, "[ %4lu ms ] %-16s %s\r\n",
                         ( unsigned long ) xRecord.xTick,
                         pcName,
                         pcEventNames[ ( UBaseType_t ) xRecord.xEvent ] );
                UART_printf( pcBuf );
            }
        }

        /* Yield after draining — lets lower priority idle task run and
         * avoids spinning when the buffer is empty.                      */
        taskYIELD();
    }
}

/* =========================================================================
 * Public API — implementation
 * ========================================================================= */

/**
 * @brief Write a trace event into the ring buffer.
 *
 * Called from tick context — must be non-blocking and allocation-free.
 * If the buffer is full, the oldest record is silently dropped by
 * advancing the tail. The scheduler must never stall waiting for output.
 */
void vTraceRecord( TickType_t       xTick,
                   TraceEventType_t xEvent,
                   UBaseType_t      uxTaskIndex )
{
    UBaseType_t uxNext = ( uxTraceHead + 1U )
                         & ( ( UBaseType_t ) TRACE_BUFFER_SIZE - 1U );

    /* Buffer full: drop oldest record to make room */
    if( uxNext == uxTraceTail )
    {
        uxTraceTail = ( uxTraceTail + 1U )
                      & ( ( UBaseType_t ) TRACE_BUFFER_SIZE - 1U );
    }

    xTraceBuffer[ uxTraceHead ].xTick       = xTick;
    xTraceBuffer[ uxTraceHead ].xEvent      = xEvent;
    xTraceBuffer[ uxTraceHead ].uxTaskIndex = uxTaskIndex;

    /* Advance head last so the drain task only sees complete records */
    uxTraceHead = uxNext;
}

/**
 * @brief Initialise the trace module and create the drain task.
 *
 * Must be called from main() after vConfigureScheduler() and before
 * vTaskStartScheduler().
 */
void vTraceInit( void )
{
    BaseType_t xReturn;

    uxTraceHead = 0U;
    uxTraceTail = 0U;

    xReturn = xTaskCreate( prvTraceDrainTask,
                           "TraceDrain",
                           ( configMINIMAL_STACK_SIZE * 2U ),
                           NULL,
                           TRACE_DRAIN_PRIORITY,
                           NULL );

    configASSERT( xReturn == pdPASS );
}

#endif /* configUSE_TIMELINE_SCHEDULER */