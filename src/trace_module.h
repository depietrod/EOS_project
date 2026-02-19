/*
 * trace_module.h
 *
 * Tick-level trace and monitoring module for the FreeRTOS timeline scheduler.
 *
 * Architecture:
 *   - vTraceRecord() writes fixed-size records into a ring buffer.
 *     It is safe to call from tick/ISR context: non-blocking, no allocation,
 *     bounded execution time.
 *   - A low-priority drain task reads the ring buffer and calls printf()
 *     at leisure, never interfering with HRT or SRT task scheduling.
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

#ifndef TRACE_MODULE_H
#define TRACE_MODULE_H

#include "FreeRTOS.h"
#include "task.h"
#include "timeline_scheduler.h"

#ifdef __cplusplus
extern "C" {
#endif

#if ( configUSE_TIMELINE_SCHEDULER == 1 )

/* =========================================================================
 * Constants
 * ========================================================================= */

/* Ring buffer capacity. Must be a power of 2 — the wrap-around uses
 * bitwise AND instead of modulo for efficiency in ISR context.
 * 128 records covers a full 100 ms frame at 1 ms resolution with room
 * to spare before the drain task empties the buffer.                        */
#define TRACE_BUFFER_SIZE       ( 128U )

/* Priority of the drain task. Must be lower than TIMELINE_SRT_PRIORITY (2)
 * so it never preempts any timeline-managed task.                           */
#define TRACE_DRAIN_PRIORITY    ( 1U )

/* =========================================================================
 * Public Types
 * ========================================================================= */

/* TraceEventType_t is defined in timeline_scheduler.h and re-exported here
 * via that include. It lives in the kernel header so that timeline_scheduler.c
 * can reference it through the extern vTraceRecord() declaration without
 * pulling any application-level header into the kernel translation unit.    */

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief Initialise the trace module and create the drain task.
 *
 * Must be called from main() after vConfigureScheduler() and before
 * vTaskStartScheduler(). Resets the ring buffer and creates the low-priority
 * drain task that formats and prints log records.
 */
void vTraceInit( void );

/**
 * @brief Write a trace event record into the ring buffer.
 *
 * Safe to call from tick/ISR context. If the buffer is full, the oldest
 * record is silently overwritten — the scheduler never blocks.
 *
 * @param xTick        Tick timestamp of the event.
 * @param xEvent       Event type (see TraceEventType_t).
 * @param uxTaskIndex  Index of the task in the schedule handle table.
 *                     Pass 0 for frame-level events (TRACE_EVENT_FRAME_START).
 */
void vTraceRecord( TickType_t       xTick,
                   TraceEventType_t xEvent,
                   UBaseType_t      uxTaskIndex );

#endif /* configUSE_TIMELINE_SCHEDULER */

#ifdef __cplusplus
}
#endif

#endif /* TRACE_MODULE_H */