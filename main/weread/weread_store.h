/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书缓存的有界 JSON、路径与原子提交工具。
 * English: Bounded JSON, paths and atomic commits for WeRead caches.
 */
#pragma once
#include "weread_types.h"
#include "cJSON.h"

#define WEREAD_BOOK_ID_CAP 64
#define WEREAD_JSON_LIMIT (2u * 1024u * 1024u)
#define WEREAD_SHELF_LIMIT 1024
#define WEREAD_TOC_LIMIT 20000

/// 跨前台/后台的可重入服务锁；作用域退出自动归还。/ Reentrant foreground/background service lock, released on scope exit.
void wr_lock(void);
void wr_unlock(int *guard);
void wr_memory_enter(void);
void wr_memory_leave(int *guard);
#define WR_GUARD __attribute__((cleanup(wr_unlock))) int wr_scope_guard = (wr_lock(), 0)
#define WR_MEMORY_SCOPE __attribute__((cleanup(wr_memory_leave))) int wr_memory_guard = (wr_memory_enter(), 0)

/// 可回收工作缓冲优先使用PSRAM，避免扩大内部任务栈。/ Reclaimable work buffers use PSRAM instead of growing internal task stacks.
void *wr_alloc(size_t bytes);
void *wr_realloc(void *pointer, size_t bytes);
/// 开机单次注册稳定的JSON分配器，仅WR_GUARD作用域内使用PSRAM。/ Register once at boot; only WR_GUARD scopes route JSON allocations to PSRAM.
void wr_memory_init(void);
bool wr_memory_failed(void);
cJSON *wr_parse_json(const char *text,size_t bytes);
void wr_release(void *pointer_slot);
bool wr_valid_id(const char *id);
const char *wr_string(const cJSON *object, const char *key);
bool wr_uint(const cJSON *object, const char *key, uint64_t *out);
bool wr_copy(char *out, size_t cap, const char *text);
void wr_display_copy(char *out, size_t cap, const char *text);
esp_err_t wr_mkdirs(const char *path);
esp_err_t wr_cache_path(const char *id, const char *leaf, char *out, size_t cap);
esp_err_t wr_read_json(const char *path, cJSON **out);
/// 仅读取指定主文件，让按账号恢复的调用方自行判定副本归属。/ Read only the named primary so account-aware recovery can validate backup ownership.
esp_err_t wr_read_json_primary(const char *path, cJSON **out);
esp_err_t wr_write_json(const char *path, const cJSON *json);
esp_err_t wr_write_bytes(const char *path, const void *data, size_t len);
esp_err_t wr_request_json(const char *endpoint, const cJSON *body, cJSON **out);
bool wr_owner_matches(const cJSON *json);
bool wr_set_owner(cJSON *json);
bool wr_set_copy(cJSON *object, const char *key, const cJSON *value);
