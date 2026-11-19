/* SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：阅读目录独立页面的布局、绘制与命中定义。
 * English: Layout, rendering and hit testing for the standalone reading directory.
 */
#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include "epdiy.h"

#define BOOK_TOC_ROWS 7
#define BOOK_TOC_BACK (-2)
#define BOOK_TOC_PREV (-3)
#define BOOK_TOC_NEXT (-4)
#define BOOK_TOC_SKIP_PREV (-5)
#define BOOK_TOC_SKIP_NEXT (-6)
#define BOOK_TOC_JUMP (-7)
#define BOOK_TOC_JUMP_CANCEL (-8)
#define BOOK_TOC_JUMP_CONFIRM (-9)
#define BOOK_TOC_JUMP_TRACK (-10)
#define BOOK_TOC_SELECT_VOLUME (-11)

int book_toc_pages(size_t chapters);
/// 注入章节缓存探针；目录绘制只读准备好的小位图，绝不在此回调里执行IO。
/// Inject the cached-chapter probe; drawing reads prepared flags only and never calls it.
void book_toc_set_cached_probe(bool (*probe)(size_t chapter, void *user), void *user);
/// 进入目录或翻页后按可见章节行准备缓存标记；卷行和当前行不查询。
/// Prepare cached markers for visible chapter rows after entry/paging; volume and current rows are skipped.
void book_toc_prepare_page(size_t chapters, size_t current, int page);
/// 进入目录时仅展开当前卷，返回当前章节所在页。/ Open only the current volume and return its selected chapter page.
int book_toc_open(size_t current);
/// 卷行只展开/折叠，不跳读正文；按新布局定位卷行。/ Toggle a volume without loading text and reposition its header.
bool book_toc_toggle(int target, int *page);
/// 一级目录选择只读取当前页卷名，取消恢复原页。/ The root picker reads only visible volume titles; cancelling restores the old page.
bool book_toc_picker_toggle(int *page);
bool book_toc_picker_close(int *page);
int book_toc_hit(uint16_t x, uint16_t y, int page, size_t chapters);
int book_toc_jump_hit(uint16_t x, uint16_t y);
int book_toc_jump_percent(uint16_t x);
int book_toc_page_from_percent(int percent, int pages);
EpdRect book_toc_row_rect(int row);
void book_toc_render(uint8_t *fb, const char *book_title, size_t chapters,
                     size_t current, int page, const char *message);
void book_toc_render_jump(uint8_t *fb, size_t chapters, int percent);
