/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：esp_wifi_init 链接器包装，在内部 DMA 内存偏紧时收敛静态接收与管理缓冲数量。
 *
 * English: Linker wrapper for esp_wifi_init that trims static RX/TX and management buffers
 * when internal DMA heap is tight.
 *
 * 冻结：仅在内部 DMA 空闲低于 64KB 时裁剪，不影响常规充足内存下的默认配置。
 * Frozen: Trim only when free internal DMA memory is below 64 KB; preserve default configs otherwise.
 */

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"

esp_err_t __real_esp_wifi_init(const wifi_init_config_t *config);

esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t *config) {
    if (!config) {
        return __real_esp_wifi_init(NULL);
    }
    wifi_init_config_t tuned = *config;
    const size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    if (dma_free < 65536u) {
        tuned.static_rx_buf_num = 2;
        if (tuned.dynamic_rx_buf_num > 16) {
            tuned.dynamic_rx_buf_num = 16;
        }
        tuned.tx_buf_type = 1;
        tuned.static_tx_buf_num = 0;
        if (tuned.dynamic_tx_buf_num == 0 || tuned.dynamic_tx_buf_num > 16) {
            tuned.dynamic_tx_buf_num = 16;
        }
        tuned.rx_mgmt_buf_type = 1;
        if (tuned.rx_mgmt_buf_num > 4) {
            tuned.rx_mgmt_buf_num = 4;
        }
        if (tuned.cache_tx_buf_num > 4) {
            tuned.cache_tx_buf_num = 4;
        }
        if (tuned.mgmt_sbuf_num > 6) {
            tuned.mgmt_sbuf_num = 6;
        }
        tuned.csi_enable = 0;
        tuned.ampdu_rx_enable = 0;
        tuned.ampdu_tx_enable = 0;
        tuned.nvs_enable = 0;
        tuned.feature_caps &= ~CONFIG_FEATURE_CACHE_TX_BUF_BIT;
    }
    return __real_esp_wifi_init(&tuned);
}
