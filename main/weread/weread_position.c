/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：UTF8 到 UTF16 单位计数，无效编码按替代字符处理。
 * English: Count UTF16 units from UTF8, treating invalid encodings as replacement characters.
 */
#include "weread_position.h"
uint32_t weread_utf16_length(const char *s, size_t len) {
    uint32_t units = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            do { ++units; ++i; } while (i < len && (unsigned char)s[i] < 0x80);
            continue;
        }
        size_t n = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
        bool valid = n <= len - i;
        for (size_t j = 1; j < n && valid; ++j) valid = ((unsigned char)s[i + j] & 0xc0) == 0x80;
        if (valid && n >= 3) {
            unsigned char b = (unsigned char)s[i + 1];
            if ((c == 0xe0 && b < 0xa0) || (c == 0xed && b >= 0xa0) ||
                (c == 0xf0 && b < 0x90) || (c == 0xf4 && b >= 0x90)) valid = false;
        }
        if (!valid) n = 1;
        units += (n == 4) ? 2u : 1u;
        i += n;
    }
    return units;
}
