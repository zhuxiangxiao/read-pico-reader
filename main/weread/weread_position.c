/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：UTF8 到 UTF16 单位计数，无效编码按替代字符处理。
 * English: Count UTF16 units from UTF8, treating invalid encodings as replacement characters.
 */
#include "weread_position.h"
uint32_t weread_utf16_length(const char *s, size_t len) {
    uint32_t units = 0;
    for (size_t i = 0; i < len; ++units) {
        unsigned char c = (unsigned char)s[i];
        size_t n = c < 0x80 ? 1 : c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 : c >= 0xf0 && c <= 0xf4 ? 4 : 1;
        bool valid = n <= len - i;
        for (size_t j = 1; j < n && valid; ++j) valid = ((unsigned char)s[i + j] & 0xc0) == 0x80;
        if (valid && n >= 3) {
            unsigned char b = (unsigned char)s[i + 1];
            if ((c == 0xe0 && b < 0xa0) || (c == 0xed && b >= 0xa0) ||
                (c == 0xf0 && b < 0x90) || (c == 0xf4 && b >= 0x90)) valid = false;
        }
        if (!valid) n = 1;
        if (n == 4) ++units;
        i += n;
    }
    return units;
}
