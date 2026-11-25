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
    WR_JOB_NOTE, WR_JOB_CHAPTER, WR_JOB_PREFETCH, WR_JOB_CONNECT, WR_JOB_STOP,WR_JOB_TOC,WR_JOB_REPORT,WR_JOB_DISCONNECT,WR_JOB_CLOUD_SYNC,
    WR_JOB_PROBE_SHELF, WR_JOB_CACHE_BOOK, WR_JOB_CACHE_STATS,
    WR_JOB_CACHE_CLEAN_FINISHED, WR_JOB_CACHE_CLEAN_ALL
} weread_job_t;
typedef struct {
    weread_job_t job;
    esp_err_t error;
    bool authenticated, auth_expired, conflict, open_ready, noted;
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
bool weread_runtime_network_active(void);
/// 已载入的同章纯文字页可继续绘制，进度任务用自己的位置快照完成；IO/生命周期边界仍取消并等待。
/// Loaded same-chapter text may render while a note uses its owned position snapshot; IO/lifecycle boundaries still join.
void weread_reader_input(bool cached_text_only);
void weread_runtime_stop(void);
void weread_runtime_media_lost(void);
unsigned weread_runtime_cache_epoch(void);
/// 仅本地读取凭据/书架，供首页、票根和开机恢复使用。/ Load credentials/shelf locally for home, tickets and boot resume.
void weread_runtime_load_local(void);
esp_err_t weread_runtime_connect(void *ctx);

typedef struct {
    bool active;
    bool storage_blocked;
    char book_id[64];
    uint16_t cached_chapters;
    uint16_t total_chapters;
} weread_shelf_cache_status_t;

typedef struct {
    bool valid;
    uint64_t total_bytes;
    uint64_t reclaimable_bytes;
    uint64_t free_bytes;
    uint64_t fs_total_bytes;
    uint32_t book_dir_count;
    uint32_t books_with_chapters;
} weread_runtime_cache_stats_t;

/// 探测可见书架条目的本地缓存章节数（最多 9 本，零联网）。
/// Probe locally cached chapter counts for visible shelf books (up to 9, zero network).
bool weread_runtime_probe_shelf(const char *const *book_ids, size_t count);
/// 零 IO 读取当前整本缓存进度快照。/ Zero-IO snapshot of active shelf whole-book caching.
bool weread_runtime_shelf_cache_status(weread_shelf_cache_status_t *out);
/// 零 IO 读取最近一次缓存空间统计快照。/ Zero-IO snapshot of the latest cache storage statistics.
bool weread_runtime_cache_stats(weread_runtime_cache_stats_t *out);

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
/// 查询当前打开会话期间用户是否已主动翻页或跳转（不随 save() 清零）。
/// Return true if the user has turned/navigated since opening (persists across save()).
bool weread_runtime_reader_has_turned(const char *book_id);
/// 用户已翻页时仅将云端待跳进度对齐到本地基线并清除待跳标志，不强行跳页。
/// Rebase local progress baseline onto a pending cloud jump without moving the reader.
esp_err_t weread_runtime_cloud_jump_rebase(const char *book_id);
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
/// 浅睡唤醒后恢复阅读态位图/实时坐标并重新触发云端进度检查（零阻塞、零磁盘 IO）。
/// Restore reader cache bitmap/live position after light-sleep wake and re-arm cloud progress check (non-blocking, zero disk IO).
void weread_reader_wake(uint64_t now_ms);
void weread_reader_end(uint64_t now_ms);

/* ---- 开书/切章阶段与手动缓存 / Open stages and manual caching ---- */
/// 前台开书或冷切章阶段；worker 写入，页面只读。
/// Foreground open or cold-load stage; written by the worker, read by pages.
typedef enum {
    WEREAD_STAGE_NONE = 0,
    WEREAD_STAGE_PROGRESS, ///< 同步云端阅读进度 / Syncing cloud progress
    WEREAD_STAGE_TOC,      ///< 获取目录 / Fetching the table of contents
    WEREAD_STAGE_CHAPTER,  ///< 下载章节正文 / Downloading the chapter body
    WEREAD_STAGE_PARSE,    ///< 校验并解析章节 / Verifying and parsing the loaded chapter
    WEREAD_STAGE_RETRY,    ///< 瞬时超时后静默重试一次 / One silent retry after a transient timeout
} weread_stage_t;
/// 读取当前阶段与章节已下载字节；无前台阶段返回 false。
/// Read the current stage and downloaded chapter bytes; false when no foreground stage is active.
bool weread_reader_stage(weread_stage_t *stage, uint64_t *bytes);
/// 当前阶段的中文短提示；无阶段返回 false。文案为内部缓冲，调用方立即拷贝。
/// Short Chinese status label for the current stage; false when idle. Copy it immediately.
bool weread_reader_stage_text(char *out, size_t cap);
/// 目录页单章缓存查询：整书位图就绪时直接查位图，否则只做一次有界缓存检查，不扫描全书。
/// Per-chapter cache probe for the TOC: bitmap when scanned, otherwise one bounded cache check.
bool weread_reader_chapter_cached(const char *uri, size_t chapter);
/// 手动缓存后续章节开关；开启后阅读中持续向后补齐，关闭抑制所有整本向后补齐，但不取消在飞章节、自动下一章预读与进度上报。
/// Manual forward-cache toggle: keep filling later chapters while reading; off suppresses all whole-book forward fill without cancelling in-flight chapters, the urgent next-chapter pre-read or reports.
void weread_reader_cache_forward_set(bool on);
bool weread_reader_cache_forward(void);
/// 整本向后补齐当前是否启用（默认启用；暂停后为false）。工具栏开关应显示此状态。
/// Whether whole-book forward fill is enabled (on by default, false after a pause). The toolbar toggle mirrors this.
bool weread_reader_cache_fill_enabled(void);
/// 自动补齐是否因连续失败或空间不足停住（用户开关仍启用）。供工具栏把一次点击变成重新触发。
/// Whether automatic fill stalled on repeated failures or low space while the switch is still enabled.
bool weread_reader_cache_forward_stalled(void);
