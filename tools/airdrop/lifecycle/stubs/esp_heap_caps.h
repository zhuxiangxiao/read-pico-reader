#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
#define MALLOC_CAP_8BIT (1u << 2)
void *heap_caps_malloc(size_t n, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps);
void *heap_caps_realloc(void *p, size_t n, uint32_t caps);
void heap_caps_free(void *p);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
uint32_t esp_get_free_internal_heap_size(void);

#ifdef __cplusplus
}
#endif