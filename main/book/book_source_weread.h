/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书虚拟书源接入现有 HTML/排版接口，不加入本地文件扫描。
 * English: Adapt virtual WeRead sources to HTML/layout without admitting them to local scans.
 */
#pragma once
#include "html_text.h"
#include "book_weread_uri.h"
#include "weread_toc.h"
#include <sys/stat.h>
bool book_weread_is_open(void);
/// 已打开书源实际采用的解析类型；调用方在成功排版后冻结。/ Capture the open source's actual parser type after successful layout.
bool book_weread_plain_text(void);
esp_err_t book_weread_open(const char *uri);
void book_weread_close(void);
esp_err_t book_weread_prepare_open(const char *id);
/// 接管已校验的目录，不复制章节数组。/ Take ownership of a validated TOC without copying its chapter array.
void book_weread_adopt_prepared(const char *id,weread_toc_t *toc);
/// 接管本地已缓存目录用于快速开书，允许暂低于书架最新元数据版本。/ Adopt a locally cached TOC for instant offline-first open even if shelf metadata is newer.
void book_weread_adopt_prepared_local(const char *id,weread_toc_t *toc);
/// 将已同步或准备好的最新目录替换到当前已打开书籍。/ Swap an updated or prepared TOC into the currently open book.
esp_err_t book_weread_reload_toc(const char *id);
bool book_weread_prepared(const char *id);
void book_weread_clear_prepared(void);
bool book_weread_cached_version(const char *id,uint32_t *version);
/// 仅返回已提交正文的虚拟文件身份；不随后台书架版本改变，不访问TF。/ Return only the committed body's virtual file identity, independently of newer shelf metadata and without TF I/O.
bool book_weread_content_identity(const char *uri,uint32_t *identity);
esp_err_t book_weread_validate_prepared(const char *id,uint32_t uid,uint32_t index,uint32_t offset,uint32_t *version);
size_t book_weread_count(void);
esp_err_t book_weread_title(size_t index, char *out, size_t cap);
esp_err_t book_weread_load(size_t index, const char *anchor, size_t *offset, html_text_t *out);
/// 分页成功才接管已载入的实际UID/版本；失败保留旧正文绑定并丢弃候选。/ Commit actual UID/version only after layout succeeds; failed layout discards the candidate and retains the old body.
esp_err_t book_weread_commit_loaded(size_t *chapter,uint32_t *version);
void book_weread_discard_loaded(void);
bool book_weread_directory_in_use(const char *root);
bool book_weread_id_in_use(const char *id);
/// 当前与候选的精确映射表常驻字节，不包含正文/目录。/ Resident exact-map bytes across the active and candidate slots, excluding body/TOC storage.
size_t book_weread_map_cache_bytes(void);
esp_err_t book_weread_image(size_t index, const char *src, uint8_t **data, size_t *len, bool *png);
uint32_t book_weread_total(void);
uint32_t book_weread_offset(size_t index);
uint32_t book_weread_uid(size_t index);
uint32_t book_weread_chapter_index(size_t index);
size_t book_weread_index(uint32_t uid);
/// 双向映射排版字节与源HTML UTF16坐标；失败时禁止上传猜测值。/ Map typeset bytes/source HTML UTF16 both ways; never upload guessed values after failure.
esp_err_t book_weread_map(size_t index, bool to_cloud, uint32_t input, uint32_t *output);
esp_err_t book_weread_map_candidate(size_t index, bool to_cloud, uint32_t input, uint32_t *output);
esp_err_t book_weread_map_version(size_t index,uint32_t version,bool to_cloud,uint32_t input,uint32_t *output);
/// 只查已提交正文的精确RAM映射；缺表返回NOT_FOUND，绝不读卡兜底。/ Query only the committed body's exact RAM map; a missing table returns NOT_FOUND without a filesystem fallback.
esp_err_t book_weread_map_cached(size_t index,uint32_t version,bool to_cloud,uint32_t input,uint32_t *output);
/// 仅供已冻结实际UID/版本/解析类型、并先验账号与原卡CID的reader恢复；不接管书源或采用新目录/书架元数据，原序号仍由caller保留。/ Recovery for a caller that captured the rendered UID/edition/parser and checked its owner/original CID; never adopt a source or newer TOC/shelf metadata, and retain the caller's captured ordinal.
esp_err_t book_weread_map_detached(const char *id,uint32_t uid,uint32_t version,bool plain_text,uint32_t typed_offset,uint32_t *raw_offset);
/// 虚拟身份仅用于恢复/进度，绝不代替本地扫描 stat。/ Virtual identity is for resume/progress, never local file enumeration.
bool book_weread_stat(const char *uri, struct stat *out);

/// 已缓存封面和元数据，仅本地读取，不启动网络。/ Cached art/metadata only; never start networking.
bool book_weread_metadata(const char *uri, char *title, size_t title_cap, char *author, size_t author_cap);
bool book_weread_cover(const char *uri, uint8_t *pixels, size_t cap);
static inline int book_target_stat(const char *path, struct stat *out) {
    return book_path_is_weread(path) ? (book_weread_stat(path,out) ? 0 : -1) : stat(path,out);
}
