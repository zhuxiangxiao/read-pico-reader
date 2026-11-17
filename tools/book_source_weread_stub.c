/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * Local TXT/EPUB regression guard: no virtual source or networking may be entered.
 */
#include "book_source_weread.h"
#include <assert.h>
#define LOCAL_ONLY() assert(!"local book suite entered the virtual source")
bool book_weread_is_open(void) { return false; }
void book_weread_close(void) {}
esp_err_t book_weread_open(const char *uri) { (void)uri; LOCAL_ONLY(); return ESP_ERR_NOT_SUPPORTED; }
size_t book_weread_count(void) { LOCAL_ONLY(); return 0; }
esp_err_t book_weread_title(size_t i,char *out,size_t cap) { (void)i;(void)out;(void)cap;LOCAL_ONLY();return ESP_ERR_NOT_SUPPORTED; }
esp_err_t book_weread_load(size_t i,const char *anchor,size_t *offset,html_text_t *out) { (void)i;(void)anchor;(void)offset;(void)out;LOCAL_ONLY();return ESP_ERR_NOT_SUPPORTED; }
uint32_t book_weread_total(void) { LOCAL_ONLY();return 0; }
uint32_t book_weread_offset(size_t i) { (void)i;LOCAL_ONLY();return 0; }
esp_err_t book_weread_image(size_t i,const char *src,uint8_t **data,size_t *len,bool *png) { (void)i;(void)src;(void)data;(void)len;(void)png;LOCAL_ONLY();return ESP_ERR_NOT_SUPPORTED; }
