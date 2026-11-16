/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead host test heap capability mocks.
 */
#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>

#define MALLOC_CAP_SPIRAM   (1 << 0)
#define MALLOC_CAP_INTERNAL (1 << 1)
#define MALLOC_CAP_8BIT     (1 << 2)

#ifdef WEREAD_TEST_MEMORY
void *heap_caps_malloc(size_t size,uint32_t caps);
void *heap_caps_calloc(size_t n,size_t size,uint32_t caps);
void *heap_caps_realloc(void *pointer,size_t size,uint32_t caps);
void heap_caps_free(void *pointer);
size_t heap_caps_get_free_size(uint32_t caps);
size_t heap_caps_get_largest_free_block(uint32_t caps);
#else

static inline void *heap_caps_malloc(size_t size, uint32_t caps) {
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_calloc(size_t n, size_t size, uint32_t caps) {
    (void)caps;
    return calloc(n, size);
}

static inline void heap_caps_free(void *ptr) {
    free(ptr);
}

static inline void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps) {
    (void)caps;
    return realloc(ptr, size);
}

static inline size_t heap_caps_get_free_size(uint32_t caps) {
    (void)caps;
    return 1024 * 1024 * 8; // 8 MiB mock free size
}

static inline size_t heap_caps_get_largest_free_block(uint32_t caps) {
    (void)caps;
    return 1024 * 1024 * 4; // 4 MiB mock largest block
}
#endif
