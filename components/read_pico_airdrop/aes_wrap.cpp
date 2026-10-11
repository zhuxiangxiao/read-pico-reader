/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：AES 硬件加速链接器包装，将 PSRAM 中的 TLS 记录缓冲切分到对齐暂存区，避免内部 DMA 弹跳缓冲分配失败。
 *
 * English: Linker wrappers for ESP-IDF hardware AES that stage unaligned or PSRAM TLS
 * buffers through cache-line-aligned scratch memory, avoiding per-call internal DMA bounce allocations.
 *
 * 冻结：不得在每次调用时动态申请内部 DMA 缓冲；所有硬件调用必须串行化访问静态暂存区。
 * Frozen: Never allocate per-call internal DMA bounce buffers; serialize access to the static scratch buffers.
 */

#include <aes/esp_aes.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdint.h>
#include <string.h>

extern "C" {

int __real_esp_aes_crypt_ecb(esp_aes_context *ctx, int mode, const unsigned char input[16], unsigned char output[16]);
int __real_esp_aes_crypt_cbc(esp_aes_context *ctx, int mode, size_t length, unsigned char iv[16],
                             const unsigned char *input, unsigned char *output);
int __real_esp_aes_crypt_ctr(esp_aes_context *ctx, size_t length, size_t *nc_off, unsigned char nonce_counter[16],
                             unsigned char stream_block[16], const unsigned char *input, unsigned char *output);

// ESP32-S3 数据缓存行大小为 64 字节；EDMA 访问 PSRAM 要求起止地址与长度均按 64 字节对齐。
// ESP32-S3 data cache lines are 64 bytes; EDMA over PSRAM requires 64-byte aligned addresses and lengths.
static constexpr size_t kCacheLine = 64;
static constexpr size_t kBulkChunk = 8192;
static constexpr size_t kTailChunk = kCacheLine;

static uint8_t *s_bulk_in = nullptr;
static uint8_t *s_bulk_out = nullptr;
static uint8_t *s_tail_in = nullptr;
static uint8_t *s_tail_out = nullptr;
static StaticSemaphore_t s_aes_mux_buf;
static SemaphoreHandle_t s_aes_mux = nullptr;

static bool ensure_scratch(void) {
    if (!s_aes_mux) {
        s_aes_mux = xSemaphoreCreateMutexStatic(&s_aes_mux_buf);
    }
    if (!s_bulk_in) {
        s_bulk_in = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(kCacheLine, kBulkChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!s_bulk_out) {
        s_bulk_out = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(kCacheLine, kBulkChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }
    if (!s_tail_in) {
        s_tail_in = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(16, kTailChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    }
    if (!s_tail_out) {
        s_tail_out = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(16, kTailChunk, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    }
    return s_aes_mux && s_tail_in && s_tail_out;
}

static bool buf_needs_bounce(const void *ptr) {
    if (esp_ptr_dma_capable(ptr)) {
        return false;
    }
    if (esp_ptr_external_ram(ptr)) {
        return (reinterpret_cast<uintptr_t>(ptr) & (kCacheLine - 1)) != 0;
    }
    return true;
}

int __wrap_esp_aes_crypt_ecb(esp_aes_context *ctx, int mode, const unsigned char input[16], unsigned char output[16]) {
    if (!buf_needs_bounce(input) && !buf_needs_bounce(output)) {
        return __real_esp_aes_crypt_ecb(ctx, mode, input, output);
    }
    if (!ensure_scratch()) {
        return -1;
    }
    xSemaphoreTake(s_aes_mux, portMAX_DELAY);
    memcpy(s_tail_in, input, 16);
    int ret = __real_esp_aes_crypt_ecb(ctx, mode, s_tail_in, s_tail_out);
    if (ret == 0) {
        memcpy(output, s_tail_out, 16);
    }
    xSemaphoreGive(s_aes_mux);
    return ret;
}

int __wrap_esp_aes_crypt_cbc(esp_aes_context *ctx, int mode, size_t length, unsigned char iv[16],
                             const unsigned char *input, unsigned char *output) {
    if (length == 0) {
        return 0;
    }
    if ((length % 16) != 0) {
        return ERR_ESP_AES_INVALID_INPUT_LENGTH;
    }
    const bool aligned_len = (length & (kCacheLine - 1)) == 0;
    if (aligned_len && !buf_needs_bounce(input) && !buf_needs_bounce(output)) {
        return __real_esp_aes_crypt_cbc(ctx, mode, length, iv, input, output);
    }
    if (!ensure_scratch()) {
        return -1;
    }

    xSemaphoreTake(s_aes_mux, portMAX_DELAY);
    int ret = 0;
    size_t offset = 0;

    if (s_bulk_in && s_bulk_out) {
        size_t bulk_total = length & ~(kCacheLine - 1);
        while (offset < bulk_total && ret == 0) {
            size_t chunk = bulk_total - offset;
            if (chunk > kBulkChunk) {
                chunk = kBulkChunk;
            }
            memcpy(s_bulk_in, input + offset, chunk);
            ret = __real_esp_aes_crypt_cbc(ctx, mode, chunk, iv, s_bulk_in, s_bulk_out);
            if (ret == 0) {
                memcpy(output + offset, s_bulk_out, chunk);
            }
            offset += chunk;
        }
    }

    while (offset < length && ret == 0) {
        size_t chunk = length - offset;
        if (chunk > kTailChunk) {
            chunk = kTailChunk;
        }
        memcpy(s_tail_in, input + offset, chunk);
        ret = __real_esp_aes_crypt_cbc(ctx, mode, chunk, iv, s_tail_in, s_tail_out);
        if (ret == 0) {
            memcpy(output + offset, s_tail_out, chunk);
        }
        offset += chunk;
    }

    xSemaphoreGive(s_aes_mux);
    return ret;
}

int __wrap_esp_aes_crypt_ctr(esp_aes_context *ctx, size_t length, size_t *nc_off, unsigned char nonce_counter[16],
                             unsigned char stream_block[16], const unsigned char *input, unsigned char *output) {
    if (length == 0) {
        return 0;
    }
    const bool aligned_len = (length & (kCacheLine - 1)) == 0;
    if (aligned_len && !buf_needs_bounce(input) && !buf_needs_bounce(output)) {
        return __real_esp_aes_crypt_ctr(ctx, length, nc_off, nonce_counter, stream_block, input, output);
    }
    if (!ensure_scratch()) {
        return -1;
    }

    xSemaphoreTake(s_aes_mux, portMAX_DELAY);
    int ret = 0;
    size_t offset = 0;

    if (s_bulk_in && s_bulk_out && (*nc_off == 0)) {
        size_t bulk_total = length & ~(kCacheLine - 1);
        while (offset < bulk_total && ret == 0) {
            size_t chunk = bulk_total - offset;
            if (chunk > kBulkChunk) {
                chunk = kBulkChunk;
            }
            memcpy(s_bulk_in, input + offset, chunk);
            ret = __real_esp_aes_crypt_ctr(ctx, chunk, nc_off, nonce_counter, stream_block, s_bulk_in, s_bulk_out);
            if (ret == 0) {
                memcpy(output + offset, s_bulk_out, chunk);
            }
            offset += chunk;
        }
    }

    while (offset < length && ret == 0) {
        size_t chunk = length - offset;
        if (chunk > kTailChunk) {
            chunk = kTailChunk;
        }
        memcpy(s_tail_in, input + offset, chunk);
        ret = __real_esp_aes_crypt_ctr(ctx, chunk, nc_off, nonce_counter, stream_block, s_tail_in, s_tail_out);
        if (ret == 0) {
            memcpy(output + offset, s_tail_out, chunk);
        }
        offset += chunk;
    }

    xSemaphoreGive(s_aes_mux);
    return ret;
}

}  // extern "C"
