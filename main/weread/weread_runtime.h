/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：单一后台任务、页面快照与阅读/射频生命周期。
 * English: One worker, page snapshots and reader/radio lifecycle ownership.
 */
#pragma once
#include "weread_shelf.h"
#include "weread_progress.h"
#define WEREAD_COVER_WIDTH 176
#define WEREAD_COVER_HEIGHT 240
typedef enum {
    WR_JOB_INIT, WR_JOB_SYNC, WR_JOB_QR, WR_JOB_POLL, WR_JOB_OPEN,
    WR_JOB_LOCAL, WR_JOB_CLOUD, WR_JOB_COVER, WR_JOB_CLEAR, WR_JOB_LOGOUT,
    WR_JOB_NOTE, WR_JOB_CHAPTER, WR_JOB_PREFETCH, WR_JOB_CONNECT, WR_JOB_STOP,WR_JOB_TOC
} weread_job_t;
typedef struct {
    weread_job_t job;
    esp_err_t error;
    bool authenticated, conflict, open_ready, noted;
    int qr_state, http_status;
    unsigned cover_retry_seconds;
    char id[64], uid[64], qr_url[256], nickname[64];
    weread_book_t *books;
    size_t count;
    bool shelf_loading;
    size_t shelf_done,shelf_total,shelf_skipped;
    uint8_t *cover;
    weread_progress_t local, cloud;
} weread_result_t;

/// 这些调度接口只由页面控制任务调用，render只读取已接管的结果。/ One page-control caller; render reads only transferred results.
void weread_runtime_init(void);
bool weread_runtime_start(weread_job_t job, const char *id, const char *value);
bool weread_runtime_choose(const char *id, const weread_progress_t *progress, bool cloud);
bool weread_runtime_busy(void);
bool weread_runtime_poll(weread_result_t *out);
void weread_runtime_result_free(weread_result_t *result);
void weread_runtime_cancel_join(void);
void weread_runtime_stop(void);
void weread_runtime_media_lost(void);
unsigned weread_runtime_cache_epoch(void);
/// 仅本地读取凭据/书架，供首页、票根和开机恢复使用。/ Load credentials/shelf locally for home, tickets and boot resume.
void weread_runtime_load_local(void);
esp_err_t weread_runtime_connect(void *ctx);

/// 阅读器加载回调前由worker准备章节，前台只解析完整缓存。/ Let the worker prepare a chapter before foreground parsing.
esp_err_t weread_reader_prepare(const char *uri, size_t chapter);
esp_err_t weread_reader_prepare_open(const char *uri);
void weread_reader_begin(const char *uri, uint64_t now_ms);
/// 仅成功翻页/跳转后调用；排版与首次显示不算导航。/ Mark committed navigation, never initial paint or reflow.
void weread_reader_navigated(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary);
esp_err_t weread_reader_resume(const char *uri, size_t *chapter, uint32_t *offset);
void weread_reader_tick(const char *uri, size_t chapter, uint32_t offset, unsigned percent,
    const char *summary, uint64_t now_ms, bool active);
void weread_reader_flush(uint64_t now_ms, bool stop_network);
void weread_reader_end(uint64_t now_ms);
