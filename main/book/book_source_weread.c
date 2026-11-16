/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信读书目录快照与离线章节适配；网络只在书源加载回调中执行。
 * English: Adapt WeRead TOC snapshots/offline chapters; networking belongs to source load callbacks.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "book_source_weread.h"
#include "book_weread_uri.h"
#include "weread_auth.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_store.h"
#include "weread_position.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#if defined(ESP_PLATFORM)
// TF 为 FAT，无符号链接。/ TF uses FAT and has no symbolic links.
#define lstat stat
#endif

static char s_id[64];
static weread_toc_t s_toc;
static bool s_txt;
static char s_prepared_id[64],s_prepared_owner[32];
static weread_toc_t s_prepared_toc;
void book_weread_clear_prepared(void){WR_GUARD;weread_toc_free(&s_prepared_toc);s_prepared_id[0]=s_prepared_owner[0]=0;}
bool book_weread_prepared(const char *id){WR_GUARD;
    const weread_auth_session_t *s=weread_auth_get_current_session();
    return s&&id&&s_prepared_toc.count&&!strcmp(id,s_prepared_id)&&!strcmp(s->vid,s_prepared_owner);
}
bool book_weread_cached_version(const char *id,uint32_t *version){WR_GUARD;
    if(!id||!version)return false;
    if(book_weread_prepared(id)){*version=s_prepared_toc.book_version;return true;}
    if(s_id[0]&&!strcmp(s_id,id)){*version=s_toc.book_version;return true;}
    return false;
}
esp_err_t book_weread_prepare_open(const char *id){WR_GUARD;
    book_weread_clear_prepared();
    if(!wr_valid_id(id)||!weread_auth_is_authenticated())return ESP_ERR_INVALID_STATE;
    esp_err_t err=weread_toc_load(id,&s_prepared_toc);
    if(err!=ESP_OK){err=weread_toc_sync(id);if(err==ESP_OK)err=weread_toc_load(id,&s_prepared_toc);}
    if(err==ESP_OK&&!s_prepared_toc.count)err=ESP_ERR_NOT_FOUND;
    if(err!=ESP_OK){book_weread_clear_prepared();return err;}
    strcpy(s_prepared_id,id);strcpy(s_prepared_owner,weread_auth_get_current_session()->vid);return ESP_OK;
}

bool book_weread_stat(const char *uri, struct stat *out) { WR_GUARD;
    if (!book_path_is_weread(uri) || !out) return false;
    weread_book_t book;
    if (!weread_shelf_find(uri + 9, &book)) return false;
    memset(out, 0, sizeof(*out));
    out->st_mode = S_IFREG | 0400;
    // 账号、书号与版本组成身份，避免跨账号误用NVS进度。/ Bind NVS progress to account, book and version.
    const weread_auth_session_t *session = weread_auth_get_current_session();
    uint32_t identity = 2166136261u;
    const char *keys[] = {session ? session->vid : "", uri + 9};
    for (unsigned k = 0; k < 2; ++k) {
        for (const unsigned char *p = (const unsigned char *)keys[k]; *p; ++p) identity = (identity ^ *p) * 16777619u;
        identity = (identity ^ 0xffu) * 16777619u;
    }
    identity = (identity ^ book.book_version) * 16777619u;
    identity &= INT32_MAX;
    out->st_size = identity ? identity : 1;
    out->st_mtime = (time_t)book.update_time;
    return true;
}
bool book_weread_is_open(void) { return s_id[0] != 0; }
void book_weread_close(void) { WR_GUARD; s_id[0] = 0; s_txt = false; weread_toc_free(&s_toc); }
esp_err_t book_weread_open(const char *uri) { WR_GUARD;
    if (!book_path_is_weread(uri) || strlen(uri + 9) >= sizeof(s_id)) return ESP_ERR_INVALID_ARG;
    if (!weread_auth_is_authenticated()) {
        esp_err_t err = weread_auth_init(NULL); if (err != ESP_OK) return err;
    }
    if (!weread_shelf_count()) (void)weread_shelf_load();
    weread_book_t b;
    if (!weread_shelf_find(uri + 9, &b)) return ESP_ERR_NOT_FOUND;
    weread_toc_t toc = {0};
    esp_err_t err=ESP_OK;
    if(book_weread_prepared(uri+9)){toc=s_prepared_toc;memset(&s_prepared_toc,0,sizeof(s_prepared_toc));s_prepared_id[0]=s_prepared_owner[0]=0;}
    else err=weread_toc_load(uri+9,&toc);
    if (err != ESP_OK) {
        err = weread_toc_sync(uri + 9);
        if (err == ESP_OK) err = weread_toc_load(uri + 9, &toc);
    }
    if (err != ESP_OK) return err;
    if (!toc.count) { weread_toc_free(&toc); return ESP_ERR_NOT_FOUND; }
    book_weread_close(); s_toc = toc;
    strcpy(s_id, uri + 9); s_txt = !strcasecmp(b.format, "txt");
    return ESP_OK;
}
size_t book_weread_count(void) { return s_toc.count; }
uint32_t book_weread_uid(size_t i) { return i < s_toc.count ? s_toc.chapters[i].uid : 0; }
uint32_t book_weread_chapter_index(size_t i) { return i < s_toc.count ? s_toc.chapters[i].index : 0; }
size_t book_weread_index(uint32_t uid) {
    for (size_t i = 0; i < s_toc.count; ++i) if (s_toc.chapters[i].uid == uid) return i;
    return SIZE_MAX;
}
esp_err_t book_weread_title(size_t i, char *out, size_t cap) {
    if (i >= s_toc.count || !out || !cap) return ESP_ERR_INVALID_ARG;
    size_t n = strlen(s_toc.chapters[i].title);
    if (n >= cap) return ESP_ERR_INVALID_SIZE;
    memcpy(out, s_toc.chapters[i].title, n + 1); return ESP_OK;
}
uint32_t book_weread_offset(size_t index) {
    uint64_t n = 0;
    for (size_t i = 0; i < index && i < s_toc.count; ++i)
        n += s_toc.chapters[i].word_count ? (uint64_t)s_toc.chapters[i].word_count * 3 : 1;
    return n > UINT32_MAX ? UINT32_MAX : (uint32_t)n;
}
uint32_t book_weread_total(void) { return book_weread_offset(s_toc.count); }

static bool collect_css(const char *dir, char *css, size_t *used, size_t cap, unsigned depth) {
    if (depth > 8) return true;
    DIR *d = opendir(dir); if (!d) return true;
    char *path __attribute__((cleanup(wr_release)))=wr_alloc(1024);
    if(!path){closedir(d);return false;}
    bool ok=true;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (*e->d_name == '.') continue;
        int n = snprintf(path, 1024, "%s/%s", dir, e->d_name);
        if (n < 0 || n >= 1024) continue;
        struct stat st; if (lstat(path, &st)) continue;
        if (S_ISDIR(st.st_mode)) {if(!collect_css(path,css,used,cap,depth+1)){ok=false;break;}continue;}
        const char *ext = strrchr(e->d_name, '.');
        if (!S_ISREG(st.st_mode) || !ext || strcasecmp(ext, ".css") || st.st_size < 0 || (uint64_t)st.st_size + 2 > cap - *used) continue;
        FILE *f = fopen(path, "rb"); if (!f) continue;
        size_t len = (size_t)st.st_size;
        if (fread(css + *used, 1, len, f) == len) { *used += len; css[(*used)++] = '\n'; }
        fclose(f);
    }
    closedir(d); css[*used] = 0;return ok;
}
static char *chapter_css(size_t i, size_t *used) {
    const size_t cap = 256u * 1024u;
    char *css = wr_alloc(cap); *used = 0;
    if (!css) return NULL;
    css[0] = 0;
    char resource[512], chapter[512];
    bool have_res = weread_chapter_resource_directory(s_id, resource, sizeof(resource)) == ESP_OK;
    bool have_ch = weread_chapter_directory(s_id, book_weread_uid(i), chapter, sizeof(chapter)) == ESP_OK;
    if ((have_res&&!collect_css(resource,css,used,cap,0)) ||
        (have_ch&&(!have_res||strcmp(resource,chapter))&&!collect_css(chapter,css,used,cap,0))) {free(css);return NULL;}
    return css;
}
esp_err_t book_weread_load(size_t i, const char *anchor, size_t *offset, html_text_t *out) { WR_GUARD;
    if (!out || i >= s_toc.count) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out)); if (offset) *offset = 0;
    char *text = NULL; size_t len = 0;
    esp_err_t err = weread_chapter_load_cached(s_id, book_weread_uid(i), &text, &len);
    if (err != ESP_OK) return err;
    if (s_txt) { out->utf8 = text; out->len = len; return ESP_OK; }
    size_t used = 0; char *css = chapter_css(i, &used);
    if (!css) { free(text); return ESP_ERR_NO_MEM; }
    err = html_to_blocks_with_css_anchor(text, len, css, used, anchor, offset, out);
    free(css); free(text); return err;
}
typedef struct {
    const char *html;
    weread_position_point_t *points;
    size_t count, next, cursor;
    uint32_t unit, goal, result;
    bool to_cloud, found;
} position_mapper_t;
static void mapped_character(void *ctx, size_t text_offset, size_t html_offset) {
    position_mapper_t *m = ctx;
    if (m->found) return;
    while (m->next < m->count && m->points[m->next].byte <= html_offset) {
        m->cursor = m->points[m->next].byte; m->unit = m->points[m->next].unit; ++m->next;
    }
    m->unit += weread_utf16_length(m->html + m->cursor, html_offset - m->cursor); m->cursor = html_offset;
    if (m->to_cloud ? text_offset >= m->goal : m->unit >= m->goal) {
        m->result = m->to_cloud ? m->unit : (uint32_t)text_offset; m->found = true;
    }
}
esp_err_t book_weread_map(size_t i, bool to_cloud, uint32_t input, uint32_t *output) { WR_GUARD;
    if (!output || i >= s_toc.count) return ESP_ERR_INVALID_ARG;
    char *path __attribute__((cleanup(wr_release)))=wr_alloc(1100),*text=NULL;size_t len=0;
    if(!path)return ESP_ERR_NO_MEM;
    esp_err_t err = weread_chapter_directory(s_id, book_weread_uid(i), path, 1024);
    if (err != ESP_OK) return err;
    strcat(path,"/position.map");
    FILE *f = fopen(path, "rb"); if (!f) return ESP_ERR_NOT_FOUND;
    weread_position_header_t header = {0};
    if (fread(&header, sizeof(header), 1, f) != 1 || header.magic != WEREAD_POSITION_MAGIC ||
        !header.count || header.count > 16384) { fclose(f); return ESP_ERR_INVALID_RESPONSE; }
    weread_position_point_t *points = wr_alloc(header.count * sizeof(*points));
    if (!points) { fclose(f); return ESP_ERR_NO_MEM; }
    bool valid = fread(points, sizeof(*points), header.count, f) == header.count && fgetc(f) == EOF;
    fclose(f);
    for (size_t j = 0; j < header.count && valid; ++j)
        valid = points[j].byte <= header.bytes && points[j].unit <= header.units &&
            (!j || (points[j].byte >= points[j - 1].byte && points[j].unit >= points[j - 1].unit));
    if (!valid) { free(points); return ESP_ERR_INVALID_RESPONSE; }
    err = weread_chapter_load_cached(s_id, book_weread_uid(i), &text, &len);
    if (err == ESP_OK && len != header.bytes) err = ESP_ERR_INVALID_RESPONSE;
    position_mapper_t mapper = {.html = text, .points = points, .count = header.count, .goal = input, .to_cloud = to_cloud};
    html_text_t parsed = {0};
    if (err == ESP_OK && s_txt) {
        for (size_t at = 0; at < len && !mapper.found; ++at)
            if (((unsigned char)text[at] & 0xc0) != 0x80) mapped_character(&mapper, at, at);
        parsed.len = len;
    } else if (err == ESP_OK) {
        size_t css_len = 0; char *css = chapter_css(i, &css_len);
        if (!css) err = ESP_ERR_NO_MEM;
        else err = html_to_blocks_mapped(text, len, css, css_len, mapped_character, &mapper, &parsed);
        free(css);
    }
    if (err == ESP_OK) *output = mapper.found ? mapper.result : to_cloud ? header.units : (uint32_t)parsed.len;
    html_text_free(&parsed); free(text); free(points); return err;
}
esp_err_t book_weread_image(size_t i, const char *src, uint8_t **data, size_t *len, bool *png) { WR_GUARD;
    if (!data || !len || !png || i >= s_toc.count) return ESP_ERR_INVALID_ARG;
    *data = NULL; *len = 0; char name[512];
    if (!weread_resource_path(NULL, src, name, sizeof(name))) return ESP_ERR_INVALID_ARG;
    for (unsigned pass = 0; pass < 2; ++pass) {
        char root[512], path[1024];
        esp_err_t err = pass ? weread_chapter_resource_directory(s_id, root, sizeof(root)) :
            weread_chapter_directory(s_id, book_weread_uid(i), root, sizeof(root));
        if (err != ESP_OK) continue;
        int n = snprintf(path, sizeof(path), "%s/%s", root, name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        FILE *f = fopen(path, "rb"); if (!f) continue;
        struct stat st;
        if (fstat(fileno(f), &st) || st.st_size <= 0 || st.st_size > 8 * 1024 * 1024) { fclose(f); return ESP_ERR_INVALID_SIZE; }
        size_t size = (size_t)st.st_size; uint8_t *buf = wr_alloc(size);
        if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
        bool ok = fread(buf, 1, size, f) == size; fclose(f);
        if (!ok) { free(buf); return ESP_FAIL; }
        *data = buf; *len = size; *png = size >= 8 && !memcmp(buf, "\x89PNG\r\n\x1a\n", 8); return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

bool book_weread_metadata(const char *uri, char *title, size_t tc, char *author, size_t ac) { WR_GUARD;
    weread_book_t b;
    if (!book_path_is_weread(uri) || !weread_shelf_find(uri+9,&b)) return false;
    if (title && tc) wr_display_copy(title,tc,b.title);
    if (author && ac) wr_display_copy(author,ac,b.author);
    return true;
}
bool book_weread_cover(const char *uri, uint8_t *pixels, size_t cap) { WR_GUARD;
    char path[512]; weread_book_t b;
    if (!book_path_is_weread(uri) || !pixels || cap < 176u*240u ||
        !weread_shelf_find(uri+9,&b) || wr_cache_path(uri+9,"cover.gray",path,sizeof(path)) != ESP_OK) return false;
    FILE *f=fopen(path,"rb"); if (!f) return false;
    bool ok=fread(pixels,1,176u*240u,f)==176u*240u && fgetc(f)==EOF;
    fclose(f); return ok;
}
