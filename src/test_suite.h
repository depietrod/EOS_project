/*
 * test_suite.h
 *
 * Public interface for the timeline scheduler automated test suite.
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

#ifndef TEST_SUITE_H
#define TEST_SUITE_H

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the test monitor task.
 *
 * Must be called from main() after vConfigureScheduler() and vTraceInit(),
 * and before vTaskStartScheduler(). The monitor task runs all tests
 * automatically once the scheduler starts and prints results via printf().
 */
void vTestSuiteStart( void );

/**
 * @brief Normal stub task — returns immediately.
 *
 * Use this as the task function for HRT/SRT tasks in tests that verify
 * normal completion behavior. Declare in TimelineTaskConfig_t as pvFunction.
 */
void vTestTask_Normal( void * pvParameters );

/**
 * @brief Spinning stub task — never returns.
 *
 * Use this as the task function for HRT tasks in tests that verify
 * deadline miss behavior. The tick handler will kill this task at its
 * deadline, setting xWasKilled = pdTRUE in the TCB.
 */
void vTestTask_SpinForever( void * pvParameters );

#ifdef __cplusplus
}
#endif

#endif /* TEST_SUITE_H */