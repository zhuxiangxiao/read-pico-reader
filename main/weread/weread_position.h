/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：保留章节源 HTML 的 UTF16 坐标，避免用排版 UTF8 字节上传进度。
 * English: Preserve source HTML UTF16 coordinates instead of uploading typeset UTF8 bytes.
 */
#pragma once
#include "weread_types.h"
#define WEREAD_POSITION_MAGIC UINT32_C(0x57525031)
typedef struct { uint32_t magic, count, bytes, units; } weread_position_header_t;
typedef struct { uint32_t byte, unit; } weread_position_point_t;
uint32_t weread_utf16_length(const char *text, size_t len);
