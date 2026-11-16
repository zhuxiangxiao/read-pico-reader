/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead host test FreeRTOS mock definitions.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef uint32_t TickType_t;
typedef int32_t  BaseType_t;
typedef uint32_t UBaseType_t;

#define pdTRUE      1
#define pdFALSE     0
#define pdPASS      1
#define pdFAIL      0

#define portMAX_DELAY       0xFFFFFFFFu
#define portTICK_PERIOD_MS  1
#define pdMS_TO_TICKS(ms)   ((TickType_t)(ms))
