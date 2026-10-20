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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"

#include <dirent.h>
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

// 接收先写同目录临时文件（隐藏前缀 + .part），全量落盘并关闭成功后才改名为最终名：
// 拔卡/掉电不会在正式文件名下留下半截文件。
// Receive into a same-directory temp file (hidden prefix + .part) and rename to the final
// name only after the whole declared size is on disk and closed; a yanked card or power
// loss can never leave a truncated file under a real name.
static constexpr const char *kTempPrefix = ".airdrop-";
static constexpr const char *kTempSuffix = ".part";
static constexpr size_t kPathCap = 320;

static bool s_running = false;
static bool s_ble_leased = false;
static bool s_sleep_paused = false;
// ad_begin()/awdl_begin() 是否已“尝试”过（可能只完成了一部分），用于失败清理与无条件 stop。
// Whether ad_begin()/awdl_begin() was ATTEMPTED (may have partially started), so failure
// paths and an unconditional stop() release exactly what exists.
static bool s_ad_begun = false;
static bool s_awdl_begun = false;
static char s_books_dir[160] = "/sdcard/books";
static char s_fonts_dir[160] = "/sdcard/fonts";
static char s_pictures_dir[160] = "/sdcard/pictures";
static char s_other_dir[160] = "/sdcard";
static read_pico_airdrop_status_t s_status;

static FILE *s_current_fp = nullptr;
static char *s_stdio_buf = nullptr;
static char s_current_path[kPathCap];   // 正在写的临时文件路径 / temp file currently open
static char s_final_path[kPathCap];     // 收全后 rename 的目标路径 / rename target once whole
static uint32_t s_temp_seq = 0;         // 本次会话临时文件名序号 / session temp-name sequence
static uint32_t s_expected_size = 0;
static uint32_t s_written_size = 0;
static airdrop_route_kind_t s_current_route = AIRDROP_ROUTE_OTHER;
static uint32_t s_last_ad_err = 0;
static bool s_rx_had_file = false;
static bool s_rx_had_error = false;

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
        if (remove(s_current_path) != 0 && errno != ENOENT) {
            // 卡已拔掉时 remove 必失败；残留的 .part 由下次 start 的启动扫描清掉。
            // Removal always fails once the card is gone; the next start's sweep clears it.
            ESP_LOGW(TAG, "remove temp %s failed (errno=%d)", s_current_path, errno);
        }
        s_current_path[0] = '\0';
    }
    s_final_path[0] = '\0';
    s_expected_size = 0;
    s_written_size = 0;
    if (err_msg && *err_msg) {
        s_rx_had_error = true;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "%s", err_msg);
        s_status.state = READ_PICO_AIRDROP_ERROR;
    }
}

// 只认本组件的临时文件前缀 + 后缀。/ Match only this component's temp prefix/suffix.
static bool is_temp_name(const char *name) {
    if (!name) {
        return false;
    }
    size_t plen = strlen(kTempPrefix);
    size_t slen = strlen(kTempSuffix);
    size_t n = strlen(name);
    return n > plen + slen && strncmp(name, kTempPrefix, plen) == 0 &&
           strcmp(name + n - slen, kTempSuffix) == 0;
}

// 清理掉电/拔卡遗留的临时文件：只扫配置目录里匹配前缀的文件，绝不碰用户文件。
// Sweep temp files left by power loss or card removal: only files matching our temp
// prefix in the configured directories, never a user's file.
static unsigned cleanup_stale_temp_files(void) {
    const char *dirs[] = {s_books_dir, s_fonts_dir, s_pictures_dir, s_other_dir};
    unsigned removed = 0;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        DIR *d = opendir(dirs[i]);
        if (!d) {
            continue;
        }
        struct dirent *entry = nullptr;
        while ((entry = readdir(d)) != nullptr) {
            if (!is_temp_name(entry->d_name)) {
                continue;
            }
            char path[kPathCap];
            int n = snprintf(path, sizeof(path), "%s/%s", dirs[i], entry->d_name);
            if (n < 0 || (size_t)n >= sizeof(path)) {
                continue;
            }
            if (remove(path) == 0) {
                ++removed;
            }
        }
        closedir(d);
    }
    if (removed) {
        ESP_LOGI(TAG, "removed %u stale temp files", removed);
    }
    return removed;
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
        s_rx_had_error = true;
        return AD_FILE_FAIL;
    }

    const char *raw_name = (f->name && *f->name) ? f->name : f->path;
    const char *base = nullptr;
    airdrop_route_kind_t route = airdrop_classify_filename(raw_name, &base);
    if (route == AIRDROP_ROUTE_SKIP) {
        return AD_FILE_SKIP;
    }
    if (route == AIRDROP_ROUTE_INVALID || !base) {
        s_rx_had_error = true;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "文件名不合法");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }
    if (!has_storage_headroom(f->size)) {
        s_rx_had_error = true;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "TF 卡剩余空间不足");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }

    const char *dir = target_dir_for_route(route);
    if (!ensure_dir_tree(dir)) {
        s_rx_had_error = true;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "无法创建保存目录");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }

    char final_name[sizeof(s_status.current_file)];
    if (!airdrop_resolve_unique_path(
            dir, base, file_exists_stat, nullptr,
            s_final_path, sizeof(s_final_path),
            final_name, sizeof(final_name))) {
        s_final_path[0] = '\0';
        s_rx_had_error = true;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "同名文件过多，请先整理目录");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }

    // 同目录临时名：序号唯一，避免与用户文件或上一轮残留撞名；收全后才改名为最终名。
    // A unique same-directory temp name: it cannot collide with a user file or a stale
    // leftover, and the final name appears only once the whole file has landed.
    uint32_t seq = s_temp_seq++;
    int tn = snprintf(s_current_path, sizeof(s_current_path), "%s/%s%08lx%s", dir, kTempPrefix,
                      (unsigned long)seq, kTempSuffix);
    if (tn < 0 || (size_t)tn >= sizeof(s_current_path)) {
        s_current_path[0] = '\0';
        s_final_path[0] = '\0';
        s_rx_had_error = true;
        snprintf(s_status.last_error, sizeof(s_status.last_error), "无法创建文件");
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return AD_FILE_FAIL;
    }

    s_current_fp = fopen(s_current_path, "wb");
    if (!s_current_fp) {
        ESP_LOGW(TAG, "fopen failed for %s (errno=%d)", s_current_path, errno);
        s_current_path[0] = '\0';
        s_final_path[0] = '\0';
        s_rx_had_error = true;
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
    s_rx_had_file = true;
    s_rx_had_error = false;
    snprintf(s_status.current_file, sizeof(s_status.current_file), "%s", final_name);
    s_status.current_bytes = 0;
    s_status.current_total = f->size;
    s_status.last_error[0] = '\0';
    s_status.state = READ_PICO_AIRDROP_RECEIVING;
    s_status.rx_active = true;
    ESP_LOGI(TAG, "receiving %s (%u bytes)", s_final_path, (unsigned)f->size);
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
        // 半截文件只在临时名下存在，删掉即可；正式名从未出现。
        // The partial file only ever existed under the temp name; deleting it is enough.
        cleanup_partial_file("投送未完成，请重试");
        return;
    }

    // 字节数已核对、flush/close 均成功，才把临时名换成正式名；rename 失败同样不留下半截。
    // Only after the declared size is on disk and flush/close succeeded is the temp file
    // published under its final name; a failed rename leaves no half-file either.
    if (rename(s_current_path, s_final_path) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed (errno=%d)", s_current_path, s_final_path, errno);
        cleanup_partial_file("写入 TF 卡失败");
        return;
    }

    ESP_LOGI(TAG, "saved %s (%u bytes)", s_final_path, (unsigned)s_written_size);
    s_current_path[0] = '\0';
    s_final_path[0] = '\0';
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

/* ---- 资源释放 / Resource release ---- */

// 结束 AWDL 并重试到“拆干净”。按 awdl_end() 的新契约：ESP_OK=已拆干净，
// ESP_ERR_INVALID_STATE=本来就没起来（可忽略，同样算干净），ESP_ERR_TIMEOUT=可重试——
// stop 请求保持有效、内存未释放、begin 继续被拒，必须再调 end()。
// Tear AWDL down, retrying until clean. Per the new awdl_end() contract: ESP_OK = fully torn
// down; ESP_ERR_INVALID_STATE = the link was not up (ignorable, equally clean);
// ESP_ERR_TIMEOUT = retryable -- the stop request stays asserted, nothing is freed and
// begin() keeps refusing, so end() must be called again.
static constexpr int kAwdlEndTries = 3;
static bool awdl_end_clean(void) {
    for (int attempt = 0; attempt < kAwdlEndTries; ++attempt) {
        esp_err_t rc = awdl_end();
        if (rc == ESP_OK || rc == ESP_ERR_INVALID_STATE) {
            s_awdl_begun = false;
            return true;
        }
        if (attempt + 1 < kAwdlEndTries) {
            // 给帧/节奏任务一个退出的时间窗，再要求一次。/ Give the tasks a window to exit, then ask again.
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    // 保留 s_awdl_begun，让下一次 stop()/pause 继续重试；绝不在半拆的链路上 begin()。
    // Keep s_awdl_begun so a later stop()/pause retries; never begin() over a half-torn link.
    ESP_LOGW(TAG, "awdl_end still incomplete after %d tries; will retry on the next stop/pause",
             kAwdlEndTries);
    return false;
}

// 拆掉射频侧（ad → awdl）；drain 为真时先把已回 200 的整包归档交付出去。
// Tear the radio side down (ad, then awdl); with drain set, deliver an acknowledged
// whole-archive handoff through ad_poll() first so ad_end() does not free it undelivered.
static void teardown_radio(bool drain) {
    if (s_ad_begun) {
        if (drain) {
            ad_poll();
            // 双检：归档可能在第一次 poll 早退之后才发布。
            // Re-check: the archive may be published after the first poll returned early.
            for (int i = 0; i < 2 && ad_arch_pending(); ++i) {
                ad_poll();
            }
            cleanup_partial_file(nullptr);
        }
        ad_end();
        s_ad_begun = false;
    }
    if (s_awdl_begun) {
        (void)awdl_end_clean();
    }
}

// 释放本组件持有的一切（射频、stdio 缓冲、BLE 租约）；失败清理与离页 stop 共用。
// Release everything this component owns (radio, stdio buffer, BLE lease); shared by the
// start-failure cleanup and by stop() on page exit.
static void release_service_resources(void) {
    teardown_radio(true);
    if (s_stdio_buf) {
        heap_caps_free(s_stdio_buf);
        s_stdio_buf = nullptr;
    }
    if (s_ble_leased) {
        ble_pt_network_release();
        s_ble_leased = false;
    }
}

// 内部启动路径：apply_dirs 为假时沿用会话内已保存的目录，不再拷贝 cfg（避免自拷贝 UB）。
// Internal start path: with apply_dirs false the session's saved directories are reused and
// cfg is not copied at all, so resume never hands snprintf the same buffer as src and dst.
static esp_err_t start_impl(const read_pico_airdrop_cfg_t *cfg, bool apply_dirs) {
    if (s_running) {
        read_pico_airdrop_stop();
    } else if (s_ad_begun || s_awdl_begun) {
        // 失败启动/失败暂停留下的射频资源：先清掉再重开。/ Radio leftovers: clear before re-open.
        teardown_radio(true);
    }
    s_sleep_paused = false;
    memset(&s_status, 0, sizeof(s_status));
    fill_device_name();
    s_status.state = READ_PICO_AIRDROP_STARTING;
    struct AdStatus ad_init = {};
    ad_stats_read(&ad_init);
    s_last_ad_err = ad_init.file_err;
    s_rx_had_file = false;
    s_rx_had_error = false;

    if (apply_dirs) {
        copy_dir_or_default(s_books_dir, sizeof(s_books_dir), cfg ? cfg->books_dir : nullptr, "/sdcard/books");
        copy_dir_or_default(s_fonts_dir, sizeof(s_fonts_dir), cfg ? cfg->fonts_dir : nullptr, "/sdcard/fonts");
        copy_dir_or_default(s_pictures_dir, sizeof(s_pictures_dir), cfg ? cfg->pictures_dir : nullptr, "/sdcard/pictures");
        copy_dir_or_default(s_other_dir, sizeof(s_other_dir), cfg ? cfg->other_dir : nullptr, "/sdcard");
        // 先清掉掉电/拔卡留下的临时文件，再开始接收。/ Sweep stale temp files, then receive anew.
        cleanup_stale_temp_files();
    }

    if (!s_stdio_buf) {
        s_stdio_buf = static_cast<char *>(heap_caps_malloc(kStdioBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    }

    if (!s_ble_leased) {
        esp_err_t ble_rc = ble_pt_network_acquire();
        if (ble_rc != ESP_OK) {
            ESP_LOGW(TAG, "ble_pt_network_acquire failed: 0x%x", (unsigned)ble_rc);
            release_service_resources();
            snprintf(s_status.last_error, sizeof(s_status.last_error), "蓝牙释放失败，请重试");
            s_status.state = READ_PICO_AIRDROP_ERROR;
            return ble_rc;
        }
        s_ble_leased = true;
    }

    struct AdFileOps ops = {
        .ctx = nullptr,
        .on_begin = on_file_begin,
        .on_data = on_file_data,
        .on_end = on_file_end,
    };
    ad_on_file(ops);

    // 记为“已尝试”：awdl_begin 可能半启动，失败也必须 awdl_end 兜底。
    // Mark as attempted: awdl_begin may start partially, so even its failure needs awdl_end.
    s_awdl_begun = true;
    esp_err_t awdl_rc = awdl_begin();
    if (awdl_rc == ESP_ERR_INVALID_STATE) {
        // 契约：上一次拆解没收干净（或链路已在）时 begin 被拒。先补一次带重试的 teardown，
        // 再重试一次 begin；仍失败就走既有 ERROR 路径。
        // Contract: begin() is refused while a previous teardown is incomplete (or the link
        // is already up). Finish the teardown with retries, retry begin() once, then fall
        // through to the existing ERROR path if it still refuses.
        ESP_LOGW(TAG, "awdl_begin refused (previous teardown incomplete); finishing teardown and retrying");
        teardown_radio(true);
        s_awdl_begun = true;   // 这一次尝试同样按“已尝试”清理 / this attempt needs cleanup too
        awdl_rc = awdl_begin();
    }
    if (awdl_rc != ESP_OK) {
        ESP_LOGW(TAG, "awdl_begin failed: 0x%x", (unsigned)awdl_rc);
        release_service_resources();
        snprintf(s_status.last_error, sizeof(s_status.last_error), "无线电初始化失败 (0x%x)", (unsigned)awdl_rc);
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return awdl_rc;
    }

    // ad_begin 内部会先占 receiver role、分配流式 ring/解码块与监听任务栈，失败同样要 ad_end。
    // ad_begin claims the receiver role and allocates the stream ring, decode block and
    // listener stack before it can fail, so its failure needs ad_end as much as success does.
    s_ad_begun = true;
    int ad_rc = ad_begin(s_status.device_name, 0);
    if (ad_rc != 0) {
        ESP_LOGW(TAG, "ad_begin failed: %d", ad_rc);
        release_service_resources();
        snprintf(s_status.last_error, sizeof(s_status.last_error), "隔空投送服务启动失败 (%d)", ad_rc);
        s_status.state = READ_PICO_AIRDROP_ERROR;
        return ESP_FAIL;
    }

    s_running = true;
    s_status.state = READ_PICO_AIRDROP_READY;
    ESP_LOGI(TAG, "AirDrop started as %s", s_status.device_name);
    return ESP_OK;
}

esp_err_t read_pico_airdrop_start(const read_pico_airdrop_cfg_t *cfg) {
    return start_impl(cfg, true);
}

void read_pico_airdrop_stop(void) {
    s_sleep_paused = false;
    cleanup_partial_file(nullptr);
    s_rx_had_file = false;
    s_rx_had_error = false;
    // 不再由 s_running 门住：失败启动、暂停中与正常运行留下的资源都由这一次收尾覆盖。
    // No longer gated on s_running: leftovers from a failed start, a paused service and a
    // running one are all released by this single teardown.
    release_service_resources();
    s_running = false;
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

    if (ad.file_err != 0 && ad.file_err != s_last_ad_err) {
        s_last_ad_err = ad.file_err;
        s_rx_had_error = true;
        if (!s_status.last_error[0]) {
            snprintf(s_status.last_error, sizeof(s_status.last_error), "投送未完成，请重试");
        }
        s_status.state = READ_PICO_AIRDROP_ERROR;
    }

    if (s_status.rx_active) {
        if (s_rx_had_error) {
            s_status.state = READ_PICO_AIRDROP_ERROR;
        } else if (s_current_fp == nullptr && s_rx_had_file) {
            // 文件已落盘关闭，等待 TLS 线程写回 200 OK 期间保持 DONE 与文件真实字节数。
            // Preserve DONE and exact file size while the TLS task finishes sending 200 OK.
            s_status.state = READ_PICO_AIRDROP_DONE;
        } else {
            s_status.state = READ_PICO_AIRDROP_RECEIVING;
            if (!s_current_fp && !s_rx_had_file && ad.rx_bytes > 0) {
                s_status.current_bytes = ad.rx_bytes;
                s_status.current_total = ad.rx_max;
            }
        }
        return;
    }

    s_rx_had_file = false;
    s_rx_had_error = false;

    if (s_current_fp) {
        cleanup_partial_file("投送中断，未完整文件已清理");
        s_rx_had_error = false;
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
    // 已回 200 但尚未交付的整包归档同样算忙，先由 ad_poll 交付再允许暂停。
    // A whole archive acknowledged but not yet delivered counts as busy too: pause only
    // after ad_poll() has handed it to the sink.
    return ad.rx_active != 0 || s_current_fp != nullptr || ad_arch_pending();
}

bool read_pico_airdrop_pause_for_sleep(void) {
    if (!s_running) {
        return true;
    }
    if (read_pico_airdrop_busy()) {
        return false;
    }
    // busy() 之后、拆射频之前可能刚回 200 并发布归档：先交付再结束，不当成垃圾释放。
    // An archive can be published between the busy() check and here: deliver it before the
    // radio goes down instead of letting ad_end() free it.
    ad_poll();
    if (ad_arch_pending()) {
        return false;
    }
    if (s_ad_begun) {
        ad_end();
        s_ad_begun = false;
    }
    if (!awdl_end_clean()) {
        // 半拆链路上 begin() 会被拒：不 ad_begin()，也不谎报暂停成功；锁屏推迟，
        // s_awdl_begun 保留给下一次 stop()/pause 继续重试。
        // begin() is refused over a half-torn link: do not ad_begin() and do not claim
        // success; postpone the lock, and leave s_awdl_begun set so a later stop()/pause
        // retries the teardown.
        ESP_LOGW(TAG, "awdl teardown incomplete; postponing lock");
        return false;
    }
    s_running = false;
    s_sleep_paused = true;
    return true;
}

void read_pico_airdrop_resume_after_sleep(void) {
    if (!s_sleep_paused) {
        return;
    }
    s_sleep_paused = false;
    read_pico_airdrop_status_t saved = s_status;
    // 目录沿用会话内静态值：内部启动路径跳过 cfg 拷贝，snprintf 的源/目标不再相同。
    // Reuse the session's static directories: the internal start path skips the cfg copy, so
    // snprintf never sees the same buffer as source and destination (the old UB).
    if (start_impl(nullptr, false) == ESP_OK) {
        s_status.completed_files = saved.completed_files;
        s_status.books_received = saved.books_received;
        s_status.fonts_received = saved.fonts_received;
        snprintf(s_status.current_file, sizeof(s_status.current_file), "%s", saved.current_file);
        s_status.current_bytes = saved.current_bytes;
        s_status.current_total = saved.current_total;
        if (saved.completed_files > 0) {
            s_status.state = READ_PICO_AIRDROP_DONE;
        }
    }
    // 失败时 start_impl 已置 state=ERROR、写入 last_error 并释放全部资源：active() 保持
    // false，而 get_status() 仍能读到 ERROR，页面据此刷新，不再静默停在“等待投送”。
    // On failure start_impl leaves state=ERROR with last_error set and every resource
    // released: active() stays false while get_status() keeps the error visible, so the page
    // can redraw it instead of silently waiting for a sender.
}
