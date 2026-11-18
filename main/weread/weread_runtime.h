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
    WR_JOB_NOTE, WR_JOB_CHAPTER, WR_JOB_PREFETCH, WR_JOB_CONNECT, WR_JOB_STOP,WR_JOB_TOC,WR_JOB_REPORT,WR_JOB_DISCONNECT,WR_JOB_CLOUD_SYNC
} weread_job_t;
typedef struct {
    weread_job_t job;
    esp_err_t error;
    bool authenticated, conflict, open_ready, noted;
    int qr_state, http_status;
    uint32_t chapter_uid;
    size_t chapter_idx;
    bool prefetch_done;
    unsigned cover_retry_seconds;
    char id[64], uid[64], qr_url[256], nickname[64];
    weread_shelf_item_t *books;
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
/// 仅请求取消，UI不等待低优先级任务清理。/ Request cancellation without blocking the UI.
void weread_runtime_cancel(void);
/// 微信书架空闲期扫描并补报；不改变其他页面网络生命周期。/ Drain durable reports during WeRead shelf idle.
bool weread_runtime_idle(uint64_t now_ms);
/// 已载入的同章纯文字页可继续绘制，进度任务用自己的位置快照完成；IO/生命周期边界仍取消并等待。
/// Loaded same-chapter text may render while a note uses its owned position snapshot; IO/lifecycle boundaries still join.
void weread_reader_input(bool cached_text_only);
void weread_runtime_stop(void);
void weread_runtime_media_lost(void);
unsigned weread_runtime_cache_epoch(void);
/// 仅本地读取凭据/书架，供首页、票根和开机恢复使用。/ Load credentials/shelf locally for home, tickets and boot resume.
void weread_runtime_load_local(void);
esp_err_t weread_runtime_connect(void *ctx);

/// 阅读器加载回调前由worker准备章节，前台只解析完整缓存。/ Let the worker prepare a chapter before foreground parsing.
esp_err_t weread_reader_prepare(const char *uri, size_t chapter);
/// 本地缓存快速判定，无网络工作；仅当后台正在下载同一章节或清理整书时返回false。/ Check local readiness without blocking on unrelated background jobs.
bool weread_reader_cached(const char *uri,size_t chapter);
/// 查询后台预取任务是否正在下载同一本书的同一章节。/ Return true if background prefetch is currently downloading the exact same chapter.
bool weread_runtime_prefetch_active(const char *id,uint32_t uid);
void weread_runtime_prefetch_adopt(uint32_t uid);
/// 提取后台云端同步确认的超前跳转目标，已读超过或非当前书籍则返回false。/ Consume a pending forward cloud-sync jump if still ahead of the active reader.
bool weread_reader_poll_cloud_jump(const char *uri,size_t *chapter_out,uint32_t *offset_out);
void weread_reader_commit_cloud_jump(const char *uri,bool committed);
/// 本地持久化先行；失败不消耗内存中的待保存导航。/ Persist locally without losing a failed navigation sample.
esp_err_t weread_reader_save(uint64_t now_ms);
weread_progress_state_t weread_reader_state(void);
bool weread_reader_save_failed(void);
bool weread_reader_pending_save(void);
/// 已保存在设备应急账本，尚未确认云端同步。/ Durable in the device rescue journal, without a cloud acknowledgement.
bool weread_reader_rescued(void);
unsigned weread_reader_offline(void);
void weread_reader_cached_progress(unsigned *cached, unsigned *total);
bool weread_reader_storage_blocked(void);
bool weread_reader_caching(void);
void weread_reader_pause_bg(void);
esp_err_t weread_reader_prepare_open(const char *uri);
bool weread_reader_open_ready(const char *uri);
void weread_reader_begin(const char *uri, uint64_t now_ms);
/// 仅成功翻页/跳转后调用；排版与首次显示不算导航。/ Mark committed navigation, never initial paint or reflow.
void weread_reader_navigated(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary);
/// 仅正文成功接管后携带实际已加载版本；不从后台元数据猜版本。/ Bind navigation to the content version actually committed by the reader.
void weread_reader_navigated_version(const char *uri,size_t chapter,uint32_t offset,unsigned percent,const char *summary,uint32_t content_version);
esp_err_t weread_reader_resume(const char *uri, size_t *chapter, uint32_t *offset);
esp_err_t weread_reader_resume_chapter(const char *uri, size_t *chapter, uint32_t *source_offset);
void weread_reader_tick(const char *uri, size_t chapter, uint32_t offset, unsigned percent,
    const char *summary, uint64_t now_ms, bool active);
void weread_reader_flush(uint64_t now_ms, bool stop_network);
void weread_reader_end(uint64_t now_ms);
