/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书增量书架、离线元数据与封面重试策略。
 * English: Incremental WeRead shelf, offline metadata and cover retry policy.
 */
#pragma once
#include "weread_types.h"

typedef struct {
    char book_id[64], title[256], author[128], cover[512], format[16];
    uint32_t book_version, total_words, progress;
    uint64_t read_update_time, update_time;
} weread_book_t;
/// 界面快照只保留展示字段和封面身份，URL留在worker元数据中。/ UI snapshots retain display fields and cover identity; URLs stay in worker metadata.
typedef struct {
    char book_id[64],title[256],author[128];
    uint64_t cover_key;
    uint32_t progress,chapter_index;
    uint64_t read_update_time,update_time;
    bool has_cover;
} weread_shelf_item_t;

/// 同步成功并落盘后才替换内存；失败保留离线书架。/ Publish only after persistence; retain offline data on errors.
esp_err_t weread_shelf_sync(bool full);
esp_err_t weread_shelf_refresh_book(const char *id);
/// 首次六本上屏，后续每次最多二十本；未完成时不前移synckey。/ Publish the first six, then at most twenty per step; retain synckey until completion.
esp_err_t weread_shelf_sync_step(bool restart);
void weread_shelf_sync_discard(void);
bool weread_shelf_sync_pending(void);
void weread_shelf_sync_progress(size_t *done,size_t *total,size_t *skipped);
esp_err_t weread_shelf_save(void);
esp_err_t weread_shelf_load(void);
void weread_shelf_clear(void);
size_t weread_shelf_count(void);
bool weread_shelf_get(size_t index, weread_book_t *out);
/// 单遍导出当前快照，输出数组归调用者。/ Export an owned snapshot in one list traversal.
esp_err_t weread_shelf_snapshot(weread_shelf_item_t **out,size_t *count);
bool weread_shelf_find(const char *book_id, weread_book_t *out);
uint32_t weread_shelf_book_version(const char *id);
esp_err_t weread_shelf_set_version(const char *id,uint32_t version);
uint64_t weread_shelf_synckey(void);
bool weread_shelf_cache_fresh(uint64_t now_seconds);
uint64_t weread_cover_key(const char *book_id,const char *raw_url);
esp_err_t weread_cover_cache_path(const char *book_id,const char *kind,char *path,size_t cap);
bool weread_cover_is_original(const char *book_id,const char *url);

/// 封面 URL 升级为 HTTPS 与 t6；不修改查询串。/ Upgrade to HTTPS/t6 without editing the query.
esp_err_t weread_cover_get_url(const char *raw_url, char *out_url, size_t cap);
typedef struct {
    unsigned failures;
    uint64_t not_before_ms;
    bool original;
} weread_cover_retry_t;
/// 非阻塞退避：到期前返回 TIMEOUT。/ Nonblocking backoff returns TIMEOUT before the deadline.
esp_err_t weread_cover_retry_url(const char *raw, const weread_cover_retry_t *retry,
                               uint64_t now_ms, char *out, size_t cap);
void weread_cover_retry_result(weread_cover_retry_t *retry, int http_status, uint64_t now_ms);
