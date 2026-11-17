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
static uint64_t s_idle_at,s_network_active;
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
static struct { char id[64]; weread_progress_t progress; uint32_t fresh_version,last_chapter;bool valid, resume_ready; } s_choice;
static struct {
    char id[64], summary[256], owner_root[256], owner_vid[WEREAD_VID_MAX_LEN], owner_device[WEREAD_DEVICE_ID_MAX_LEN];
    uint32_t owner_version,nav_version;unsigned owner_epoch;
    size_t chapter, nav_chapter;
    uint32_t offset, percent, nav_offset, nav_percent, anchor_uid, anchor_index, anchor_offset, anchor_percent, revision;
    unsigned pages;
    uint32_t durable_revision;
    uint64_t last_tick, active_ms, last_store, last_move, prefetch_retry;
    uint32_t prefetched;
    uint64_t report_at;
    unsigned prefetch_mask, prefetch_failures;
    weread_progress_state_t state;
    bool seen, changed, retry, cover_warmed, anchor, navigation_pending, storage_blocked, pending_coordinate, save_failed;
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
            s_network_active=now_ms();return cancelled(NULL)?ESP_ERR_INVALID_STATE:ESP_OK;
        }
        if(status.state==READ_PICO_TRANSFER_STOPPED){
            s_owns_network=false;
            read_pico_transfer_cfg_t cfg={.mode=READ_PICO_TRANSFER_MODE_STA,.network_only=true,.cover_lookup=true};
            esp_err_t err=read_pico_transfer_start(&cfg);
            if(err==ESP_OK){s_owns_network=true;s_network_active=now_ms();}
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
    s_owns_network = true;s_network_active=now_ms(); return ESP_OK;
#endif
}
static void stop_network(void) {
#if defined(ESP_PLATFORM)
    if (s_owns_network) read_pico_transfer_stop();
#endif
    s_owns_network = false;s_network_active=0;
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
    const weread_auth_session_t *session = weread_auth_get_current_session();
    if (session) wr_display_copy(s_result.nickname, sizeof(s_result.nickname), session->user_name);
    esp_err_t err=weread_shelf_snapshot(&s_result.books,&s_result.count);
    if(err!=ESP_OK)s_result.error=err;
}
static esp_err_t cover_job(const char *url);
static esp_err_t prepare_open(bool choose, bool cloud) {
    WR_GUARD;
    uint64_t phase[5]={now_ms(),0,0,0,0};
    weread_shelf_sync_discard();
    book_weread_clear_prepared();weread_toc_t toc = {0};
    if (choose) {
        s_result.local = s_choice.progress;
    } else {
        bool conflict = false;
        strcpy(s_choice.id,s_work.id);s_choice.valid=false;s_choice.fresh_version=s_choice.last_chapter=0;
        esp_err_t progress=weread_progress_sync_version(s_work.id, &s_result.local, &s_result.cloud, &conflict,&s_choice.fresh_version,&s_choice.last_chapter);(void)progress;
        ESP_LOGI("weread","progress result=%d local=%u cloud=%u",(int)progress,(unsigned)s_result.local.progress,(unsigned)s_result.cloud.progress);
        if(progress==WEREAD_ERR_VERSION||progress==ESP_ERR_NO_MEM||(progress!=ESP_OK&&!s_result.local.chapter_uid))return progress;
        if (conflict && !cancelled(NULL)) { s_result.conflict = true; weread_toc_free(&toc); return ESP_OK; }
    }
    phase[1]=now_ms();
    weread_progress_t selected = choose ? s_choice.progress : s_result.local;
    esp_err_t prepared=weread_toc_prepare(s_work.id,false,s_choice.fresh_version,&toc);
    if(prepared!=ESP_OK||!toc.count){weread_toc_free(&toc);return prepared!=ESP_OK?prepared:ESP_ERR_NOT_FOUND;}
    uint32_t uid = selected.chapter_uid;
    bool exists = false, moved=false;
    for (size_t i = 0; i < toc.count; ++i) if (toc.chapters[i].uid == uid) {
        exists=true;moved=selected.chapter_index&&selected.chapter_index!=toc.chapters[i].index;
    }
    if((uid&&(!exists||moved))||s_choice.last_chapter>toc.chapters[toc.count-1].index){
        weread_toc_free(&toc);prepared=weread_toc_prepare(s_work.id,true,s_choice.fresh_version,&toc);
        if(prepared!=ESP_OK||!toc.count){weread_toc_free(&toc);return prepared!=ESP_OK?prepared:ESP_ERR_NOT_FOUND;}
        exists=moved=false;
        for(size_t i=0;i<toc.count;++i)if(toc.chapters[i].uid==uid){
            exists=true;moved=selected.chapter_index&&selected.chapter_index!=toc.chapters[i].index;
        }
        if(moved){weread_toc_free(&toc);return WEREAD_ERR_POSITION;}
    }
    ESP_LOGI("weread","target p=%u uid=%u idx=%u off=%u choice=%d ok=%d",(unsigned)selected.progress,(unsigned)uid,(unsigned)selected.chapter_index,(unsigned)selected.chapter_offset,choose,exists);
    if(!exists&&uid){weread_toc_free(&toc);return ESP_ERR_INVALID_RESPONSE;}
    if (!exists) { uid = toc.chapters[0].uid; selected.chapter_uid = uid; selected.chapter_offset = 0; selected.chapter_index = toc.chapters[0].index; selected.book_version = toc.book_version; }
    phase[2]=now_ms();
    // 仅缓存命中保留typed目录；下载解压前释放，维持原峰值内存。/ Retain the typed TOC only on cache hits; release it before download/inflate to retain the memory budget.
    bool chapter_hit=weread_chapter_cached(s_work.id,uid);
    if(chapter_hit)book_weread_adopt_prepared(s_work.id,&toc);
    weread_toc_free(&toc);
    esp_err_t err = cancelled(NULL) ? ESP_ERR_INVALID_STATE : chapter_hit?ESP_OK:weread_chapter_download(s_work.id, uid);
    phase[3]=now_ms();
    if (err == ESP_OK) {
        err=book_weread_prepare_open(s_work.id);
        if(err!=ESP_OK)return err;
        err=book_weread_validate_prepared(s_work.id,uid,selected.chapter_index,selected.chapter_offset,&selected.book_version);
        if(err!=ESP_OK){book_weread_clear_prepared();return err;}
        err=choose?weread_progress_select(s_work.id,&selected,cloud):weread_progress_checkpoint(s_work.id,&selected);
        if(err!=ESP_OK){book_weread_clear_prepared();return err;}
        if(choose){selected.dirty=selected.position_pending=!cloud;if(cloud)selected.pending_pages=selected.reading_time=0;}
        strcpy(s_choice.id, s_work.id); s_choice.progress = selected; s_choice.valid = true; s_result.open_ready = true;
    }
    phase[4]=now_ms();
    ESP_LOGI("weread","open timing progress=%u toc=%u chapter=%u verify=%u total=%u cached=%d",
        (unsigned)(phase[1]-phase[0]),(unsigned)(phase[2]-phase[1]),(unsigned)(phase[3]-phase[2]),(unsigned)(phase[4]-phase[3]),(unsigned)(phase[4]-phase[0]),chapter_hit);
    (void)phase;
    if(err!=ESP_OK)book_weread_clear_prepared();
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
    if (s_work.job != WR_JOB_STOP && s_work.job != WR_JOB_CONNECT && s_work.job!=WR_JOB_DISCONNECT && !read_pico_sd_io_ready()) {
        s_result.error=ESP_ERR_INVALID_STATE;return;
    }
#endif
    weread_client_set_polling(true);
    weread_client_set_background(s_work.job==WR_JOB_NOTE||s_work.job==WR_JOB_REPORT||s_work.job==WR_JOB_PREFETCH||s_work.job==WR_JOB_CHAPTER||s_work.job==WR_JOB_COVER);
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
            if(!strcmp(s_work.value,"remote")||!strcmp(s_work.value,"original")){
                weread_book_t book;char url[512];weread_cover_retry_t retry={.original=!strcmp(s_work.value,"original")};
                err=weread_shelf_find(s_work.id,&book)?weread_cover_retry_url(book.cover,&retry,0,url,sizeof(url)):ESP_ERR_NOT_FOUND;
                if(err==ESP_OK)err=cover_job(url);
            }else if(!strcmp(s_work.value,"warm")){
                char ready[512];struct stat st;
                if(weread_cover_cache_path(s_work.id,"gray",ready,sizeof(ready))==ESP_OK&&!stat(ready,&st)&&S_ISREG(st.st_mode)&&st.st_size==WEREAD_COVER_WIDTH*WEREAD_COVER_HEIGHT)break;
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
            weread_progress_outbox_reset();err = weread_auth_clear_session(); weread_shelf_clear(); memset(&s_choice,0,sizeof(s_choice)); ++s_cache_epoch; stop_network(); snapshot(); break;
        case WR_JOB_NOTE: case WR_JOB_REPORT: {
            if(s_work.id[0] || weread_progress_outbox_next(s_result.id,sizeof(s_result.id),(uint64_t)time(NULL))==ESP_OK)
                err=weread_progress_outbox_report(s_result.id,s_work.now,(uint64_t)time(NULL),&s_result.local);
            break;
        }
        case WR_JOB_CHAPTER: s_result.chapter_uid=(uint32_t)s_work.chapter;err = weread_chapter_download(s_work.id,(uint32_t)s_work.chapter); break;
        case WR_JOB_PREFETCH: err = weread_chapter_download_ex(s_work.id, (uint32_t)s_work.chapter, true); break;
        case WR_JOB_CONNECT: err = connect_network(); break;
        case WR_JOB_DISCONNECT: stop_network();break;
        case WR_JOB_STOP: weread_progress_outbox_reset();book_weread_clear_prepared();weread_shelf_sync_discard();stop_network(); break;
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
    weread_chapter_set_cancel(cancelled, NULL); weread_client_set_network(weread_runtime_connect, NULL);
    s_configured = true;
}
bool weread_runtime_busy(void) { return atomic_load(&s_busy); }
static void consume_reader_result(void) {
    if (!s_reader.id[0] || strcmp(s_reader.id, s_result.id)) return;
    if (s_result.job == WR_JOB_NOTE || s_result.job == WR_JOB_REPORT) {
        if(s_result.local.revision==s_reader.durable_revision){
            s_reader.state=weread_progress_state(&s_result.local);
            s_reader.retry=s_result.local.dirty&&s_result.local.position_pending;
        }
    }
    if (s_result.job == WR_JOB_PREFETCH) {
        bool matching=false;
        for(unsigned i=0;i<3;++i)if(book_weread_uid(s_reader.chapter+1+i)==s_work.chapter){
            matching=true;
            if(s_result.error==ESP_OK)s_reader.prefetch_mask|=1u<<i;
            else if(s_result.error==WEREAD_ERR_STORAGE)s_reader.storage_blocked=true;
            else if(s_result.error!=ESP_ERR_INVALID_STATE)++s_reader.prefetch_failures;
        }
        if(matching)s_reader.prefetch_retry=s_reader.last_tick+(s_result.error==ESP_OK?0:15000);
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
void weread_reader_input(bool cached_text_only){
    if(cached_text_only&&weread_runtime_busy()){
        if(s_work.job!=WR_JOB_NOTE&&s_work.job!=WR_JOB_REPORT&&s_work.job!=WR_JOB_CHAPTER)weread_runtime_cancel();
        return;
    }
    weread_runtime_cancel_join();
}
bool weread_runtime_start(weread_job_t job, const char *id, const char *value) {
    weread_runtime_init();
    if (weread_runtime_busy() || (id && *id && !wr_valid_id(id)) || (value && strlen(value) >= sizeof(s_work.value))) return false;
    if((job==WR_JOB_OPEN||job==WR_JOB_LOCAL||job==WR_JOB_CLOUD)&&weread_reader_pending_save()&&weread_reader_save(now_ms())!=ESP_OK)return false;
    if (atomic_exchange(&s_done, false)) consume_reader_result();
    weread_runtime_result_free(&s_result);
    memset(&s_work, 0, sizeof(s_work)); s_work.job = job;
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
    }
    if (job == WR_JOB_PREFETCH) {
        s_work.chapter = book_weread_uid(s_reader.chapter + (value&&*value?(unsigned)strtoul(value,NULL,10):1));
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
    weread_runtime_stop();weread_progress_outbox_reset(); memset(&s_reader, 0, sizeof(s_reader)); memset(&s_choice,0,sizeof(s_choice)); ++s_cache_epoch;
    weread_shelf_clear();weread_auth_forget_session();
}
unsigned weread_runtime_cache_epoch(void){return s_cache_epoch;}
bool weread_runtime_idle(uint64_t now){
    if(weread_runtime_busy()||now<s_idle_at)return false;
    if(s_reader.id[0]&&s_reader.save_failed&&(weread_reader_save(now)!=ESP_OK))return false;
    if(s_owns_network&&s_network_active&&now>=s_network_active&&now-s_network_active>=60000)
        return weread_runtime_start(WR_JOB_DISCONNECT,NULL,NULL);
    s_idle_at=now+1000;
    return weread_runtime_start(WR_JOB_REPORT,NULL,NULL);
}
bool weread_reader_cached(const char *uri,size_t chapter){
    if(!book_path_is_weread(uri)||(weread_runtime_busy()&&s_work.job!=WR_JOB_NOTE&&s_work.job!=WR_JOB_REPORT))return false;
    uint32_t uid=book_weread_uid(chapter);
    return uid&&weread_chapter_cached(uri+9,uid);
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
        s_reader.retry=s_choice.progress.dirty&&s_choice.progress.position_pending;s_reader.state=weread_progress_state(&s_choice.progress);s_reader.durable_revision=s_choice.progress.revision;s_reader.anchor=true;s_choice.resume_ready=false;
    }
}
void weread_reader_navigated_version(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary,uint32_t content_version){
    if(book_path_is_weread(uri)&&!strcmp(s_reader.id,uri+9)){
        if(s_reader.nav_version!=content_version){
            s_reader.prefetch_mask=s_reader.prefetch_failures=0;s_reader.prefetch_retry=0;s_reader.storage_blocked=false;
        }
        s_reader.nav_version=content_version;
        ++s_reader.revision;s_reader.anchor=false;s_reader.pending_coordinate=false;s_reader.navigation_pending=true;
        s_reader.nav_chapter=chapter;s_reader.nav_offset=offset;s_reader.nav_percent=percent;
        wr_display_copy(s_reader.summary,sizeof(s_reader.summary),summary);
    }
}
void weread_reader_navigated(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary){
    weread_reader_navigated_version(uri,chapter,offset,percent,summary,s_reader.owner_version);
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
    uint32_t known=weread_shelf_book_version(uri+9);if(known&&known!=version)return WEREAD_ERR_VERSION;
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
    if(weread_runtime_busy()&&s_work.job!=WR_JOB_NOTE&&s_work.job!=WR_JOB_REPORT)return ESP_ERR_NOT_FINISHED;
    if(!s_reader.changed&&s_reader.active_ms<1000)return ESP_OK;
    WR_GUARD;
    const weread_auth_session_t *session=weread_auth_get_current_session();
    if(!session||s_reader.owner_epoch!=s_cache_epoch||strcmp(s_reader.owner_root,weread_auth_get_storage_root())||
        strcmp(s_reader.owner_vid,session->vid)||strcmp(s_reader.owner_device,session->device_id)){
        s_reader.save_failed=true;s_reader.state=WR_PROGRESS_CONFLICT;return ESP_ERR_INVALID_STATE;
    }
    weread_progress_t sample={0};bool noted=false;
    bool mapped=s_reader.anchor||s_reader.pending_coordinate;
    sample.chapter_uid=mapped?s_reader.anchor_uid:book_weread_uid(s_reader.nav_chapter);
    sample.chapter_index=mapped?s_reader.anchor_index:book_weread_chapter_index(s_reader.nav_chapter);
    sample.chapter_offset=mapped?s_reader.anchor_offset:s_reader.nav_offset;
    sample.progress=s_reader.anchor?s_reader.anchor_percent:s_reader.nav_percent;
    sample.reading_time=(uint32_t)(s_reader.active_ms/1000);sample.pending_pages=s_reader.pages;
    wr_display_copy(sample.summary,sizeof(sample.summary),s_reader.summary);
    esp_err_t err=mapped?ESP_OK:book_weread_map_version(s_reader.nav_chapter,s_reader.nav_version,true,s_reader.nav_offset,&sample.chapter_offset);
    if(err==ESP_OK&&!s_reader.anchor&&s_reader.nav_version!=s_reader.owner_version){
        uint32_t proven=0;
        err=book_weread_validate_prepared(s_reader.id,sample.chapter_uid,sample.chapter_index,sample.chapter_offset,&proven);
        if(err==ESP_OK&&proven!=s_reader.nav_version)err=WEREAD_ERR_VERSION;
    }
    if(err==ESP_OK&&!s_reader.anchor){
        s_reader.owner_version=s_reader.nav_version;
        s_reader.pending_coordinate=true;s_reader.anchor_uid=sample.chapter_uid;s_reader.anchor_index=sample.chapter_index;
        s_reader.anchor_offset=sample.chapter_offset;s_reader.anchor_percent=sample.progress;
    }
    if(err==ESP_OK)err=weread_progress_store_version(s_reader.id,&sample,!s_reader.anchor,s_reader.nav_version,(uint64_t)time(NULL),&noted);
    if(noted){
        s_reader.pages=0;s_reader.active_ms%=1000;s_reader.changed=false;s_reader.last_store=now;
        s_reader.anchor_uid=sample.chapter_uid;s_reader.anchor_index=sample.chapter_index;
        s_reader.anchor_offset=sample.chapter_offset;s_reader.anchor_percent=sample.progress;s_reader.anchor=true;
        s_reader.state=weread_progress_state(&sample);s_reader.retry=sample.dirty&&sample.position_pending;
        s_reader.durable_revision=sample.revision;s_reader.pending_coordinate=false;
        if(!mapped||s_reader.nav_version!=s_reader.owner_version)s_reader.owner_version=s_reader.nav_version;
    }
    s_reader.save_failed=err!=ESP_OK;
    return err;
}
weread_progress_state_t weread_reader_state(void){return s_reader.state;}
bool weread_reader_save_failed(void){return s_reader.save_failed;}
bool weread_reader_pending_save(void){return s_reader.changed||s_reader.navigation_pending||s_reader.save_failed;}
unsigned weread_reader_offline(void){unsigned n=0;while(n<3&&(s_reader.prefetch_mask&(1u<<n)))++n;return n;}
bool weread_reader_storage_blocked(void){return s_reader.storage_blocked;}
bool weread_reader_caching(void){return weread_runtime_busy()&&s_work.job==WR_JOB_PREFETCH&&!strcmp(s_work.id,s_reader.id);}
void weread_reader_tick(const char *uri, size_t chapter, uint32_t offset, unsigned percent,
    const char *summary, uint64_t now, bool active) {
    if (!book_path_is_weread(uri) || strcmp(s_reader.id, uri + 9)) return;
    if (active && now >= s_reader.last_tick && now - s_reader.last_tick <= 2000) s_reader.active_ms += now - s_reader.last_tick;
    s_reader.last_tick = now;
    if (!s_reader.seen || chapter != s_reader.chapter || offset != s_reader.offset) {
        s_reader.last_move = now;
        if (!s_reader.seen || chapter != s_reader.chapter) { s_reader.prefetched = 0; s_reader.prefetch_retry = 0;s_reader.prefetch_mask=s_reader.prefetch_failures=0;s_reader.storage_blocked=false; }
        s_reader.chapter = chapter; s_reader.offset = offset; s_reader.percent = percent;
        s_reader.seen = true;
    }
    if(s_reader.navigation_pending){
        ++s_reader.pages;s_reader.changed=true;s_reader.navigation_pending=false;
    }
    (void)summary;
    weread_result_t result;
    // 冷章结果由页面准备状态机消费，采样不能提前吞掉错误。
    // The page transition owns cold-chapter completion, including errors.
    if (!weread_runtime_busy()&&s_result.job!=WR_JOB_CHAPTER&&weread_runtime_poll(&result))weread_runtime_result_free(&result);
    if (weread_runtime_busy() || !active) return;
    if((s_reader.changed&&now-s_reader.last_store>=1000)||s_reader.active_ms>=60000){
        (void)weread_reader_save(now);return;
    }
    if(s_reader.retry&&now>=s_reader.report_at&&now-s_reader.last_move>=1000){
        s_reader.report_at=now+15000;(void)weread_runtime_start(WR_JOB_NOTE,s_reader.id,NULL);return;
    }
    if(now-s_reader.last_move<1000)return;
    if(!(s_reader.prefetch_mask&1u)&&book_weread_uid(chapter+1)&&!s_reader.storage_blocked&&
        s_reader.prefetch_failures<3&&now>=s_reader.prefetch_retry){
        (void)weread_runtime_start(WR_JOB_PREFETCH,s_reader.id,"1");return;
    }
    if(!s_reader.cover_warmed){s_reader.cover_warmed=weread_runtime_start(WR_JOB_COVER,s_reader.id,"warm");return;}
    if(!s_reader.storage_blocked&&s_reader.prefetch_failures<3&&now>=s_reader.prefetch_retry)
        for(unsigned i=1;i<3;++i)if(!(s_reader.prefetch_mask&(1u<<i))&&book_weread_uid(chapter+1+i)){
            const char *distance=i==1?"2":"3";(void)weread_runtime_start(WR_JOB_PREFETCH,s_reader.id,distance);return;
        }
    (void)weread_runtime_idle(now);
}
void weread_reader_flush(uint64_t now, bool stop) {
    if(stop||!s_reader.id[0]||(weread_runtime_busy()&&s_work.job!=WR_JOB_NOTE&&s_work.job!=WR_JOB_REPORT))weread_runtime_cancel_join();
    weread_result_t result;if(weread_runtime_poll(&result))weread_runtime_result_free(&result);
    (void)weread_reader_save(now);
    if(stop)weread_runtime_stop();
}
void weread_reader_end(uint64_t now) {
    if (s_reader.id[0]) weread_reader_flush(now, true);
    if(!s_reader.changed&&!s_reader.navigation_pending&&s_reader.active_ms<1000)memset(&s_reader,0,sizeof(s_reader));
}
