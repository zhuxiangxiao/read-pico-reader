/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：流式章节缓存，校验成功后发布，失败和取消不暴露半成品。
 * English: Stream chapter caches and publish after validation, never exposing partial/cancelled data.
 */
#pragma once
#include "weread_client.h"
#define WEREAD_ERR_PURCHASE 0x7901
#define WEREAD_ERR_SOLD_OUT 0x7902
#define WEREAD_ERR_VERSION 0x7903
#define WEREAD_ERR_POSITION 0x7904
#define WEREAD_ERR_STORAGE 0x7906

void weread_chapter_set_cancel(weread_cancel_fn fn, void *ctx);
esp_err_t weread_chapter_download(const char *book_id, uint32_t uid);
/// 调度用的账号/版本/正文/映射头一致性检查；实际打开仍深校验映射。/ Scheduling check for owner/version/body/map-header consistency; opening still validates every map point.
bool weread_chapter_cached(const char *book_id,uint32_t uid);
/// 单遍扫描目录内已缓存章节，填充调用方 UID 位图并统计数量。/ Scan cached chapters in one directory pass and populate caller UID bitset.
esp_err_t weread_chapter_scan_cached_uids(const char *book_id, const uint32_t *uids, size_t count,
    uint8_t *bits, uint32_t *cached_count, weread_cancel_fn cancel, void *cancel_ctx);
/// 自动补章预留最坏归档/展开/成品空间；已缓存章不受此门限影响。/ Reserve worst-case archive/extraction/body space for automatic prefetch; cached chapters remain readable.
esp_err_t weread_chapter_prefetch_space(uint64_t *available);

typedef struct {
    uint64_t chapter_bytes;
    uint64_t cover_bytes;
    uint64_t meta_bytes;
    uint64_t reclaimable_bytes;
    uint64_t total_bytes;
    uint32_t chapter_manifest_count;
} weread_chapter_book_cache_info_t;

typedef struct {
    uint64_t total_bytes;
    uint64_t reclaimable_bytes;
    uint64_t free_bytes;
    uint64_t fs_total_bytes;
    uint32_t book_dir_count;
    uint32_t books_with_chapters;
} weread_chapter_cache_stats_t;

/// 查询微信读书存储挂载点的可用空间与总容量；不强制 104 MiB 预取门限。
/// Query free and total filesystem bytes for the WeRead storage root without enforcing the 104 MiB prefetch floor.
esp_err_t weread_chapter_storage_info(uint64_t *free_bytes, uint64_t *total_bytes);

/// 统计单本书缓存占用，区分可回收章节/临时文件与保留的元数据/成品封面。
/// Inspect disk usage for one book's cache directory, separating reclaimable chapter/temp artifacts from preserved metadata and .gray covers.
esp_err_t weread_chapter_inspect_book_cache(const char *book_id, weread_chapter_book_cache_info_t *out,
    weread_cancel_fn cancel, void *cancel_ctx);

/// 统计全部缓存目录占用与文件系统可用空间。
/// Inspect aggregate cache usage and storage space across cache/<id>/ directories.
esp_err_t weread_chapter_inspect_cache_stats(weread_chapter_cache_stats_t *out,
    weread_cancel_fn cancel, void *cancel_ctx);

/// 选择性清理单本书离线章节与临时封面，严格保留 progress.json*、chapters.json* 与 cover-*.gray/cover.gray。
/// Selectively remove one book's offline chapters and temp cover artifacts while preserving progress, TOC, and .gray covers.
esp_err_t weread_chapter_clean_book_cache(const char *book_id, uint64_t *freed_bytes,
    weread_cancel_fn cancel, void *cancel_ctx);

/// 选择性清理全部书籍离线章节与临时封面，跳过正在阅读的书籍，保留 auth/shelf/progress/chapters/成品封面。
/// Selectively remove offline chapters and temp cover artifacts across all cached books (skipping any book currently open), preserving auth, shelf, progress, TOC, and .gray covers.
esp_err_t weread_chapter_clean_all_caches(uint64_t *freed_bytes, uint32_t *cleaned_books,
    weread_cancel_fn cancel, void *cancel_ctx);

/// 依据本地目录与已校验正文/映射，统计单本书的已缓存章节数与总章节数。
/// Count locally cached chapters (verified via weread_chapter_cached) against the book's local TOC.
esp_err_t weread_chapter_count_cached(const char *book_id, uint16_t *cached_chapters,
    uint16_t *total_chapters, weread_cancel_fn cancel, void *cancel_ctx);
esp_err_t weread_chapter_download_ex(const char *book_id, uint32_t uid, bool preload);
/// 已缓存时不联网；输出正文由调用方 free。/ Cached reads stay offline; caller frees returned text.
esp_err_t weread_chapter_load(const char *book_id, uint32_t uid, char **text, size_t *len);
esp_err_t weread_chapter_load_cached(const char *book_id,uint32_t uid,char **text,size_t *len);
/// 仅在同一WR_GUARD内读取已验证的缓存路径，保留正文大小和完整读取检查。/ Read a cache path validated in the same WR_GUARD, retaining body-size and complete-read checks.
esp_err_t weread_chapter_read_validated(const char *path,char **text,size_t *len);
/// 仅返回受账号/版本校验的目录；调用方可读取其中的 CSS/图片。/ Return an account/version-validated directory for CSS/image access.
esp_err_t weread_chapter_directory(const char *book_id, uint32_t uid, char *out, size_t cap);
/// 同事务读取实际正文/公共资源代次及manifest版本，供阅读器冻结内容身份。/ Snapshot actual body/resource generations and manifest version in one transaction.
esp_err_t weread_chapter_snapshot(const char *book_id,uint32_t uid,char *root,size_t root_cap,char *resources,size_t resources_cap,uint32_t *version);
esp_err_t weread_chapter_resource_directory(const char *book_id, char *out, size_t cap);
/// 相对资源路径规范化，禁止越出归档根目录或联网。/ Resolve relative resources without leaving the archive root or networking.
bool weread_resource_path(const char *base, const char *relative, char *out, size_t cap);
/// 对已规范化的相对路径做百分号解码（不允许路径逃逸/协议头/反斜杠）。/ Percent-decode an already-normalized path without traversal, schemes, or backslashes.
bool weread_resource_path_decode(const char *normalized, char *out, size_t cap);
/// 把不兼容 FAT32 的归档相对路径映射为确定性的槽位安全路径（保留 .css 后缀）。/ Map FAT32-unsafe archive paths to deterministic storage-safe names while retaining .css.
const char *weread_chapter_storage_path(const char *normalized, char mapped[48]);
