#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *out, BaseType_t core);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                           void *arg, UBaseType_t prio, StackType_t *stackbuf,
                                           StaticTask_t *tcb, BaseType_t core);

#ifdef __cplusplus
}
#endif