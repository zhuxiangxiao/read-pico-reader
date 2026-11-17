/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：本地与微信书架共用展示；条目、封面及生命周期由各数据源拥有。
 * English: Shared shelf presentation; sources own entries, artwork and lifecycle.
 */
#pragma once
#include "ui_kit.h"

int ui_shelf_rows(int style);
EpdRect ui_shelf_entry(int style,int row);
EpdRect ui_shelf_cover(int style,int row);
void ui_shelf_cover_begin(uint8_t *fb,EpdRect image,int style);
void ui_shelf_cover_end(uint8_t *fb,EpdRect image,int style,bool binary);
void ui_shelf_cover_pixels(uint8_t *fb,EpdRect image,const uint8_t *gray,unsigned width,unsigned height,bool binary);
void ui_shelf_cover_placeholder(uint8_t *fb,EpdRect image,const char *title);
void ui_shelf_furniture(uint8_t *fb,int style,int top,bool binary,void (*guard)(uint8_t*,EpdRect,int));
void ui_shelf_begin(void);
void ui_shelf_progress(uint8_t *fb,EpdRect image,int style,unsigned progress);
void ui_shelf_list_row(uint8_t *fb,EpdRect card,const char *title,const char *author,unsigned progress,unsigned current,unsigned total);
EpdRect ui_shelf_search_rect(int page,int pages);
EpdRect ui_shelf_page_label_rect(int page,int pages);
void ui_shelf_pager(uint8_t *fb,int page,int pages,bool filtered,bool key_mode,int pressed,bool search_pressed);
EpdRect ui_shelf_order_rect(int row);
int ui_shelf_order_mode(unsigned order);
bool ui_shelf_order_desc(unsigned order);
unsigned ui_shelf_order_value(int mode,bool descending);
void ui_shelf_order_button(uint8_t *fb,EpdRect rect,unsigned order,bool open,bool pressed,const char *time_label,bool compact);
void ui_shelf_order_menu(uint8_t *fb,unsigned order,const char *time_label);
bool ui_shelf_order_select(int x,int y,unsigned order,unsigned *selected);
