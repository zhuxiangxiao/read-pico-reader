/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书进度持久化、冲突候选与节流上报。
 * English: Persistent WeRead progress, conflict candidates and throttled reporting.
 */
#pragma once
#include "weread_types.h"
typedef struct {
    uint32_t chapter_uid, chapter_index, chapter_offset, progress, reading_time, book_version;
    uint64_t updated_at, last_report_ms;
    unsigned pending_pages;
    bool dirty, position_pending;
    /// 云端基线及事务代次不属于协议上报字段。/ Cloud baseline and transaction revision are local-only.
    uint32_t revision, base_uid, base_index, base_offset, base_version;
    uint64_t retry_after;
    unsigned retry_count;
    bool base_known, sync_conflict;
    char summary[256];
} weread_progress_t;

esp_err_t weread_progress_load(const char *id, weread_progress_t *out);
/// 返回云端候选但不替用户覆盖冲突位置；网络失败仍返回本地快照。/ Return a cloud candidate without overwriting conflicts; keep a local snapshot on network errors.
esp_err_t weread_progress_sync(const char *id, weread_progress_t *local, weread_progress_t *cloud, bool *conflict);
esp_err_t weread_progress_sync_version(const char *id,weread_progress_t *local,weread_progress_t *cloud,bool *conflict,uint32_t *fresh_version,uint32_t *last_chapter_index);
/// 显式选择云端或本地后保存；本地选择继续标脏。/ Persist an explicit cloud/local selection; local choices stay dirty.
esp_err_t weread_progress_select(const char *id, const weread_progress_t *choice, bool from_cloud);
esp_err_t weread_progress_checkpoint(const char *id,const weread_progress_t *progress);
esp_err_t weread_progress_note_time(const char *id,uint32_t seconds,uint64_t timestamp);
/// 合并采样，先持久化再报告，并返回最终已持久化状态。/ Merge a sample, persist before reporting, and return the final durable state.
esp_err_t weread_progress_update(const char *id,weread_progress_t *sample,bool moved,bool force,uint64_t now_ms,uint64_t timestamp,bool *noted);
esp_err_t weread_progress_note(const char *id, uint32_t uid, uint32_t index, uint32_t offset,
    uint32_t percent, uint32_t seconds, unsigned pages, const char *summary, uint64_t timestamp);
/// 非强制上报等候 60 秒或十页；失败保持磁盘 dirty。/ Nonforced reports wait 60 seconds or ten pages; errors retain on-disk dirty state.
esp_err_t weread_progress_report(const char *id, bool force, uint64_t now_ms);

#define WEREAD_ERR_PROGRESS_CONFLICT 0x7905
typedef enum {
    WR_PROGRESS_LOCAL, WR_PROGRESS_PENDING, WR_PROGRESS_SYNCED, WR_PROGRESS_CONFLICT
} weread_progress_state_t;
/// 只保存本机采样，不启动网络。/ Persist a local sample without starting network work.
esp_err_t weread_progress_store(const char *id,weread_progress_t *sample,bool moved,uint64_t timestamp,bool *noted);
/// 仅由已校验正文/映射的调用方传入实际版本（含未知0），不采用后台更新的书架版本。/ Validated content/map callers bind the actual edition, including unknown0, rather than later shelf metadata.
esp_err_t weread_progress_store_version(const char *id,weread_progress_t *sample,bool moved,uint32_t content_version,uint64_t timestamp,bool *noted);
#define WEREAD_MEDIA_ID_BYTES 28
bool weread_progress_capture_media(uint8_t out[WEREAD_MEDIA_ID_BYTES]);
/// TF失败时才将已验证采样存入两槽应急账本；before/CID由同一reader捕获，成功仍仅代表本地待同步。/ Only TF failures use the two-slot rescue journal; the caller supplies its last durable snapshot and captured card identity.
esp_err_t weread_progress_store_version_resilient(const char *id,weread_progress_t *sample,bool moved,uint32_t content_version,uint64_t timestamp,bool *noted,const weread_progress_t *durable_before,const uint8_t captured_media_id[WEREAD_MEDIA_ID_BYTES],bool *rescued);
#if !defined(ESP_PLATFORM)
/// 独立进程故障测试替换持久介质；生产使用私有NVS命名空间。/ Fault tests replace durable storage; production uses its private NVS namespace.
typedef struct {
    esp_err_t (*read)(unsigned slot,void *data,size_t bytes,void *ctx);
    esp_err_t (*write)(unsigned slot,const void *data,size_t bytes,void *ctx);
    esp_err_t (*erase)(unsigned slot,void *ctx);
    bool (*media)(uint8_t out[WEREAD_MEDIA_ID_BYTES],void *ctx);
    void *ctx;
} weread_progress_rescue_backend_t;
void weread_progress_rescue_backend(const weread_progress_rescue_backend_t *backend);
#endif
/// 单worker有界扫描持久待报记录；非本账号、冲突和退避中的记录不会被选中。/ Bounded worker-only scan, skipping foreign owners, conflicts and backoff.
esp_err_t weread_progress_outbox_next(char *id,size_t cap,uint64_t now_seconds);
/// 核对新鲜云端基线再补报；ACK按事务代次合并，不能清除新导航。/ Recheck the cloud baseline, then merge acknowledgements without clearing newer navigation.
esp_err_t weread_progress_outbox_report(const char *id,uint64_t now_ms,uint64_t now_seconds,weread_progress_t *out);
/// 账号或媒体生命周期交接后重置扫描器。/ Reset the scanner after account or media handoff.
void weread_progress_outbox_reset(void);
/// 只读取已接管快照，不在绘制中访问磁盘。/ Inspect an adopted snapshot without disk access while rendering.
weread_progress_state_t weread_progress_state(const weread_progress_t *progress);
