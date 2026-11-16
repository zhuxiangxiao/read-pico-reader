/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：下载归档先写 TF，4 KiB 流式解包到独立目录，按 info.txt 顺序组装。
 * English: Spool archives to TF, extract in 4 KiB chunks to a private directory and follow info.txt order.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "weread_chapter.h"
#include "weread_auth.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_store.h"
#include "esp_log.h"
#include "book_weread_crypto.h"
#include "book_source_weread.h"
#include "weread_position.h"
#include <dirent.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#if defined(ESP_PLATFORM)
// TF 为 FAT，无符号链接。/ TF uses FAT and has no symbolic links.
#define lstat stat
#endif
#include <unistd.h>

#define CHAPTER_LIMIT (4u * 1024u * 1024u)
#define ARCHIVE_LIMIT (32u * 1024u * 1024u)
static weread_cancel_fn s_cancel;
static void *s_cancel_ctx;
void weread_chapter_set_cancel(weread_cancel_fn fn, void *ctx) { s_cancel = fn; s_cancel_ctx = ctx; }
static bool cancelled(void) { return s_cancel && s_cancel(s_cancel_ctx); }
static uint64_t number(const cJSON *o, const char *key) { uint64_t n = 0; wr_uint(o, key, &n); return n; }

bool weread_resource_path(const char *base, const char *relative, char *out, size_t cap) {
    if (!out || !cap || !relative || !*relative || *relative == '/' || strchr(relative, ':') || strchr(relative, '\\')) return false;
    char raw[1024];
    const char *slash = base ? strrchr(base, '/') : NULL;
    size_t n = slash ? (size_t)(slash - base + 1) : 0;
    if (n + strlen(relative) >= sizeof(raw)) return false;
    if (n) memcpy(raw, base, n);
    strcpy(raw + n, relative);
    char *end = strpbrk(raw, "?#"); if (end) *end = 0;
    size_t used = 0; char *p = raw;
    while (*p) {
        char *next = strchr(p, '/'); size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len == 2 && !memcmp(p, "..", 2)) {
            if (!used) return false;
            while (used && out[used - 1] != '/') --used;
            if (used) --used;
        } else if (len && !(len == 1 && *p == '.')) {
            if (used + (used ? 1 : 0) + len >= cap) return false;
            if (used) out[used++] = '/';
            memcpy(out + used, p, len); used += len;
        }
        if (!next) break;
        p = next + 1;
    }
    if (!used || used >= cap) return false;
    out[used] = 0; return true;
}
static esp_err_t join(const char *dir, const char *leaf, char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/%s", dir, leaf);
    return n >= 0 && (size_t)n < cap ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static void remove_tree(const char *path, unsigned depth) {
    if (depth > 32) return;
    struct stat st;
    if (lstat(path, &st)) return;
    if (!S_ISDIR(st.st_mode)) { unlink(path); return; }
    DIR *dir = opendir(path);
    if (!dir) return;
    char *child __attribute__((cleanup(wr_release)))=wr_alloc(1024);
    if(!child){closedir(dir);return;}
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (join(path, entry->d_name, child, 1024) == ESP_OK) remove_tree(child, depth + 1);
    }
    closedir(dir); rmdir(path);
}
static uint32_t version(const char *id) {
    uint32_t v = weread_shelf_book_version(id);
    if (v) return v;
    if(book_weread_cached_version(id,&v))return v;
    weread_toc_t toc = {0};
    if (weread_toc_load(id, &toc) == ESP_OK && toc.book_version) v = toc.book_version;
    weread_toc_free(&toc); return v;
}
static esp_err_t manifest(const char *id, uint32_t uid, bool res, cJSON **out) {
    char leaf[64], path[512]; *out = NULL;
    if (res) strcpy(leaf, "resources.json"); else snprintf(leaf, sizeof(leaf), "chapter-%u.json", (unsigned)uid);
    esp_err_t err = wr_cache_path(id, leaf, path, sizeof(path));
    if (err == ESP_OK) err = wr_read_json(path, out);
    if (err == ESP_OK && (!wr_owner_matches(*out) || number(*out, "bookVersion") != version(id) ||
        (!res && number(*out, "chapterUid") != uid))) err = ESP_ERR_INVALID_RESPONSE;
    const char *root = wr_string(*out, "root");
    if (err == ESP_OK && (!root || !*root || strstr(root, "..") || root[0] == '/' || strchr(root, '\\'))) err = ESP_ERR_INVALID_RESPONSE;
    if (err != ESP_OK) { cJSON_Delete(*out); *out = NULL; }
    return err;
}
static esp_err_t directory(const char *id, uint32_t uid, bool res, char *out, size_t cap) {
    cJSON *m = NULL;
    esp_err_t err = manifest(id, uid, res, &m);
    if (err == ESP_OK) err = wr_cache_path(id, wr_string(m, "root"), out, cap);
    struct stat st;
    if (err == ESP_OK && (stat(out, &st) || !S_ISDIR(st.st_mode))) err = ESP_ERR_NOT_FOUND;
    cJSON_Delete(m); return err;
}
esp_err_t weread_chapter_directory(const char *id, uint32_t uid, char *out, size_t cap) { WR_GUARD; return directory(id, uid, false, out, cap); }
esp_err_t weread_chapter_resource_directory(const char *id, char *out, size_t cap) { WR_GUARD; return directory(id, 0, true, out, cap); }
static esp_err_t read_file(const char *path, char **out, size_t *len) {
    *out = NULL; *len = 0;
    FILE *f = fopen(path, "rb"); if (!f) return ESP_ERR_NOT_FOUND;
    struct stat st;
    if (fstat(fileno(f), &st) || st.st_size < 0 || st.st_size > CHAPTER_LIMIT) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    size_t n = (size_t)st.st_size; char *buf = wr_alloc(n + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    bool ok = fread(buf, 1, n, f) == n; if (fclose(f)) ok = false;
    if (!ok) { free(buf); return ESP_FAIL; }
    buf[n] = 0; *out = buf; *len = n; return ESP_OK;
}
typedef struct { char root[512], name[512]; FILE *file; esp_err_t error; } extract_t;
static bool extract(void *ctx, const char *name, const uint8_t *bytes, size_t n, size_t offset, size_t total) {
    extract_t *x = ctx;
    if (cancelled()) return false;
    if (!offset) {
        if (x->file) { x->error = ESP_ERR_INVALID_RESPONSE; return false; }
        char path[1024], parent[1024];
        if (!strcmp(name, "body.html") || !strcmp(name, "position.map") || strchr(name, ':') ||
            join(x->root, name, path, sizeof(path)) != ESP_OK || !wr_copy(parent, sizeof(parent), path)) return false;
        char *slash = strrchr(parent, '/'); *slash = 0;
        x->error = wr_mkdirs(parent); if (x->error != ESP_OK) return false;
        struct stat st;
        if (!stat(path, &st)) { x->error = ESP_ERR_INVALID_RESPONSE; return false; }
        x->file = fopen(path, "wb"); if (!x->file) { x->error = ESP_FAIL; return false; }
        wr_copy(x->name, sizeof(x->name), name);
    }
    if (!x->file || strcmp(name, x->name) || fwrite(bytes, 1, n, x->file) != n) { x->error = ESP_FAIL; return false; }
    if (offset + n == total) {
        bool ok = !fflush(x->file); if (fclose(x->file)) ok = false; x->file = NULL;
        if (!ok) { x->error = ESP_FAIL; return false; }
    }
    return true;
}
static bool append(FILE *f, const void *data, size_t len, size_t *total) {
    if (len > CHAPTER_LIMIT - *total || fwrite(data, 1, len, f) != len) return false;
    *total += len; return true;
}
typedef struct { FILE *file; uint32_t units, count; } position_writer_t;
static bool position_anchor(position_writer_t *p, size_t at) {
    weread_position_point_t point = {(uint32_t)at, p->units};
    return p->count++ < 16384 && fwrite(&point, sizeof(point), 1, p->file) == 1;
}
static bool append_source(FILE *f, const char *data, size_t len, size_t *total, position_writer_t *p) {
    if (!position_anchor(p, *total) || !append(f, data, len, total)) return false;
    p->units += weread_utf16_length(data, len); return true;
}
static bool append_html(FILE *f, const char *data, size_t len, const char *source, size_t *total, position_writer_t *map) {
    size_t start = 0; bool tag = false;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '<') tag = true;
        if (data[i] == '>') tag = false;
        if (!tag || !i || !isspace((unsigned char)data[i - 1])) continue;
        size_t k = len - i >= 3 && !strncasecmp(data + i, "src", 3) ? 3 :
            len - i >= 4 && !strncasecmp(data + i, "href", 4) ? 4 : 0;
        if (!k) continue;
        size_t j = i + k;
        while (j < len && isspace((unsigned char)data[j])) ++j;
        if (j >= len || data[j++] != '=') continue;
        while (j < len && isspace((unsigned char)data[j])) ++j;
        if (j == len) break;
        char quote = data[j] == '\'' || data[j] == '"' ? data[j++] : 0;
        size_t value = j;
        while (j < len && (quote ? data[j] != quote : !isspace((unsigned char)data[j]) && data[j] != '>')) ++j;
        if (j == len || j - value >= 512) continue;
        char raw[512], normalized[512];
        memcpy(raw, data + value, j - value); raw[j - value] = 0;
        if (weread_resource_path(source, raw, normalized, sizeof(normalized))) {
            if (!append_source(f, data + start, value - start, total, map) ||
                !append(f, normalized, strlen(normalized), total)) return false;
            const char *fragment = k == 4 ? strchr(raw, '#') : NULL;
            if (fragment && !append(f, fragment, strlen(fragment), total)) return false;
            map->units += weread_utf16_length(data + value, j - value);
            start = j;
        }
        i = j;
    }
    return append_source(f, data + start, len - start, total, map);
}
static esp_err_t assemble(const char *root, const char *id, uint32_t uid, uint32_t version, const char *format) {
    char path[1024]; if (join(root, "info.txt", path, sizeof(path)) != ESP_OK) return ESP_ERR_INVALID_SIZE;
    cJSON *info = NULL; esp_err_t err = wr_read_json(path, &info);
    const char *info_id = wr_string(info, "bookId");
    if (err == ESP_OK && (!info_id || strcmp(info_id, id))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && cJSON_HasObjectItem(info,"bookVersion") && number(info, "bookVersion") != version) err = WEREAD_ERR_VERSION;
    cJSON *files = cJSON_GetObjectItemCaseSensitive(info, "files"), *entry;
    cJSON *chapters = cJSON_GetObjectItemCaseSensitive(info, "chapters");
    if (cJSON_IsArray(chapters)) files = NULL;
    cJSON_ArrayForEach(entry, chapters)
        if (number(entry, "chapterUid") == uid) { files = cJSON_GetObjectItemCaseSensitive(entry, "files"); break; }
    if (err == ESP_OK && (!cJSON_IsArray(files) || cJSON_GetArraySize(files) < 1 || cJSON_GetArraySize(files) > 128)) err = ESP_ERR_INVALID_RESPONSE;
    FILE *f = NULL;
    position_writer_t map = {0};
    if (err == ESP_OK) {
        join(root, "body.html", path, sizeof(path)); f = fopen(path, "wb"); if (!f) err = ESP_FAIL;
        join(root, "position.map", path, sizeof(path)); map.file = fopen(path, "wb");
        weread_position_header_t header = {0};
        if (!map.file || fwrite(&header, sizeof(header), 1, map.file) != 1) err = ESP_FAIL;
    }
    size_t total = 0;
    if (err == ESP_OK) cJSON_ArrayForEach(entry, files) {
        if (!cJSON_IsString(entry) || cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        char normalized[512];
        if (!weread_resource_path(NULL, entry->valuestring, normalized, sizeof(normalized)) ||
            join(root, normalized, path, sizeof(path)) != ESP_OK || !strcmp(normalized, "body.html")) { err = ESP_ERR_INVALID_RESPONSE; break; }
        char *data = NULL; size_t len = 0;
        err = read_file(path, &data, &len); if (err != ESP_OK) break;
        if (!strcmp(format, "txt")) {
            const weread_auth_session_t *s = weread_auth_get_current_session();
            uint8_t *plain = NULL; size_t plain_len = 0;
            if (s && book_weread_unwrap_envelope((const uint8_t *)data, len, s->vid, NULL, 0, &plain, &plain_len)) {
                free(data); data = (char *)plain; len = plain_len;
            }
        }
        if (!strcmp(format, "txt") ? !append_source(f, data, len, &total, &map) : !append_html(f, data, len, normalized, &total, &map)) err = ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK && !append(f, "\n", 1, &total)) err = ESP_ERR_INVALID_SIZE;
        free(data); if (err != ESP_OK) break;
    }
    if (f) { if (fflush(f)) err = ESP_FAIL; if (fclose(f)) err = ESP_FAIL; }
    if (map.file) {
        weread_position_header_t header = {WEREAD_POSITION_MAGIC, map.count + 1, (uint32_t)total, map.units};
        if (err == ESP_OK && (!position_anchor(&map, total) || fseek(map.file, 0, SEEK_SET) ||
            fwrite(&header, sizeof(header), 1, map.file) != 1 || fflush(map.file))) err = ESP_FAIL;
        if (fclose(map.file)) err = ESP_FAIL;
    }
    cJSON_Delete(info); return err;
}
esp_err_t weread_chapter_download(const char *id, uint32_t uid) { WR_GUARD; return weread_chapter_download_ex(id, uid, false); }
// 大型路径/响应/解包缓冲可回收，网络任务只保留小内部栈。/ Reclaim large path/response/extraction buffers while keeping a small internal network stack.
typedef struct {
    weread_book_t b;
    char leaf[128],root[512],parent[512],path[600],url[1024],json[4096];
    weread_download_info_t response;
    extract_t x;
    uint8_t password[256];
} chapter_download_work_t;
static void release_chapter_work(chapter_download_work_t **p) {
    if(*p)weread_secure_zero(*p,sizeof(**p));
    wr_release(p);
}
esp_err_t weread_chapter_download_ex(const char *id, uint32_t uid, bool preload) { WR_GUARD;
    if (!wr_valid_id(id) || !uid) return ESP_ERR_INVALID_ARG;
    char existing[512];
    if (weread_chapter_directory(id, uid, existing, sizeof(existing)) == ESP_OK) {
        char cache_path[600]; struct stat st; join(existing, "body.html", cache_path, sizeof(cache_path));
        bool body_ok = !stat(cache_path, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= CHAPTER_LIMIT;
        join(existing, "position.map", cache_path, sizeof(cache_path));
        if (body_ok && !stat(cache_path, &st) && S_ISREG(st.st_mode) && st.st_size >= (off_t)sizeof(weread_position_header_t)) return ESP_OK;
    }
    chapter_download_work_t *w __attribute__((cleanup(release_chapter_work)))=wr_alloc(sizeof(*w));
    if(!w)return ESP_ERR_NO_MEM;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        if (cancelled()) return ESP_ERR_INVALID_STATE;
        if (!weread_shelf_find(id, &w->b)) return ESP_ERR_NOT_FOUND;
        if (strcmp(w->b.format, "epub") && strcmp(w->b.format, "txt")) return ESP_ERR_NOT_SUPPORTED;
        uint32_t v = version(id);
        bool common = !strcmp(w->b.format, "epub") && weread_chapter_resource_directory(id, existing, sizeof(existing)) != ESP_OK;
        snprintf(w->leaf, sizeof(w->leaf), "%s/v%u-c%u", common ? "res" : "chapters", (unsigned)v, (unsigned)uid);
        if (wr_cache_path(id, w->leaf, w->parent, sizeof(w->parent)) != ESP_OK) return ESP_ERR_INVALID_SIZE;
        char *slash = strrchr(w->parent, '/'); *slash = 0;
        esp_err_t err = wr_mkdirs(w->parent); if (err != ESP_OK) return err;
        bool made = false;
        for (unsigned gen = 1; gen <= 1024; ++gen) {
            snprintf(w->leaf, sizeof(w->leaf), "%s/v%u-c%u-g%u", common ? "res" : "chapters", (unsigned)v, (unsigned)uid, gen);
            if (wr_cache_path(id, w->leaf, w->root, sizeof(w->root)) != ESP_OK) return ESP_ERR_INVALID_SIZE;
            if (!mkdir(w->root, 0700)) { made = true; break; }
            if (errno != EEXIST) return ESP_FAIL;
        }
        if (!made) return ESP_ERR_INVALID_SIZE;
        join(w->root, "archive.part", w->path, sizeof(w->path));
        FILE *archive = fopen(w->path, "w+b");
        if (!archive) { remove_tree(w->root, 0); return ESP_FAIL; }
        snprintf(w->url, sizeof(w->url), "%s/book/chapterdownload?bookId=%s&chapters=%s%u&pf=wechat_wx-2001-android-100-weread&pfkey=pfKey&zoneId=1&bookVersion=%u&bookType=%s&release=1&stopAutoPayWhenBNE=1&preload=%u&preview=0&offline=0",
            weread_client_get_base_url(), id, common ? "0," : "", (unsigned)uid, (unsigned)v, w->b.format, preload ? 1u : 0u);
        err = weread_client_download(w->url, archive, ARCHIVE_LIMIT, s_cancel, s_cancel_ctx, &w->response);
        int code = 0;
        if (err == ESP_OK && w->response.bytes < 4096) {
            rewind(archive); size_t n = fread(w->json, 1, sizeof(w->json) - 1, archive); w->json[n] = 0;
            cJSON *r = wr_parse_json(w->json,strlen(w->json)+1), *c = cJSON_GetObjectItem(r, "errcode");
            if (!c) c = cJSON_GetObjectItem(r, "ret");
            if (cJSON_IsNumber(c)) code = c->valueint;
            cJSON_Delete(r);
        }
        if (err == ESP_OK && code == -2063) err = WEREAD_ERR_VERSION;
        else if (err == ESP_OK && w->response.status == 402) err = WEREAD_ERR_PURCHASE;
        else if (err == ESP_OK && w->response.status == 499) err = WEREAD_ERR_SOLD_OUT;
        else if (err == ESP_OK && w->response.status != 200) err = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGI("weread","chapter response result=%d http=%d bytes=%u key=%u code=%d",(int)err,w->response.status,(unsigned)w->response.bytes,(unsigned)strlen(w->response.encrypt_key),code);
        memset(&w->x,0,sizeof(w->x)); wr_copy(w->x.root, sizeof(w->x.root), w->root);
        if (err == ESP_OK && !strcmp(w->b.format, "epub")) {
            size_t n = 0;
            const weread_auth_session_t *s = weread_auth_get_current_session();
            const char *secrets[] = {s ? s->vid : "", s ? s->access_token : ""};
            if (!book_weread_decrypt_encrypt_key_candidates(w->response.encrypt_key, secrets, 2, w->password, sizeof(w->password), &n)) err = ESP_ERR_INVALID_RESPONSE;
            else err = book_weread_unpack_zip_file(archive, w->password, n, extract, &w->x);
            weread_secure_zero(w->password, sizeof(w->password));
        } else if (err == ESP_OK) err = book_weread_unpack_tar_file(archive, extract, &w->x);
        if (w->x.file) fclose(w->x.file);
        ESP_LOGI("weread","chapter unpack result=%d",(int)err);
        if (fclose(archive) && err == ESP_OK) err = ESP_FAIL;
        unlink(w->path);
        if (err != ESP_OK && w->x.error != ESP_OK) err = w->x.error;
        if (err == ESP_OK) err = assemble(w->root, id, uid, v, w->b.format);
        ESP_LOGI("weread","chapter assembly result=%d",(int)err);
        cJSON *m = NULL;
        if (err == ESP_OK) {
            m = cJSON_CreateObject();
            if (!m || !wr_set_owner(m) || !cJSON_AddStringToObject(m, "root", w->leaf) ||
                !cJSON_AddNumberToObject(m, "bookVersion", v) || !cJSON_AddNumberToObject(m, "chapterUid", uid)) err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK && cancelled()) err = ESP_ERR_INVALID_STATE;
        bool published = false;
        if (err == ESP_OK && common) {
            wr_cache_path(id, "resources.json", w->path, sizeof(w->path)); err = wr_write_json(w->path, m);
            published = err == ESP_OK;
        }
        if (err == ESP_OK) {
            char manifest_leaf[64]; snprintf(manifest_leaf, sizeof(manifest_leaf), "chapter-%u.json", (unsigned)uid);
            wr_cache_path(id, manifest_leaf, w->path, sizeof(w->path)); err = wr_write_json(w->path, m);
        }
        cJSON_Delete(m);
        if (err == ESP_OK) return ESP_OK;
        if (!published) remove_tree(w->root, 0);
        if (err != WEREAD_ERR_VERSION || attempt) return err;
        err = weread_shelf_refresh_book(id); if (err == ESP_OK) err = weread_toc_sync(id);
        if (err != ESP_OK) return err;
    }
    return WEREAD_ERR_VERSION;
}
esp_err_t weread_chapter_load(const char *id, uint32_t uid, char **text, size_t *len) { WR_GUARD;
    if (!text || !len) return ESP_ERR_INVALID_ARG;
    *text = NULL; *len = 0;
    esp_err_t err = weread_chapter_download(id, uid);
    return err==ESP_OK?weread_chapter_load_cached(id,uid,text,len):err;
}
esp_err_t weread_chapter_load_cached(const char *id,uint32_t uid,char **text,size_t *len){WR_GUARD;
    if(!text||!len)return ESP_ERR_INVALID_ARG;
    *text=NULL;*len=0;
    char *path __attribute__((cleanup(wr_release)))=wr_alloc(1100);
    if(!path)return ESP_ERR_NO_MEM;
    esp_err_t err = weread_chapter_directory(id, uid, path, 1024);
    if(err==ESP_OK)strcat(path,"/body.html");
    return err == ESP_OK ? read_file(path, text, len) : err;
}
