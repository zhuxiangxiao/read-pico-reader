/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：两板共用有界图标无损解码，不改变原始掩模或绘制算法。
 * English: Shared bounded lossless icon decoding; preserve canonical masks and rendering.
 */
#include "../assets/ui_icons_packed.h"
_Static_assert(UI_ICON_PACKED_COUNT == UI_ICON_COUNT, "packed icon order");
_Static_assert(sizeof(ui_icon_packed_offsets)/sizeof(ui_icon_packed_offsets[0]) == UI_ICON_COUNT+1, "packed icon offsets");
_Static_assert(UI_ICON_UNPACKED_BYTES <= 3072, "bounded icon scratch stack");
_Static_assert(UI_ICON_UNPACKED_BYTES == UI_ICON_MASK_SIZE * UI_ICON_MASK_SIZE / 2, "packed icon dimensions");

// 单次有界解码，不保留像素缓存；原始掩模仍是生成器的权威输入。
// Decode one bounded mask without a persistent cache; canonical masks remain the generator input.
static bool icon_unpack(ui_icon_t icon, uint8_t *mask) {
    if (icon < 0 || icon >= UI_ICON_COUNT || !mask) return false;
    size_t at = ui_icon_packed_offsets[icon], end = ui_icon_packed_offsets[icon + 1], used = 0;
    if (at > end || end > sizeof(ui_icon_packed)) return false;
    while (at < end) {
        uint8_t command = ui_icon_packed[at++];
        size_t count = (command & 127u) + 1u;
        if (count > UI_ICON_UNPACKED_BYTES - used) return false;
        if (command & 128u) {
            if (at == end) return false;
            memset(mask + used, ui_icon_packed[at++], count);
        } else {
            if (count > end - at) return false;
            memcpy(mask + used, ui_icon_packed + at, count);
            at += count;
        }
        used += count;
    }
    return used == UI_ICON_UNPACKED_BYTES;
}
