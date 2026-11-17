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

void weread_chapter_set_cancel(weread_cancel_fn fn, void *ctx);
esp_err_t weread_chapter_download(const char *book_id, uint32_t uid);
bool weread_chapter_cached(const char *book_id,uint32_t uid);
esp_err_t weread_chapter_download_ex(const char *book_id, uint32_t uid, bool preload);
/// 已缓存时不联网；输出正文由调用方 free。/ Cached reads stay offline; caller frees returned text.
esp_err_t weread_chapter_load(const char *book_id, uint32_t uid, char **text, size_t *len);
esp_err_t weread_chapter_load_cached(const char *book_id,uint32_t uid,char **text,size_t *len);
/// 仅在同一WR_GUARD内读取已验证的缓存路径，保留正文大小和完整读取检查。/ Read a cache path validated in the same WR_GUARD, retaining body-size and complete-read checks.
esp_err_t weread_chapter_read_validated(const char *path,char **text,size_t *len);
/// 仅返回受账号/版本校验的目录；调用方可读取其中的 CSS/图片。/ Return an account/version-validated directory for CSS/image access.
esp_err_t weread_chapter_directory(const char *book_id, uint32_t uid, char *out, size_t cap);
esp_err_t weread_chapter_resource_directory(const char *book_id, char *out, size_t cap);
/// 相对资源路径规范化，禁止越出归档根目录或联网。/ Resolve relative resources without leaving the archive root or networking.
bool weread_resource_path(const char *base, const char *relative, char *out, size_t cap);
