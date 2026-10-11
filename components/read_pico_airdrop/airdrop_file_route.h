/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：隔空投送接收文件的纯函数分类、macOS 元数据过滤与重名加后缀规则。
 *
 * English: Pure helpers for AirDrop file extension routing, macOS sidecar filtering
 * and duplicate filename suffixing (_1.._99).
 *
 * 冻结：重名文件不得覆盖已有文件；苹果系统附属文件必须静默跳过。
 * Frozen: Never overwrite existing files on collision; silently skip macOS metadata sidecars.
 */

#pragma once

#include <ctype.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 文件归类目标类型。/ Target category for received files.
typedef enum {
    AIRDROP_ROUTE_SKIP = 0,  ///< 忽略附属文件 / Skip metadata sidecar
    AIRDROP_ROUTE_INVALID,   ///< 非法文件名 / Invalid filename
    AIRDROP_ROUTE_BOOK,      ///< 书籍 (.epub/.txt) / Book (.epub/.txt)
    AIRDROP_ROUTE_FONT,      ///< 字体 (.ttf/.otf) / Font (.ttf/.otf)
    AIRDROP_ROUTE_PICTURE,   ///< 图片 (.jpg/.jpeg/.png) / Picture (.jpg/.jpeg/.png)
    AIRDROP_ROUTE_OTHER,     ///< 其他文件 / Other file
} airdrop_route_kind_t;

/// 提取安全的文件基础名。/ Extract a safe basename from a potentially nested archive path.
static inline const char *airdrop_extract_basename(const char *name) {
    if (!name || !*name) {
        return NULL;
    }
    const char *base = name;
    for (const char *p = name; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    if (!*base || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        return NULL;
    }
    for (const unsigned char *u = (const unsigned char *)base; *u; ++u) {
        if (*u < 0x20 || *u == 0x7f) {
            return NULL;
        }
    }
    return base;
}

/// 不区分大小写比较后缀。/ Case-insensitive string comparison for ASCII extensions.
static inline bool airdrop_ext_equals(const char *ext, const char *target) {
    if (!ext || !target) {
        return false;
    }
    while (*ext && *target) {
        if (tolower((unsigned char)*ext) != tolower((unsigned char)*target)) {
            return false;
        }
        ++ext;
        ++target;
    }
    return *ext == '\0' && *target == '\0';
}

/// 根据文件名判断归类目标。/ Classify a filename into its destination category.
static inline airdrop_route_kind_t airdrop_classify_filename(const char *name, const char **out_base) {
    const char *base = airdrop_extract_basename(name);
    if (out_base) {
        *out_base = base;
    }
    if (!base) {
        return AIRDROP_ROUTE_INVALID;
    }
    if (strncmp(base, "._", 2) == 0 || strcmp(base, ".DS_Store") == 0) {
        return AIRDROP_ROUTE_SKIP;
    }
    const char *dot = strrchr(base, '.');
    if (!dot || dot == base || dot[1] == '\0') {
        return AIRDROP_ROUTE_OTHER;
    }
    if (airdrop_ext_equals(dot, ".epub") || airdrop_ext_equals(dot, ".txt")) {
        return AIRDROP_ROUTE_BOOK;
    }
    if (airdrop_ext_equals(dot, ".ttf") || airdrop_ext_equals(dot, ".otf")) {
        return AIRDROP_ROUTE_FONT;
    }
    if (airdrop_ext_equals(dot, ".jpg") || airdrop_ext_equals(dot, ".jpeg") || airdrop_ext_equals(dot, ".png")) {
        return AIRDROP_ROUTE_PICTURE;
    }
    return AIRDROP_ROUTE_OTHER;
}

/// 生成不覆盖已有文件的保存路径（冲突时追加 _1.._99）。
/// Build a non-colliding path in dir, appending _1.._99 before the extension when needed.
static inline bool airdrop_resolve_unique_path(
    const char *dir, const char *base, bool (*exists_fn)(const char *path, void *ctx), void *ctx,
    char *out_path, size_t out_path_cap, char *out_name, size_t out_name_cap
) {
    if (!dir || !*dir || !base || !*base || !exists_fn || !out_path || out_path_cap == 0) {
        return false;
    }
    int n = snprintf(out_path, out_path_cap, "%s/%s", dir, base);
    if (n < 0 || (size_t)n >= out_path_cap) {
        return false;
    }
    if (!exists_fn(out_path, ctx)) {
        if (out_name && out_name_cap > 0) {
            snprintf(out_name, out_name_cap, "%s", base);
        }
        return true;
    }

    const char *dot = strrchr(base, '.');
    size_t stem_len = (dot && dot != base) ? (size_t)(dot - base) : strlen(base);
    const char *ext = (dot && dot != base) ? dot : "";

    for (int idx = 1; idx <= 99; ++idx) {
        char candidate_name[256];
        int cn = snprintf(candidate_name, sizeof(candidate_name), "%.*s_%d%s", (int)stem_len, base, idx, ext);
        if (cn < 0 || (size_t)cn >= sizeof(candidate_name)) {
            return false;
        }
        n = snprintf(out_path, out_path_cap, "%s/%s", dir, candidate_name);
        if (n < 0 || (size_t)n >= out_path_cap) {
            return false;
        }
        if (!exists_fn(out_path, ctx)) {
            if (out_name && out_name_cap > 0) {
                snprintf(out_name, out_name_cap, "%s", candidate_name);
            }
            return true;
        }
    }
    return false;
}

#ifdef __cplusplus
}
#endif
