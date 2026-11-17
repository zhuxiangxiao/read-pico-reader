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
    char summary[256];
} weread_progress_t;

esp_err_t weread_progress_load(const char *id, weread_progress_t *out);
/// 返回云端候选但不替用户覆盖冲突位置；网络失败仍返回本地快照。/ Return a cloud candidate without overwriting conflicts; keep a local snapshot on network errors.
esp_err_t weread_progress_sync(const char *id, weread_progress_t *local, weread_progress_t *cloud, bool *conflict);
/// 显式选择云端或本地后保存；本地选择继续标脏。/ Persist an explicit cloud/local selection; local choices stay dirty.
esp_err_t weread_progress_select(const char *id, const weread_progress_t *choice, bool from_cloud);
esp_err_t weread_progress_checkpoint(const char *id,const weread_progress_t *progress);
esp_err_t weread_progress_note_time(const char *id,uint32_t seconds,uint64_t timestamp);
esp_err_t weread_progress_note(const char *id, uint32_t uid, uint32_t index, uint32_t offset,
    uint32_t percent, uint32_t seconds, unsigned pages, const char *summary, uint64_t timestamp);
/// 非强制上报等候 60 秒或十页；失败保持磁盘 dirty。/ Nonforced reports wait 60 seconds or ten pages; errors retain on-disk dirty state.
esp_err_t weread_progress_report(const char *id, bool force, uint64_t now_ms);
