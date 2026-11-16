/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书目录增量同步与离线快照。
 * English: Incremental WeRead chapter index and offline snapshots.
 */
#pragma once
#include "weread_types.h"

typedef struct {
    uint32_t uid, index, level, word_count;
    uint64_t update_time;
    char title[256];
} weread_chapter_info_t;
typedef struct {
    uint64_t synckey, update_time;
    uint32_t book_version;
    size_t count;
    weread_chapter_info_t *chapters;
} weread_toc_t;

/// 同步到磁盘；clearAll 先丢弃旧项，失败保留旧快照。/ Persist a delta; clearAll replaces old entries, failures retain the old snapshot.
esp_err_t weread_toc_sync(const char *book_id);
/// 输出须为零初始化或已释放的对象。/ Output must be zero initialized or previously freed.
esp_err_t weread_toc_load(const char *book_id, weread_toc_t *out);
void weread_toc_free(weread_toc_t *toc);
