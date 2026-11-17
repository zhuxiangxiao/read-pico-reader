/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：网络启停始终由同一工作任务执行；取消只shutdown自有socket，等待后才释放媒体。
 * English: One worker owns network start/stop; cancel shuts down only its socket and joins before media release.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
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
static atomic_uint_fast64_t s_deadline;
static bool s_configured, s_owns_network;
static unsigned s_cache_epoch;
static weread_result_t s_result;
static struct {
    weread_job_t job;
    char id[64], value[512], summary[256];
    size_t chapter;
    uint32_t offset, percent, seconds, uid, index, revision;
    unsigned pages;
    uint64_t now;
    bool force, raw_position;
} s_work;
static struct { char id[64]; weread_progress_t progress; bool valid, resume_ready; } s_choice;
static struct {
    char id[64], summary[256];
    size_t chapter, nav_chapter;
    uint32_t offset, percent, nav_offset, nav_percent, anchor_uid, anchor_index, anchor_offset, anchor_percent, revision;
    unsigned pages;
    uint64_t last_tick, active_ms, last_store, last_move, prefetch_retry;
    uint32_t prefetched;
    bool seen, changed, retry, cover_warmed, anchor, navigation_pending;
} s_reader;
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
static esp_err_t connect_network(void) {
    if (cancelled(NULL)) return ESP_ERR_INVALID_STATE;
#if defined(ESP_PLATFORM)
    read_pico_transfer_status_t status;
    for(unsigned i=0;i<150 && !cancelled(NULL);++i){
        read_pico_transfer_get_status(&status);
        if(status.network_ready && status.mode==READ_PICO_TRANSFER_MODE_STA){
            if(time(NULL)<1700000000){
                uint32_t utc;
                esp_err_t err=read_pico_transfer_sync_time_online_cancelable(&utc,cancelled,NULL);
                if(err!=ESP_OK)return err;
            }
            return cancelled(NULL)?ESP_ERR_INVALID_STATE:ESP_OK;
        }
        if(status.state==READ_PICO_TRANSFER_STOPPED){
            s_owns_network=false;
            read_pico_transfer_cfg_t cfg={.mode=READ_PICO_TRANSFER_MODE_STA,.network_only=true,.cover_lookup=true};
            esp_err_t err=read_pico_transfer_start(&cfg);
            if(err==ESP_OK)s_owns_network=true;
            else if(err!=ESP_ERR_INVALID_STATE && err!=ESP_ERR_NOT_FINISHED)return err;
        } else if(status.mode!=READ_PICO_TRANSFER_MODE_STA){
            return ESP_ERR_INVALID_STATE;
        } else if(status.state==READ_PICO_TRANSFER_ERROR){
            return status.last_error!=ESP_OK?status.last_error:ESP_ERR_INVALID_STATE;
        }
        // 已有STA正在连接时等待IP，不中断外部会话。/ Wait for an existing STA's IP without interrupting its owner.
        read_pico_transfer_service_poll();pause_ms(100);
    }
    return cancelled(NULL)?ESP_ERR_INVALID_STATE:ESP_ERR_TIMEOUT;
#else
    s_owns_network = true; return ESP_OK;
#endif
}
static void stop_network(void) {
#if defined(ESP_PLATFORM)
    if (s_owns_network) read_pico_transfer_stop();
#endif
    s_owns_network = false;
}
void weread_runtime_result_free(weread_result_t *r) {
    if (r) { free(r->books); free(r->cover); memset(r, 0, sizeof(*r)); }
}
void weread_runtime_load_local(void) {
    WR_GUARD;
#if defined(ESP_PLATFORM)
    if (!read_pico_sd_io_ready()) {weread_auth_forget_session();weread_shelf_clear();return;}
#endif
    // 每次进入重新绑定当前卡；其他页面发生的换卡也不能复用旧账号。/ Rebind on entry even when media changed while another page was active.
    (void)weread_auth_init(NULL); weread_shelf_clear();(void)weread_shelf_load();
}
static void snapshot(void) {
    WR_GUARD;
    s_result.authenticated = weread_auth_is_authenticated();
    const weread_auth_session_t *session = weread_auth_get_current_session();
    if (session) wr_display_copy(s_result.nickname, sizeof(s_result.nickname), session->user_name);
    s_result.count = weread_shelf_count();
    if (s_result.count) {
        s_result.books = wr_alloc(s_result.count * sizeof(*s_result.books));
        if (!s_result.books) { s_result.count = 0; s_result.error = ESP_ERR_NO_MEM; return; }
        for (size_t i = 0; i < s_result.count; ++i) weread_shelf_get(i, &s_result.books[i]);
    }
}
static esp_err_t cover_job(const char *url);
static esp_err_t prepare_open(bool choose, bool cloud) {
    WR_GUARD;
    weread_shelf_sync_discard();
    weread_toc_t toc = {0};
    esp_err_t synced = choose ? ESP_OK : weread_toc_sync(s_work.id);
    // 同步失败保留原目录文件，之后只加载一次，避免同时保留两个大型目录。/ Failed sync retains disk cache; load once afterward instead of retaining duplicate TOCs.
    esp_err_t cached = weread_toc_load(s_work.id, &toc);
    if(choose && (cached!=ESP_OK || !toc.count)){
        weread_toc_free(&toc);synced=weread_toc_sync(s_work.id);cached=weread_toc_load(s_work.id,&toc);
    }
    if (cached != ESP_OK || !toc.count) { weread_toc_free(&toc); return synced != ESP_OK ? synced : ESP_ERR_NOT_FOUND; }
    if (choose) {
        s_result.local = s_choice.progress;
    } else {
        bool conflict = false;
        esp_err_t progress=weread_progress_sync(s_work.id, &s_result.local, &s_result.cloud, &conflict);(void)progress;
        ESP_LOGI("weread","progress result=%d local=%u cloud=%u",(int)progress,(unsigned)s_result.local.progress,(unsigned)s_result.cloud.progress);
        if(progress!=ESP_OK&&!s_result.local.chapter_uid){weread_toc_free(&toc);return progress;}
        if (conflict && !cancelled(NULL)) { s_result.conflict = true; weread_toc_free(&toc); return ESP_OK; }
    }
    weread_progress_t selected = choose ? s_choice.progress : s_result.local;
    uint32_t uid = selected.chapter_uid;
    bool exists = false;
    for (size_t i = 0; i < toc.count; ++i) if (toc.chapters[i].uid == uid) exists = true;
    ESP_LOGI("weread","target p=%u uid=%u idx=%u off=%u choice=%d ok=%d",(unsigned)selected.progress,(unsigned)uid,(unsigned)selected.chapter_index,(unsigned)selected.chapter_offset,choose,exists);
    if(!exists&&uid){weread_toc_free(&toc);return ESP_ERR_INVALID_RESPONSE;}
    if (!exists) { uid = toc.chapters[0].uid; selected.chapter_uid = uid; selected.chapter_offset = 0; selected.chapter_index = toc.chapters[0].index; selected.book_version = toc.book_version; }
    weread_toc_free(&toc);
    esp_err_t err = cancelled(NULL) ? ESP_ERR_INVALID_STATE : weread_chapter_download(s_work.id, uid);
    if (err == ESP_OK) {
        err=book_weread_prepare_open(s_work.id);
        if(err!=ESP_OK)return err;
        err=book_weread_validate_prepared(s_work.id,uid,selected.chapter_index,selected.chapter_offset,&selected.book_version);
        if(err!=ESP_OK)return err;
        err=choose?weread_progress_select(s_work.id,&selected,cloud):weread_progress_checkpoint(s_work.id,&selected);
        if(err!=ESP_OK)return err;
        if(choose){selected.dirty=selected.position_pending=!cloud;if(cloud)selected.pending_pages=selected.reading_time=0;}
        strcpy(s_choice.id, s_work.id); s_choice.progress = selected; s_choice.valid = true; s_result.open_ready = true;
    }
    weread_toc_free(&toc); return err;
}
static esp_err_t remove_cache(const char *path, unsigned depth) {
    if (depth > 34 || cancelled(NULL)) return ESP_ERR_INVALID_STATE;
    DIR *dir = opendir(path); if (!dir) return errno==ENOENT?ESP_OK:ESP_FAIL;
    char *child __attribute__((cleanup(wr_release)))=wr_alloc(768);
    if(!child){closedir(dir);return ESP_ERR_NO_MEM;}
    struct dirent *entry; esp_err_t err = ESP_OK;
    while ((entry = readdir(dir)) && err == ESP_OK) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        int n = snprintf(child, 768, "%s/%s", path, entry->d_name);
        if (n < 0 || n >= 768) { err = ESP_ERR_INVALID_SIZE; break; }
        struct stat st;
#if defined(ESP_PLATFORM)
        if (stat(child, &st)) { err = ESP_FAIL; break; }
#else
        if (lstat(child, &st)) { err = ESP_FAIL; break; }
#endif
        // 保留离线进度，清理内容后仍可向云端补报。/ Retain offline progress for later cloud reconciliation.
        if (depth == 0 && !strncmp(entry->d_name, "progress.json", 13)) continue;
        if (S_ISDIR(st.st_mode)) {
            err = remove_cache(child, depth + 1);
            if (err == ESP_OK) (void)rmdir(child);
        } else if (unlink(child)) err = ESP_FAIL;
    }
    closedir(dir); return err;
}
static esp_err_t cover_job_impl(const char *url,bool retry) {
    WR_GUARD;
#if defined(ESP_PLATFORM)
    char path[512], temporary[544],uri[80];
    snprintf(uri,sizeof(uri),"weread://%s",s_work.id);
    s_result.cover=wr_alloc(WEREAD_COVER_WIDTH*WEREAD_COVER_HEIGHT);
    if (!s_result.cover) return ESP_ERR_NO_MEM;
    if (book_weread_cover(uri,s_result.cover,WEREAD_COVER_WIDTH*WEREAD_COVER_HEIGHT)) return ESP_OK;
    free(s_result.cover);s_result.cover=NULL;
    if(!url&&!retry&&weread_cover_cache_path(s_work.id,"failed",path,sizeof(path))==ESP_OK){
        uint64_t failed=0;FILE *f=fopen(path,"rb");
        if(f){if(fread(&failed,sizeof(failed),1,f)!=1)failed=0;fclose(f);}
        uint64_t now=(uint64_t)time(NULL);
        if(failed>=1700000000&&now>=failed&&now-failed<300){
            // 失败缓存仅阻远端：合法原图在解码器临时OOM后仍可本地重试。
            // Back off only remote requests; a valid raw image can recover after transient decoder OOM.
            s_result.cover_retry_seconds=(unsigned)(300-(now-failed));
        }
    }
    if (weread_cover_cache_path(s_work.id,"img",path,sizeof(path)) != ESP_OK) return ESP_ERR_INVALID_ARG;
    if (url && *url) {
        snprintf(temporary, sizeof(temporary), "%s.part", path);
        char parent[512]; strcpy(parent, path); *strrchr(parent, '/') = 0;
        if (wr_mkdirs(parent) != ESP_OK) return ESP_FAIL;
        FILE *out = fopen(temporary, "w+b"); if (!out) return ESP_FAIL;
        weread_download_info_t info={0};
        esp_err_t err = weread_client_download(url, out, 1024u * 1024u, cancelled, NULL, &info);
        s_result.http_status = info.status;
        bool ok = err == ESP_OK && info.status == 200 && !fflush(out);
        unsigned char magic[12] = {0}; rewind(out); size_t n = fread(magic, 1, sizeof(magic), out);
        ok = ok && n >= 3 && (!memcmp(magic, "\x89PNG", 4) || (magic[0] == 255 && magic[1] == 216 && magic[2] == 255) ||
            (n==12&&!memcmp(magic,"RIFF",4)&&!memcmp(magic+8,"WEBP",4)));
        if (fclose(out)) ok = false;
        if (!ok || cancelled(NULL)) { unlink(temporary); return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE; }
        unlink(path); if (rename(temporary, path)) { unlink(temporary); return ESP_FAIL; }
    }
    FILE *file = fopen(path, "rb"); if (!file) return ESP_ERR_NOT_FOUND;
    unsigned char first[8]; size_t n = fread(first, 1, 8, file); fclose(file);
    bool png = n == 8 && !memcmp(first, "\x89PNG", 4);
    s_result.cover = wr_alloc(WEREAD_COVER_WIDTH * WEREAD_COVER_HEIGHT);
    if (!s_result.cover) return ESP_ERR_NO_MEM;
    if (!book_image_file_grayscale(path, png, WEREAD_COVER_WIDTH, WEREAD_COVER_HEIGHT, s_result.cover)) {
        free(s_result.cover); s_result.cover = NULL; return ESP_ERR_INVALID_RESPONSE;
    }
    if (weread_cover_cache_path(s_work.id,"gray",path,sizeof(path)) == ESP_OK)
        (void)wr_write_bytes(path,s_result.cover,WEREAD_COVER_WIDTH*WEREAD_COVER_HEIGHT);
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
        char path[512];
        if(weread_cover_cache_path(s_work.id,"failed",path,sizeof(path))==ESP_OK){
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
static void perform_job(void) {
    s_result.job = s_work.job; strcpy(s_result.id, s_work.id);
    esp_err_t err = ESP_OK;
#if defined(ESP_PLATFORM)
    if (s_work.job != WR_JOB_STOP && s_work.job != WR_JOB_CONNECT && !read_pico_sd_io_ready()) {
        s_result.error=ESP_ERR_INVALID_STATE;return;
    }
#endif
    switch (s_work.job) {
        case WR_JOB_INIT: weread_runtime_load_local(); snapshot(); break;
        case WR_JOB_SYNC:
            err=weread_shelf_sync_step(strcmp(s_work.value,"continue")!=0);snapshot();
            s_result.shelf_loading=err==ESP_OK&&weread_shelf_sync_pending();
            weread_shelf_sync_progress(&s_result.shelf_done,&s_result.shelf_total,&s_result.shelf_skipped);break;
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
            if(!strcmp(s_work.value,"warm")){
                err=cover_job(NULL);
                weread_book_t b;char url[512];
                if(err!=ESP_OK&&!s_result.cover_retry_seconds&&!cancelled(NULL)&&weread_shelf_find(s_work.id,&b)&&
                    weread_cover_get_url(b.cover,url,sizeof(url))==ESP_OK)err=cover_job(url);
            }else err = cover_job(s_work.value);
            break;
        case WR_JOB_CLEAR: {
            char path[512];
            if (wr_cache_path(s_work.id, ".", path, sizeof(path)) != ESP_OK) err = ESP_ERR_INVALID_ARG;
            else { *strrchr(path, '/') = 0; err = remove_cache(path, 0); }
            break;
        }
        case WR_JOB_LOGOUT:
            err = weread_auth_clear_session(); weread_shelf_clear(); memset(&s_choice,0,sizeof(s_choice)); ++s_cache_epoch; stop_network(); snapshot(); break;
        case WR_JOB_NOTE: {
            uint32_t co = s_work.offset;
            err = s_work.raw_position?ESP_OK:book_weread_map(s_work.chapter, true, s_work.offset, &co);
            if(err==ESP_OK&&s_work.raw_position)err=weread_progress_note_time(s_work.id,s_work.seconds,(uint64_t)time(NULL));
            else if (err == ESP_OK) err = weread_progress_note(s_work.id, s_work.uid,
                s_work.index, co, s_work.percent, s_work.seconds,
                s_work.pages, s_work.summary, (uint64_t)time(NULL));
            s_result.noted = err == ESP_OK;
            if(s_result.noted){
                s_result.local.chapter_uid=s_work.uid;s_result.local.chapter_index=s_work.index;
                s_result.local.chapter_offset=co;s_result.local.progress=s_work.percent;
                wr_display_copy(s_result.local.summary,sizeof(s_result.local.summary),s_work.summary);
            }
            if (err == ESP_OK && !cancelled(NULL)) err = weread_progress_report(s_work.id, s_work.force, s_work.now);
            if(s_result.noted){
                s_result.local.dirty=true;
                weread_progress_t saved;if(weread_progress_load(s_work.id,&saved)==ESP_OK)s_result.local=saved;
            }
            break;
        }
        case WR_JOB_CHAPTER: err = weread_chapter_download(s_work.id,(uint32_t)s_work.chapter); break;
        case WR_JOB_PREFETCH: err = weread_chapter_download_ex(s_work.id, (uint32_t)s_work.chapter, true); break;
        case WR_JOB_CONNECT: err = connect_network(); break;
        case WR_JOB_STOP: book_weread_clear_prepared();weread_shelf_sync_discard();stop_network(); break;
        case WR_JOB_TOC: err=book_weread_prepare_open(s_work.id);break;
    }
    if (s_result.conflict) {
        // 候选由UI显式选择后再复制进下一条任务。/ UI explicitly selects a candidate before the next job.
        s_choice.valid = false;
    }
    if (cancelled(NULL) && err == ESP_OK && s_work.job != WR_JOB_STOP) err = ESP_ERR_INVALID_STATE;
    if (err != ESP_OK || s_result.error == ESP_OK) s_result.error = err;
    s_result.authenticated = weread_auth_is_authenticated();
#if defined(ESP_PLATFORM)
    if(s_work.job!=WR_JOB_COVER&&s_work.job!=WR_JOB_NOTE)
        ESP_LOGI("weread","memory job=%u result=%d internal=%u largest=%u psram=%u stack=%u",
            (unsigned)s_work.job,(int)s_result.error,(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT),(unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif
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
    weread_chapter_set_cancel(cancelled, NULL); weread_client_set_network(weread_runtime_connect, NULL);
    s_configured = true;
}
bool weread_runtime_busy(void) { return atomic_load(&s_busy); }
static void consume_reader_result(void) {
    if (!s_reader.id[0] || strcmp(s_reader.id, s_result.id)) return;
    if (s_result.job == WR_JOB_NOTE) {
        if (!s_result.noted) {
            s_reader.pages += s_work.pages;
            s_reader.active_ms += (uint64_t)s_work.seconds * 1000;
            s_reader.changed = true;
        }
        if(s_result.noted&&s_work.revision==s_reader.revision){
            s_reader.anchor_uid=s_result.local.chapter_uid;s_reader.anchor_index=s_result.local.chapter_index;
            s_reader.anchor_offset=s_result.local.chapter_offset;s_reader.anchor_percent=s_result.local.progress;
            wr_display_copy(s_reader.summary,sizeof(s_reader.summary),s_result.local.summary);s_reader.anchor=true;
        }
        s_reader.retry = s_result.error != ESP_OK || (s_result.local.dirty&&s_result.local.position_pending);
    }
    if (s_result.job == WR_JOB_PREFETCH) {
        s_reader.prefetched = s_result.error == ESP_OK ? (uint32_t)s_work.chapter : 0;
        s_reader.prefetch_retry = s_reader.last_tick + 15000;
    }
}
static void join_job(void) { while (weread_runtime_busy()) pause_ms(1); }
void weread_runtime_cancel_join(void) {
    if (weread_runtime_busy()) { atomic_store(&s_cancel, true); weread_client_abort_active(); join_job(); }
    atomic_store(&s_cancel, false); atomic_store(&s_deadline, 0);
}
bool weread_runtime_start(weread_job_t job, const char *id, const char *value) {
    weread_runtime_init();
    if (weread_runtime_busy() || (id && *id && !wr_valid_id(id)) || (value && strlen(value) >= sizeof(s_work.value))) return false;
    if (atomic_exchange(&s_done, false)) consume_reader_result();
    weread_runtime_result_free(&s_result);
    memset(&s_work, 0, sizeof(s_work)); s_work.job = job;
    if (id) wr_copy(s_work.id, sizeof(s_work.id), id);
    if (value) wr_copy(s_work.value, sizeof(s_work.value), value);
    if (job == WR_JOB_LOCAL || job == WR_JOB_CLOUD) {
        if (!id || strcmp(id, s_choice.id) || (!s_choice.progress.chapter_uid &&
            (job != WR_JOB_CLOUD || s_choice.progress.chapter_index || s_choice.progress.chapter_offset || s_choice.progress.progress))) return false;
    }
    if (job == WR_JOB_NOTE) {
        s_work.raw_position=s_reader.anchor;s_work.revision=s_reader.revision;
        s_work.uid=s_reader.anchor?s_reader.anchor_uid:book_weread_uid(s_reader.nav_chapter);
        s_work.index=s_reader.anchor?s_reader.anchor_index:book_weread_chapter_index(s_reader.nav_chapter);
        s_work.chapter = s_reader.nav_chapter; s_work.offset = s_reader.anchor?s_reader.anchor_offset:s_reader.nav_offset;
        s_work.percent = s_reader.anchor?s_reader.anchor_percent:s_reader.nav_percent;
        s_work.pages = s_reader.pages; s_work.seconds = (uint32_t)(s_reader.active_ms / 1000);
        s_work.now = s_reader.last_tick; s_work.force = value && *value == '1';
        wr_copy(s_work.summary, sizeof(s_work.summary), s_reader.summary);
    }
    if (job == WR_JOB_CHAPTER) {
        char *end=NULL; unsigned long uid=value?strtoul(value,&end,10):0;
        if (!uid || uid>UINT32_MAX || !end || *end) return false;
        s_work.chapter=(uint32_t)uid;
    }
    if (job == WR_JOB_PREFETCH) {
        s_work.chapter = book_weread_uid(s_reader.chapter + 1);
        if (!s_work.chapter) return false;
    }
    atomic_store(&s_cancel, false);
    atomic_store(&s_deadline, job == WR_JOB_NOTE && s_work.force ? now_ms() + 1800 : 0);
    atomic_store(&s_busy, true);
    if (!atomic_load(&s_alive)) {
        atomic_store(&s_alive, true);
#if defined(ESP_PLATFORM)
        if (!s_queue) s_queue = xQueueCreateStatic(1, 1, &s_queue_byte, &s_queue_storage);
        // WiFi/NVS会关闭Flash缓存，调用栈必须位于内部RAM。/ WiFi/NVS disables flash caches, so the caller's stack must stay in internal RAM.
        if (!s_queue || xTaskCreateWithCaps(worker, "weread", 20480, NULL, 1, &s_task,
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
            ESP_LOGW("weread", "worker allocation failed internal=%u largest=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
            atomic_store(&s_alive, false); atomic_store(&s_busy, false); return false;
        }
#else
        if (s_joinable) { pthread_join(s_thread, NULL); s_joinable = false; }
        if (pthread_create(&s_thread, NULL, worker, NULL)) { atomic_store(&s_alive, false); atomic_store(&s_busy, false); return false; }
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
    if (in_worker()) return connect_network();
    if (!weread_runtime_start(WR_JOB_CONNECT, NULL, NULL)) return ESP_ERR_INVALID_STATE;
    join_job(); esp_err_t err = s_result.error; return err;
}
void weread_runtime_stop(void) {
    weread_runtime_cancel_join();
    if (atomic_load(&s_alive) && weread_runtime_start(WR_JOB_STOP, NULL, NULL)) join_job();
#if !defined(ESP_PLATFORM)
    if (s_joinable) { pthread_join(s_thread, NULL); s_joinable = false; }
#endif
    weread_runtime_result_free(&s_result); atomic_store(&s_done, false);
}
void weread_runtime_media_lost(void) {
    weread_runtime_stop(); memset(&s_reader, 0, sizeof(s_reader)); memset(&s_choice,0,sizeof(s_choice)); ++s_cache_epoch;
    weread_shelf_clear();weread_auth_forget_session();
}
unsigned weread_runtime_cache_epoch(void){return s_cache_epoch;}
esp_err_t weread_reader_prepare(const char *uri,size_t chapter) {
    if (!book_path_is_weread(uri)) return ESP_ERR_INVALID_ARG;
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
void weread_reader_begin(const char *uri, uint64_t now) {
    memset(&s_reader, 0, sizeof(s_reader));
    if (book_path_is_weread(uri)&&s_choice.resume_ready&&!strcmp(s_choice.id,uri+9)) {
        wr_copy(s_reader.id,sizeof(s_reader.id),uri+9);s_reader.last_tick=s_reader.last_store=now;
        s_reader.anchor_uid=s_choice.progress.chapter_uid;s_reader.anchor_index=s_choice.progress.chapter_index;
        s_reader.anchor_offset=s_choice.progress.chapter_offset;s_reader.anchor_percent=s_choice.progress.progress;
        wr_display_copy(s_reader.summary,sizeof(s_reader.summary),s_choice.progress.summary);
        s_reader.retry=s_choice.progress.dirty&&s_choice.progress.position_pending;s_reader.anchor=true;s_choice.resume_ready=false;
    }
}
void weread_reader_navigated(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary){
    if(book_path_is_weread(uri)&&!strcmp(s_reader.id,uri+9)){
        ++s_reader.revision;s_reader.anchor=false;s_reader.navigation_pending=true;
        s_reader.nav_chapter=chapter;s_reader.nav_offset=offset;s_reader.nav_percent=percent;
        wr_display_copy(s_reader.summary,sizeof(s_reader.summary),summary);
    }
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
static bool checkpoint(uint64_t now, bool force) {
    s_reader.last_tick = now;
    if (!s_reader.seen || (!s_reader.changed&&!s_reader.retry&&s_reader.active_ms<1000) ||
        !weread_runtime_start(WR_JOB_NOTE, s_reader.id, force ? "1" : "0")) return false;
    s_reader.pages = 0; s_reader.active_ms %= 1000; s_reader.changed = s_reader.retry = false; s_reader.last_store = now;
    return true;
}
void weread_reader_tick(const char *uri, size_t chapter, uint32_t offset, unsigned percent,
    const char *summary, uint64_t now, bool active) {
    if (!book_path_is_weread(uri) || strcmp(s_reader.id, uri + 9)) return;
    if (active && now >= s_reader.last_tick && now - s_reader.last_tick <= 2000) s_reader.active_ms += now - s_reader.last_tick;
    s_reader.last_tick = now;
    if (!s_reader.seen || chapter != s_reader.chapter || offset != s_reader.offset) {
        s_reader.last_move = now;
        if (!s_reader.seen || chapter != s_reader.chapter) { s_reader.prefetched = 0; s_reader.prefetch_retry = 0; }
        s_reader.chapter = chapter; s_reader.offset = offset; s_reader.percent = percent;
        s_reader.seen = true;
    }
    if(s_reader.navigation_pending){
        ++s_reader.pages;s_reader.changed=true;s_reader.navigation_pending=false;
    }
    (void)summary;
    weread_result_t result;
    if (weread_runtime_poll(&result)) {
        weread_runtime_result_free(&result);
    }
    if (weread_runtime_busy() || !active) return;
    if ((s_reader.changed && now - s_reader.last_store >= 1000) || s_reader.active_ms >= 60000 ||
        (s_reader.retry && now - s_reader.last_store >= 15000)) { (void)checkpoint(now, false); return; }
    uint32_t uid = book_weread_uid(chapter + 1);
    if (uid && uid != s_reader.prefetched && now >= s_reader.prefetch_retry && now - s_reader.last_move >= 1000) {
        (void)weread_runtime_start(WR_JOB_PREFETCH, s_reader.id, NULL);return;
    }
    if(!s_reader.cover_warmed&&now-s_reader.last_move>=1000)
        s_reader.cover_warmed=weread_runtime_start(WR_JOB_COVER,s_reader.id,"warm");
}
void weread_reader_flush(uint64_t now, bool stop) {
    weread_runtime_cancel_join();
    weread_result_t result;if(weread_runtime_poll(&result))weread_runtime_result_free(&result);
    if (s_reader.id[0] && s_reader.seen && checkpoint(now, true)) join_job();
    if (stop) weread_runtime_stop();
}
void weread_reader_end(uint64_t now) {
    if (s_reader.id[0]) weread_reader_flush(now, true);
    memset(&s_reader, 0, sizeof(s_reader));
}
