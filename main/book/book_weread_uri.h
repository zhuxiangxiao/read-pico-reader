/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 *
 * 中文：微信读书虚拟 URI 判断与目标支持检测。
 * English: WeRead virtual URI check and supported-target detection.
 */
#pragma once
#include <stdbool.h>
#include <string.h>
#include <ctype.h>

#if __has_include("book/book_file_path.h")
#include "book/book_file_path.h"
#elif __has_include("book_file_path.h")
#include "book_file_path.h"
#else
#include "book/book_file_path.h"
#endif

static inline bool book_path_is_weread(const char *path) {
    if (!path || strncmp(path, "weread://", 9) != 0) return false;
    const char *id = path + 9;
    if (!*id || strlen(id) >= 64) return false;
    for (const char *p = id; *p; ++p) {
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_')) return false;
    }
    return true;
}

static inline bool book_path_is_supported_target(const char *path) {
    return book_file_path_supported(path) || book_path_is_weread(path);
}
