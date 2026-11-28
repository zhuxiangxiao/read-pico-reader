/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：网络启停始终由同一工作任务执行；取消只shutdown自有socket，等待后才释放媒体。
 * English: One worker owns network start/stop; cancel shuts down only its socket and joins before media release.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Os")
#endif
#include "weread_runtime.h"
#include "esp_log.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_store.h"
#include "book_source_weread.h"
#include "book_weread_uri.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(ESP_PLATFORM)
#include "kiiko_board_config.h"
#include "book_cover.h"
#include "read_pico_transfer.h"
#include "read_pico_sd.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/idf_additions.h"
static TaskHandle_t s_task;
static StaticQueue_t s_queue_storage;
static QueueHandle_t s_queue;
static uint8_t s_queue_byte;
#else
#include <pthread.h>
static pthread_t s_thread;
static bool s_joinable, s_dispatch;
static pthread_mutex_t s_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_queue_cond = PTHREAD_COND_INITIALIZER;
#endif
static atomic_bool s_busy, s_done, s_cancel, s_alive;
static atomic_uint_fast64_t s_deadline, s_live_pos, s_fg_input_ms;
static atomic_bool s_live_valid, s_cloud_jump_ready;
static atomic_uint_fast32_t s_sync_downloading_uid, s_prefetch_downloading_uid, s_adopting_prefetch_uid, s_cached_count, s_total_count;
static bool s_configured;
static atomic_bool s_owns_network;
static unsigned s_cache_epoch;
static uint64_t s_idle_at;
static atomic_uint_fast64_t s_network_active;
static WR_EXT_BSS weread_result_t s_result;
static WR_EXT_BSS struct {
    weread_job_t job;
    char id[64], value[640];
    char probe_ids[9][64];
    size_t probe_count;
    size_t chapter, chapter_idx;
    uint64_t now;
    bool prefetch_forward;
} s_work;
static WR_EXT_BSS struct { char id[64]; weread_progress_t progress; uint32_t fresh_version,last_chapter;bool valid, resume_ready, pending_cloud_sync; } s_choice;
static WR_EXT_BSS struct { char id[64]; weread_progress_t progress; bool rebased; } s_cloud_jump;
static WR_EXT_BSS char s_shelf_cache_id[64];
static atomic_bool s_shelf_cache_active, s_shelf_cache_storage_blocked;
static atomic_uint_fast32_t s_shelf_cache_done, s_shelf_cache_total;
static WR_EXT_BSS weread_runtime_cache_stats_t s_cache_stats;
#if defined(ESP_PLATFORM)
static portMUX_TYPE s_stats_lock = portMUX_INITIALIZER_UNLOCKED;
#else
static pthread_mutex_t s_stats_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static struct {
    char id[64];
    size_t total;
    uint32_t *uids;
    uint8_t *bits;
    size_t bits_bytes;
    unsigned cached;
    bool scanned;
} s_book_cache;
static WR_EXT_BSS struct {
    char id[64], summary[256], owner_root[256], owner_vid[WEREAD_VID_MAX_LEN], owner_device[WEREAD_DEVICE_ID_MAX_LEN];
    uint32_t owner_version,nav_version;unsigned owner_epoch;
    size_t chapter, nav_chapter, total_chapters, prefetch_cursor;
    uint32_t offset, percent, nav_offset, nav_percent, nav_uid, nav_index, anchor_uid, anchor_index, anchor_offset, anchor_percent, revision;
    unsigned pages, session_pages;
    weread_progress_t durable;
    uint8_t media_id[WEREAD_MEDIA_ID_BYTES];
    uint64_t last_tick, active_ms, last_store, last_move, prefetch_retry;
    uint32_t prefetched;
    uint64_t report_at, pause_report_at, report_due, cloud_sync_retry_at, cloud_sync_ui_until;
    unsigned prefetch_mask, prefetch_failures;
    uint8_t cloud_sync_failures, cloud_sync_connect_timeouts;
    weread_progress_state_t state;
    weread_cloud_sync_ui_state_t cloud_sync_ui;
    bool seen, changed, retry, cover_warmed, anchor, navigation_pending, storage_blocked, pending_coordinate, save_failed;
    bool durable_valid, media_valid, rescued, active, urgent, plain_text, cloud_sync_pending;
    bool prefetch_done, user_turned_since_open;
    bool cache_forward, cache_forward_off;
} s_reader;
/* ---- 开书/切章阶段 / Open and cold-chapter stages ---- */
static atomic_uint s_stage;
static atomic_uint_fast64_t s_stage_bytes;
static bool s_retry_phase, s_retry_blocked;
// Wi-Fi 建连 15 秒未就绪也算超时；静默重试只会让离线用户多等一倍时间，故不重放。
// A 15 s Wi-Fi connect timeout also maps to ESP_ERR_TIMEOUT; replaying it would double an offline wait.
static bool s_connect_timed_out;
static void stage_store(weread_stage_t stage, uint64_t bytes) {
    // 先写字节再写阶段，UI 短暂的新旧组合不越界。/ Publish bytes before the stage; readers tolerate one mixed sample.
    atomic_store(&s_stage_bytes, bytes);
    atomic_store(&s_stage, (unsigned)stage);
}
static void stage_phase(weread_stage_t stage, uint64_t bytes) {
    // 静默重试期间保持「正在重试」，不被同一任务体的阶段覆盖。
    // Hold the retry notice during the silent replay instead of letting the body's phases overwrite it.
    if (!s_retry_phase) stage_store(stage, bytes);
}
static void stage_download_progress(uint64_t bytes, bool done, void *ctx) {
    (void)ctx;
    unsigned stage = atomic_load(&s_stage);
    if (stage == WEREAD_STAGE_NONE) {
        // 阅读页接管正在下载的预取章时，从字节回调发布阶段，避免 UI 线程写阶段状态。
        // When the reader adopts an in-flight prefetch download, publish the stage from the byte callback
        // instead of writing stage state from the UI thread.
        unsigned adopted = atomic_load(&s_adopting_prefetch_uid);
        if (!adopted || s_work.job != WR_JOB_PREFETCH ||
            (s_work.chapter != adopted && atomic_load(&s_prefetch_downloading_uid) != adopted)) return;
    } else if (stage == WEREAD_STAGE_RETRY) {
        // 重试已重新进入下载：让 KB 进度覆盖“正在重试”，否则大包下载全程看不到推进。
        // The retry is downloading again: let KB progress replace the retry label, otherwise a large package shows no movement.
        s_retry_phase = false;
    } else if (stage != WEREAD_STAGE_CHAPTER && stage != WEREAD_STAGE_PARSE) {
        return;
    }
    stage_store(done ? WEREAD_STAGE_PARSE : WEREAD_STAGE_CHAPTER, bytes);
}
bool weread_reader_stage(weread_stage_t *stage, uint64_t *bytes) {
    unsigned value = atomic_load(&s_stage);
    if (stage) *stage = (weread_stage_t)value;
    if (bytes) *bytes = atomic_load(&s_stage_bytes);
    return value != WEREAD_STAGE_NONE;
}
bool weread_reader_stage_text(char *out, size_t cap) {
    if (!out || cap < 2) return false;
    out[0] = 0;
    uint64_t bytes = atomic_load(&s_stage_bytes);
    switch (atomic_load(&s_stage)) {
        case WEREAD_STAGE_PROGRESS: snprintf(out, cap, "正在同步进度…"); return true;
        case WEREAD_STAGE_TOC: snprintf(out, cap, "正在获取目录…"); return true;
        case WEREAD_STAGE_CHAPTER: {
            uint64_t kb = bytes / 1024u;
            if (!kb) snprintf(out, cap, "正在下载章节…");
            else if (kb < 1024u) snprintf(out, cap, "正在下载章节 %u KB…", (unsigned)kb);
            else snprintf(out, cap, "正在下载章节 %u MB…", (unsigned)(kb / 1024u));
            return true;
        }
        case WEREAD_STAGE_PARSE: snprintf(out, cap, "正在校验章节…"); return true;
        case WEREAD_STAGE_RETRY: snprintf(out, cap, "网络超时，正在重试…"); return true;
        default: return false;
    }
}
static uint64_t now_ms(void) {
#if defined(ESP_PLATFORM)
    return (uint64_t)esp_timer_get_time() / 1000;
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
#endif
}
static void pause_ms(unsigned ms) {
#if defined(ESP_PLATFORM)
    vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1));
#else
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000}; nanosleep(&ts, NULL);
#endif
}
static bool cancelled(void *ctx) {
    (void)ctx; uint64_t deadline = atomic_load(&s_deadline);
    return atomic_load(&s_cancel) || (deadline && now_ms() >= deadline);
}
static int timeout_budget(void *ctx) {
    (void)ctx;uint64_t deadline=atomic_load(&s_deadline),now=now_ms();
    if(!deadline)return 0;
    if(deadline<=now)return 1;
    return deadline-now>15000?15000:(int)(deadline-now);
}
static bool in_worker(void) {
#if defined(ESP_PLATFORM)
    return s_task && xTaskGetCurrentTaskHandle() == s_task;
#else
    return atomic_load(&s_alive) && s_joinable && pthread_equal(pthread_self(), s_thread);
#endif
}
static void stats_lock(void) {
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_stats_lock);
#else
    pthread_mutex_lock(&s_stats_lock);
#endif
}
static void stats_unlock(void) {
#if defined(ESP_PLATFORM)
    portEXIT_CRITICAL(&s_stats_lock);
#else
    pthread_mutex_unlock(&s_stats_lock);
#endif
}
static void cache_stats_store(const weread_chapter_cache_stats_t *st) {
    if (!st) return;
    stats_lock();
    s_cache_stats.valid = true;
    s_cache_stats.total_bytes = st->total_bytes;
    s_cache_stats.reclaimable_bytes = st->reclaimable_bytes;
    s_cache_stats.free_bytes = st->free_bytes;
    s_cache_stats.fs_total_bytes = st->fs_total_bytes;
    s_cache_stats.book_dir_count = st->book_dir_count;
    s_cache_stats.books_with_chapters = st->books_with_chapters;
    stats_unlock();
}
static void cache_stats_invalidate(void) {
    stats_lock();
    s_cache_stats.valid = false;
    stats_unlock();
}
static void on_http_activity(void *ctx) {
    (void)ctx;
    if (atomic_load(&s_owns_network)) atomic_store(&s_network_active, now_ms());
}
#if defined(ESP_PLATFORM) && (defined(__GNUC__) || defined(__clang__))
__attribute__((noinline))
#endif
static bool network_online_ready(void) {
#if defined(ESP_PLATFORM)
    read_pico_transfer_status_t status;
    read_pico_transfer_get_status(&status);
    return status.network_ready && status.mode == READ_PICO_TRANSFER_MODE_STA &&
        status.state != READ_PICO_TRANSFER_STOPPED && status.state != READ_PICO_TRANSFER_ERROR;
#else
    return atomic_load(&s_owns_network);
#endif
}
static void stop_network(void) {
#if defined(ESP_PLATFORM)
    weread_client_set_radio_awake(false);
    if (s_owns_network) read_pico_transfer_stop();
#endif
    s_owns_network = false;
    s_network_active = 0;
}
static esp_err_t connect_network(void) {
    if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    // 每次建连尝试都从干净状态开始：前一次建连超时不能阻止后续 HTTP 超时的静默重试。
    // Start every connect attempt clean: an earlier connect timeout must not block a later HTTP timeout replay.
    s_connect_timed_out = false;
#if defined(ESP_PLATFORM)
    read_pico_transfer_status_t status;
    bool started_here = false, cleared_error = false;
    for(unsigned i=0;i<150 && !cancelled(NULL);++i){
        read_pico_transfer_get_status(&status);
        if(status.network_ready && status.mode==READ_PICO_TRANSFER_MODE_STA){
            if(time(NULL)<1700000000){
                uint32_t utc;
                esp_err_t err=read_pico_transfer_sync_time_online_cancelable(&utc,cancelled,NULL);
                if(err!=ESP_OK){if(started_here)stop_network();return err;}
            }
            weread_client_set_radio_awake(true);
            s_network_active = now_ms();
            if(cancelled(NULL)){if(started_here)stop_network();return ESP_ERR_INVALID_STATE;}
            return ESP_OK;
        }
        // 前次 Wi-Fi 启动失败（如 OOM/超时）时，先复位错误状态再重试建连。
        // Clear a prior failed start (e.g., OOM or connect timeout) before attempting a fresh start.
        if(!started_here && !cleared_error && status.state==READ_PICO_TRANSFER_ERROR &&
           (!status.wifi_enabled || status.last_error==ESP_ERR_TIMEOUT)){
            ESP_LOGW("weread","clearing prior transfer error state err=%d wifi=%d",(int)status.last_error,(int)status.wifi_enabled);
            cleared_error=true;
            read_pico_transfer_stop();
            read_pico_transfer_get_status(&status);
        }
        if(status.state==READ_PICO_TRANSFER_STOPPED){
            if(started_here){
                s_owns_network = false;
                return ESP_ERR_INVALID_STATE;
            }
            s_owns_network = false;
            read_pico_transfer_cfg_t cfg={.mode=READ_PICO_TRANSFER_MODE_STA,.network_only=true,.cover_lookup=true};
            esp_err_t err=read_pico_transfer_start(&cfg);
            if(err==ESP_OK){started_here=true;s_owns_network=true;s_network_active=now_ms();}
            else if(err!=ESP_ERR_INVALID_STATE && err!=ESP_ERR_NOT_FINISHED){
                ESP_LOGW("weread","connect start failed err=%d",(int)err);
                return err;
            }
        } else if(status.mode!=READ_PICO_TRANSFER_MODE_STA){
            ESP_LOGW("weread","connect aborted mode=%d",(int)status.mode);
            if(started_here)s_owns_network = false;
            return ESP_ERR_INVALID_STATE;
        } else if(status.state==READ_PICO_TRANSFER_ERROR){
            esp_err_t err=status.last_error!=ESP_OK?status.last_error:ESP_ERR_INVALID_STATE;
            ESP_LOGW("weread","connect failed err=%d owned=%d",(int)err,(int)started_here);
            if(started_here){
                stop_network();
                if(err==ESP_ERR_TIMEOUT)s_connect_timed_out=true;
            }
            return err;
        }
        // 已有STA正在连接时等待IP，不中断外部会话。/ Wait for an existing STA's IP without interrupting its owner.
        read_pico_transfer_service_poll();pause_ms(100);
    }
    ESP_LOGW("weread","connect timed out cancel=%d owned=%d",(int)cancelled(NULL),(int)started_here);
    if (started_here) stop_network();
    if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    s_connect_timed_out = true;
    return ESP_ERR_TIMEOUT;
#else
    s_owns_network = true;
    s_network_active = now_ms();
    return ESP_OK;
#endif
}
void weread_runtime_result_free(weread_result_t *r) {
    if (r) { free(r->books); free(r->cover); memset(r, 0, sizeof(*r)); }
}
void weread_runtime_load_local(void) {
    WR_GUARD;weread_progress_outbox_reset();
#if defined(ESP_PLATFORM)
    if (!read_pico_sd_io_ready()) {weread_auth_forget_session();weread_shelf_clear();return;}
#endif
    // 每次进入重新绑定当前卡；其他页面发生的换卡也不能复用旧账号。/ Rebind on entry even when media changed while another page was active.
    (void)weread_auth_init(NULL); weread_shelf_clear();(void)weread_shelf_load();
}
static void snapshot(void) {
    WR_GUARD;
    s_result.authenticated = weread_auth_is_authenticated();
    s_result.auth_expired = weread_auth_is_expired();
    const weread_auth_session_t *session = weread_auth_get_current_session();
    if (session) wr_display_copy(s_result.nickname, sizeof(s_result.nickname), session->user_name);
    esp_err_t err=weread_shelf_snapshot(&s_result.books,&s_result.count);
    if(err!=ESP_OK)s_result.error=err;
}
static esp_err_t cover_job(const char *url);
static void book_cache_init(const char *id);
static bool cloud_ahead_of_live(const weread_progress_t *cloud) {
    if (!cloud || !cloud->chapter_uid || !atomic_load(&s_live_valid)) return false;
    uint64_t pos = atomic_load(&s_live_pos);
    weread_progress_t live = {
        .chapter_uid = 1,
        .chapter_index = (uint32_t)(pos >> 32),
        .chapter_offset = (uint32_t)pos,
    };
    return weread_progress_ahead(cloud, &live);
}
static esp_err_t cloud_sync_job(void) { WR_MEMORY_SCOPE;
    if (!weread_auth_is_authenticated()) return ESP_ERR_INVALID_STATE;
    uint32_t open_version = 0, active_uid = 0;
    {
        WR_GUARD;
        if (!s_reader.active || strcmp(s_reader.id, s_work.id)) return ESP_ERR_INVALID_STATE;
        open_version = s_reader.owner_version;
        active_uid = s_reader.anchor_uid ? s_reader.anchor_uid : s_reader.nav_uid;
    }
    weread_progress_t cloud = {0};
    uint32_t fresh_version = 0, last_chapter = 0;
    esp_err_t err = weread_progress_fetch_version(s_work.id, &cloud, &fresh_version, &last_chapter);
    if (err != ESP_OK || cancelled(NULL)) return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
    if (!cloud_ahead_of_live(&cloud)) {
        if (fresh_version && fresh_version != open_version) {
            weread_toc_t toc = {0};
            if (weread_toc_prepare(s_work.id, true, fresh_version, &toc) == ESP_OK && toc.count) {
                bool keep_uid = false;
                for (size_t i = 0; i < toc.count; ++i)
                    if (toc.chapters[i].uid == active_uid) { keep_uid = true; break; }
                WR_GUARD;
                if (keep_uid && !cancelled(NULL) && s_reader.active && !strcmp(s_reader.id, s_work.id)) {
                    book_weread_adopt_prepared(s_work.id, &toc);
                    if (book_weread_reload_toc(s_work.id) == ESP_OK) {
                        s_reader.total_chapters = book_weread_count();
                        s_reader.prefetch_done = false;
                        book_cache_init(s_work.id);
                    } else {
                        book_weread_clear_prepared();
                    }
                }
            }
            weread_toc_free(&toc);
        }
        return weread_progress_rebase(s_work.id, &cloud, fresh_version, &s_result.local);
    }
    weread_toc_t toc = {0};
    err = weread_toc_prepare(s_work.id, false, fresh_version, &toc);
    if (err != ESP_OK || !toc.count) { weread_toc_free(&toc); return err != ESP_OK ? err : ESP_ERR_NOT_FOUND; }
    uint32_t uid = cloud.chapter_uid;
    bool exists = false, moved = false;
    for (size_t i = 0; i < toc.count; ++i) if (toc.chapters[i].uid == uid) {
        exists = true; moved = cloud.chapter_index && cloud.chapter_index != toc.chapters[i].index;
    }
    if ((uid && (!exists || moved)) || last_chapter > toc.chapters[toc.count - 1].index) {
        weread_toc_free(&toc);
        err = weread_toc_prepare(s_work.id, true, fresh_version, &toc);
        if (err != ESP_OK || !toc.count) { weread_toc_free(&toc); return err != ESP_OK ? err : ESP_ERR_NOT_FOUND; }
        exists = moved = false;
        for (size_t i = 0; i < toc.count; ++i) if (toc.chapters[i].uid == uid) {
            exists = true; moved = cloud.chapter_index && cloud.chapter_index != toc.chapters[i].index;
        }
        if (moved) { weread_toc_free(&toc); return WEREAD_ERR_POSITION; }
    }
    if (!exists || !uid) { weread_toc_free(&toc); return ESP_ERR_INVALID_RESPONSE; }
    if (!cloud_ahead_of_live(&cloud)) {
        weread_toc_free(&toc);
        return weread_progress_rebase(s_work.id, &cloud, fresh_version, &s_result.local);
    }
    bool chapter_hit = weread_chapter_cached(s_work.id, uid);
    if (chapter_hit) {
        WR_GUARD;
        if (cancelled(NULL) || !s_reader.active || strcmp(s_reader.id, s_work.id)) {
            weread_toc_free(&toc); return ESP_ERR_INVALID_STATE;
        }
        book_weread_adopt_prepared(s_work.id, &toc);
    }
    weread_toc_free(&toc);
    if (!chapter_hit) {
        atomic_store(&s_sync_downloading_uid, uid);
        err = cancelled(NULL) ? ESP_ERR_INVALID_STATE : weread_chapter_download(s_work.id, uid);
        atomic_store(&s_sync_downloading_uid, 0);
    }
    if (err == ESP_OK) {
        err = book_weread_prepare_open(s_work.id);
        if (err == ESP_OK)
            err = book_weread_validate_prepared(s_work.id, uid, cloud.chapter_index, cloud.chapter_offset, &cloud.book_version);
    }
    {
        WR_GUARD;
        if (err != ESP_OK || cancelled(NULL) || !s_reader.active || strcmp(s_reader.id, s_work.id) || !cloud_ahead_of_live(&cloud)) {
            book_weread_clear_prepared();
            if (!cloud_ahead_of_live(&cloud) && !cancelled(NULL) && s_reader.active && !strcmp(s_reader.id, s_work.id))
                return weread_progress_rebase(s_work.id, &cloud, fresh_version, &s_result.local);
            return err != ESP_OK ? err : ESP_ERR_INVALID_STATE;
        }
        wr_copy(s_cloud_jump.id, sizeof(s_cloud_jump.id), s_work.id);
        s_cloud_jump.progress = cloud;
        s_cloud_jump.rebased = false;
        atomic_store(&s_cloud_jump_ready, true);
    }
    ESP_LOGI("weread", "cloud jump ready uid=%u idx=%u off=%u", (unsigned)uid, (unsigned)cloud.chapter_index, (unsigned)cloud.chapter_offset);
    return ESP_OK;
}
static void trim_cover_cache_for_open(void) {
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
#if !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
    metalio_cover_cache_trim(0);
#else
    metalio_cover_cache_trim(512u * 1024u);
#endif
#endif
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline))
#endif
static esp_err_t heal_open_version(const char *id) {
    book_weread_clear_prepared();
    if (!weread_auth_is_authenticated() || cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = weread_shelf_refresh_book(id);
    if (err == ESP_OK && !cancelled(NULL)) {
        err = weread_toc_sync_version(id, weread_shelf_book_version(id));
    }
    return err;
}
static esp_err_t map_heal_error(esp_err_t heal_err, esp_err_t orig_err) {
    if (heal_err == ESP_ERR_NO_MEM || cancelled(NULL) || weread_auth_is_expired()) {
        return heal_err != ESP_OK ? heal_err : ESP_ERR_INVALID_STATE;
    }
    return orig_err;
}
static esp_err_t prepare_open_offline(bool choose, bool cloud, weread_progress_t selected) {
    if (!weread_auth_is_expired()) return ESP_ERR_INVALID_STATE;
    if (!choose) {
        memset(&selected, 0, sizeof(selected));
        if (weread_progress_load(s_work.id, &selected) != ESP_OK || !selected.chapter_uid) {
            memset(&selected, 0, sizeof(selected));
        }
        WR_GUARD;
        s_result.local = selected;
        memset(&s_result.cloud, 0, sizeof(s_result.cloud));
    }
    weread_toc_t toc = {0};
    esp_err_t err = weread_toc_load(s_work.id, &toc);
    if (err == ESP_ERR_NO_MEM) {
        trim_cover_cache_for_open();
        err = weread_toc_load(s_work.id, &toc);
    }
    if (err != ESP_OK || !toc.count) {
        weread_toc_free(&toc);
        return err == ESP_ERR_NO_MEM ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_STATE;
    }
    uint32_t uid = selected.chapter_uid;
    bool exists = false, moved = false;
    for (size_t i = 0; i < toc.count; ++i) {
        if (toc.chapters[i].uid == uid) {
            exists = true;
            moved = selected.chapter_index && selected.chapter_index != toc.chapters[i].index;
            break;
        }
    }
    if (moved) { weread_toc_free(&toc); return WEREAD_ERR_POSITION; }
    if (!exists && uid) { weread_toc_free(&toc); return ESP_ERR_INVALID_RESPONSE; }
    if (!exists) {
        uid = toc.chapters[0].uid;
        selected.chapter_uid = uid;
        selected.chapter_offset = 0;
        selected.chapter_index = toc.chapters[0].index;
        selected.book_version = toc.book_version;
    }
    if (!weread_chapter_cached(s_work.id, uid)) {
        weread_toc_free(&toc);
        return ESP_ERR_INVALID_STATE;
    }
    {
        WR_GUARD;
        book_weread_adopt_prepared_local(s_work.id, &toc);
    }
    weread_toc_free(&toc);
    stage_phase(WEREAD_STAGE_PARSE, 0);
    err = book_weread_prepare_open(s_work.id);
    if (err != ESP_OK) { book_weread_clear_prepared(); return err; }
    WR_GUARD;
    err = book_weread_validate_prepared(s_work.id, uid, selected.chapter_index,
        selected.chapter_offset, &selected.book_version);
    if (err != ESP_OK) { book_weread_clear_prepared(); return err; }
    s_retry_blocked = true;
    if (choose) {
        err = weread_progress_select(s_work.id, &selected, cloud);
        if (err == ESP_OK) {
            selected.dirty = selected.position_pending = !cloud;
            selected.sync_conflict = false;
            if (cloud) selected.pending_pages = selected.reading_time = 0;
            s_result.local = selected;
        }
    } else {
        selected.sync_conflict = false;
        selected.retry_after = 0;
        selected.retry_count = 0;
        err = weread_progress_checkpoint(s_work.id, &selected);
        if (err == ESP_OK) s_result.local = selected;
    }
    if (err != ESP_OK) { book_weread_clear_prepared(); return err; }
    strcpy(s_choice.id, s_work.id);
    s_choice.progress = selected;
    s_choice.valid = true;
    s_choice.pending_cloud_sync = false;
    s_result.open_ready = true;
    return ESP_OK;
}
static esp_err_t prepare_open(bool choose, bool cloud) { WR_MEMORY_SCOPE;
    uint64_t phase[5]={now_ms(),0,0,0,0};
    weread_toc_t toc = {0};
    bool picked_cloud = false, have_local = false;
    esp_err_t progress = ESP_OK;
    uint32_t fresh_version = 0, last_chapter = 0;
    weread_progress_t selected = {0};
    {
        WR_GUARD;
        weread_shelf_sync_discard();
        book_weread_clear_prepared();
        if (choose) {
            s_result.local = s_choice.progress;
            s_choice.pending_cloud_sync = false;
            selected = s_choice.progress;
            fresh_version = s_choice.fresh_version;
            last_chapter = s_choice.last_chapter;
        } else {
            strcpy(s_choice.id,s_work.id);s_choice.valid=false;s_choice.fresh_version=s_choice.last_chapter=0;
            s_choice.pending_cloud_sync = false;
            weread_progress_t local = {0};
            if (weread_progress_load(s_work.id, &local) == ESP_OK && local.chapter_uid) {
                esp_err_t local_toc_err = weread_toc_load(s_work.id, &toc);
                if (local_toc_err == ESP_ERR_NO_MEM) {
                    trim_cover_cache_for_open();
                    local_toc_err = weread_toc_load(s_work.id, &toc);
                }
                if (local_toc_err == ESP_OK && toc.count) {
                    size_t idx = SIZE_MAX;
                    for (size_t i = 0; i < toc.count; ++i) {
                        if (toc.chapters[i].uid == local.chapter_uid) { idx = i; break; }
                    }
                    bool version_ok = !toc.book_version || !local.book_version || toc.book_version == local.book_version;
                    bool index_ok = idx != SIZE_MAX && (!local.chapter_index || local.chapter_index == toc.chapters[idx].index);
                    if (version_ok && index_ok) {
                        if (!local.chapter_index) local.chapter_index = toc.chapters[idx].index;
                        if (!local.book_version) local.book_version = toc.book_version;
                        book_weread_adopt_prepared_local(s_work.id, &toc);
                        weread_toc_free(&toc);
                        stage_phase(WEREAD_STAGE_PARSE, 0);
                        esp_err_t verr = book_weread_validate_prepared(s_work.id, local.chapter_uid, local.chapter_index,
                            local.chapter_offset, &local.book_version);
                        if (verr == ESP_OK) {
                            s_result.local = local;
                            s_choice.progress = local; s_choice.valid = true; s_choice.pending_cloud_sync = weread_auth_is_authenticated();
                            s_result.open_ready = true;
                            phase[4] = now_ms();
                            ESP_LOGI("weread", "open local-fast total=%u uid=%u off=%u",
                                (unsigned)(phase[4] - phase[0]), (unsigned)local.chapter_uid, (unsigned)local.chapter_offset);
                            return ESP_OK;
                        }
                        book_weread_clear_prepared();
                    }
                }
            }
            weread_toc_free(&toc);
            have_local = weread_progress_load(s_work.id, &s_result.local) == ESP_OK && s_result.local.chapter_uid != 0;
        }
    }
    trim_cover_cache_for_open();
    if (!weread_auth_is_authenticated()) {
        return prepare_open_offline(choose, cloud, selected);
    }
    esp_err_t err = ESP_OK;
    bool chapter_hit = false;
    bool healed = false;
    for (;;) {
        weread_toc_free(&toc);
        if (healed && choose) {
            fresh_version = weread_shelf_book_version(s_work.id);
            WR_GUARD;
            s_choice.fresh_version = fresh_version;
        }
        if (!choose) {
            if (healed) {
                WR_GUARD;
                memset(&s_result.cloud, 0, sizeof(s_result.cloud));
                picked_cloud = false; fresh_version = 0; last_chapter = 0;
                have_local = weread_progress_load(s_work.id, &s_result.local) == ESP_OK && s_result.local.chapter_uid != 0;
            }
            weread_progress_t cloud_prog = {0};
            stage_phase(WEREAD_STAGE_PROGRESS, 0);
            progress = weread_progress_fetch_version(s_work.id, &cloud_prog, &fresh_version, &last_chapter);
            ESP_LOGI("weread","progress result=%d local=%u cloud=%u",(int)progress,(unsigned)s_result.local.progress,(unsigned)cloud_prog.progress);
            {
                WR_GUARD;
                s_result.cloud = cloud_prog;
                s_choice.fresh_version = fresh_version; s_choice.last_chapter = last_chapter;
            }
            if (progress == WEREAD_ERR_VERSION) {
                if (!healed) {
                    esp_err_t herr = heal_open_version(s_work.id);
                    if (herr == ESP_OK) { healed = true; continue; }
                    err = map_heal_error(herr, progress);
                    break;
                }
                err = progress; break;
            }
            if (progress == ESP_ERR_NO_MEM || (progress != ESP_OK && (!have_local || weread_auth_is_expired()))) { err = progress; break; }
            picked_cloud = progress == ESP_OK && (!have_local || weread_progress_ahead(&s_result.cloud, &s_result.local));
            if (picked_cloud) {
                s_result.local = s_result.cloud; s_result.local.reading_time = 0; s_result.local.pending_pages = 0;
                s_result.local.dirty = s_result.local.position_pending = s_result.local.sync_conflict = false;
            }
            selected = s_result.local;
        }
        phase[1]=now_ms();
        stage_phase(WEREAD_STAGE_TOC, 0);
        esp_err_t prepared=weread_toc_prepare(s_work.id,false,fresh_version,&toc);
        if (prepared == WEREAD_ERR_VERSION) {
            weread_toc_free(&toc);
            if (!healed) {
                esp_err_t herr = heal_open_version(s_work.id);
                if (herr == ESP_OK) { healed = true; continue; }
                err = map_heal_error(herr, prepared);
                break;
            }
            err = prepared; break;
        }
        if(prepared!=ESP_OK||!toc.count){weread_toc_free(&toc);err=prepared!=ESP_OK?prepared:ESP_ERR_NOT_FOUND;break;}
        uint32_t uid = selected.chapter_uid;
        bool exists = false, moved=false;
        for (size_t i = 0; i < toc.count; ++i) if (toc.chapters[i].uid == uid) {
            exists=true;moved=selected.chapter_index&&selected.chapter_index!=toc.chapters[i].index;
        }
        if((uid&&(!exists||moved))||last_chapter>toc.chapters[toc.count-1].index){
            weread_toc_free(&toc);stage_phase(WEREAD_STAGE_TOC, 0);prepared=weread_toc_prepare(s_work.id,true,fresh_version,&toc);
            if (prepared == WEREAD_ERR_VERSION) {
                weread_toc_free(&toc);
                if (!healed) {
                    esp_err_t herr = heal_open_version(s_work.id);
                    if (herr == ESP_OK) { healed = true; continue; }
                    err = map_heal_error(herr, prepared);
                    break;
                }
                err = prepared; break;
            }
            if(prepared!=ESP_OK||!toc.count){weread_toc_free(&toc);err=prepared!=ESP_OK?prepared:ESP_ERR_NOT_FOUND;break;}
            exists=moved=false;
            for(size_t i=0;i<toc.count;++i)if(toc.chapters[i].uid==uid){
                exists=true;moved=selected.chapter_index&&selected.chapter_index!=toc.chapters[i].index;
            }
            if(moved){weread_toc_free(&toc);err=WEREAD_ERR_POSITION;break;}
        }
        ESP_LOGI("weread","target p=%u uid=%u idx=%u off=%u choice=%d ok=%d",(unsigned)selected.progress,(unsigned)uid,(unsigned)selected.chapter_index,(unsigned)selected.chapter_offset,choose,exists);
        if(!exists&&uid){weread_toc_free(&toc);err=ESP_ERR_INVALID_RESPONSE;break;}
        if (!exists) { uid = toc.chapters[0].uid; selected.chapter_uid = uid; selected.chapter_offset = 0; selected.chapter_index = toc.chapters[0].index; selected.book_version = toc.book_version; }
        phase[2]=now_ms();
        // 仅缓存命中保留typed目录；下载解压前释放，维持原峰值内存。/ Retain the typed TOC only on cache hits; release it before download/inflate to retain the memory budget.
        chapter_hit=weread_chapter_cached(s_work.id,uid);
        if(chapter_hit){WR_GUARD;book_weread_adopt_prepared(s_work.id,&toc);}
        weread_toc_free(&toc);
        if (!chapter_hit) stage_phase(WEREAD_STAGE_CHAPTER, 0);
        uint32_t ver_before = weread_shelf_book_version(s_work.id);
        err = cancelled(NULL) ? ESP_ERR_INVALID_STATE : chapter_hit?ESP_OK:weread_chapter_download(s_work.id, uid);
        if (err == WEREAD_ERR_VERSION || weread_shelf_book_version(s_work.id) != ver_before) healed = true;
        phase[3]=now_ms();
        if (err == ESP_OK) {
            stage_phase(WEREAD_STAGE_PARSE, 0);
            err=book_weread_prepare_open(s_work.id);
            if (err == WEREAD_ERR_VERSION && !healed) {
                esp_err_t herr = heal_open_version(s_work.id);
                if (herr == ESP_OK) { healed = true; continue; }
                err = map_heal_error(herr, err);
                break;
            }
            if(err!=ESP_OK)break;
            {
                WR_GUARD;
                err=book_weread_validate_prepared(s_work.id,uid,selected.chapter_index,selected.chapter_offset,&selected.book_version);
                if(err!=ESP_OK)book_weread_clear_prepared();
            }
            if (err == WEREAD_ERR_VERSION && !healed) {
                esp_err_t herr = heal_open_version(s_work.id);
                if (herr == ESP_OK) { healed = true; continue; }
                err = map_heal_error(herr, err);
                break;
            }
            if(err!=ESP_OK)break;
            // 位置写入从这里开始；重放超时可能重复提交进度，因此此后不再重试。
            // Persistence starts here; replaying a timeout could duplicate a progress commit, so no retry past this point.
            WR_GUARD;
            s_retry_blocked = true;
            if(choose){
                err=weread_progress_select(s_work.id,&selected,cloud);
                if(err==ESP_OK){selected.dirty=selected.position_pending=!cloud;selected.sync_conflict=false;if(cloud)selected.pending_pages=selected.reading_time=0;s_result.local=selected;}
            }else if(picked_cloud){
                err=weread_progress_select(s_work.id,&selected,true);
                if(err==ESP_OK){selected.dirty=selected.position_pending=selected.sync_conflict=false;selected.pending_pages=selected.reading_time=0;s_result.local=selected;}
            }else{
                if(progress==ESP_OK&&have_local&&s_result.cloud.chapter_uid){
                    selected.base_uid=s_result.cloud.base_uid;selected.base_index=s_result.cloud.base_index;
                    selected.base_offset=s_result.cloud.base_offset;selected.base_version=s_result.cloud.base_version;
                    selected.base_known=s_result.cloud.base_known;
                }
                selected.sync_conflict=false;selected.retry_after=0;selected.retry_count=0;
                err=weread_progress_checkpoint(s_work.id,&selected);
                if(err==ESP_OK)s_result.local=selected;
            }
            if(err!=ESP_OK){book_weread_clear_prepared();break;}
            strcpy(s_choice.id, s_work.id); s_choice.progress = selected; s_choice.valid = true; s_result.open_ready = true;
        }
        break;
    }
    if (err != ESP_OK && !s_retry_blocked && !weread_auth_is_authenticated() && weread_auth_is_expired() && !cancelled(NULL)) {
        book_weread_clear_prepared();
        weread_toc_free(&toc);
        return prepare_open_offline(choose, cloud, selected);
    }
    phase[4]=now_ms();
    ESP_LOGI("weread","open timing progress=%u toc=%u chapter=%u verify=%u total=%u cached=%d",
        (unsigned)(phase[1]-phase[0]),(unsigned)(phase[2]-phase[1]),(unsigned)(phase[3]-phase[2]),(unsigned)(phase[4]-phase[3]),(unsigned)(phase[4]-phase[0]),chapter_hit);
    (void)phase;
    if(err!=ESP_OK)book_weread_clear_prepared();
    weread_toc_free(&toc); return err;
}
static uint16_t s_prefetch_dirty_count;
static void update_shelf_counts_best_effort(const char *id, uint16_t cached, uint16_t total) {
    if (!wr_valid_id(id)) return;
    (void)weread_shelf_set_chapter_counts(id, cached, total);
}
static void update_shelf_counts_mem_best_effort(const char *id, uint16_t cached, uint16_t total, bool flush) {
    if (!wr_valid_id(id)) return;
    bool changed = false;
    (void)weread_shelf_set_chapter_counts_mem(id, cached, total, &changed);
    if (changed) {
        if (s_prefetch_dirty_count < UINT16_MAX) ++s_prefetch_dirty_count;
    }
    if (flush || s_prefetch_dirty_count >= 10u) {
        if (weread_shelf_flush_counts() == ESP_OK) s_prefetch_dirty_count = 0;
    }
}
static uint16_t lookup_shelf_total_chapters(const char *id) {
    if (!wr_valid_id(id)) return 0;
    uint16_t total = 0;
    if (!weread_shelf_get_chapter_counts(id, NULL, &total) || !total) {
        weread_toc_t toc = {0};
        if (weread_toc_load(id, &toc) == ESP_OK && toc.count) {
            total = toc.count > UINT16_MAX ? UINT16_MAX : (uint16_t)toc.count;
        }
        weread_toc_free(&toc);
    }
    return total;
}
static void book_cache_clear(void) {
    WR_GUARD;
    free(s_book_cache.uids); free(s_book_cache.bits);
    memset(&s_book_cache, 0, sizeof(s_book_cache));
    atomic_store(&s_cached_count, 0);
    atomic_store(&s_total_count, 0);
}
static void book_cache_init(const char *id) {
    WR_GUARD;
    free(s_book_cache.uids); free(s_book_cache.bits);
    memset(&s_book_cache, 0, sizeof(s_book_cache));
    if (!id || !*id) { atomic_store(&s_cached_count, 0); atomic_store(&s_total_count, 0); return; }
    wr_copy(s_book_cache.id, sizeof(s_book_cache.id), id);
    size_t total = book_weread_count();
    s_book_cache.total = total;
    atomic_store(&s_total_count, (uint32_t)total);
    atomic_store(&s_cached_count, 0);
    if (total) {
        s_book_cache.uids = wr_alloc(total * sizeof(uint32_t));
        s_book_cache.bits_bytes = (total + 7u) / 8u;
        s_book_cache.bits = wr_alloc(s_book_cache.bits_bytes);
        if (s_book_cache.uids) for (size_t i = 0; i < total; ++i) s_book_cache.uids[i] = book_weread_uid(i);
    }
}
static void book_cache_mark(const char *id, size_t idx, bool cached) {
    WR_GUARD;
    if (!id || strcmp(s_book_cache.id, id) || idx >= s_book_cache.total || !s_book_cache.bits) return;
    uint8_t mask = (uint8_t)(1u << (idx & 7u));
    bool was = (s_book_cache.bits[idx >> 3u] & mask) != 0;
    if (cached && !was) {
        s_book_cache.bits[idx >> 3u] |= mask;
        ++s_book_cache.cached;
        atomic_store(&s_cached_count, s_book_cache.cached);
    } else if (!cached && was) {
        s_book_cache.bits[idx >> 3u] &= (uint8_t)~mask;
        if (s_book_cache.cached) --s_book_cache.cached;
        atomic_store(&s_cached_count, s_book_cache.cached);
    }
}
static bool book_cache_bit(const char *id, size_t idx) {
    WR_GUARD;
    if (!id || strcmp(s_book_cache.id, id) || idx >= s_book_cache.total || !s_book_cache.bits) return false;
    return (s_book_cache.bits[idx >> 3u] & (uint8_t)(1u << (idx & 7u))) != 0;
}
static void book_cache_invalidate(const char *id) {
    WR_GUARD;
    if (!id || strcmp(s_book_cache.id, id)) return;
    if (s_book_cache.uids) for (size_t i = 0; i < s_book_cache.total; ++i) s_book_cache.uids[i] = book_weread_uid(i);
    if (s_book_cache.bits && s_book_cache.bits_bytes) memset(s_book_cache.bits, 0, s_book_cache.bits_bytes);
    s_book_cache.cached = 0;
    s_book_cache.scanned = false;
    atomic_store(&s_cached_count, 0);
}
static bool book_cache_ready(const char *id) {
    WR_GUARD;
    return id && !strcmp(s_book_cache.id, id) && s_book_cache.bits != NULL;
}
static bool book_cache_scanned(const char *id) {
    WR_GUARD;
    return id && !strcmp(s_book_cache.id, id) && s_book_cache.scanned;
}
static uint32_t book_cache_uid_for(const char *id, size_t idx, size_t *total_out) {
    WR_GUARD;
    if (!id || strcmp(s_book_cache.id, id)) { if (total_out) *total_out = 0; return 0; }
    if (total_out) *total_out = s_book_cache.total;
    if (s_book_cache.total > 0) return idx < s_book_cache.total && s_book_cache.uids ? s_book_cache.uids[idx] : 0;
    return book_weread_uid(idx);
}
typedef struct {
    uint32_t uid;
    uint32_t idx;
} scan_uid_entry_t;
static int compare_scan_uid(const void *a, const void *b) {
    uint32_t x = ((const scan_uid_entry_t *)a)->uid, y = ((const scan_uid_entry_t *)b)->uid;
    return x == y ? 0 : x < y ? -1 : 1;
}
static bool scan_fg_active(void) {
    uint64_t fg = atomic_load(&s_fg_input_ms);
    if (!fg) return false;
    uint64_t now = now_ms();
    return now >= fg && now - fg < 600;
}
static esp_err_t scan_book_cache(const char *id) {
    size_t total = 0;
    scan_uid_entry_t *entries = NULL;
    {
        WR_GUARD;
        if (!id || strcmp(s_book_cache.id, id)) return ESP_ERR_INVALID_STATE;
        if (s_book_cache.scanned || !s_book_cache.total) return ESP_OK;
        if (!s_book_cache.uids) return ESP_ERR_INVALID_STATE;
        total = s_book_cache.total;
        entries = wr_alloc(total * sizeof(*entries));
        if (!entries) return ESP_ERR_NO_MEM;
        for (size_t i = 0; i < total; ++i) {
            entries[i] = (scan_uid_entry_t){ .uid = s_book_cache.uids[i], .idx = (uint32_t)i };
        }
    }
    qsort(entries, total, sizeof(*entries), compare_scan_uid);
    char dir_path[512];
    if (wr_cache_path(id, "chapters.json", dir_path, sizeof(dir_path)) != ESP_OK) {
        free(entries);
        return ESP_ERR_INVALID_ARG;
    }
    char *slash = strrchr(dir_path, '/');
    if (slash) *slash = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) {
        free(entries);
        if (errno == ENOENT) {
            WR_GUARD;
            if (!strcmp(s_book_cache.id, id)) s_book_cache.scanned = true;
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    size_t seen = 0, verified = 0;
    bool ok = true;
    struct dirent *e;
    for (;;) {
        if (cancelled(NULL) || scan_fg_active()) { ok = false; break; }
        weread_client_yield_for_display(cancelled, NULL);
        errno = 0;
        e = readdir(dir);
        if (!e) { if (errno) ok = false; break; }
        if (!(++seen & 31u)) pause_ms(2);
        if (strncmp(e->d_name, "chapter-", 8)) continue;
        const char *p = e->d_name + 8;
        if (*p < '0' || *p > '9') continue;
        char *end = NULL;
        unsigned long parsed = strtoul(p, &end, 10);
        if (!parsed || parsed > UINT32_MAX || !end || strcmp(end, ".json")) continue;
        scan_uid_entry_t key = { .uid = (uint32_t)parsed, .idx = 0 };
        const scan_uid_entry_t *match = bsearch(&key, entries, total, sizeof(*entries), compare_scan_uid);
        if (!match) continue;
        bool hit = weread_chapter_cached(id, key.uid);
        book_cache_mark(id, match->idx, hit);
        if (!(++verified & 3u)) pause_ms(2);
    }
    closedir(dir);
    free(entries);
    if (!ok || cancelled(NULL) || scan_fg_active()) return ESP_ERR_INVALID_STATE;
    uint16_t c16 = 0, t16 = 0;
    {
        WR_GUARD;
        if (!strcmp(s_book_cache.id, id)) {
            s_book_cache.scanned = true;
            c16 = s_book_cache.cached > UINT16_MAX ? UINT16_MAX : (uint16_t)s_book_cache.cached;
            t16 = s_book_cache.total > UINT16_MAX ? UINT16_MAX : (uint16_t)s_book_cache.total;
        }
    }
    if (t16 > 0) update_shelf_counts_best_effort(id, c16, t16);
    ESP_LOGI("weread", "cache scan id=%s entries=%u verified=%u cached=%u/%u",
        id, (unsigned)seen, (unsigned)verified,
        (unsigned)atomic_load(&s_cached_count), (unsigned)atomic_load(&s_total_count));
    return ESP_OK;
}
static void sync_open_book_shelf_counts(const char *id, bool flush) {
    uint16_t c16 = 0, t16 = 0;
    bool ready = false;
    {
        WR_GUARD;
        if (id && !strcmp(s_book_cache.id, id) && s_book_cache.scanned && s_book_cache.total > 0) {
            c16 = s_book_cache.cached > UINT16_MAX ? UINT16_MAX : (uint16_t)s_book_cache.cached;
            t16 = s_book_cache.total > UINT16_MAX ? UINT16_MAX : (uint16_t)s_book_cache.total;
            ready = true;
        }
    }
    if (ready) update_shelf_counts_mem_best_effort(id, c16, t16, flush);
    else if (flush && weread_shelf_flush_counts() == ESP_OK) s_prefetch_dirty_count = 0;
}
static esp_err_t prefetch_job(void) {
    if (s_work.prefetch_forward && !strcmp(s_work.value, "scan")) {
        s_result.chapter_idx = SIZE_MAX;
        return scan_book_cache(s_work.id);
    }
    size_t target_idx = s_work.chapter_idx;
    uint32_t target_uid = (uint32_t)s_work.chapter;
    if (!target_uid) {
        if (!book_cache_scanned(s_work.id)) {
            esp_err_t scan_err = scan_book_cache(s_work.id);
            if (scan_err != ESP_OK) return scan_err;
        }
        bool scanned = book_cache_scanned(s_work.id);
        size_t probes = 0;
        for (size_t i = s_work.chapter_idx; ; ++i) {
            if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
            weread_client_yield_for_display(cancelled, NULL);
            size_t t = 0;
            uint32_t uid = book_cache_uid_for(s_work.id, i, &t);
            if (t && i >= t) {
                s_result.prefetch_done = true;
                s_result.chapter_idx = i;
                sync_open_book_shelf_counts(s_work.id, true);
                return ESP_OK;
            }
            if (!uid) {
                if (t) return ESP_ERR_INVALID_STATE;
                s_result.prefetch_done = true;
                s_result.chapter_idx = i;
                sync_open_book_shelf_counts(s_work.id, true);
                return ESP_OK;
            }
            if (scanned && book_cache_bit(s_work.id, i)) continue;
            if (!(++probes & 3u)) pause_ms(2);
            if (weread_chapter_cached(s_work.id, uid)) {
                book_cache_mark(s_work.id, i, true);
                continue;
            }
            target_idx = i;
            target_uid = uid;
            break;
        }
    } else if (weread_chapter_cached(s_work.id, target_uid)) {
        book_cache_mark(s_work.id, target_idx, true);
        s_result.chapter_uid = target_uid;
        s_result.chapter_idx = target_idx;
        sync_open_book_shelf_counts(s_work.id, false);
        return ESP_OK;
    }
    if (!weread_auth_is_authenticated()) return ESP_ERR_INVALID_STATE;
    atomic_store(&s_prefetch_downloading_uid, target_uid);
    esp_err_t err = weread_chapter_download_ex(s_work.id, target_uid, true);
    atomic_store(&s_prefetch_downloading_uid, 0);
    s_result.chapter_uid = target_uid;
    s_result.chapter_idx = target_idx;
    if (err == ESP_OK && !cancelled(NULL)) {
        book_cache_mark(s_work.id, target_idx, true);
        sync_open_book_shelf_counts(s_work.id, false);
    }
    return err;
}
static esp_err_t cover_job_impl(const char *url,bool retry) {
    WR_MEMORY_SCOPE;
#if defined(ESP_PLATFORM)
    char uri[80];
    snprintf(uri,sizeof(uri),"weread://%s",s_work.id);
    s_result.cover=wr_alloc(WEREAD_COVER_WIDTH*WEREAD_COVER_HEIGHT);
    if (!s_result.cover) return ESP_ERR_NO_MEM;
    if (book_weread_cover(uri,s_result.cover,WEREAD_COVER_WIDTH*WEREAD_COVER_HEIGHT)) return ESP_OK;
    free(s_result.cover);s_result.cover=NULL;
    struct { char path[512], temporary[544], parent[512]; } *io __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*io));
    if (!io) return ESP_ERR_NO_MEM;
    if(!url&&!retry&&weread_cover_cache_path(s_work.id,"failed",io->path,sizeof(io->path))==ESP_OK){
        uint64_t failed=0;FILE *f=fopen(io->path,"rb");
        if(f){if(fread(&failed,sizeof(failed),1,f)!=1)failed=0;fclose(f);}
        uint64_t now=(uint64_t)time(NULL);
        if(failed>=1700000000&&now>=failed&&now-failed<300){
            // 失败缓存仅阻远端：合法原图在解码器临时OOM后仍可本地重试。
            // Back off only remote requests; a valid raw image can recover after transient decoder OOM.
            s_result.cover_retry_seconds=(unsigned)(300-(now-failed));
        }
    }
    if (weread_cover_cache_path(s_work.id,"img",io->path,sizeof(io->path)) != ESP_OK) return ESP_ERR_INVALID_ARG;
    bool have_magic = false;
    unsigned char magic[12] = {0};
    if (url && *url) {
        snprintf(io->temporary, sizeof(io->temporary), "%s.part", io->path);
        strcpy(io->parent, io->path); *strrchr(io->parent, '/') = 0;
        if (wr_mkdirs(io->parent) != ESP_OK) return ESP_FAIL;
        FILE *out = fopen(io->temporary, "w+b"); if (!out) return ESP_FAIL;
        weread_download_info_t info={0};
        esp_err_t err = weread_client_download(url, out, 1024u * 1024u, cancelled, NULL, &info);
        s_result.http_status = info.status;
        bool ok = err == ESP_OK && info.status == 200 && !fflush(out);
        rewind(out); size_t n = fread(magic, 1, sizeof(magic), out);
        have_magic = n >= 4;
        ok = ok && n >= 3 && (!memcmp(magic, "\x89PNG", 4) || (magic[0] == 255 && magic[1] == 216 && magic[2] == 255) ||
            (n==12&&!memcmp(magic,"RIFF",4)&&!memcmp(magic+8,"WEBP",4)));
        if (fclose(out)) ok = false;
        if (!ok || cancelled(NULL)) { unlink(io->temporary); return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE; }
        unlink(io->path); if (rename(io->temporary, io->path)) { unlink(io->temporary); return ESP_FAIL; }
    }
    bool png = false;
    if (have_magic) {
        png = !memcmp(magic, "\x89PNG", 4);
    } else {
        FILE *file = fopen(io->path, "rb"); if (!file) return ESP_ERR_NOT_FOUND;
        unsigned char first[8]; size_t n = fread(first, 1, 8, file); fclose(file);
        png = n == 8 && !memcmp(first, "\x89PNG", 4);
    }
    s_result.cover = wr_alloc(WEREAD_COVER_WIDTH * WEREAD_COVER_HEIGHT);
    if (!s_result.cover) return ESP_ERR_NO_MEM;
    if (!book_image_file_grayscale(io->path, png, WEREAD_COVER_WIDTH, WEREAD_COVER_HEIGHT, s_result.cover)) {
        free(s_result.cover); s_result.cover = NULL; return ESP_ERR_INVALID_RESPONSE;
    }
    if (weread_cover_cache_path(s_work.id,"gray",io->path,sizeof(io->path)) == ESP_OK)
        (void)wr_write_bytes(io->path,s_result.cover,WEREAD_COVER_WIDTH*WEREAD_COVER_HEIGHT);
    s_result.cover_retry_seconds=0;
    return ESP_OK;
#else
    (void)url;(void)retry; return ESP_ERR_NOT_SUPPORTED;
#endif
}
static esp_err_t cover_job(const char *url){
    bool retry=url&&!strcmp(url,"retry");if(retry)url=NULL;
    esp_err_t err=cover_job_impl(url,retry);
#if defined(ESP_PLATFORM)
    if((err==ESP_OK||(url&&*url))&&err!=ESP_ERR_NO_MEM&&err!=ESP_ERR_INVALID_STATE&&!cancelled(NULL)&&read_pico_sd_io_ready()){
        char *path __attribute__((cleanup(wr_release))) = wr_alloc(512);
        if(!path)return err==ESP_OK?ESP_OK:ESP_ERR_NO_MEM;
        if(weread_cover_cache_path(s_work.id,"failed",path,512)==ESP_OK){
            if(err==ESP_OK)(void)unlink(path);
            else if(s_result.http_status!=404||weread_cover_is_original(s_work.id,url)){
                uint64_t now=(uint64_t)time(NULL);
                if(now>=1700000000&&now<=INT64_MAX){
                    (void)wr_write_bytes(path,&now,sizeof(now));s_result.cover_retry_seconds=300;
                }
            }
        }
    }
#endif
    return err;
}
static esp_err_t cover_dispatch(void) {
    if (!strcmp(s_work.value, "remote") || !strcmp(s_work.value, "original")) {
        struct { weread_book_t book; char url[512]; } *s __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*s));
        if (!s) return ESP_ERR_NO_MEM;
        weread_cover_retry_t retry = {.original = !strcmp(s_work.value, "original")};
        esp_err_t err = weread_shelf_find(s_work.id, &s->book)
            ? weread_cover_retry_url(s->book.cover, &retry, 0, s->url, sizeof(s->url))
            : ESP_ERR_NOT_FOUND;
        return err == ESP_OK ? cover_job(s->url) : err;
    }
    if (!strcmp(s_work.value, "warm") || !strcmp(s_work.value, "warm-disk")) {
        struct { weread_book_t book; char url[512], ready[512]; } *s __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*s));
        if (!s) return ESP_ERR_NO_MEM;
        struct stat st;
        if (weread_cover_cache_path(s_work.id, "gray", s->ready, sizeof(s->ready)) == ESP_OK &&
            !stat(s->ready, &st) && S_ISREG(st.st_mode) && st.st_size == WEREAD_COVER_WIDTH * WEREAD_COVER_HEIGHT)
            return ESP_OK;
        esp_err_t err = cover_job(NULL);
        if (err != ESP_OK && !s_result.cover_retry_seconds && !cancelled(NULL) &&
            weread_shelf_find(s_work.id, &s->book) &&
            weread_cover_get_url(s->book.cover, s->url, sizeof(s->url)) == ESP_OK)
            err = cover_job(s->url);
        return err;
    }
    return cover_job(s_work.value);
}
static esp_err_t probe_shelf_job(void) { WR_MEMORY_SCOPE;
    if (!s_work.probe_count) return ESP_ERR_INVALID_ARG;
    bool updated = false;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < s_work.probe_count; ++i) {
        if (cancelled(NULL)) { err = ESP_ERR_INVALID_STATE; break; }
        if (i) pause_ms(1);
        uint16_t cached = 0, total = 0;
        esp_err_t cerr = weread_chapter_count_cached(s_work.probe_ids[i], &cached, &total, cancelled, NULL);
        if (cerr == ESP_ERR_NOT_FOUND) continue;
        if (cerr == ESP_ERR_INVALID_STATE || cerr == ESP_ERR_NO_MEM) { err = cerr; break; }
        if (cerr != ESP_OK) continue;
        if (total > 0 || cached > 0) {
            uint16_t prev_c = 0, prev_t = 0;
            bool had = weread_shelf_get_chapter_counts(s_work.probe_ids[i], &prev_c, &prev_t);
            if (!had || prev_c != cached || prev_t != total) {
                bool chg = false;
                esp_err_t serr = weread_shelf_set_chapter_counts_mem(s_work.probe_ids[i], cached, total, &chg);
                if (serr == ESP_OK && chg) updated = true;
                else if (serr == ESP_ERR_NO_MEM) { err = serr; break; }
            }
        }
    }
    if (updated) {
        (void)weread_shelf_flush_counts();
        s_prefetch_dirty_count = 0;
        snapshot();
    }
    return err;
}
static esp_err_t cache_book_job(void) { WR_MEMORY_SCOPE;
    if (!wr_valid_id(s_work.id)) return ESP_ERR_INVALID_ARG;
    if (!weread_auth_is_authenticated() || cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    bool healed = false;
    bool cache_active = false;
    uint32_t *uids __attribute__((cleanup(wr_release))) = NULL;
    uint8_t *bits __attribute__((cleanup(wr_release))) = NULL;
    esp_err_t err = ESP_OK;
    uint32_t version = 0;
    uint16_t total_u16 = 0;
    uint32_t cached_count = 0;
    uint32_t last_persisted = UINT32_MAX;

    for (;;) {
        free(uids); uids = NULL;
        free(bits); bits = NULL;
        total_u16 = 0;
        cached_count = 0;
        last_persisted = UINT32_MAX;
        version = weread_shelf_book_version(s_work.id);
        weread_toc_t toc = {0};
        err = weread_toc_prepare(s_work.id, false, version, &toc);
        if (err == WEREAD_ERR_VERSION) {
            weread_toc_free(&toc);
            if (!healed) {
                esp_err_t herr = heal_open_version(s_work.id);
                if (herr == ESP_OK) { healed = true; continue; }
                err = map_heal_error(herr, err);
            }
            break;
        }
        if (err != ESP_OK || !toc.count) {
            weread_toc_free(&toc);
            err = err != ESP_OK ? err : ESP_ERR_NOT_FOUND;
            break;
        }
        size_t total = toc.count;
        uids = wr_alloc(total * sizeof(uint32_t));
        size_t bits_bytes = (total + 7u) / 8u;
        bits = wr_alloc(bits_bytes);
        if (!uids || !bits) {
            weread_toc_free(&toc);
            err = ESP_ERR_NO_MEM;
            break;
        }
        for (size_t i = 0; i < total; ++i) uids[i] = toc.chapters[i].uid;
        weread_toc_free(&toc);

        err = weread_chapter_scan_cached_uids(s_work.id, uids, total, bits, &cached_count, cancelled, NULL);
        if (err != ESP_OK) break;

        total_u16 = total > UINT16_MAX ? UINT16_MAX : (uint16_t)total;
        if (!cache_active) {
            stats_lock();
            wr_copy(s_shelf_cache_id, sizeof(s_shelf_cache_id), s_work.id);
            stats_unlock();
            atomic_store(&s_shelf_cache_storage_blocked, false);
            atomic_store(&s_shelf_cache_active, true);
            cache_active = true;
        }
        atomic_store(&s_shelf_cache_total, total_u16);
        atomic_store(&s_shelf_cache_done, cached_count > total_u16 ? total_u16 : cached_count);

        if (total_u16 > 0) {
            uint16_t c16 = cached_count > total_u16 ? total_u16 : (uint16_t)cached_count;
            update_shelf_counts_best_effort(s_work.id, c16, total_u16);
            last_persisted = c16;
        }
        bool restart = false;
        for (size_t i = 0; i < total && err == ESP_OK; ++i) {
            if (bits[i >> 3u] & (uint8_t)(1u << (i & 7u))) continue;
            if (cancelled(NULL) || !weread_auth_is_authenticated()) { err = ESP_ERR_INVALID_STATE; break; }
            esp_err_t derr = weread_chapter_download_ex(s_work.id, uids[i], true);
            if (derr == ESP_OK) {
                if (weread_shelf_book_version(s_work.id) != version) {
                    if (cancelled(NULL)) { err = ESP_ERR_INVALID_STATE; break; }
                    if (!healed) {
                        healed = true;
                        restart = true;
                        break;
                    }
                    err = WEREAD_ERR_VERSION;
                    break;
                }
                bits[i >> 3u] |= (uint8_t)(1u << (i & 7u));
                ++cached_count;
                uint16_t c16 = cached_count > total_u16 ? total_u16 : (uint16_t)cached_count;
                atomic_store(&s_shelf_cache_done, c16);
                if (last_persisted == UINT32_MAX || c16 >= last_persisted + 10u) {
                    update_shelf_counts_best_effort(s_work.id, c16, total_u16);
                    last_persisted = c16;
                }
            } else {
                if (derr == WEREAD_ERR_STORAGE) atomic_store(&s_shelf_cache_storage_blocked, true);
                err = derr;
                break;
            }
        }
        if (restart) continue;
        break;
    }
    if (!cache_active) {
        if (healed) snapshot();
        return err;
    }
    uint16_t final_c16 = cached_count > total_u16 ? total_u16 : (uint16_t)cached_count;
    uint16_t final_t16 = total_u16;
    if ((weread_shelf_book_version(s_work.id) != version || !final_t16) && !cancelled(NULL)) {
        uint16_t rc = 0, rt = 0;
        if (weread_chapter_count_cached(s_work.id, &rc, &rt, cancelled, NULL) == ESP_OK && rt > 0) {
            final_c16 = rc;
            final_t16 = rt;
            last_persisted = UINT32_MAX;
        }
    }
    if (final_t16 > 0 && final_c16 != last_persisted) {
        update_shelf_counts_best_effort(s_work.id, final_c16, final_t16);
    }
    s_result.shelf_done = final_c16;
    s_result.shelf_total = final_t16;
    cache_stats_invalidate();
    snapshot();
    atomic_store(&s_shelf_cache_active, false);
    return err;
}
static esp_err_t cache_stats_job(void) { WR_MEMORY_SCOPE;
    weread_chapter_cache_stats_t st = {0};
    esp_err_t err = weread_chapter_inspect_cache_stats(&st, cancelled, NULL);
    if (err == ESP_OK) cache_stats_store(&st);
    return err;
}
static esp_err_t cache_clean_finished_job(void) { WR_MEMORY_SCOPE;
    weread_shelf_item_t *items = NULL;
    size_t count = 0;
    esp_err_t err = weread_shelf_snapshot(&items, &count);
    if (err != ESP_OK) return err;
    weread_progress_t *prog __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*prog));
    if (!prog) { free(items); return ESP_ERR_NO_MEM; }
    uint32_t cleaned_books = 0;
    for (size_t i = 0; i < count; ++i) {
        if (cancelled(NULL)) { err = ESP_ERR_INVALID_STATE; break; }
        esp_err_t perr = weread_progress_load(items[i].book_id, prog);
        bool finished = (perr == ESP_OK && prog->chapter_uid != 0)
            ? (prog->progress >= 100)
            : (items[i].progress >= 100);
        if (!finished) continue;
        if (book_weread_id_in_use(items[i].book_id)) continue;
        uint64_t freed = 0;
        esp_err_t cerr = weread_chapter_clean_book_cache(items[i].book_id, &freed, cancelled, NULL);
        if (cerr != ESP_OK) { err = cerr; break; }
        book_cache_invalidate(items[i].book_id);
        update_shelf_counts_best_effort(items[i].book_id, 0, items[i].total_chapters);
        if (freed > 0 || items[i].cached_chapters > 0) ++cleaned_books;
    }
    free(items);
    cache_stats_invalidate();
    if (err == ESP_OK) {
        weread_chapter_cache_stats_t st = {0};
        if (weread_chapter_inspect_cache_stats(&st, cancelled, NULL) == ESP_OK) cache_stats_store(&st);
        s_result.shelf_done = cleaned_books;
    }
    snapshot();
    return err;
}
static esp_err_t cache_clean_all_job(void) { WR_MEMORY_SCOPE;
    uint64_t freed = 0;
    uint32_t cleaned = 0;
    esp_err_t err = weread_chapter_clean_all_caches(&freed, &cleaned, cancelled, NULL);
    if (err == ESP_OK) {
        weread_shelf_item_t *items = NULL;
        size_t count = 0;
        if (weread_shelf_snapshot(&items, &count) == ESP_OK) {
            for (size_t i = 0; i < count; ++i) {
                if (book_weread_id_in_use(items[i].book_id)) continue;
                book_cache_invalidate(items[i].book_id);
                if (items[i].cached_chapters != 0) {
                    update_shelf_counts_best_effort(items[i].book_id, 0, items[i].total_chapters);
                }
            }
            free(items);
        }
    }
    cache_stats_invalidate();
    if (err == ESP_OK) {
        weread_chapter_cache_stats_t st = {0};
        if (weread_chapter_inspect_cache_stats(&st, cancelled, NULL) == ESP_OK) cache_stats_store(&st);
        s_result.shelf_done = cleaned;
    }
    snapshot();
    return err;
}
static bool retryable_job(weread_job_t job) {
    // 前台开书/本地/云端开书、冷章和静默云端同步在瞬时超时后静默重放一次；目录任务由阅读页在UI线程同步等待，重试只会加倍开书冻结，故不重试。
    // Foreground open/local/cloud open, cold chapters and silent cloud sync replay once after a transient timeout; the TOC job joins synchronously on the UI thread, so replaying it would double the open freeze.
    return job == WR_JOB_OPEN || job == WR_JOB_LOCAL || job == WR_JOB_CLOUD ||
        job == WR_JOB_CHAPTER || job == WR_JOB_CLOUD_SYNC;
}
static esp_err_t job_run(void) {
    esp_err_t err = ESP_OK;
    switch (s_work.job) {
        case WR_JOB_INIT: weread_runtime_load_local(); snapshot(); break;
        case WR_JOB_SYNC:
            if (!weread_auth_is_authenticated()) {
                weread_shelf_sync_discard();
                err = ESP_ERR_INVALID_STATE;
            } else {
                err = weread_shelf_sync_step(strcmp(s_work.value, "continue") != 0);
            }
            snapshot();
            s_result.shelf_loading = err == ESP_OK && weread_shelf_sync_pending();
            weread_shelf_sync_progress(&s_result.shelf_done, &s_result.shelf_total, &s_result.shelf_skipped);
            break;
        case WR_JOB_QR: err = weread_auth_start_qr_login(s_result.uid, sizeof(s_result.uid), s_result.qr_url, sizeof(s_result.qr_url)); break;
        case WR_JOB_POLL: {
            weread_qr_state_t state; char code[64];
            err = weread_auth_poll_qr_login(s_work.value, &state, code, sizeof(code)); s_result.qr_state = state;
            if (err == ESP_OK && state == WEREAD_QR_STATE_CONFIRMED) {
                err = weread_auth_exchange_code(code, NULL); if (err == ESP_OK) snapshot();
            }
            break;
        }
        case WR_JOB_OPEN: err = prepare_open(false, false); break;
        case WR_JOB_LOCAL: case WR_JOB_CLOUD:
            err = prepare_open(true, s_work.job == WR_JOB_CLOUD);
            break;
        case WR_JOB_COVER:
            err = cover_dispatch();
            break;
        case WR_JOB_CLEAR: {
            uint16_t total = lookup_shelf_total_chapters(s_work.id);
            err = weread_chapter_clean_book_cache(s_work.id, NULL, cancelled, NULL);
            if (err == ESP_OK) {
                book_cache_invalidate(s_work.id);
                update_shelf_counts_best_effort(s_work.id, 0, total);
                cache_stats_invalidate();
                snapshot();
            }
            break;
        }
        case WR_JOB_LOGOUT:
            weread_progress_outbox_reset();err = weread_auth_clear_session(); weread_shelf_clear(); memset(&s_choice,0,sizeof(s_choice)); ++s_cache_epoch; cache_stats_invalidate(); stop_network(); snapshot(); break;
        case WR_JOB_NOTE: case WR_JOB_REPORT: {
            if (!weread_auth_is_authenticated()) { err = ESP_ERR_INVALID_STATE; break; }
            bool force = !strcmp(s_work.value, "force");
            if (s_work.id[0] || weread_progress_outbox_next(s_result.id, sizeof(s_result.id), (uint64_t)time(NULL)) == ESP_OK) {
                err = weread_progress_outbox_report_ex(s_result.id, s_work.now, (uint64_t)time(NULL), force, &s_result.local);
                if (force && err == ESP_ERR_INVALID_STATE && !cancelled(NULL) &&
                    weread_auth_is_authenticated() && s_result.local.chapter_uid && !s_result.local.sync_conflict) {
                    err = weread_progress_outbox_report_ex(s_result.id, s_work.now, (uint64_t)time(NULL), true, &s_result.local);
                }
            }
            break;
        }
        case WR_JOB_CHAPTER:
            s_result.chapter_uid = (uint32_t)s_work.chapter;
            s_result.chapter_idx = s_work.chapter_idx;
            stage_phase(WEREAD_STAGE_CHAPTER, 0);
            err = weread_chapter_download(s_work.id, (uint32_t)s_work.chapter);
            if (err == ESP_OK && !cancelled(NULL) && s_work.chapter_idx != SIZE_MAX) {
                book_cache_mark(s_work.id, s_work.chapter_idx, true);
                sync_open_book_shelf_counts(s_work.id, false);
            }
            break;
        case WR_JOB_PREFETCH:
            err = prefetch_job();
            break;
        case WR_JOB_CLOUD_SYNC: err = cloud_sync_job(); break;
        case WR_JOB_CONNECT: err = connect_network(); break;
        case WR_JOB_DISCONNECT: stop_network();break;
        case WR_JOB_STOP: weread_progress_outbox_reset();book_weread_clear_prepared();weread_shelf_sync_discard();stop_network(); break;
        case WR_JOB_TOC:
            // 目录准备可能重新联网同步目录；解析与校验仍由目录服务内部完成。
            // TOC preparation may re-sync the TOC; parsing/validation stay inside the TOC service.
            trim_cover_cache_for_open();
            stage_phase(WEREAD_STAGE_TOC, 0);
            err=book_weread_prepare_open(s_work.id);
            break;
        case WR_JOB_PROBE_SHELF: err = probe_shelf_job(); break;
        case WR_JOB_CACHE_BOOK: err = cache_book_job(); break;
        case WR_JOB_CACHE_STATS: err = cache_stats_job(); break;
        case WR_JOB_CACHE_CLEAN_FINISHED: err = cache_clean_finished_job(); break;
        case WR_JOB_CACHE_CLEAN_ALL: err = cache_clean_all_job(); break;
    }
    return err;
}
static void perform_job(void) {
    s_result.job = s_work.job; strcpy(s_result.id, s_work.id);
    esp_err_t err = ESP_OK;
#if defined(ESP_PLATFORM)
    if (s_work.job != WR_JOB_STOP && s_work.job != WR_JOB_CONNECT && s_work.job!=WR_JOB_DISCONNECT && !read_pico_sd_io_ready()) {
        s_result.error=ESP_ERR_INVALID_STATE;atomic_store(&s_shelf_cache_active,false);return;
    }
#endif
    weread_client_set_polling(true);
    weread_client_set_background(s_work.job==WR_JOB_REPORT||s_work.job==WR_JOB_PREFETCH||s_work.job==WR_JOB_COVER);
    bool retryable = retryable_job(s_work.job);
    // 超时只来自网络阶段：目录/版本缓存重写幂等，位置检查点之后的写入禁止重放（s_retry_blocked）。
    // Timeouts come from network phases: TOC/version cache writes are idempotent, while persistence after the progress checkpoint forbids replay (s_retry_blocked).
    for (unsigned attempt = 0;; ++attempt) {
        s_retry_phase = attempt != 0;
        s_retry_blocked = false;
        s_connect_timed_out = false;
        memset(&s_result.local, 0, sizeof(s_result.local));
        memset(&s_result.cloud, 0, sizeof(s_result.cloud));
        if (s_retry_phase && s_work.job != WR_JOB_CLOUD_SYNC) stage_store(WEREAD_STAGE_RETRY, 0);
        err = job_run();
        if (!retryable || err != ESP_ERR_TIMEOUT || s_retry_blocked || s_connect_timed_out ||
            cancelled(NULL) || attempt) break;
        ESP_LOGW("weread", "transient timeout retry job=%u", (unsigned)s_work.job);
    }
    atomic_store(&s_shelf_cache_active, false);
    s_retry_phase = false;
    stage_store(WEREAD_STAGE_NONE, 0);
    if (cancelled(NULL) && err == ESP_OK && s_work.job != WR_JOB_STOP) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK || s_result.error == ESP_OK) s_result.error = err;
    s_result.authenticated = weread_auth_is_authenticated();
    s_result.auth_expired = weread_auth_is_expired();
#if defined(ESP_PLATFORM)
    if((s_work.job==WR_JOB_NOTE||s_work.job==WR_JOB_REPORT)&&s_result.error!=ESP_OK)
        ESP_LOGW("weread","sync upload fail job=%u id=%s err=%d auth=%d exp=%d",
            (unsigned)s_work.job,s_result.id,(int)s_result.error,(int)s_result.authenticated,(int)s_result.auth_expired);
    if(s_work.job!=WR_JOB_COVER&&s_work.job!=WR_JOB_NOTE&&s_work.job!=WR_JOB_REPORT&&s_work.job!=WR_JOB_DISCONNECT)
        ESP_LOGI("weread","memory job=%u result=%d internal=%u largest=%u psram=%u stack=%u",
            (unsigned)s_work.job,(int)s_result.error,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),(unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
    weread_client_set_background(false);weread_client_set_polling(false);
}
#if defined(ESP_PLATFORM)
static void worker(void *arg) {
    (void)arg; unsigned char message;
    for (;;) {
        xQueueReceive(s_queue, &message, portMAX_DELAY); perform_job();
        bool stop = s_work.job == WR_JOB_STOP;
        if (stop) { s_task = NULL; atomic_store(&s_alive, false); }
        atomic_store(&s_done, true); atomic_store(&s_busy, false);
        if (stop) { vTaskDeleteWithCaps(NULL); return; }
    }
}
#else
static void *worker(void *arg) {
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&s_queue_mutex);
        while (!s_dispatch) pthread_cond_wait(&s_queue_cond, &s_queue_mutex);
        s_dispatch = false; pthread_mutex_unlock(&s_queue_mutex);
        perform_job(); bool stop = s_work.job == WR_JOB_STOP;
        if (stop) atomic_store(&s_alive, false);
        atomic_store(&s_done, true); atomic_store(&s_busy, false);
        if (stop) return NULL;
    }
}
#endif
void weread_runtime_init(void) {
    if (s_configured) return;
    wr_memory_init();
    weread_client_init(); weread_client_set_cancel(cancelled, NULL);
    weread_client_set_timeout_budget(timeout_budget,NULL);
    weread_client_set_download_progress(stage_download_progress, NULL);
    weread_client_set_activity_cb(on_http_activity, NULL);
    weread_chapter_set_cancel(cancelled, NULL); weread_client_set_network(weread_runtime_connect, NULL);
    s_configured = true;
}
bool weread_runtime_busy(void) { return atomic_load(&s_busy); }
static void consume_reader_result(void) {
    if (!s_reader.id[0] || strcmp(s_reader.id, s_result.id)) return;
    if (s_result.job == WR_JOB_NOTE || s_result.job == WR_JOB_REPORT) {
        bool matching_rev = s_reader.durable_valid && s_result.local.revision == s_reader.durable.revision;
        bool newer_ack = s_reader.durable_valid && s_result.error == ESP_OK &&
            s_result.local.chapter_uid &&
            (int32_t)(s_result.local.revision - s_reader.durable.revision) > 0;
        if (matching_rev || newer_ack) {
            s_reader.durable = s_result.local;
            s_reader.state = weread_progress_state(&s_result.local);
            s_reader.retry = s_result.local.dirty && s_result.local.position_pending && !s_result.local.sync_conflict;
            if (s_result.error == ESP_OK) {
                s_reader.rescued = false; s_reader.report_at = 0; s_reader.report_due = s_reader.last_tick + 60000;
            }
        }
        if (s_result.error == ESP_ERR_INVALID_STATE && s_reader.active && s_reader.retry &&
            weread_auth_is_authenticated() && s_result.local.chapter_uid &&
            !s_result.local.sync_conflict && s_result.local.retry_after == 0) {
            s_reader.pause_report_at = s_reader.last_tick;
            s_reader.report_at = s_reader.last_tick;
            s_reader.urgent = true;
        }
    }
    if (s_result.job == WR_JOB_CLOUD_SYNC) {
        if (s_result.error == ESP_OK) {
            s_reader.cloud_sync_failures = 0;
            s_reader.cloud_sync_connect_timeouts = 0;
            s_reader.cloud_sync_retry_at = 0;
            s_reader.cloud_sync_ui = WEREAD_CLOUD_SYNC_IDLE;
            s_reader.cloud_sync_ui_until = 0;
            if (!atomic_load(&s_cloud_jump_ready) &&
                s_reader.durable_valid && s_result.local.chapter_uid &&
                s_result.local.revision == s_reader.durable.revision) {
                s_reader.durable = s_result.local;
                s_reader.state = weread_progress_state(&s_result.local);
                s_reader.retry = s_result.local.dirty && s_result.local.position_pending && !s_result.local.sync_conflict;
            }
        } else if (s_reader.active && weread_auth_is_authenticated() && !weread_auth_is_expired() &&
                   s_connect_timed_out && s_reader.cloud_sync_connect_timeouts < 1) {
            ++s_reader.cloud_sync_connect_timeouts;
            s_reader.cloud_sync_pending = true;
            s_reader.cloud_sync_retry_at = s_reader.last_tick + 3000u;
        } else if (s_reader.active && weread_auth_is_authenticated() && !weread_auth_is_expired() &&
                   !s_connect_timed_out &&
                   s_result.error != WEREAD_ERR_PURCHASE && s_result.error != WEREAD_ERR_SOLD_OUT &&
                   s_result.error != WEREAD_ERR_POSITION && s_result.error != WEREAD_ERR_VERSION &&
                   s_result.error != ESP_ERR_NOT_FOUND && s_reader.cloud_sync_failures < 3) {
            ++s_reader.cloud_sync_failures;
            s_reader.cloud_sync_pending = true;
            s_reader.cloud_sync_retry_at = s_reader.last_tick + (uint64_t)s_reader.cloud_sync_failures * 5000u;
        } else if (s_reader.active) {
            if (weread_auth_is_expired()) {
                s_reader.cloud_sync_ui = WEREAD_CLOUD_SYNC_EXPIRED;
                s_reader.cloud_sync_ui_until = UINT64_MAX;
            } else if (s_result.error == WEREAD_ERR_VERSION) {
                s_reader.cloud_sync_ui = WEREAD_CLOUD_SYNC_VERSION;
                s_reader.cloud_sync_ui_until = s_reader.last_tick + 3000u;
            } else if (s_result.error == ESP_ERR_TIMEOUT && !s_connect_timed_out) {
                s_reader.cloud_sync_ui = WEREAD_CLOUD_SYNC_TIMEOUT;
                s_reader.cloud_sync_ui_until = s_reader.last_tick + 3000u;
            } else if (s_connect_timed_out || s_result.error == ESP_ERR_INVALID_STATE || s_result.error == ESP_ERR_NOT_FOUND) {
                s_reader.cloud_sync_ui = WEREAD_CLOUD_SYNC_OFFLINE;
                s_reader.cloud_sync_ui_until = s_reader.last_tick + 3000u;
            } else {
                s_reader.cloud_sync_ui = WEREAD_CLOUD_SYNC_FAILED;
                s_reader.cloud_sync_ui_until = s_reader.last_tick + 3000u;
            }
        }
    }
    if (s_result.job == WR_JOB_CHAPTER && s_result.error == ESP_OK && s_result.chapter_idx != SIZE_MAX) {
        book_cache_mark(s_reader.id, s_result.chapter_idx, true);
    }
    if (s_result.job == WR_JOB_PREFETCH) {
        bool scan_only = !s_result.prefetch_done && s_result.chapter_idx == SIZE_MAX;
        bool matching = !s_result.prefetch_done && !scan_only && s_result.chapter_idx > s_reader.chapter;
        if (s_result.error == ESP_OK) {
            if (s_result.prefetch_done) {
                s_reader.prefetch_done = true;
            } else if (!scan_only) {
                if (s_result.chapter_idx >= s_reader.prefetch_cursor)
                    s_reader.prefetch_cursor = s_result.chapter_idx + 1;
                if (matching) {
                    size_t delta = s_result.chapter_idx - s_reader.chapter - 1;
                    if (delta < 32) s_reader.prefetch_mask |= 1u << delta;
                }
                if (s_reader.total_chapters && s_reader.prefetch_cursor >= s_reader.total_chapters)
                    s_reader.prefetch_done = true;
            }
            s_reader.prefetch_failures = 0;
            s_reader.prefetch_retry = 0;
            s_reader.storage_blocked = false;
        } else if (s_result.error == WEREAD_ERR_STORAGE) {
            s_reader.storage_blocked = true;
            s_reader.prefetch_retry = s_reader.last_tick + 15000;
            ESP_LOGW("weread", "prefetch paused: storage low cached=%u/%u",
                (unsigned)atomic_load(&s_cached_count), (unsigned)atomic_load(&s_total_count));
        } else if (s_result.error != ESP_ERR_INVALID_STATE) {
            ++s_reader.prefetch_failures;
            s_reader.prefetch_retry = s_reader.last_tick + 15000;
            ESP_LOGW("weread", "prefetch error=%d uid=%u idx=%u failures=%u",
                (int)s_result.error, (unsigned)s_result.chapter_uid,
                (unsigned)s_result.chapter_idx, (unsigned)s_reader.prefetch_failures);
        }
    }
}
static void join_job(void) { while (weread_runtime_busy()) pause_ms(1); }
void weread_runtime_cancel_join(void) {
    if (weread_runtime_busy()) {
        uint64_t begin=now_ms();unsigned job=s_work.job;
        atomic_store(&s_cancel, true); weread_client_abort_active(); join_job();
        if(now_ms()-begin>=25)ESP_LOGI("weread","foreground wait job=%u ms=%u",job,(unsigned)(now_ms()-begin));
    }
    atomic_store(&s_cancel, false); atomic_store(&s_deadline, 0);
}
void weread_runtime_cancel(void){
    if(weread_runtime_busy()){atomic_store(&s_cancel,true);weread_client_abort_active();}
}
void weread_reader_pause_bg(void) {
    if (weread_runtime_busy() && s_work.job != WR_JOB_NOTE && s_work.job != WR_JOB_REPORT)
        weread_runtime_cancel();
}
void weread_reader_pause_bg_join(uint32_t timeout_ms) {
    weread_reader_pause_bg();
    if (!weread_runtime_busy()) return;
    uint64_t deadline = now_ms() + timeout_ms;
    while (weread_runtime_busy() && now_ms() < deadline) pause_ms(10);
    if (weread_runtime_busy()) weread_runtime_cancel_join();
}
void weread_reader_input(bool cached_text_only){
    atomic_store(&s_fg_input_ms, now_ms());
    if (weread_runtime_busy()) {
        if (!cached_text_only) {
            if (s_work.job != WR_JOB_NOTE && s_work.job != WR_JOB_REPORT &&
                s_work.job != WR_JOB_CHAPTER && s_work.job != WR_JOB_CLOUD_SYNC)
                weread_runtime_cancel();
        } else {
            if (s_work.job != WR_JOB_NOTE && s_work.job != WR_JOB_REPORT &&
                s_work.job != WR_JOB_CHAPTER && s_work.job != WR_JOB_PREFETCH &&
                s_work.job != WR_JOB_CLOUD_SYNC)
                weread_runtime_cancel();
        }
    }
}
bool weread_runtime_start(weread_job_t job, const char *id, const char *value) {
    weread_runtime_init();
    if (weread_runtime_busy() || (id && *id && !wr_valid_id(id)) || (value && strlen(value) >= sizeof(s_work.value))) return false;
    if ((job == WR_JOB_CLEAR || job == WR_JOB_CACHE_BOOK) && (!id || !*id)) return false;
    bool replaces_reader = job == WR_JOB_OPEN || job == WR_JOB_LOCAL || job == WR_JOB_CLOUD || job == WR_JOB_LOGOUT ||
        ((job == WR_JOB_CLEAR || job == WR_JOB_CACHE_CLEAN_ALL || job == WR_JOB_CACHE_CLEAN_FINISHED) &&
         s_reader.id[0] && (job != WR_JOB_CLEAR || (id && !strcmp(id, s_reader.id))));
    if(replaces_reader&&weread_reader_pending_save()&&weread_reader_save(now_ms())!=ESP_OK)return false;
    if (atomic_exchange(&s_done, false)) consume_reader_result();
    weread_runtime_result_free(&s_result);
    memset(&s_work, 0, sizeof(s_work)); s_work.job = job; s_work.chapter_idx = SIZE_MAX;
    if (id) wr_copy(s_work.id, sizeof(s_work.id), id);
    if (value) wr_copy(s_work.value, sizeof(s_work.value), value);
    if (job == WR_JOB_LOCAL || job == WR_JOB_CLOUD) {
        if (!id || strcmp(id, s_choice.id) || (!s_choice.progress.chapter_uid &&
            (job != WR_JOB_CLOUD || s_choice.progress.chapter_index || s_choice.progress.chapter_offset || s_choice.progress.progress))) return false;
    }
    if(job==WR_JOB_NOTE||job==WR_JOB_REPORT)s_work.now=now_ms();
    if (job == WR_JOB_CHAPTER) {
        char *end=NULL; unsigned long uid=value?strtoul(value,&end,10):0;
        if (!uid || uid>UINT32_MAX || !end || *end) return false;
        s_work.chapter=(uint32_t)uid;
        if (s_reader.id[0] && !strcmp(s_reader.id, s_work.id))
            s_work.chapter_idx = book_weread_index((uint32_t)uid);
    }
    if (job == WR_JOB_PREFETCH) {
        if (value && (!strcmp(value, "forward") || !strcmp(value, "scan"))) {
            s_work.prefetch_forward = true;
            s_work.chapter = 0;
            if (!strcmp(value, "scan")) {
                if (!s_reader.total_chapters) return false;
                s_work.chapter_idx = s_reader.total_chapters;
            } else {
                s_work.chapter_idx = s_reader.prefetch_cursor ? s_reader.prefetch_cursor : (s_reader.chapter + 1);
                if (s_reader.total_chapters && s_work.chapter_idx >= s_reader.total_chapters) return false;
                if (!s_reader.total_chapters && !book_weread_uid(s_work.chapter_idx)) return false;
            }
        } else {
            unsigned offset = value && *value ? (unsigned)strtoul(value, NULL, 10) : 1;
            s_work.chapter_idx = s_reader.chapter + offset;
            s_work.chapter = book_weread_uid(s_work.chapter_idx);
            if (!s_work.chapter) return false;
        }
    }
    if (job == WR_JOB_PROBE_SHELF) {
        const char *src = (value && *value) ? value : id;
        if (!src || !*src) return false;
        const char *p = src;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            if (!len || len >= sizeof(s_work.probe_ids[0]) || s_work.probe_count >= 9) return false;
            memcpy(s_work.probe_ids[s_work.probe_count], p, len);
            s_work.probe_ids[s_work.probe_count][len] = 0;
            if (!wr_valid_id(s_work.probe_ids[s_work.probe_count])) return false;
            ++s_work.probe_count;
            if (!comma) break;
            p = comma + 1;
            if (!*p) return false;
        }
        if (!s_work.probe_count) return false;
    }
    if (job == WR_JOB_CACHE_BOOK) {
        stats_lock();
        wr_copy(s_shelf_cache_id, sizeof(s_shelf_cache_id), id);
        stats_unlock();
        atomic_store(&s_shelf_cache_done, 0);
        atomic_store(&s_shelf_cache_total, 0);
        atomic_store(&s_shelf_cache_storage_blocked, false);
        atomic_store(&s_shelf_cache_active, true);
    }
    atomic_store(&s_cancel, false);
    atomic_store(&s_deadline, 0);
    atomic_store(&s_busy, true);
    if (!atomic_load(&s_alive)) {
        atomic_store(&s_alive, true);
#if defined(ESP_PLATFORM)
        if (!s_queue) s_queue = xQueueCreateStatic(1, 1, &s_queue_byte, &s_queue_storage);
        // WiFi/NVS会关闭Flash缓存，调用栈必须位于内部RAM。/ WiFi/NVS disables flash caches, so the caller's stack must stay in internal RAM.
        if (!s_queue || xTaskCreateWithCaps(worker, "weread", 16384, NULL, 1, &s_task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGW("weread", "worker allocation failed internal=%u largest=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
            atomic_store(&s_shelf_cache_active, false);
            atomic_store(&s_alive, false); atomic_store(&s_busy, false); return false;
        }
#else
        if (s_joinable) { pthread_join(s_thread, NULL); s_joinable = false; }
        if (pthread_create(&s_thread, NULL, worker, NULL)) {
            atomic_store(&s_shelf_cache_active, false);
            atomic_store(&s_alive, false); atomic_store(&s_busy, false); return false;
        }
        s_joinable = true;
#endif
    }
#if defined(ESP_PLATFORM)
    unsigned char message = 1; xQueueSend(s_queue, &message, portMAX_DELAY);
#else
    pthread_mutex_lock(&s_queue_mutex); s_dispatch = true; pthread_cond_signal(&s_queue_cond); pthread_mutex_unlock(&s_queue_mutex);
#endif
    return true;
}
bool weread_runtime_probe_shelf(const char *const *book_ids, size_t count) {
    if (!book_ids || !count || count > 9 || weread_runtime_busy()) return false;
    char *buf __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(s_work.value));
    if (!buf) return false;
    size_t used = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!book_ids[i] || !wr_valid_id(book_ids[i])) return false;
        size_t len = strlen(book_ids[i]);
        size_t need = len + (i ? 1u : 0u);
        if (used + need >= sizeof(s_work.value)) return false;
        if (i) buf[used++] = ',';
        memcpy(buf + used, book_ids[i], len);
        used += len;
    }
    buf[used] = 0;
    return weread_runtime_start(WR_JOB_PROBE_SHELF, NULL, buf);
}
bool weread_runtime_shelf_cache_status(weread_shelf_cache_status_t *out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    stats_lock();
    bool active = atomic_load(&s_shelf_cache_active);
    out->active = active;
    out->storage_blocked = atomic_load(&s_shelf_cache_storage_blocked);
    out->cached_chapters = (uint16_t)atomic_load(&s_shelf_cache_done);
    out->total_chapters = (uint16_t)atomic_load(&s_shelf_cache_total);
    if (s_shelf_cache_id[0]) wr_copy(out->book_id, sizeof(out->book_id), s_shelf_cache_id);
    stats_unlock();
    return active;
}
bool weread_runtime_cache_stats(weread_runtime_cache_stats_t *out) {
    if (!out) return false;
    stats_lock();
    *out = s_cache_stats;
    stats_unlock();
    return out->valid;
}
bool weread_runtime_choose(const char *id, const weread_progress_t *progress, bool cloud) {
    if (!wr_valid_id(id) || !progress || weread_runtime_busy()) return false;
    strcpy(s_choice.id, id); s_choice.progress = *progress;
    return weread_runtime_start(cloud ? WR_JOB_CLOUD : WR_JOB_LOCAL, id, NULL);
}
bool weread_runtime_poll(weread_result_t *out) {
    if (!out || weread_runtime_busy() || !atomic_exchange(&s_done, false)) return false;
    consume_reader_result();
    *out = s_result; memset(&s_result, 0, sizeof(s_result)); return true;
}
esp_err_t weread_runtime_connect(void *ctx) {
    (void)ctx;
    if (in_worker()) {
        if (!weread_auth_is_authenticated() && s_work.job != WR_JOB_QR &&
            s_work.job != WR_JOB_POLL && s_work.job != WR_JOB_CONNECT) {
            return ESP_ERR_INVALID_STATE;
        }
        return connect_network();
    }
    if (!weread_runtime_start(WR_JOB_CONNECT, NULL, NULL)) return ESP_ERR_INVALID_STATE;
    join_job(); esp_err_t err = s_result.error; return err;
}
void weread_runtime_stop(void) {
    weread_runtime_cancel_join();
    if (atomic_load(&s_alive) && weread_runtime_start(WR_JOB_STOP, NULL, NULL)) join_job();
#if !defined(ESP_PLATFORM)
    if (s_joinable) { pthread_join(s_thread, NULL); s_joinable = false; }
#endif
    (void)weread_shelf_flush_counts();
    s_prefetch_dirty_count = 0;
    book_cache_clear();
    atomic_store(&s_cloud_jump_ready, false); atomic_store(&s_live_valid, false);
    atomic_store(&s_sync_downloading_uid, 0); atomic_store(&s_prefetch_downloading_uid, 0);
    atomic_store(&s_shelf_cache_active, false);
    stats_lock(); s_shelf_cache_id[0] = 0; s_cache_stats.valid = false; stats_unlock();
    { WR_GUARD; memset(&s_cloud_jump, 0, sizeof(s_cloud_jump)); }
    weread_runtime_result_free(&s_result); atomic_store(&s_done, false);
    s_retry_phase = false;
    stage_store(WEREAD_STAGE_NONE, 0);
}
void weread_runtime_media_lost(void) {
    weread_runtime_stop();
    // 尚未清账号/正文映射时抢救；失败保留RAM，不能把拔卡当保存成功。
    // Rescue before forgetting the owner/map; a failure retains RAM rather than claiming success.
    (void)weread_reader_save(now_ms());
    s_reader.active=false;
    bool pending=weread_reader_pending_save();
    weread_progress_outbox_reset();if(!pending)memset(&s_reader,0,sizeof(s_reader));
    memset(&s_choice,0,sizeof(s_choice)); ++s_cache_epoch;
    if(pending)s_reader.owner_epoch=s_cache_epoch;
    weread_shelf_clear();weread_auth_forget_session();
}
unsigned weread_runtime_cache_epoch(void){return s_cache_epoch;}
bool weread_runtime_idle(uint64_t now){
    if(weread_runtime_busy()||now<s_idle_at)return false;
    if(s_reader.id[0]&&s_reader.save_failed&&(weread_reader_save(now)!=ESP_OK))return false;
    uint64_t net_active = atomic_load(&s_network_active);
    if(atomic_load(&s_owns_network)&&net_active&&now>=net_active&&now-net_active>=600000)
        return weread_runtime_start(WR_JOB_DISCONNECT,NULL,NULL);
    if(s_reader.active||!weread_auth_is_authenticated())return false;
    s_idle_at=now+1000;
    return weread_runtime_start(WR_JOB_REPORT,NULL,NULL);
}
bool weread_runtime_network_active(void){
    return weread_runtime_busy()||atomic_load(&s_owns_network);
}
bool weread_reader_cached(const char *uri,size_t chapter){
    if(!book_path_is_weread(uri))return false;
    uint32_t uid=book_weread_uid(chapter);
    if(!uid)return false;
    if(weread_runtime_busy()){
        if(s_work.job==WR_JOB_CLEAR||s_work.job==WR_JOB_STOP||s_work.job==WR_JOB_LOGOUT||
           s_work.job==WR_JOB_CACHE_CLEAN_ALL||s_work.job==WR_JOB_CACHE_CLEAN_FINISHED)return false;
        if((s_work.job==WR_JOB_CHAPTER||s_work.job==WR_JOB_PREFETCH)&&!strcmp(s_work.id,uri+9)&&
           (s_work.chapter==uid||atomic_load(&s_prefetch_downloading_uid)==uid))return false;
        if(s_work.job==WR_JOB_CLOUD_SYNC&&!strcmp(s_work.id,uri+9)&&atomic_load(&s_sync_downloading_uid)==uid)return false;
    }
    {
        WR_GUARD;
        if (book_cache_scanned(uri + 9) && chapter < s_book_cache.total && !book_cache_bit(uri + 9, chapter)) {
            return false;
        }
    }
    return weread_chapter_cached(uri+9,uid);
}
bool weread_reader_chapter_cached(const char *uri,size_t chapter){
    // 目录页只查可见章：整本位图已扫描时零IO查位；否则每章一次有界存在性检查，不触发整书扫描。
    // TOC pages probe visible chapters: a scanned book bitmap answers with zero IO, otherwise one bounded existence check per chapter, never a full scan.
    if(!uri||!book_path_is_weread(uri))return false;
    const char *id=uri+9;
    bool bitmap=false;
    {
        WR_GUARD;
        bitmap=book_cache_ready(id);
        if(bitmap&&book_cache_scanned(id)&&chapter<s_book_cache.total)return book_cache_bit(id,chapter);
        bitmap=bitmap&&chapter<s_book_cache.total;
    }
    uint32_t uid=book_weread_uid(chapter);
    if(!uid||!wr_valid_id(id))return false;
    bool hit=weread_chapter_cached(id,uid);
    if(hit&&bitmap)book_cache_mark(id,chapter,true);
    return hit;
}
bool weread_runtime_prefetch_active(const char *id,uint32_t uid){
    return weread_runtime_busy()&&s_work.job==WR_JOB_PREFETCH&&id&&uid&&
        !strcmp(s_work.id,id)&&(s_work.chapter==uid||atomic_load(&s_prefetch_downloading_uid)==uid);
}
void weread_runtime_prefetch_adopt(uint32_t uid){
    atomic_store(&s_adopting_prefetch_uid,uid);
}
esp_err_t weread_reader_prepare(const char *uri,size_t chapter) {
    if (!book_path_is_weread(uri)) return ESP_ERR_INVALID_ARG;
    if(weread_reader_cached(uri,chapter))return ESP_OK;
    uint32_t uid=book_weread_uid(chapter); if (!uid) return ESP_ERR_INVALID_ARG;
    weread_runtime_cancel_join();char value[16];snprintf(value,sizeof(value),"%u",(unsigned)uid);
    if (!weread_runtime_start(WR_JOB_CHAPTER,uri+9,value)) return ESP_ERR_INVALID_STATE;
    join_job();return s_result.error;
}
esp_err_t weread_reader_prepare_open(const char *uri){
    if(!book_path_is_weread(uri))return ESP_ERR_INVALID_ARG;
    if(book_weread_prepared(uri+9))return ESP_OK;
    weread_runtime_cancel_join();
    if(!weread_runtime_start(WR_JOB_TOC,uri+9,NULL))return ESP_ERR_INVALID_STATE;
    join_job();return s_result.error;
}
bool weread_reader_open_ready(const char *uri){
    return book_path_is_weread(uri)&&s_choice.valid&&!strcmp(s_choice.id,uri+9)&&book_weread_prepared(uri+9);
}
void weread_reader_begin(const char *uri, uint64_t now) {
    if(weread_reader_pending_save()&&weread_reader_save(now)!=ESP_OK)return;
    memset(&s_reader, 0, sizeof(s_reader));
    atomic_store(&s_cloud_jump_ready, false);
    atomic_store(&s_live_valid, false);
    { WR_GUARD; memset(&s_cloud_jump, 0, sizeof(s_cloud_jump)); }
    if (book_path_is_weread(uri)&&s_choice.resume_ready&&!strcmp(s_choice.id,uri+9)) {
        WR_GUARD;
        const weread_auth_session_t *session=weread_auth_get_current_session();
        if(!session||!wr_copy(s_reader.owner_root,sizeof(s_reader.owner_root),weread_auth_get_storage_root())||
            !wr_copy(s_reader.owner_vid,sizeof(s_reader.owner_vid),session->vid)||
            !wr_copy(s_reader.owner_device,sizeof(s_reader.owner_device),session->device_id))return;
        s_reader.owner_epoch=s_cache_epoch;(void)book_weread_cached_version(uri+9,&s_reader.owner_version);s_reader.nav_version=s_reader.owner_version;
        wr_copy(s_reader.id,sizeof(s_reader.id),uri+9);s_reader.last_tick=s_reader.last_store=now;
        s_reader.anchor_uid=s_choice.progress.chapter_uid;s_reader.anchor_index=s_choice.progress.chapter_index;
        s_reader.anchor_offset=s_choice.progress.chapter_offset;s_reader.anchor_percent=s_choice.progress.progress;
        wr_display_copy(s_reader.summary,sizeof(s_reader.summary),s_choice.progress.summary);
        s_reader.retry=s_choice.progress.dirty&&s_choice.progress.position_pending&&!s_choice.progress.sync_conflict;
        s_reader.state=weread_progress_state(&s_choice.progress);s_reader.durable=s_choice.progress;s_reader.durable_valid=true;
        s_reader.media_valid=weread_progress_capture_media(s_reader.media_id);s_reader.active=true;s_reader.report_due=now+60000;
        s_reader.plain_text=book_weread_plain_text();
        s_reader.total_chapters=book_weread_count();
        s_reader.prefetch_cursor=1;
        book_cache_init(uri+9);
        s_reader.anchor=true;s_reader.cloud_sync_pending=s_choice.pending_cloud_sync;
        atomic_store(&s_live_pos,((uint64_t)s_choice.progress.chapter_index<<32)|s_choice.progress.chapter_offset);
        atomic_store(&s_live_valid,true);
        s_choice.resume_ready=false;s_choice.pending_cloud_sync=false;
    }
}
void weread_reader_navigated_version(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary,uint32_t content_version){
    if(book_path_is_weread(uri)&&!strcmp(s_reader.id,uri+9)){
        if(chapter!=s_reader.chapter||content_version!=s_reader.nav_version)s_reader.urgent=true;
        if(s_reader.nav_version!=content_version){
            s_reader.prefetch_mask=s_reader.prefetch_failures=0;s_reader.prefetch_retry=0;s_reader.storage_blocked=false;
            s_reader.prefetch_cursor=chapter+1;s_reader.prefetch_done=false;
            book_cache_invalidate(s_reader.id);
        }
        s_reader.nav_version=content_version;
        ++s_reader.revision;++s_reader.session_pages;s_reader.user_turned_since_open=true;
        s_reader.anchor=false;s_reader.pending_coordinate=false;s_reader.navigation_pending=true;
        s_reader.nav_chapter=chapter;s_reader.nav_offset=offset;s_reader.nav_percent=percent;
        s_reader.nav_uid=book_weread_uid(chapter);s_reader.nav_index=book_weread_chapter_index(chapter);
        uint32_t raw=0;
        if(book_weread_map_cached(chapter,content_version,true,offset,&raw)==ESP_OK){
            s_reader.pending_coordinate=true;s_reader.anchor_uid=s_reader.nav_uid;s_reader.anchor_index=s_reader.nav_index;
            s_reader.anchor_offset=raw;s_reader.anchor_percent=percent;s_reader.owner_version=content_version;
            atomic_store(&s_live_pos,((uint64_t)s_reader.nav_index<<32)|raw);
            atomic_store(&s_live_valid,true);
        }else if(s_reader.nav_index&&(uint32_t)(atomic_load(&s_live_pos)>>32)!=s_reader.nav_index){
            atomic_store(&s_live_pos,(uint64_t)s_reader.nav_index<<32);
            atomic_store(&s_live_valid,true);
        }
        wr_display_copy(s_reader.summary,sizeof(s_reader.summary),summary);
    }
}
bool weread_runtime_reader_has_turned(const char *book_id) {
    if (!book_id || !*book_id) return false;
    const char *id = book_path_is_weread(book_id) ? book_id + 9 : book_id;
    return s_reader.active && s_reader.id[0] && !strcmp(s_reader.id, id) &&
        (s_reader.user_turned_since_open || s_reader.session_pages > 0);
}
esp_err_t weread_runtime_cloud_jump_rebase(const char *book_id) {
    if (!book_id || !*book_id) return ESP_ERR_INVALID_ARG;
    const char *id = book_path_is_weread(book_id) ? book_id + 9 : book_id;
    if (!*id || !s_reader.active || strcmp(s_reader.id, id)) return ESP_ERR_INVALID_STATE;
    esp_err_t save_err = weread_reader_pending_save() ? weread_reader_save(now_ms()) : ESP_OK;
    WR_GUARD;
    bool ready = atomic_exchange(&s_cloud_jump_ready, false);
    bool has_jump = !strcmp(s_cloud_jump.id, id) && (ready || s_cloud_jump.rebased) &&
        s_cloud_jump.progress.chapter_uid != 0;
    book_weread_clear_prepared();
    if (!has_jump) return ESP_ERR_INVALID_STATE;
    if (save_err != ESP_OK) {
        s_cloud_jump.rebased = false;
        return save_err;
    }
    weread_progress_t *local __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*local));
    if (!local) {
        s_cloud_jump.rebased = false;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = weread_progress_load(id, local);
    if (err == ESP_OK && !local->chapter_uid) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        local->base_uid = s_cloud_jump.progress.chapter_uid;
        local->base_index = s_cloud_jump.progress.chapter_index;
        local->base_offset = s_cloud_jump.progress.chapter_offset;
        local->base_version = s_cloud_jump.progress.base_version ? s_cloud_jump.progress.base_version : s_cloud_jump.progress.book_version;
        local->base_known = true;
        local->dirty = true;
        local->position_pending = true;
        local->sync_conflict = false;
        local->retry_after = 0;
        local->retry_count = 0;
        local->revision = local->revision == UINT32_MAX ? 1 : local->revision + 1;
        err = weread_progress_checkpoint(id, local);
        if (err == ESP_OK) {
            s_reader.durable = *local;
            s_reader.durable_valid = true;
            s_reader.revision = local->revision;
            s_reader.state = weread_progress_state(local);
            s_reader.retry = true;
        }
    }
    s_cloud_jump.rebased = (err == ESP_OK);
    return err;
}
void weread_reader_navigated(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary){
    weread_reader_navigated_version(uri,chapter,offset,percent,summary,s_reader.owner_version);
}
esp_err_t weread_reader_resume_chapter(const char *uri, size_t *chapter, uint32_t *source_offset) {
    s_choice.resume_ready = false;
    if (!book_path_is_weread(uri) || !chapter || !source_offset) return ESP_ERR_INVALID_ARG;
    bool cold_resume = !s_choice.valid || strcmp(s_choice.id, uri + 9) != 0;
    if (cold_resume) {
        s_choice.valid = false;
        s_choice.pending_cloud_sync = false;
        esp_err_t err = weread_progress_load(uri + 9, &s_choice.progress);
        if (err != ESP_OK) return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : err;
    }
    s_choice.valid = false;
    uint32_t version = 0;
    if (!book_weread_cached_version(uri + 9, &version) ||
        version != s_choice.progress.book_version) return ESP_ERR_INVALID_RESPONSE;
    size_t i = book_weread_index(s_choice.progress.chapter_uid);
    if (i == SIZE_MAX) return ESP_ERR_INVALID_RESPONSE;
    *chapter = i;
    *source_offset = s_choice.progress.chapter_offset;
    strcpy(s_choice.id, uri + 9);
    if (cold_resume) s_choice.pending_cloud_sync = weread_auth_is_authenticated();
    s_choice.resume_ready = true;
    ESP_LOGI("weread", "resume target uid=%u chapter=%u source_off=%u",
        (unsigned)s_choice.progress.chapter_uid, (unsigned)i, (unsigned)*source_offset);
    return ESP_OK;
}
esp_err_t weread_reader_resume(const char *uri, size_t *chapter, uint32_t *offset) {
    s_choice.resume_ready=false;
    if (!book_path_is_weread(uri) || !chapter || !offset) return ESP_ERR_INVALID_ARG;
    if (!s_choice.valid || strcmp(s_choice.id, uri + 9)) {
        // 深睡后NVS身份可能随版本变化，改从账号归属的TF记录恢复，不猜首章。
        // Version changes can invalidate NVS after deep sleep; recover the account-bound TF record instead of guessing chapter zero.
        s_choice.valid = false;
        esp_err_t err = weread_progress_load(uri + 9, &s_choice.progress);
        if (err != ESP_OK) return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : err;
    }
    s_choice.valid = false;
    uint32_t version = 0;
    if (!book_weread_cached_version(uri + 9, &version) ||
        version != s_choice.progress.book_version) return ESP_ERR_INVALID_RESPONSE;
    size_t i = book_weread_index(s_choice.progress.chapter_uid);
    if (i == SIZE_MAX) return ESP_ERR_INVALID_RESPONSE;
    esp_err_t err = book_weread_map(i, false, s_choice.progress.chapter_offset, offset);
    if (err == ESP_OK) {*chapter = i;strcpy(s_choice.id,uri+9);s_choice.resume_ready=true;}
    ESP_LOGI("weread","resume uid=%u chapter=%u offset=%u result=%d",(unsigned)s_choice.progress.chapter_uid,(unsigned)i,(unsigned)*offset,(int)err);
    return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_RESPONSE : err;
}
esp_err_t weread_reader_save(uint64_t now){
    if(!s_reader.id[0])return ESP_OK;
    if(s_reader.navigation_pending){++s_reader.pages;s_reader.changed=true;s_reader.navigation_pending=false;s_reader.seen=true;}
    if(!s_reader.seen)return ESP_OK;
    if(weread_runtime_busy()&&(s_work.job==WR_JOB_CLEAR||s_work.job==WR_JOB_STOP||s_work.job==WR_JOB_LOGOUT||
        s_work.job==WR_JOB_CACHE_CLEAN_ALL||s_work.job==WR_JOB_CACHE_CLEAN_FINISHED))return ESP_ERR_NOT_FINISHED;
    if(!s_reader.changed&&s_reader.active_ms<1000)return ESP_OK;
    WR_GUARD;
    const weread_auth_session_t *session=weread_auth_get_current_session();
    if(!session||s_reader.owner_epoch!=s_cache_epoch||strcmp(s_reader.owner_root,weread_auth_get_storage_root())||
        strcmp(s_reader.owner_vid,session->vid)||strcmp(s_reader.owner_device,session->device_id)){
        s_reader.save_failed=true;s_reader.state=WR_PROGRESS_CONFLICT;return ESP_ERR_INVALID_STATE;
    }
    uint8_t current_media[WEREAD_MEDIA_ID_BYTES];
    bool present=s_reader.media_valid&&weread_progress_capture_media(current_media);
    if(present&&memcmp(current_media,s_reader.media_id,sizeof(current_media))){s_reader.save_failed=true;return ESP_ERR_INVALID_STATE;}
    weread_progress_t *sample __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*sample));
    if(!sample){s_reader.save_failed=true;return ESP_ERR_NO_MEM;}
    bool noted=false,rescued=false;
    bool mapped=s_reader.anchor||s_reader.pending_coordinate;
    sample->chapter_uid=mapped?s_reader.anchor_uid:s_reader.nav_uid;
    sample->chapter_index=mapped?s_reader.anchor_index:s_reader.nav_index;
    sample->chapter_offset=mapped?s_reader.anchor_offset:s_reader.nav_offset;
    sample->progress=s_reader.anchor?s_reader.anchor_percent:s_reader.nav_percent;
    sample->reading_time=(uint32_t)(s_reader.active_ms/1000);sample->pending_pages=s_reader.pages;
    wr_display_copy(sample->summary,sizeof(sample->summary),s_reader.summary);
    esp_err_t err=mapped?ESP_OK:s_reader.media_valid&&!present?
        book_weread_map_cached(s_reader.nav_chapter,s_reader.nav_version,true,s_reader.nav_offset,&sample->chapter_offset):
        book_weread_map_version(s_reader.nav_chapter,s_reader.nav_version,true,s_reader.nav_offset,&sample->chapter_offset);
    if(!mapped&&err==ESP_ERR_INVALID_STATE&&present&&!book_weread_is_open())
        err=book_weread_map_detached(s_reader.id,s_reader.nav_uid,s_reader.nav_version,s_reader.plain_text,
            s_reader.nav_offset,&sample->chapter_offset);
    if(err==ESP_OK&&!s_reader.anchor){
        s_reader.owner_version=s_reader.nav_version;
        s_reader.pending_coordinate=true;s_reader.anchor_uid=sample->chapter_uid;s_reader.anchor_index=sample->chapter_index;
        s_reader.anchor_offset=sample->chapter_offset;s_reader.anchor_percent=sample->progress;
        atomic_store(&s_live_pos,((uint64_t)sample->chapter_index<<32)|sample->chapter_offset);
        atomic_store(&s_live_valid,true);
    }
    if(err==ESP_OK)err=weread_progress_store_version_resilient(s_reader.id,sample,!s_reader.anchor,s_reader.nav_version,
        (uint64_t)time(NULL),&noted,s_reader.durable_valid?&s_reader.durable:NULL,s_reader.media_valid?s_reader.media_id:NULL,&rescued);
    if(noted){
        s_reader.pages=0;s_reader.active_ms%=1000;s_reader.changed=false;s_reader.last_store=now;
        s_reader.anchor_uid=sample->chapter_uid;s_reader.anchor_index=sample->chapter_index;
        s_reader.anchor_offset=sample->chapter_offset;s_reader.anchor_percent=sample->progress;s_reader.anchor=true;
        s_reader.state=weread_progress_state(sample);s_reader.retry=sample->dirty&&sample->position_pending&&!sample->sync_conflict;
        s_reader.durable=*sample;s_reader.durable_valid=true;s_reader.rescued=rescued;s_reader.pending_coordinate=false;
        if(!mapped||s_reader.nav_version!=s_reader.owner_version)s_reader.owner_version=s_reader.nav_version;
    }
    s_reader.save_failed=err!=ESP_OK;
    return err;
}
weread_progress_state_t weread_reader_state(void){return s_reader.state;}
weread_cloud_sync_ui_state_t weread_reader_cloud_sync_state(void) {
    if (!s_reader.active || !s_reader.id[0]) return WEREAD_CLOUD_SYNC_IDLE;
    if (weread_auth_is_expired()) return WEREAD_CLOUD_SYNC_EXPIRED;
    if (s_reader.cloud_sync_pending) return WEREAD_CLOUD_SYNC_ACTIVE;
    if (weread_runtime_busy() && s_work.job == WR_JOB_CLOUD_SYNC && !strcmp(s_work.id, s_reader.id))
        return WEREAD_CLOUD_SYNC_ACTIVE;
    if (s_reader.cloud_sync_ui != WEREAD_CLOUD_SYNC_IDLE) {
        if (s_reader.cloud_sync_ui == WEREAD_CLOUD_SYNC_EXPIRED ||
            s_reader.last_tick < s_reader.cloud_sync_ui_until)
            return s_reader.cloud_sync_ui;
    }
    return WEREAD_CLOUD_SYNC_IDLE;
}
bool weread_reader_save_failed(void){return s_reader.save_failed;}
bool weread_reader_pending_save(void){return s_reader.changed||s_reader.navigation_pending||s_reader.save_failed;}
bool weread_reader_rescued(void){return s_reader.rescued;}
unsigned weread_reader_offline(void){
    if(s_reader.total_chapters>0){
        WR_GUARD;
        if(!strcmp(s_book_cache.id,s_reader.id)&&s_book_cache.bits){
            unsigned n=0;
            for(size_t i=s_reader.chapter+1;i<s_book_cache.total;++i){
                if(!(s_book_cache.bits[i>>3u]&(uint8_t)(1u<<(i&7u))))break;
                ++n;
            }
            return n;
        }
    }
    unsigned n=0;while(n<32&&(s_reader.prefetch_mask&(1u<<n)))++n;return n;
}
void weread_reader_cached_progress(unsigned *cached, unsigned *total){
    if(cached)*cached=atomic_load(&s_cached_count);
    if(total)*total=atomic_load(&s_total_count);
}
bool weread_reader_storage_blocked(void){return s_reader.storage_blocked;}
// 会话级手动缓存开关：仅由阅读页读写，开书重置；后台worker不读取。
// Session-only manual cache flag: read/written by the reader page only, reset on book begin; the worker never reads it.
void weread_reader_cache_forward_set(bool on){
    s_reader.cache_forward=on;
    // 显式关闭只抑制整本向后补齐（含自动空闲补齐），不取消在飞章节，也不影响自动下一章预读。
    // Explicit off only suppresses the whole-book forward fill (including automatic idle fill); in-flight chapters, the urgent next-chapter pre-read and reports stay.
    s_reader.cache_forward_off=!on;
    if(on){
        // 重新开启时清掉自动预取的失败/退避/空间暂停与“已到末尾”闩锁，否则按钮亮起却不再派发下载。
        // Re-enabling clears the automatic prefetch failure/backoff/space pause and the end-of-book latch;
        // otherwise the toggle can light up while no download is ever dispatched again.
        s_reader.prefetch_failures=0;
        s_reader.prefetch_retry=0;
        s_reader.storage_blocked=false;
        if(s_reader.prefetch_done){
            s_reader.prefetch_done=false;
            s_reader.prefetch_cursor=s_reader.chapter+1;
        }
    }
}
bool weread_reader_cache_forward(void){return s_reader.cache_forward;}
bool weread_reader_cache_fill_enabled(void){return !s_reader.cache_forward_off;}
bool weread_reader_cache_forward_stalled(void){
    // 自动预取的连续失败/空间暂停会让开关仍亮却不再派发；工具栏据此把一次点击变成“立即重试”。
    // Automatic-prefetch failure/space pauses leave the switch lit but idle; the toolbar uses this to make one tap retry.
    return s_reader.prefetch_failures>=3||s_reader.storage_blocked;
}
bool weread_reader_caching(void){return weread_runtime_busy()&&s_work.job==WR_JOB_PREFETCH&&!strcmp(s_work.id,s_reader.id);}
bool weread_reader_poll_cloud_jump(const char *uri,size_t *chapter_out,uint32_t *offset_out){
    if(!book_path_is_weread(uri)||!chapter_out||!offset_out||!atomic_load(&s_cloud_jump_ready)||
       strcmp(s_reader.id,uri+9))return false;
    weread_progress_t *jump __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*jump));
    if(!jump)return false;
    bool same_id = false;
    {
        WR_GUARD;
        same_id = !strcmp(s_cloud_jump.id, uri + 9);
        if (same_id) *jump = s_cloud_jump.progress;
    }
    if (!same_id) return false;
    if(!cloud_ahead_of_live(jump)){
        atomic_store(&s_cloud_jump_ready,false);
        { WR_GUARD; s_cloud_jump.rebased = false; }
        book_weread_clear_prepared();
        if (weread_progress_rebase(s_reader.id,jump,jump->base_version,&s_reader.durable)==ESP_OK) {
            s_reader.state = weread_progress_state(&s_reader.durable);
            s_reader.retry = s_reader.durable.dirty && s_reader.durable.position_pending && !s_reader.durable.sync_conflict;
        }
        return false;
    }
    if(book_weread_reload_toc(s_reader.id)!=ESP_OK){
        atomic_store(&s_cloud_jump_ready,false);book_weread_clear_prepared();return false;
    }
    size_t count = book_weread_count();
    if (count != s_reader.total_chapters || jump->book_version != s_reader.owner_version) {
        s_reader.total_chapters = count;
        s_reader.prefetch_done = false;
        book_cache_init(s_reader.id);
    }
    size_t idx=book_weread_index(jump->chapter_uid);
    uint32_t mapped=0;
    esp_err_t err=(idx!=SIZE_MAX)?book_weread_map(idx,false,jump->chapter_offset,&mapped):ESP_ERR_INVALID_RESPONSE;
    if(err!=ESP_OK){
        atomic_store(&s_cloud_jump_ready,false);book_weread_clear_prepared();return false;
    }
    *chapter_out=idx;*offset_out=mapped;
    return true;
}
void weread_reader_commit_cloud_jump(const char *uri,bool committed){
    bool ready = atomic_exchange(&s_cloud_jump_ready,false);
    weread_progress_t *jump __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*jump));
    if (!jump) { book_weread_clear_prepared(); return; }
    bool was_rebased = false, same_id = false;
    {
        WR_GUARD;
        was_rebased = s_cloud_jump.rebased;
        s_cloud_jump.rebased = false;
        same_id = book_path_is_weread(uri) && !strcmp(s_reader.id, uri + 9) && !strcmp(s_cloud_jump.id, uri + 9);
        if (same_id) *jump = s_cloud_jump.progress;
    }
    if (!ready && !was_rebased) return;
    if(!committed||!same_id){
        book_weread_clear_prepared();
        return;
    }
    if(weread_progress_select(s_reader.id,jump,true)!=ESP_OK){
        return;
    }
    (void)weread_progress_load(s_reader.id,&s_reader.durable);
    s_reader.durable_valid=true;s_reader.retry=false;s_reader.state=weread_progress_state(&s_reader.durable);
    s_reader.anchor_uid=jump->chapter_uid;s_reader.anchor_index=jump->chapter_index;
    s_reader.anchor_offset=jump->chapter_offset;s_reader.anchor_percent=jump->progress;
    wr_display_copy(s_reader.summary,sizeof(s_reader.summary),jump->summary);
    s_reader.anchor=true;s_reader.changed=false;s_reader.navigation_pending=false;s_reader.save_failed=false;s_reader.rescued=false;
    s_reader.owner_version=jump->book_version;s_reader.nav_version=jump->book_version;
    s_reader.last_store=now_ms();
    atomic_store(&s_live_pos,((uint64_t)jump->chapter_index<<32)|jump->chapter_offset);
    atomic_store(&s_live_valid,true);
}
void weread_reader_tick(const char *uri, size_t chapter, uint32_t offset, unsigned percent,
    const char *summary, uint64_t now, bool active) {
    if (!book_path_is_weread(uri) || strcmp(s_reader.id, uri + 9)) return;
    if (active && now >= s_reader.last_tick && now - s_reader.last_tick <= 2000) s_reader.active_ms += now - s_reader.last_tick;
    s_reader.last_tick = now;
    if (!s_reader.seen || chapter != s_reader.chapter || offset != s_reader.offset) {
        s_reader.last_move = now;
        if (!s_reader.seen || chapter != s_reader.chapter) {
            s_reader.prefetched = 0; s_reader.prefetch_retry = 0;
            s_reader.prefetch_mask = s_reader.prefetch_failures = 0;
            s_reader.storage_blocked = false;
            s_reader.prefetch_cursor = chapter + 1;
            s_reader.prefetch_done = false;
            if (s_reader.total_chapters) book_cache_mark(s_reader.id, chapter, true);
        }
        s_reader.chapter = chapter; s_reader.offset = offset; s_reader.percent = percent;
        s_reader.seen = true;
    }
    if(s_reader.navigation_pending){
        ++s_reader.pages;s_reader.changed=true;s_reader.navigation_pending=false;
    }
    (void)summary;
    // 冷章结果由页面准备状态机消费，采样不能提前吞掉错误。
    // The page transition owns cold-chapter completion, including errors.
    if (!weread_runtime_busy()&&atomic_load_explicit(&s_done,memory_order_acquire)){
        uint32_t adopted=atomic_load(&s_adopting_prefetch_uid);
        if(s_result.job!=WR_JOB_CHAPTER&&!(s_result.job==WR_JOB_PREFETCH&&adopted&&s_result.chapter_uid==adopted)&&
           atomic_exchange(&s_done,false)){
            consume_reader_result();weread_runtime_result_free(&s_result);
        }
    }
    if (weread_runtime_busy() || !active) return;
    if((s_reader.changed&&now-s_reader.last_store>=1000)||s_reader.active_ms>=60000){
        (void)weread_reader_save(now);return;
    }
    if(now-s_reader.last_move<300)return;
    if(s_reader.cloud_sync_pending){
        if(!weread_auth_is_authenticated()){
            s_reader.cloud_sync_pending=false;
            if(weread_auth_is_expired()){s_reader.cloud_sync_ui=WEREAD_CLOUD_SYNC_EXPIRED;s_reader.cloud_sync_ui_until=UINT64_MAX;}
        }else if(now>=s_reader.cloud_sync_retry_at&&weread_runtime_start(WR_JOB_CLOUD_SYNC,s_reader.id,NULL)){
            s_reader.cloud_sync_pending=false;return;
        }
    }
    bool bits_ready = book_cache_ready(s_reader.id);
    bool next_ready = bits_ready ? book_cache_bit(s_reader.id, chapter + 1) : ((s_reader.prefetch_mask & 1u) != 0);
    if(!next_ready&&book_weread_uid(chapter+1)&&!s_reader.storage_blocked&&
        s_reader.prefetch_failures<3&&now>=s_reader.prefetch_retry&&weread_auth_is_authenticated()){
        (void)weread_runtime_start(WR_JOB_PREFETCH,s_reader.id,"1");return;
    }
    bool pause_report = now >= s_reader.last_move && now - s_reader.last_move >= 8000 &&
        now >= s_reader.pause_report_at && network_online_ready() && weread_auth_is_authenticated();
    bool urgent_report = now >= s_reader.last_move && now - s_reader.last_move >= 1000 &&
        now >= s_reader.report_at && weread_auth_is_authenticated() &&
        (s_reader.urgent || s_reader.durable.pending_pages >= 10 || now >= s_reader.report_due);
    if(s_reader.retry&&(pause_report||urgent_report)){
        if(weread_runtime_start(WR_JOB_NOTE,s_reader.id,NULL)){
            s_reader.report_at=now+15000;
            s_reader.pause_report_at=now+15000;
            s_reader.urgent=false;
        }
        return;
    }
    // 手动缓存不等翻页静止1秒，但前台输入冷却仍生效；关闭只停止新的派发，已启动章节照常完成。
    // Manual caching skips the1s idle-after-move window while foreground input still pauses it; turning it off only stops new dispatches.
    if(!s_reader.cache_forward&&now-s_reader.last_move<1000)return;
    uint64_t fg_ms = atomic_load(&s_fg_input_ms);
    if (fg_ms) {
        uint64_t sys_now = now_ms();
        if (sys_now >= fg_ms && sys_now - fg_ms < 800) return;
    }
    if(!s_reader.cover_warmed){s_reader.cover_warmed=weread_runtime_start(WR_JOB_COVER,s_reader.id,"warm");return;}
    if(!s_reader.prefetch_done&&!s_reader.storage_blocked&&s_reader.prefetch_failures<3&&now>=s_reader.prefetch_retry){
        // 显式关闭只抑制新的整本向后补齐；目录扫描、自动下一章预读与在飞章节都不受影响。
        // Explicit off suppresses only new whole-book forward dispatches; the cache scan, urgent next-chapter pre-read and in-flight chapters stay.
        bool forward=!s_reader.cache_forward_off&&weread_auth_is_authenticated();
        if (s_reader.total_chapters) {
            if (forward && bits_ready && s_reader.prefetch_cursor < s_reader.total_chapters &&
                weread_runtime_start(WR_JOB_PREFETCH, s_reader.id, "forward")) return;
            if (bits_ready && !book_cache_scanned(s_reader.id) &&
                weread_runtime_start(WR_JOB_PREFETCH, s_reader.id, "scan")) return;
            if (s_reader.prefetch_cursor >= s_reader.total_chapters && (!bits_ready || book_cache_scanned(s_reader.id)))
                s_reader.prefetch_done = true;
        } else if (forward && book_weread_uid(s_reader.prefetch_cursor) &&
                   weread_runtime_start(WR_JOB_PREFETCH, s_reader.id, "forward")) {
            return;
        }
    }
    (void)weread_runtime_idle(now);
}
void weread_reader_flush(uint64_t now, bool stop) {
    if(stop||!s_reader.id[0])weread_runtime_cancel_join();
    else weread_reader_pause_bg();
    if(!weread_runtime_busy()&&atomic_exchange(&s_done,false)){consume_reader_result();weread_runtime_result_free(&s_result);}
    (void)weread_reader_save(now);
    if(stop){book_cache_clear();weread_runtime_stop();}
}
bool weread_reader_flush_for_lock(uint64_t now, uint32_t timeout_ms) {
    if (!s_reader.id[0]) {
        weread_runtime_cancel_join();
        return true;
    }
    if (weread_runtime_busy() && s_work.job != WR_JOB_NOTE && s_work.job != WR_JOB_REPORT) {
        weread_runtime_cancel_join();
    }
    (void)weread_reader_save(now);
    if (!weread_auth_is_authenticated() || s_reader.save_failed) {
        weread_runtime_cancel_join();
        return !s_reader.retry && !s_reader.save_failed;
    }
    bool has_pending = s_reader.retry ||
        (s_reader.durable_valid && s_reader.durable.dirty &&
         s_reader.durable.position_pending && !s_reader.durable.sync_conflict);
    if (!has_pending) {
        weread_runtime_cancel_join();
        return true;
    }
#if defined(ESP_PLATFORM)
    bool wifi_ready = network_online_ready();
    if (!wifi_ready) {
        bool configured = false;
        char ssid[33] = {0};
        if (read_pico_transfer_get_saved_wifi(ssid, &configured) == ESP_OK)
            wifi_ready = configured;
        else {
            read_pico_transfer_status_t st = {0};
            read_pico_transfer_get_status(&st);
            wifi_ready = st.wifi_configured;
        }
    }
    if (!wifi_ready) {
        weread_runtime_cancel_join();
        return false;
    }
#endif
    uint64_t start = now_ms();
    if (weread_runtime_busy()) {
        if (s_work.job == WR_JOB_NOTE || s_work.job == WR_JOB_REPORT) {
            weread_reader_pause_bg_join(timeout_ms);
        } else {
            weread_runtime_cancel_join();
        }
    }
    if (!weread_runtime_busy() && atomic_exchange(&s_done, false)) {
        consume_reader_result();
        weread_runtime_result_free(&s_result);
    }
    has_pending = s_reader.retry ||
        (s_reader.durable_valid && s_reader.durable.dirty &&
         s_reader.durable.position_pending && !s_reader.durable.sync_conflict);
    if (!has_pending) return true;
    uint64_t elapsed = now_ms() - start;
    if (elapsed >= timeout_ms) {
        weread_runtime_cancel_join();
        return false;
    }
    uint32_t remain = (uint32_t)(timeout_ms - elapsed);
    if (!weread_runtime_start(WR_JOB_NOTE, s_reader.id, "force")) {
        return false;
    }
    uint64_t wait_until = now_ms() + remain;
    atomic_store(&s_deadline, wait_until);
    while (weread_runtime_busy() && now_ms() < wait_until) pause_ms(10);
    atomic_store(&s_deadline, 0);
    if (weread_runtime_busy()) {
        weread_runtime_cancel_join();
        return false;
    }
    if (atomic_exchange(&s_done, false)) {
        consume_reader_result();
        weread_runtime_result_free(&s_result);
    }
    return !s_reader.retry;
}
void weread_reader_wake(uint64_t now) {
    if (!s_reader.active || !s_reader.id[0]) return;
    if (!book_cache_ready(s_reader.id)) {
        book_cache_init(s_reader.id);
        if (s_reader.seen && s_reader.total_chapters) book_cache_mark(s_reader.id, s_reader.chapter, true);
        s_reader.prefetch_done = false;
        s_reader.prefetch_cursor = s_reader.seen ? s_reader.chapter + 1 : 1;
    }
    if (s_reader.anchor || s_reader.pending_coordinate) {
        atomic_store(&s_live_pos, ((uint64_t)s_reader.anchor_index << 32) | s_reader.anchor_offset);
        atomic_store(&s_live_valid, true);
    } else if (s_reader.durable_valid && s_reader.durable.chapter_uid) {
        atomic_store(&s_live_pos, ((uint64_t)s_reader.durable.chapter_index << 32) | s_reader.durable.chapter_offset);
        atomic_store(&s_live_valid, true);
    }
    s_reader.user_turned_since_open = false;
    s_reader.session_pages = 0;
    s_reader.cloud_sync_pending = weread_auth_is_authenticated();
    s_reader.cloud_sync_failures = 0;
    s_reader.cloud_sync_connect_timeouts = 0;
    s_reader.cloud_sync_retry_at = 0;
    s_reader.cloud_sync_ui = WEREAD_CLOUD_SYNC_IDLE;
    s_reader.cloud_sync_ui_until = 0;
    s_reader.last_tick = now;
    s_reader.last_move = now;
}
void weread_reader_end(uint64_t now) {
    s_reader.active=false;
    atomic_store(&s_cloud_jump_ready,false);
    atomic_store(&s_live_valid,false);
    atomic_store(&s_adopting_prefetch_uid,0);
    { WR_GUARD; s_cloud_jump.rebased = false; }
    if (s_reader.id[0]) weread_reader_flush(now, false);
    if (weread_shelf_flush_counts() == ESP_OK) s_prefetch_dirty_count = 0;
    book_cache_clear();
    char exit_id[64] = {0};
    bool exit_note = s_reader.id[0] && !s_reader.save_failed && s_reader.retry &&
        !weread_runtime_busy() && network_online_ready() && weread_auth_is_authenticated();
    if (exit_note) wr_copy(exit_id, sizeof(exit_id), s_reader.id);
    if(!s_reader.changed&&!s_reader.navigation_pending&&s_reader.active_ms<1000){
        memset(&s_reader,0,sizeof(s_reader));
    }
    if (exit_note && exit_id[0]) {
        (void)weread_runtime_start(WR_JOB_NOTE, exit_id, NULL);
    }
}
