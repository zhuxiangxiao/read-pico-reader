#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include "esp_err.h"
esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *total_bytes, uint64_t *free_bytes);

#ifdef __cplusplus
}
#endif