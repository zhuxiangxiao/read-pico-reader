/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书虚拟书源接入现有 HTML/排版接口，不加入本地文件扫描。
 * English: Adapt virtual WeRead sources to HTML/layout without admitting them to local scans.
 */
#pragma once
#include "html_text.h"
#include "book_weread_uri.h"
#include <sys/stat.h>
bool book_weread_is_open(void);
esp_err_t book_weread_open(const char *uri);
void book_weread_close(void);
esp_err_t book_weread_prepare_open(const char *id);
bool book_weread_prepared(const char *id);
void book_weread_clear_prepared(void);
bool book_weread_cached_version(const char *id,uint32_t *version);
esp_err_t book_weread_validate_prepared(const char *id,uint32_t uid,uint32_t index,uint32_t offset,uint32_t *version);
size_t book_weread_count(void);
esp_err_t book_weread_title(size_t index, char *out, size_t cap);
esp_err_t book_weread_load(size_t index, const char *anchor, size_t *offset, html_text_t *out);
esp_err_t book_weread_image(size_t index, const char *src, uint8_t **data, size_t *len, bool *png);
uint32_t book_weread_total(void);
uint32_t book_weread_offset(size_t index);
uint32_t book_weread_uid(size_t index);
uint32_t book_weread_chapter_index(size_t index);
size_t book_weread_index(uint32_t uid);
/// 双向映射排版字节与源HTML UTF16坐标；失败时禁止上传猜测值。/ Map typeset bytes/source HTML UTF16 both ways; never upload guessed values after failure.
esp_err_t book_weread_map(size_t index, bool to_cloud, uint32_t input, uint32_t *output);
/// 虚拟身份仅用于恢复/进度，绝不代替本地扫描 stat。/ Virtual identity is for resume/progress, never local file enumeration.
bool book_weread_stat(const char *uri, struct stat *out);

/// 已缓存封面和元数据，仅本地读取，不启动网络。/ Cached art/metadata only; never start networking.
bool book_weread_metadata(const char *uri, char *title, size_t title_cap, char *author, size_t author_cap);
bool book_weread_cover(const char *uri, uint8_t *pixels, size_t cap);
static inline int book_target_stat(const char *path, struct stat *out) {
    return book_path_is_weread(path) ? (book_weread_stat(path,out) ? 0 : -1) : stat(path,out);
}
