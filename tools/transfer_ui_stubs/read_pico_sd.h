#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
typedef struct { bool mounted; uint64_t capacity_bytes, free_bytes; } read_pico_sd_info_t;
static bool test_sd_mounted = true;
static inline esp_err_t read_pico_sd_get_info(read_pico_sd_info_t *info) {
    info->mounted = test_sd_mounted;
    info->capacity_bytes = 64ULL * 1024 * 1024 * 1024;
    info->free_bytes = test_sd_mounted ? 32ULL * 1024 * 1024 * 1024 : 0;
    return ESP_OK;
}
