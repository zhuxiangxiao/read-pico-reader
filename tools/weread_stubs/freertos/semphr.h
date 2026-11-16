/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead host test FreeRTOS semaphore mock definitions.
 */
#pragma once
#include "FreeRTOS.h"
#include <pthread.h>
#include <stdlib.h>

typedef void *SemaphoreHandle_t;

typedef struct {
    pthread_mutex_t mtx;
    bool valid;
} weread_mock_mutex_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    weread_mock_mutex_t *m = (weread_mock_mutex_t *)malloc(sizeof(weread_mock_mutex_t));
    if (!m) return NULL;
    pthread_mutex_init(&m->mtx, NULL);
    m->valid = true;
    return (SemaphoreHandle_t)m;
}

static inline BaseType_t xSemaphoreTake(SemaphoreHandle_t xSemaphore, TickType_t xBlockTime)
{
    (void)xBlockTime;
    if (!xSemaphore) return pdFAIL;
    weread_mock_mutex_t *m = (weread_mock_mutex_t *)xSemaphore;
    return (pthread_mutex_lock(&m->mtx) == 0) ? pdPASS : pdFAIL;
}

static inline BaseType_t xSemaphoreGive(SemaphoreHandle_t xSemaphore)
{
    if (!xSemaphore) return pdFAIL;
    weread_mock_mutex_t *m = (weread_mock_mutex_t *)xSemaphore;
    return (pthread_mutex_unlock(&m->mtx) == 0) ? pdPASS : pdFAIL;
}

static inline void vSemaphoreDelete(SemaphoreHandle_t xSemaphore)
{
    if (!xSemaphore) return;
    weread_mock_mutex_t *m = (weread_mock_mutex_t *)xSemaphore;
    pthread_mutex_destroy(&m->mtx);
    free(m);
}
