/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 *
 * 中文：微信读书传输加解密、归档解包（ZipCrypto ZIP / USTAR TAR）与 Web 进度签名。
 * English: WeRead transport decryption, archive unpacking (ZipCrypto ZIP / USTAR TAR),
 *          and Web reading-progress signing.
 */
#pragma once
#include <stdio.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifndef ESP_ERR_INVALID_STATE
#define ESP_ERR_INVALID_STATE 0x103
#endif
#ifndef ESP_ERR_INVALID_RESPONSE
#define ESP_ERR_INVALID_RESPONSE 0x108
#endif
#ifndef ESP_ERR_INVALID_CRC
#define ESP_ERR_INVALID_CRC 0x109
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Base64 解码（兼容标准与 URL 安全字符集，忽略空白）。
/// Decode base64 (standard or URL-safe alphabet, ignoring ASCII whitespace).
bool book_weread_base64_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len);

/// 使用单个密钥（vid 或 wr_skey）通过 AES-128-CBC 解密 HTTP `encryptkey`。
/// Decrypt HTTP `encryptkey` (hex or base64) with one secret via AES-128-CBC + PKCS#7 unpadding.
bool book_weread_decrypt_encrypt_key(const char *encrypt_key, const char *secret,
                                     uint8_t *out, size_t out_cap, size_t *out_len);

/// 依次尝试候选密钥解密 `encryptkey`，直到 PKCS#7 校验通过。
/// Try candidate secrets in order until PKCS#7 unpadding succeeds.
bool book_weread_decrypt_encrypt_key_candidates(const char *encrypt_key,
                                                const char *const *secrets, size_t secret_count,
                                                uint8_t *out, size_t out_cap, size_t *out_len);

/// 原地执行 GilbertVernam 流解密。/ Apply GilbertVernam XOR stream decryption in place.
void book_weread_gilbert_vernam(uint8_t *data, size_t len,
                                const uint8_t *key, size_t key_len, size_t start);

/// 若为 QDataStream 信封格式则解包；返回 true 表示匹配且 *out 由调用方 free。
/// Unwrap a QDataStream envelope if present; true means matched and caller owns *out.
bool book_weread_unwrap_envelope(const uint8_t *blob, size_t len,
                                 const char *secret,
                                 const uint8_t *fallback_key, size_t fallback_len,
                                 uint8_t **out, size_t *out_len);

/// 归档条目回调：name 为规范化相对路径，data 仅在回调期间有效；返回 false 中止解包。
/// Archive entry callback: normalized relative path and borrowed bytes; false aborts unpacking.
typedef bool (*book_weread_archive_entry_fn)(void *ctx, const char *name,
                                             const uint8_t *data, size_t size);

/// 解包 ZIP 归档（支持传统 PKWARE ZipCrypto 加密、Deflate/Stored 与 CRC32 校验）。
/// Unpack a ZIP archive (supports PKWARE ZipCrypto, Deflate/Stored, and CRC32 verification).
esp_err_t book_weread_unpack_zip(const uint8_t *payload, size_t len,
                                 const uint8_t *password, size_t password_len,
                                 book_weread_archive_entry_fn on_entry, void *ctx);

/// 解包 USTAR TAR 归档。/ Unpack a USTAR TAR archive.
esp_err_t book_weread_unpack_tar(const uint8_t *payload, size_t len,
                                 book_weread_archive_entry_fn on_entry, void *ctx);

/// 文件归档按 4 KiB 解密/解压；回调只能写暂存区，整体成功后才能发布。
/// Stream file archives in 4 KiB chunks; callbacks write staging only, published after complete success.
typedef bool (*book_weread_archive_chunk_fn)(void *ctx, const char *name,
    const uint8_t *bytes, size_t len, size_t offset, size_t total);
esp_err_t book_weread_unpack_zip_file(FILE *archive, const uint8_t *password, size_t password_len,
                                     book_weread_archive_chunk_fn sink, void *ctx);
esp_err_t book_weread_unpack_tar_file(FILE *archive, book_weread_archive_chunk_fn sink, void *ctx);

/// 对 bookId / chapterUid / timestamp 执行微信读书 Web 混淆编码。
/// Encode a bookId, chapterUid, or timestamp using WeRead web obfuscation.
bool book_weread_encode_web_id(const char *value, char *out, size_t cap);

/// 由 User-Agent 生成 Web 端 `appId` 指纹。
/// Compute the WeRead web `appId` fingerprint from the User-Agent string.
bool book_weread_generate_web_app_id(const char *user_agent, char *out, size_t cap);

/// 生成 `POST https://weread.qq.com/web/book/read` 的完整已签名 JSON 请求体。
/// Build the signed JSON payload (including `s`) for POST https://weread.qq.com/web/book/read.
bool book_weread_build_web_read_body(const char *book_id, uint32_t chapter_uid,
                                     uint32_t chapter_idx, uint32_t chapter_offset,
                                     uint32_t progress, const char *summary,
                                     int64_t timestamp, const char *user_agent,
                                     char *out, size_t out_cap);

#ifdef __cplusplus
}
#endif
