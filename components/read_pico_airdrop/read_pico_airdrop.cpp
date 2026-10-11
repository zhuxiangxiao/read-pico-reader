/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：隔空投送（Apple AirDrop 接收端）组件实现：管理 AWDL 射频生命周期、蓝牙租约与 TF 卡流式归类写入。
 *
 * English: Apple AirDrop receiver implementation managing AWDL radio lifecycle, BLE lease
 * and streaming file classification onto the TF card.
 *
 * 冻结：仅向已挂载的 TF 卡写入，同名文件自动加 _1.._99 后缀，中断或停止时清理半截文件。
 * Frozen: Write only to the mounted TF card, suffix colliding names with _1.._99, and remove partial files on abort or stop.
 */

#include "read_pico_airdrop.h"
#include "airdrop_file_route.h"
#include "ESP32AWDL.h"
#include "ESP32Drop.h"
#include "ble_page_turner.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *TAG = "airdrop";

// TF 卡保留 4MB 安全余量，防止 FAT 表或日志写满卡死。
// Keep a 4 MiB reserve on the TF card so FAT metadata never runs out of clusters.
static constexpr uint64_t kSdReserveBytes = 4ULL * 1024ULL * 1024ULL;
static constexpr size_t kStdioBufSize = 32768u;

static bool s_running = false;
static bool s_ble_leased = false;
static bool s_sleep_paused = false;
static char s_books_dir[160] = "/sdcard/books";
static char s_fonts_dir[160] = "/sdcard/fonts";
static char s_pictures_dir[160] = "/sdcard/pictures";
static char s_other_dir[160] = "/sdcard";
static read_pico_airdrop_status_t s_status;

static FILE *s_current_fp = nullptr;
static char *s_stdio_buf = nullptr;
static char s_current_path[320];
static uint32_t s_expected_size = 0;
static uint32_t s_written_size = 0;
static airdrop_route_kind_t s_current_route = AIRDROP_ROUTE_OTHER;
static uint32_t s_last_ad_err = 0;

static void copy_dir_or_default(char *dst, size_t cap, const char *src, const char *fallback) {
    const char *use = (src && *src) ? src : fallback;
    snprintf(dst, cap, "%s", use);
    size_t len = strlen(dst);
    while (len > 1 && (dst[len - 1] == '/' || dst[len - 1] == '\\')) {
        dst[--len] = '\0';
    }
}

static bool file_exists_stat(const char *path, void *ctx) {
    (void)ctx;
    struct stat st;
    return path && stat(path, &st) == 0;
}

static bool ensure_dir_tree(const char *dir) {
    if (!dir || !*dir) {
        return false;
    }
    if (strcmp(dir, "/sdcard") == 0) {
        struct stat st;
        return stat("/sdcard", &st) == 0 && S_ISDIR(st.st_mode);
    }
    char tmp[320];
    int n = snprintf(tmp, sizeof(tmp), "%s", dir);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        return false;
    }
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (strcmp(tmp, "/sdcard") != 0) {
                if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                    return false;
                }
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

static bool has_storage_headroom(uint32_t file_size) {
    uint64_t total_bytes = 0;
    uint64_t free_bytes = 0;
    if (esp_vfs_fat_info("/sdcard", &total_bytes, &free_bytes) != ESP_OK) {
        return false;
    }
    if (free_bytes <= kSdReserveBytes) {
        return false;
    }
    return (uint64_t)file_size <= (free_bytes - kSdReserveBytes);
}

static void cleanup_partial_file(const char *err_msg) {
    if (s_current_fp) {
        fclose(s_current_fp);
        s_current_fp = nullptr;
    }
    if (s_current_path[0]) {
        remove(s_current_path);
        s_current_path[0] = '\0';
    }
    s_expected_size = 0;
    s_written_size = 0;
    if (err_msg && *err_msg) {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", err_msg);
        s_status.state = READ_PICO_AIRDROP_ERROR;
    }
}

static const char *target_dir_for_route(airdrop_route_kind_t route) {
    switch (route) {
        case AIRDROP_ROUTE_BOOK:
            return s_books_dir;
        case AIRDROP_ROUTE_FONT:
            return s_fonts_dir;
        case AIRDROP_ROUTE_PICTURE:
            return s_pictures_dir;
        default:
            return s_other_dir;
    }
}

static int on_file_begin(void *ctx, const struct AdFile *f) {
    (void)ctx;
    cleanup_partial_file(nullptr);
    if (!f) {
        return AD_FILE_FAIL;
    }

    const char *raw_name = (f->name && *f->name) ? f->name : f->path;
    const char *base = nullptr;
    airdrop_route_kind_t route = airdrop_classify_filename(raw_name, &base);
    if (route == AIRDROP_ROUTE_SKIP) {
        return AD_FILE_SKIP;
    }
    if (route == AIRDROP_ROUTE_INVALID || !base) {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "文件名不合法");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }
    if (!has_storage_headroom(f->size)) {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "TF 卡剩余空间不足");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }

    const char *dir = target_dir_for_route(route);
    if (!ensure_dir_tree(dir)) {
        snprintf(s_status.last_error, sizeof(s_status.last_error), "无法创建保存目录");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }

    char final_name[sizeof(s_status.current_file)];
    if (!airdrop_resolve_unique_path(
            dir, base, file_exists_stat, nullptr,
            s_current_path, sizeof(s_current_path),
            final_name, sizeof(final_name))) {
        s_current_path[0] = '\0';
        snprintf(s_status.last_error, sizeof(s_status.last_error), "同名文件过多，请先整理目录");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }

    s_current_fp = fopen(s_current_path, "wb");
    if (!s_current_fp) {
        ESP_LOGW(TAG, "fopen failed for %s (errno=%d)", s_current_path, errno);
        s_current_path[0] = '\0';
        snprintf(s_status.last_error, sizeof(s_status.last_error), "无法创建文件");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }
    if (s_stdio_buf) {
        setvbuf(s_current_fp, s_stdio_buf, _IOFBF, kStdioBufSize);
    }

    s_current_route = route;
    s_expected_size = f->size;
    s_written_size = 0;
    snprintf(s_status.current_file, sizeof(s_status.current_file), "%s", final_name);
    s_status.current_bytes = 0;
    s_status.current_total = f->size;
    s_status.last_error[0] = '\0';
    s_status.state = READ_PICO_AIRDROP_RECEIVING;
    s_status.rx_active = true;
    ESP_LOGI(TAG, "receiving %s (%u bytes)", s_current_path, (unsigned)f->size);
    return AD_FILE_TAKE;
}

static bool on_file_data(void *ctx, const uint8_t *data, uint32_t len) {
    (void)ctx;
    if (!s_current_fp) {
        return false;
    }
    if (len == 0) {
        return true;
    }
    size_t written = fwrite(data, 1, len, s_current_fp);
    if (written != len) {
        ESP_LOGW(TAG, "fwrite short (%u/%u, errno=%d)", (unsigned)written, (unsigned)len, errno);
        cleanup_partial_file("写入 TF 卡失败");
        return false;
    }
    s_written_size += len;
    s_status.current_bytes = s_written_size;
    return true;
}

static void on_file_end(void *ctx, bool complete) {
    (void)ctx;
    if (!s_current_fp) {
        return;
    }
    int flush_rc = fflush(s_current_fp);
    int close_rc = fclose(s_current_fp);
    s_current_fp = nullptr;

    if (!complete || flush_rc != 0 || close_rc != 0 || s_written_size != s_expected_size) {
        ESP_LOGW(TAG, "transfer incomplete for %s (complete=%d, %u/%u)",
                 s_current_path, (int)complete, (unsigned)s_written_size, (unsigned)s_expected_size);
        if (s_current_path[0]) {
            remove(s_current_path);
            s_current_path[0] = '\0';
        }
        s_expected_size = 0;
        s_written_size = 0;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "投送未完成，请重试");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return;
    }

    ESP_LOGI(TAG, "saved %s (%u bytes)", s_current_path, (unsigned)s_written_size);
    s_current_path[0] = '\0';
    s_status.current_bytes = s_written_size;
    s_status.current_total = s_expected_size;
    s_expected_size = 0;
    s_written_size = 0;
    ++s_status.completed_files;
    if (s_current_route == AIRDROP_ROUTE_BOOK) {
        ++s_status.books_received;
    } else if (s_current_route == AIRDROP_ROUTE_FONT) {
        ++s_status.fonts_received;
    }
    s_status.last_error[0] = '\0';
    s_status.state = READ_PICO_AIRDROP_DONE;
}

static void fill_device_name(void) {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_status.device_name, sizeof(s_status.device_name), "kiikoread-%02X%02X", mac[4], mac[5]);
}

esp_err_t read_pico_airdrop_start(const read_pico_airdrop_cfg_t *cfg) {
    if (s_running) {
        read_pico_airdrop_stop();
    }
    s_sleep_paused = false;
    memset(&s_status, 0, sizeof(s_status));
    fill_device_name();
    s_status.state = READ_PICO_AIRDROP_STARTING;
    s_last_ad_err = 0;

    if (cfg) {
        copy_dir_or_default(s_books_dir, sizeof(s_books_dir), cfg->books_dir, "/sdcard/books");
        copy_dir_or_default(s_fonts_dir, sizeof(s_fonts_dir), cfg->fonts_dir, "/sdcard/fonts");
        copy_dir_or_default(s_pictures_dir, sizeof(s_pictures_dir), cfg->pictures_dir, "/sdcard/pictures");
        copy_dir_or_default(s_other_dir, sizeof(s_other_dir), cfg->other_dir, "/sdcard");
    } else {
        copy_dir_or_default(s_books_dir, sizeof(s_books_dir), nullptr, "/sdcard/books");
        copy_dir_or_default(s_fonts_dir, sizeof(s_fonts_dir), nullptr, "/sdcard/fonts");
        copy_dir_or_default(s_pictures_dir, sizeof(s_pictures_dir), nullptr, "/sdcard/pictures");
        copy_dir_or_default(s_other_dir, sizeof(s_other_dir), nullptr, "/sdcard");
    }

    if (!s_stdio_buf) {
        s_stdio_buf = static_cast<char *>(heap_caps_malloc(kStdioBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }

    if (!s_ble_leased) {
        ble_pt_network_acquire();
        s_ble_leased = true;
    }

    struct AdFileOps ops = {
        .ctx = nullptr,
        .on_begin = on_file_begin,
        .on_data = on_file_data,
        .on_end = on_file_end,
    };
    ad_on_file(ops);

    esp_err_t awdl_rc = awdl_begin();
    if (awdl_rc != ESP_OK) {
        ESP_LOGW(TAG, "awdl_begin failed: 0x%x", (unsigned)awdl_rc);
        ble_pt_network_release();
        s_ble_leased = false;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "无线电初始化失败 (0x%x)", (unsigned)awdl_rc);
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return awdl_rc;
    }

    int ad_rc = ad_begin(s_status.device_name, 0);
    if (ad_rc != 0) {
        ESP_LOGW(TAG, "ad_begin failed: %d", ad_rc);
        (void)awdl_end();
        ble_pt_network_release();
        s_ble_leased = false;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "隔空投送服务启动失败 (%d)", ad_rc);
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return ESP_FAIL;
    }

    s_running = true;
    s_status.state = READ_PICO_AIRDROP_READY;
    ESP_LOGI(TAG, "AirDrop started as %s", s_status.device_name);
    return ESP_OK;
}

void read_pico_airdrop_stop(void) {
    s_sleep_paused = false;
    cleanup_partial_file(nullptr);
    if (s_running) {
        ad_end();
        (void)awdl_end();
        s_running = false;
    }
    if (s_stdio_buf) {
        heap_caps_free(s_stdio_buf);
        s_stdio_buf = nullptr;
    }
    if (s_ble_leased) {
        ble_pt_network_release();
        s_ble_leased = false;
    }
    s_status.rx_active = false;
    s_status.state = READ_PICO_AIRDROP_STOPPED;
}

void read_pico_airdrop_poll(void) {
    if (!s_running) {
        return;
    }
    ad_poll();

    struct AdStatus ad = {};
    ad_stats_read(&ad);
    s_status.rx_active = ad.rx_active != 0;

    if (s_status.rx_active) {
        s_status.state = READ_PICO_AIRDROP_RECEIVING;
        if (!s_current_fp && ad.rx_bytes > 0) {
            s_status.current_bytes = ad.rx_bytes;
            s_status.current_total = ad.rx_max;
        }
        return;
    }

    if (s_current_fp) {
        cleanup_partial_file("投送中断，未完整文件已清理");
        return;
    }

    if (ad.file_err != 0 && ad.file_err != s_last_ad_err) {
        s_last_ad_err = ad.file_err;
        if (!s_status.last_error[0]) {
            snprintf(s_status.last_error, sizeof(s_status.last_error), "投送未完成，请重试");
        }
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return;
    }

    if (s_status.state == READ_PICO_AIRDROP_RECEIVING) {
        s_status.state = s_status.completed_files > 0 ? READ_PICO_AIRDROP_DONE : READ_PICO_AIRDROP_READY;
    }
}

void read_pico_airdrop_get_status(read_pico_airdrop_status_t *out) {
    if (!out) {
        return;
    }
    if (!s_status.device_name[0]) {
        fill_device_name();
    }
    *out = s_status;
}

bool read_pico_airdrop_active(void) {
    return s_running;
}

bool read_pico_airdrop_busy(void) {
    if (!s_running) {
        return false;
    }
    struct AdStatus ad = {};
    ad_stats_read(&ad);
    return ad.rx_active != 0 || s_current_fp != nullptr;
}

bool read_pico_airdrop_pause_for_sleep(void) {
    if (!s_running) {
        return true;
    }
    if (read_pico_airdrop_busy()) {
        return false;
    }
    ad_end();
    (void)awdl_end();
    s_running = false;
    s_sleep_paused = true;
    return true;
}

void read_pico_airdrop_resume_after_sleep(void) {
    if (!s_sleep_paused) {
        return;
    }
    s_sleep_paused = false;
    read_pico_airdrop_cfg_t cfg = {
        .books_dir = s_books_dir,
        .fonts_dir = s_fonts_dir,
        .pictures_dir = s_pictures_dir,
        .other_dir = s_other_dir,
    };
    (void)read_pico_airdrop_start(&cfg);
}
