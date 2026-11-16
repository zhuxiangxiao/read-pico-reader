/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead host test FreeRTOS task mock definitions.
 */
#pragma once
#include "FreeRTOS.h"
#include <unistd.h>

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

static inline BaseType_t xTaskCreate(TaskFunction_t pxTaskCode,
                                     const char * const pcName,
                                     const uint32_t usStackDepth,
                                     void * const pvParameters,
                                     UBaseType_t uxPriority,
                                     TaskHandle_t * const pxCreatedTask)
{
    (void)pxTaskCode;
    (void)pcName;
    (void)usStackDepth;
    (void)pvParameters;
    (void)uxPriority;
    if (pxCreatedTask) {
        *pxCreatedTask = (TaskHandle_t)0x1000;
    }
    return pdPASS;
}

static inline void vTaskDelete(TaskHandle_t xTask)
{
    (void)xTask;
}

static inline void vTaskDelay(const TickType_t xTicksToDelay)
{
    usleep((useconds_t)xTicksToDelay * 1000);
}
