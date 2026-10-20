#pragma once
#include <stdio.h>
#define ESP_LOGE(tag, fmt, ...) ((void)tag, (void)printf(fmt, ##__VA_ARGS__))
#define ESP_LOGW(tag, fmt, ...) ((void)tag, (void)printf(fmt, ##__VA_ARGS__))
#define ESP_LOGI(tag, fmt, ...) ((void)tag, (void)printf(fmt, ##__VA_ARGS__))
#define ESP_LOGD(tag, fmt, ...) ((void)tag, (void)printf(fmt, ##__VA_ARGS__))
#define ESP_LOGV(tag, fmt, ...) ((void)tag, (void)printf(fmt, ##__VA_ARGS__))
