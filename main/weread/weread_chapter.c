/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：下载归档先写 TF，4 KiB 流式解包到独立目录，按 info.txt 顺序组装。
 * English: Spool archives to TF, extract in 4 KiB chunks to a private directory and follow info.txt order.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Oz")
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
#include <inttypes.h>
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
#if defined(ESP_PLATFORM)
#include "esp_vfs_fat.h"
#include "esp_timer.h"
#include "read_pico_sd.h"
#if defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
#include "read_pico_sd_access.h"
#endif
#else
#include <sys/statvfs.h>
#endif

#define CHAPTER_LIMIT (4u * 1024u * 1024u)
#define ARCHIVE_LIMIT (32u * 1024u * 1024u)
static weread_cancel_fn s_cancel;
static void *s_cancel_ctx;
void weread_chapter_set_cancel(weread_cancel_fn fn, void *ctx) { s_cancel = fn; s_cancel_ctx = ctx; }
static bool cancelled(void) { return s_cancel && s_cancel(s_cancel_ctx); }
static uint64_t number(const cJSON *o, const char *key) { uint64_t n = 0; wr_uint(o, key, &n); return n; }

static bool has_uri_scheme_or_drive(const char *s) {
    if (!s) return false;
    unsigned char c = (unsigned char)s[0];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))) return false;
    for (size_t i = 1; s[i]; ++i) {
        c = (unsigned char)s[i];
        if (c == ':') return true;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.'))
            return false;
    }
    return false;
}
static bool valid_archive_name(const char *name) {
    if (!name || !*name || name[0] == '/' || strchr(name, '\\') || has_uri_scheme_or_drive(name)) return false;
    const char *p = name;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (!len || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return false;
        if (!slash) break;
        p = slash + 1;
        if (!*p) return false;
    }
    return true;
}
static bool fat_safe_path(const char *p) {
    if (!p || !*p) return false;
    size_t total = strlen(p);
    if (!total || total > 200 || p[0] == '/' || p[total - 1] == '/') return false;
    const char *seg = p;
    bool first = true;
    while (*seg) {
        const char *slash = strchr(seg, '/');
        size_t len = slash ? (size_t)(slash - seg) : strlen(seg);
        if (!len || len > 180) return false;
        if (first && len == 3 && !strncasecmp(seg, "_wr", 3)) return false;
        if (seg[0] == '.' || seg[0] == ' ' || seg[len - 1] == '.' || seg[len - 1] == ' ') return false;
        for (size_t i = 0; i < len;) {
            uint8_t c = (uint8_t)seg[i];
            if (c < 0x20u || c == 0x7Fu || strchr("*:<>|\"?\\", (int)c)) return false;
            if (c < 0x80u) { ++i; continue; }
            uint8_t n = c < 0xC2u ? 0u : c < 0xE0u ? 2u : c < 0xF0u ? 3u : c < 0xF5u ? 4u : 0u;
            if (!n || len - i < n) return false;
            for (uint8_t k = 1; k < n; ++k) if (((uint8_t)seg[i + k] & 0xC0u) != 0x80u) return false;
            i += n;
        }
        if (!slash) break;
        seg = slash + 1;
        first = false;
    }
    return true;
}
static uint64_t fnv64_salted(const char *s, uint64_t seed, uint8_t salt) {
    uint64_t h = seed;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        h = (h ^ (uint8_t)(*p ^ salt)) * UINT64_C(0x100000001b3);
    return h;
}
const char *weread_chapter_storage_path(const char *normalized, char mapped[48]) {
    if (!normalized || !*normalized || !mapped) return NULL;
    if (fat_safe_path(normalized)) return normalized;
    uint64_t h1 = fnv64_salted(normalized, UINT64_C(0xcbf29ce484222325), 0);
    uint64_t h2 = fnv64_salted(normalized, UINT64_C(0x9e3779b97f4a7c15), 0x5au);
    size_t n = strlen(normalized);
    bool css = n >= 4 && !strcasecmp(normalized + n - 4, ".css");
    snprintf(mapped, 48, "_wr/%016" PRIx64 "%016" PRIx64 "%s", h1, h2, css ? ".css" : "");
    return mapped;
}
static int hex_nibble(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool weread_resource_path_decode(const char *normalized, char *out, size_t cap) {
    if (!normalized || !out || !cap || !strchr(normalized, '%')) return false;
    char tmp[512];
    size_t used = 0;
    bool changed = false;
    for (size_t i = 0; normalized[i]; ++i) {
        unsigned char c = (unsigned char)normalized[i];
        if (c == '%') {
            int hi = hex_nibble((unsigned char)normalized[i + 1]);
            int lo = hi >= 0 ? hex_nibble((unsigned char)normalized[i + 2]) : -1;
            if (hi < 0 || lo < 0) return false;
            c = (unsigned char)((hi << 4) | lo);
            if (!c || c == '/' || c == '\\') return false;
            changed = true;
            i += 2;
        }
        if (used + 1 >= sizeof(tmp)) return false;
        tmp[used++] = (char)c;
    }
    tmp[used] = 0;
    if (!changed || !valid_archive_name(tmp) || used >= cap) return false;
    memcpy(out, tmp, used + 1);
    return true;
}

bool weread_resource_path(const char *base, const char *relative, char *out, size_t cap) {
    if (!out || !cap || !relative || !*relative || *relative == '/' || has_uri_scheme_or_drive(relative) || strchr(relative, '\\')) return false;
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
static esp_err_t join_path(const char *dir, const char *leaf, char *out, size_t cap) {
    if (!dir || !leaf || !out || !cap) return ESP_ERR_INVALID_ARG;
    size_t dlen = strlen(dir);
    size_t llen = strlen(leaf);
    if (dlen >= cap || llen + 2 > cap - dlen) return ESP_ERR_INVALID_SIZE;
    memcpy(out, dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, leaf, llen + 1);
    return ESP_OK;
}
static char *wr_dup_str(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *p = wr_alloc(len + 1);
    if (p) memcpy(p, s, len + 1);
    return p;
}
#if defined(ESP_PLATFORM)
static bool sd_path(const char *p) {
    return p && !strncmp(p, "/sdcard", 7) && (!p[7] || p[7] == '/');
}
static bool sd_io_ready_for(const char *p) {
    return !sd_path(p) || read_pico_sd_io_ready();
}
#else
static bool sd_io_ready_for(const char *p) {
    (void)p;
    return true;
}
#endif
typedef struct {
    char *path;
    uint16_t depth;
    bool visited;
} wr_dir_frame_t;
static esp_err_t remove_tree_iterative(const char *root_path, uint64_t *freed_bytes,
    weread_cancel_fn cancel_fn, void *cancel_ctx) {
    if (!root_path || !*root_path) return ESP_ERR_INVALID_ARG;
    if (cancel_fn && cancel_fn(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    struct stat st;
    if (lstat(root_path, &st)) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode)) {
        if (book_weread_directory_in_use(root_path)) return ESP_ERR_INVALID_STATE;
        if (unlink(root_path) == 0) {
            if (freed_bytes && st.st_size > 0) *freed_bytes += (uint64_t)st.st_size;
            return ESP_OK;
        }
        return errno == ENOENT ? ESP_OK : ESP_FAIL;
    }
    if (book_weread_directory_in_use(root_path)) return ESP_ERR_INVALID_STATE;
    if (!sd_io_ready_for(root_path)) return ESP_ERR_INVALID_STATE;
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    read_pico_sd_access_scope_t sd_access __attribute__((cleanup(read_pico_sd_access_scope_end))) =
        read_pico_sd_access_scope(root_path);
    if (!sd_access.allowed) return ESP_ERR_INVALID_STATE;
#endif
    char *child = wr_alloc(1024);
    size_t stack_cap = 32, stack_top = 0;
    wr_dir_frame_t *stack = wr_alloc(stack_cap * sizeof(*stack));
    if (!child || !stack) {
        free(child); free(stack);
        return ESP_ERR_NO_MEM;
    }
    stack[0].path = wr_dup_str(root_path);
    stack[0].depth = 0;
    stack[0].visited = false;
    if (!stack[0].path) {
        free(child); free(stack);
        return ESP_ERR_NO_MEM;
    }
    stack_top = 1;
    bool had_skipped = false;
    esp_err_t err = ESP_OK;
    while (stack_top > 0) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        wr_dir_frame_t *top = &stack[stack_top - 1];
        if (top->visited) {
            char *dir_path = top->path;
            --stack_top;
            if (rmdir(dir_path) != 0 && errno != ENOENT) {
                if (!had_skipped || (errno != ENOTEMPTY && errno != EEXIST)) err = ESP_FAIL;
            }
            free(dir_path);
            continue;
        }
        if (book_weread_directory_in_use(top->path)) {
            had_skipped = true;
            free(top->path);
            --stack_top;
            continue;
        }
        top->visited = true;
        uint16_t cur_depth = top->depth;
        size_t parent_idx = stack_top - 1;
        DIR *dir = opendir(top->path);
        if (!dir) {
            if (errno != ENOENT) err = ESP_FAIL;
            free(top->path);
            --stack_top;
            continue;
        }
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (join_path(stack[parent_idx].path, entry->d_name, child, 1024) != ESP_OK) {
                err = ESP_ERR_INVALID_SIZE;
                continue;
            }
            struct stat cst;
            if (lstat(child, &cst)) {
                if (errno != ENOENT) err = ESP_FAIL;
                continue;
            }
            if (!S_ISDIR(cst.st_mode)) {
                if (unlink(child) == 0) {
                    if (freed_bytes && cst.st_size > 0) *freed_bytes += (uint64_t)cst.st_size;
                } else if (errno != ENOENT) {
                    err = ESP_FAIL;
                }
            } else if (book_weread_directory_in_use(child)) {
                had_skipped = true;
            } else if (cur_depth >= 32) {
                had_skipped = true;
            } else {
                if (stack_top == stack_cap) {
                    if (stack_cap >= 4096) { err = ESP_ERR_INVALID_SIZE; break; }
                    size_t next_cap = stack_cap * 2;
                    wr_dir_frame_t *bigger = wr_realloc(stack, next_cap * sizeof(*stack));
                    if (!bigger) { err = ESP_ERR_NO_MEM; break; }
                    stack = bigger;
                    stack_cap = next_cap;
                }
                char *sub = wr_dup_str(child);
                if (!sub) { err = ESP_ERR_NO_MEM; break; }
                stack[stack_top].path = sub;
                stack[stack_top].depth = (uint16_t)(cur_depth + 1);
                stack[stack_top].visited = false;
                ++stack_top;
            }
        }
        closedir(dir);
        if (err != ESP_OK && err != ESP_FAIL) break;
    }
    while (stack_top > 0) free(stack[--stack_top].path);
    free(stack);
    free(child);
    return err;
}
static void remove_tree(const char *path, unsigned depth) {
    (void)depth;
    (void)remove_tree_iterative(path, NULL, NULL, NULL);
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
esp_err_t weread_chapter_snapshot(const char *id,uint32_t uid,char *root,size_t cap,char *resources,size_t resources_cap,uint32_t *v){WR_GUARD;
    if(!root||!cap||!resources||!resources_cap||!v)return ESP_ERR_INVALID_ARG;
    cJSON *m=NULL;esp_err_t err=manifest(id,uid,false,&m);
    if(err==ESP_OK)err=wr_cache_path(id,wr_string(m,"root"),root,cap);
    struct stat st;
    if(err==ESP_OK&&(stat(root,&st)||!S_ISDIR(st.st_mode)))err=ESP_ERR_NOT_FOUND;
    if(err==ESP_OK)*v=(uint32_t)number(m,"bookVersion");
    cJSON_Delete(m);resources[0]=0;
    if(err==ESP_OK){esp_err_t res=directory(id,0,true,resources,resources_cap);if(res!=ESP_OK)resources[0]=0;if(res!=ESP_OK&&res!=ESP_ERR_NOT_FOUND)err=res;}
    return err;
}
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
esp_err_t weread_chapter_read_validated(const char *path,char **out,size_t *len){WR_GUARD;
    if(!path||!out||!len)return ESP_ERR_INVALID_ARG;
    return read_file(path,out,len);
}
typedef struct { char root[512], name[512], parent[1024], last_path[1024]; FILE *file; esp_err_t error; unsigned completed; char vbuf[4096]; } extract_t;
static bool extract(void *ctx, const char *name, const uint8_t *bytes, size_t n, size_t offset, size_t total) {
    extract_t *x = ctx;
    if (cancelled()) return false;
    weread_client_yield_for_display(s_cancel, s_cancel_ctx);
    if (!offset) {
        if (x->file) { x->error = ESP_ERR_INVALID_RESPONSE; goto fail; }
        char path[1024], mapped[48];
        const char *disk = !strcmp(name, "body.html") || !strcmp(name, "position.map") ||
            !strcmp(name, "archive.part") || !valid_archive_name(name)
            ? NULL : weread_chapter_storage_path(name, mapped);
        if (!disk || join(x->root, disk, path, sizeof(path)) != ESP_OK) {
            x->error = ESP_ERR_INVALID_RESPONSE;
#if defined(ESP_PLATFORM)
            ESP_LOGW("weread", "extract reject name=%.64s", name);
#endif
            return false;
        }
        char *slash = strrchr(path, '/'); *slash = 0;
        if(strcmp(x->parent,path)){
            x->error=wr_mkdirs(path);if(x->error!=ESP_OK){x->parent[0]=0;goto fail;}
            if(!wr_copy(x->parent,sizeof(x->parent),path)){x->error=ESP_ERR_INVALID_SIZE;goto fail;}
        }
        *slash='/';
        struct stat st;
        if (!stat(path, &st)) { x->error = ESP_ERR_INVALID_RESPONSE; goto fail; }
        wr_copy(x->last_path, sizeof(x->last_path), path);
        x->file = fopen(path, "wb"); if (!x->file) { x->error = ESP_FAIL; goto fail; }
        (void)setvbuf(x->file, x->vbuf, _IOFBF, sizeof(x->vbuf));
        wr_copy(x->name, sizeof(x->name), name);
    }
    if (!x->file || strcmp(name, x->name) || fwrite(bytes, 1, n, x->file) != n) { x->error = ESP_FAIL; goto fail; }
    if (offset + n == total) {
        bool ok = !fflush(x->file); if (fclose(x->file)) ok = false; x->file = NULL;
        if (!ok) { x->error = ESP_FAIL; goto fail; }
        ++x->completed;
        x->last_path[0] = 0;
    }
    return true;
fail:
#if defined(ESP_PLATFORM)
    ESP_LOGW("weread", "extract err=%d name=%.64s", (int)x->error, name);
#endif
    return false;
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
static bool merged_file(const cJSON *files, const char *path) {
    cJSON *file; char normalized[512], decoded[512];
    cJSON_ArrayForEach(file,files) {
        if(!cJSON_IsString(file)||!weread_resource_path(NULL,file->valuestring,normalized,sizeof(normalized)))continue;
        if(!strcmp(normalized,path))return true;
        if(strchr(normalized,'%')&&weread_resource_path_decode(normalized,decoded,sizeof(decoded))&&!strcmp(decoded,path))return true;
    }
    return false;
}
static void note_id(char out[24], const char *source, const char *fragment) {
    uint64_t hash=UINT64_C(14695981039346656037);
    const char *parts[]={source,fragment};
    for(unsigned i=0;i<2;++i) {
        const unsigned char *p=(const unsigned char *)parts[i];
        do {hash=(hash^*p)*UINT64_C(1099511628211);}while(*p++);
    }
    // 保留解析器对noteref回链的过滤。/ Retain the parser's noteref backlink filter.
    snprintf(out,24,"%s%016" PRIx64,!strncmp(fragment,"noteref",7)?"noteref":"w",hash);
}
static bool append_html(FILE *f, const char *data, size_t len, const char *source, const cJSON *files, size_t *total, position_writer_t *map) {
    size_t start = 0; bool tag = false;
    for (size_t i = 0; i < len; ++i) {
        if (data[i] == '<') tag = true;
        if (data[i] == '>') tag = false;
        if (!tag || !i || !isspace((unsigned char)data[i - 1])) continue;
        size_t k = len - i >= 3 && !strncasecmp(data + i, "src", 3) ? 3 :
            len - i >= 4 && !strncasecmp(data + i, "href", 4) ? 4 :
            len - i >= 2 && !strncasecmp(data + i, "id", 2) ? 2 : 0;
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
        char alias[24];
        if(k==2) {
            note_id(alias,source,raw);
            if(quote)++j;
            // 保留原id的CSS/语义，只给合并脚注添加隔离别名；新增属性不计原文UTF16。
            // Keep the original CSS/semantic id; add a document-bound note alias without counting synthetic UTF16 units.
            if(!append_source(f,data+start,j-start,total,map)||!append(f," data-wr-note=\"",15,total)||
                !append(f,alias,strlen(alias),total)||!append(f,"\"",1,total))return false;
            start=j;i=j-1;continue;
        }
        bool relative=raw[0]!='#'&&weread_resource_path(source,raw,normalized,sizeof(normalized));
        const char *fragment=k==4?strchr(raw,'#'):NULL;
        if(fragment&&fragment[1]) {
            char decoded[512];
            const char *target=NULL;
            if(raw[0]=='#'&&wr_copy(normalized,sizeof(normalized),source))target=normalized;
            else if(relative&&merged_file(files,normalized))target=normalized;
            else if(relative&&strchr(normalized,'%')&&weread_resource_path_decode(normalized,decoded,sizeof(decoded))&&merged_file(files,decoded))target=decoded;
            if(target) {
                note_id(alias,target,fragment+1);normalized[0]='#';strcpy(normalized+1,alias);
                fragment=NULL;relative=true;
            }
        }
        if (relative) {
            if (!append_source(f, data + start, value - start, total, map) ||
                !append(f, normalized, strlen(normalized), total)) return false;
            if (fragment && !append(f, fragment, strlen(fragment), total)) return false;
            map->units += weread_utf16_length(data + value, j - value);
            start = j;
        }
        i = j;
    }
    return append_source(f, data + start, len - start, total, map);
}
// 只识别短小且明确为空的body，复杂/过长内容交回普通解析，不分配第二份正文。/ Recognize only small unambiguously empty bodies, leaving complex/long content to normal parsing without another text allocation.
static bool empty_background_body(const char *p,const char *end){
    if(!p||p>end)return false;
    if(end-p>4096)return false;
    bool closed=false;
    while(p<end){
        if(isspace((unsigned char)*p)){++p;continue;}
        if(end-p>=4&&!memcmp(p,"<!--",4)){
            p+=4;while(end-p>=3&&memcmp(p,"-->",3))++p;
            if(end-p<3)return false;
            p+=3;continue;
        }
        if(*p++!='<')return false;
        bool close=p<end&&*p=='/';if(close)++p;
        const char *name=p;while(p<end&&isalpha((unsigned char)*p))++p;
        size_t n=(size_t)(p-name);
        bool body=n==4&&!strncasecmp(name,"body",4),html=n==4&&!strncasecmp(name,"html",4);
        bool wrapper=(n==1&&!strncasecmp(name,"p",1))||(n==3&&!strncasecmp(name,"div",3))||
            (n==4&&!strncasecmp(name,"span",4))||(n==2&&!strncasecmp(name,"br",2));
        if(closed?!(close&&html):!(wrapper||(close&&body)))return false;
        while(p<end&&isspace((unsigned char)*p))++p;
        if(!close&&p<end&&*p=='/')++p;
        if(p==end||*p++!='>')return false;
        if(close&&body)closed=true;
    }
    return closed;
}
// 仅补微信归档的空白背景图页，不改通用HTML/CSS或把装饰背景变成正文。/ Promote empty WeRead body backgrounds only, leaving generic HTML/CSS and decorative text backgrounds alone.
static esp_err_t background_image(const char *data,size_t len,const char *source,char out[512]){
    out[0]=0;const char *p=data,*end=data+(len>16384?16384:len),*style=NULL,*style_end=NULL,*body=NULL;
    while(p<end){
        if(*p++!='<')continue;
        if(end-p>=3&&!memcmp(p,"!--",3)){
            const char *close=p+3;
            while(end-close>=3&&memcmp(close,"-->",3))++close;
            if(end-close<3)return ESP_OK;
            p=close+3;continue;
        }
        const char *tag=p;char quote=0;
        while(p<end){char c=*p++;if(quote){if(c==quote)quote=0;}else if(c=='\''||c=='"')quote=c;else if(c=='>')break;}
        const char *tag_end=p;
        const char *skip=NULL;
        if(tag_end-tag>=5&&!strncasecmp(tag,"head",4)&&(isspace((unsigned char)tag[4])||tag[4]=='>'))skip="</head";
        else if(tag_end-tag>=7&&!strncasecmp(tag,"script",6)&&(isspace((unsigned char)tag[6])||tag[6]=='>'))skip="</script";
        else if(tag_end-tag>=6&&!strncasecmp(tag,"style",5)&&(isspace((unsigned char)tag[5])||tag[5]=='>'))skip="</style";
        if(skip){size_t n=strlen(skip);while(end-p>=(ptrdiff_t)n&&strncasecmp(p,skip,n))++p;if(end-p<(ptrdiff_t)n)return ESP_OK;continue;}
        if(tag_end-tag<5||strncasecmp(tag,"body",4)||(!isspace((unsigned char)tag[4])&&tag[4]!='>'))continue;
        body=tag_end;
        for(const char *a=tag+4;a<tag_end;){
            while(a<tag_end&&isspace((unsigned char)*a))++a;
            const char *key=a;while(a<tag_end&&!isspace((unsigned char)*a)&&*a!='='&&*a!='>')++a;
            size_t key_len=(size_t)(a-key);while(a<tag_end&&isspace((unsigned char)*a))++a;
            if(a==tag_end||*a!='='){if(a<tag_end)++a;continue;}++a;
            while(a<tag_end&&isspace((unsigned char)*a))++a;
            char q=a<tag_end&&(*a=='\''||*a=='"')?*a++:0;const char *value=a;
            while(a<tag_end&&(q?*a!=q:!isspace((unsigned char)*a)&&*a!='>'))++a;
            if(key_len==5&&!strncasecmp(key,"style",5)){style=value;style_end=a;break;}
            if(q&&a<tag_end)++a;
        }
        break;
    }
    if(!style||!empty_background_body(body,data+len))return ESP_OK;
    for(p=style;p<style_end;){
        while(p<style_end&&(isspace((unsigned char)*p)||*p==';'))++p;
        const char *key=p;while(p<style_end&&*p!=':'&&*p!=';')++p;
        const char *key_end=p;while(key_end>key&&isspace((unsigned char)key_end[-1]))--key_end;
        if(p==style_end||*p!=':')break;
        ++p;
        const char *value=p;while(p<style_end&&*p!=';')++p;
        size_t n=(size_t)(key_end-key);
        if(!((n==10&&!strncasecmp(key,"background",10))||(n==16&&!strncasecmp(key,"background-image",16))))continue;
        while(value<p&&isspace((unsigned char)*value))++value;
        if(p-value<5||strncasecmp(value,"url(",4))continue;
        value+=4;
        while(value<p&&isspace((unsigned char)*value))++value;
        char q=value<p&&(*value=='\''||*value=='"')?*value++:0;const char *last=value;
        while(last<p&&(q?*last!=q:*last!=')'))++last;
        if(last==p)continue;
        const char *close=last+(q?1:0);while(close<p&&isspace((unsigned char)*close))++close;
        if(close==p||*close!=')')continue;
        while(last>value&&isspace((unsigned char)last[-1]))--last;
        size_t length=(size_t)(last-value);if(!length||length>=512)continue;
        char raw[512];memcpy(raw,value,length);raw[length]=0;
        bool safe=true;for(size_t i=0;i<length;++i)if((unsigned char)raw[i]<32||strchr("\"'<>&",raw[i]))safe=false;
        if(!safe||!weread_resource_path(source,raw,out,512)){out[0]=0;continue;}
        return ESP_OK;
    }
    return ESP_OK;
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline))
#endif
static esp_err_t assemble(const char *root, const char *id, uint32_t uid, uint32_t version, const char *format) {
    char path[1024]; if (join(root, "info.txt", path, sizeof(path)) != ESP_OK) return ESP_ERR_INVALID_SIZE;
    cJSON *info = NULL; esp_err_t err = wr_read_json(path, &info);
    if (err != ESP_OK) ESP_LOGW("weread", "assemble info read err=%d", (int)err);
    const char *info_id = wr_string(info, "bookId");
    if (err == ESP_OK && (!info_id || strcmp(info_id, id))) {
        ESP_LOGW("weread", "assemble bookId mismatch info=%s want=%s", info_id ? info_id : "(null)", id);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && cJSON_HasObjectItem(info,"bookVersion") && number(info, "bookVersion") != version) err = WEREAD_ERR_VERSION;
    cJSON *top_files = cJSON_GetObjectItemCaseSensitive(info, "files");
    cJSON *files = top_files, *entry, *matched = NULL;
    cJSON *chapters = cJSON_GetObjectItemCaseSensitive(info, "chapters");
    if (cJSON_IsArray(chapters)) {
        files = NULL;
        cJSON_ArrayForEach(entry, chapters)
            if (number(entry, "chapterUid") == uid) { matched = entry; files = cJSON_GetObjectItemCaseSensitive(entry, "files"); break; }
        if (!files && cJSON_IsArray(top_files)) files = top_files;
    }
    if (err == ESP_OK && !files && matched && !strcmp(format, "txt")) {
        // 真实微信读书 TXT 章节包 info.txt 常省略 files，正文文件名固定为 <bookId>_<chapterUid>_o。
        // Real WeRead TXT archives often omit "files" in info.txt; the extracted file is <bookId>_<chapterUid>_o.
        char txt_leaf[96];
        snprintf(txt_leaf, sizeof(txt_leaf), "%s_%u_o", id, (unsigned)uid);
        cJSON *synth = cJSON_CreateArray();
        cJSON *item = synth ? cJSON_CreateString(txt_leaf) : NULL;
        if (!synth || !item || !cJSON_AddItemToArray(synth, item)) {
            cJSON_Delete(item);
            cJSON_Delete(synth);
            err = ESP_ERR_NO_MEM;
        } else {
            cJSON_AddItemToObject(matched, "files", synth);
            files = synth;
        }
    }
    if (err == ESP_OK && (!cJSON_IsArray(files) || cJSON_GetArraySize(files) < 1 || cJSON_GetArraySize(files) > 128)) {
        ESP_LOGW("weread", "assemble files invalid uid=%u fmt=%s files=%d",
            (unsigned)uid, format, cJSON_IsArray(files) ? cJSON_GetArraySize(files) : -1);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    FILE *f = NULL;
    position_writer_t map = {0};
    char *io_vbuf = NULL;
    if (err == ESP_OK) {
        io_vbuf = wr_alloc(8192);
        join(root, "body.html", path, sizeof(path)); f = fopen(path, "wb"); if (!f) err = ESP_FAIL;
        else if (io_vbuf) (void)setvbuf(f, io_vbuf, _IOFBF, 4096);
        join(root, "position.map", path, sizeof(path)); map.file = fopen(path, "wb");
        if (map.file && io_vbuf) (void)setvbuf(map.file, io_vbuf + 4096, _IOFBF, 4096);
        weread_position_header_t header = {0};
        if (!map.file || fwrite(&header, sizeof(header), 1, map.file) != 1) err = ESP_FAIL;
    }
    size_t total = 0;
    if (err == ESP_OK) cJSON_ArrayForEach(entry, files) {
        if (!cJSON_IsString(entry) || cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        weread_client_yield_for_display(s_cancel, s_cancel_ctx);
        char normalized[512], mapped[48];
        const char *disk = weread_resource_path(NULL, entry->valuestring, normalized, sizeof(normalized))
            ? weread_chapter_storage_path(normalized, mapped) : NULL;
        if (!disk || !strcmp(normalized, "body.html") || !strcmp(normalized, "position.map") ||
            join(root, disk, path, sizeof(path)) != ESP_OK) {
            ESP_LOGW("weread", "assemble bad path=%s", entry->valuestring);
            err = ESP_ERR_INVALID_RESPONSE; break;
        }
        char *data = NULL; size_t len = 0;
        err = read_file(path, &data, &len);
        if (err == ESP_ERR_NOT_FOUND && strchr(normalized, '%')) {
            char decoded[512];
            const char *dec_disk = weread_resource_path_decode(normalized, decoded, sizeof(decoded)) &&
                strcmp(decoded, "body.html") && strcmp(decoded, "position.map")
                ? weread_chapter_storage_path(decoded, mapped) : NULL;
            if (dec_disk && join(root, dec_disk, path, sizeof(path)) == ESP_OK) {
                err = read_file(path, &data, &len);
                if (err == ESP_OK) wr_copy(normalized, sizeof(normalized), decoded);
            }
        }
        if (err != ESP_OK) {
#if defined(ESP_PLATFORM)
            ESP_LOGW("weread", "assemble read err=%d file=%.64s", (int)err, normalized);
#endif
            break;
        }
        if (!strcmp(format, "txt")) {
            char env_vid[WEREAD_VID_MAX_LEN] = "";
            {
                WR_GUARD;
                const weread_auth_session_t *s = weread_auth_get_current_session();
                if (s) wr_copy(env_vid, sizeof(env_vid), s->vid);
            }
            uint8_t *plain = NULL; size_t plain_len = 0;
            if (env_vid[0] && book_weread_unwrap_envelope((const uint8_t *)data, len, env_vid, NULL, 0, &plain, &plain_len)) {
                free(data); data = (char *)plain; len = plain_len;
            }
            weread_secure_zero(env_vid, sizeof(env_vid));
        }
        char background[512]="";
        if(strcmp(format,"txt"))err=background_image(data,len,normalized,background);
        if(err==ESP_OK&&(!strcmp(format, "txt") ? !append_source(f, data, len, &total, &map) : !append_html(f, data, len, normalized, files, &total, &map))) err = ESP_ERR_INVALID_SIZE;
        // 合成图片不增加源UTF16坐标；映射锚点仍绑定原文件末尾。/ Synthetic images add no source UTF16 units; map anchors remain at the original file end.
        if(err==ESP_OK&&background[0]&&(!position_anchor(&map,total)||!append(f,"<img src=\"",10,&total)||
            !append(f,background,strlen(background),&total)||!append(f,"\"/>",3,&total)))err=ESP_ERR_INVALID_SIZE;
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
    free(io_vbuf);
    cJSON_Delete(info); return err;
}
esp_err_t weread_chapter_download(const char *id, uint32_t uid) { return weread_chapter_download_ex(id, uid, false); }
// 大型路径/响应/解包缓冲可回收，网络任务只保留小内部栈。/ Reclaim large path/response/extraction buffers while keeping a small internal network stack.
typedef struct {
    weread_book_t b;
    char leaf[128],root[512],parent[512],path[600],url[1024],json[4096];
    weread_download_info_t response;
    extract_t x;
    uint8_t password[256];
    char archive_vbuf[8192];
} chapter_download_work_t;
static void release_chapter_work(chapter_download_work_t **p) {
    if(*p)weread_secure_zero(*p,sizeof(**p));
    wr_release(p);
}
bool weread_chapter_cached(const char *id,uint32_t uid){WR_GUARD;
    if(!wr_valid_id(id)||!uid)return false;
    struct { char existing[512], cache_path[600]; } *paths __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*paths));
    if(!paths)return false;
    if (weread_chapter_directory(id, uid, paths->existing, sizeof(paths->existing)) == ESP_OK) {
        struct stat st={0}; join(paths->existing, "body.html", paths->cache_path, sizeof(paths->cache_path));
        bool body_ok = !stat(paths->cache_path, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= CHAPTER_LIMIT;
        off_t body_size=st.st_size;
        join(paths->existing, "position.map", paths->cache_path, sizeof(paths->cache_path));
        if (!body_ok || stat(paths->cache_path, &st) || !S_ISREG(st.st_mode)) return false;
        FILE *f=fopen(paths->cache_path,"rb");if(!f)return false;
        weread_position_header_t h={0};bool ok=fread(&h,sizeof(h),1,f)==1;
        if(fclose(f))ok=false;
        return ok&&h.magic==WEREAD_POSITION_MAGIC&&h.bytes==(uint32_t)body_size&&h.count&&h.count<=16384&&
            st.st_size==(off_t)(sizeof(h)+(size_t)h.count*sizeof(weread_position_point_t));
    }
    return false;
}
esp_err_t weread_chapter_storage_info(uint64_t *free_bytes, uint64_t *total_bytes){WR_GUARD;
    if(free_bytes)*free_bytes=0;
    if(total_bytes)*total_bytes=0;
    const char *root=weread_auth_get_storage_root();
#if defined(ESP_PLATFORM)
    if(!root||root[0]!='/')return ESP_ERR_INVALID_ARG;
    if(!sd_io_ready_for(root))return ESP_ERR_INVALID_STATE;
    const char *end=strchr(root+1,'/');
    char mount[32];size_t n=end?(size_t)(end-root):strlen(root);
    if(!n||n>=sizeof(mount))return ESP_ERR_INVALID_ARG;
    memcpy(mount,root,n);mount[n]=0;
    uint64_t total=0,avail=0;esp_err_t err=esp_vfs_fat_info(mount,&total,&avail);
    if(err!=ESP_OK)return err;
    if(free_bytes)*free_bytes=avail;
    if(total_bytes)*total_bytes=total;
    return ESP_OK;
#else
    if(!root||!*root)return ESP_ERR_INVALID_ARG;
    struct statvfs info;memset(&info,0,sizeof(info));
    if(statvfs(root,&info))return ESP_FAIL;
    uint64_t avail=0,total=0;
    if(info.f_frsize&&info.f_bavail>UINT64_MAX/info.f_frsize)avail=UINT64_MAX;
    else avail=(uint64_t)info.f_bavail*info.f_frsize;
    if(info.f_frsize&&info.f_blocks>UINT64_MAX/info.f_frsize)total=UINT64_MAX;
    else total=(uint64_t)info.f_blocks*info.f_frsize;
    if(free_bytes)*free_bytes=avail;
    if(total_bytes)*total_bytes=total;
    return ESP_OK;
#endif
}
esp_err_t weread_chapter_prefetch_space(uint64_t *available){WR_GUARD;
    if(available)*available=0;
    uint64_t free_bytes=0;
    esp_err_t err=weread_chapter_storage_info(&free_bytes,NULL);
    if(err!=ESP_OK)return err;
    if(available)*available=free_bytes;
    // 32MiB归档和64MiB展开同时存在，另留4MiB正文、映射及事务余量。/ Archive32MiB and expanded64MiB coexist; retain body4MiB plus map/transaction slack.
    return free_bytes>=104u*1024u*1024u?ESP_OK:WEREAD_ERR_STORAGE;
}
esp_err_t weread_chapter_download_ex(const char *id, uint32_t uid, bool preload) { WR_MEMORY_SCOPE;
    if (!wr_valid_id(id) || !uid) return ESP_ERR_INVALID_ARG;
    if(weread_chapter_cached(id,uid))return ESP_OK;
    if(!weread_auth_is_authenticated())return ESP_ERR_INVALID_STATE;
    if(preload){esp_err_t space=weread_chapter_prefetch_space(NULL);if(space!=ESP_OK)return space;}
    char existing[512];
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
        (void)setvbuf(archive, w->archive_vbuf, _IOFBF, sizeof(w->archive_vbuf));
        snprintf(w->url, sizeof(w->url), "%s/book/chapterdownload?bookId=%s&chapters=%s%u&pf=wechat_wx-2001-android-100-weread&pfkey=pfKey&zoneId=1&bookVersion=%u&bookType=%s&release=1&stopAutoPayWhenBNE=1&preload=%u&preview=0&offline=0",
            weread_client_get_base_url(), id, common ? "0," : "", (unsigned)uid, (unsigned)v, w->b.format, preload ? 1u : 0u);
#if defined(ESP_PLATFORM)
        int64_t started=esp_timer_get_time();
#endif
        err = weread_client_download(w->url, archive, ARCHIVE_LIMIT, s_cancel, s_cancel_ctx, &w->response);
#if defined(ESP_PLATFORM)
        int64_t downloaded=esp_timer_get_time();
#endif
        int code = 0;
        if (err == ESP_OK && w->response.bytes < 4096) {
            rewind(archive); size_t n = fread(w->json, 1, sizeof(w->json) - 1, archive); w->json[n] = 0;
            cJSON *r = wr_parse_json(w->json,strlen(w->json)+1), *c = cJSON_GetObjectItem(r, "errcode");
            if (!c) c = cJSON_GetObjectItem(r, "errCode");
            if (!c) c = cJSON_GetObjectItem(r, "ret");
            if (cJSON_IsNumber(c)) code = c->valueint;
            cJSON_Delete(r);
        }
        if (err == ESP_OK && code == -2063) err = WEREAD_ERR_VERSION;
        else if (err == ESP_OK && w->response.status == 402) err = WEREAD_ERR_PURCHASE;
        else if (err == ESP_OK && w->response.status == 499) err = WEREAD_ERR_SOLD_OUT;
        else if (err == ESP_OK && w->response.status != 200) err = ESP_ERR_INVALID_RESPONSE;
        ESP_LOGI("weread","chapter result=%d http=%d bytes=%u",(int)err,w->response.status,(unsigned)w->response.bytes);
        memset(&w->x,0,sizeof(w->x)); wr_copy(w->x.root, sizeof(w->x.root), w->root);
        if (err == ESP_OK && !strcmp(w->b.format, "epub")) {
            char sec_vid[WEREAD_VID_MAX_LEN] = "", sec_tok[WEREAD_TOKEN_MAX_LEN] = "";
            {
                WR_GUARD;
                const weread_auth_session_t *s = weread_auth_get_current_session();
                if (s) {
                    wr_copy(sec_vid, sizeof(sec_vid), s->vid);
                    wr_copy(sec_tok, sizeof(sec_tok), s->access_token);
                }
            }
            const char *secrets[] = {sec_vid, sec_tok};
            err = ESP_ERR_INVALID_RESPONSE;
            for (size_t k = 0; k < 2; ++k) {
                size_t n = 0;
                if (!secrets[k][0] ||
                    !book_weread_decrypt_encrypt_key(w->response.encrypt_key, secrets[k], w->password, sizeof(w->password), &n)) {
                    continue;
                }
                memset(&w->x, 0, sizeof(w->x));
                wr_copy(w->x.root, sizeof(w->x.root), w->root);
                err = book_weread_unpack_zip_file(archive, w->password, n, extract, &w->x);
                if (w->x.file) { fclose(w->x.file); w->x.file = NULL; }
                if (err == ESP_OK) break;
                if (err == ESP_ERR_INVALID_STATE && !cancelled() && w->x.error == ESP_OK && !w->x.completed && k + 1 < 2) {
                    if (w->x.last_path[0]) { unlink(w->x.last_path); w->x.last_path[0] = 0; }
                    continue;
                }
                break;
            }
            weread_secure_zero(sec_vid, sizeof(sec_vid));
            weread_secure_zero(sec_tok, sizeof(sec_tok));
            weread_secure_zero(w->password, sizeof(w->password));
        } else if (err == ESP_OK) err = book_weread_unpack_tar_file(archive, extract, &w->x);
        if (w->x.file) { fclose(w->x.file); w->x.file = NULL; }
        if (fclose(archive) && err == ESP_OK) err = ESP_FAIL;
        unlink(w->path);
        if (err != ESP_OK && w->x.error != ESP_OK) err = w->x.error;
#if defined(ESP_PLATFORM)
        int64_t unpacked=esp_timer_get_time();
#endif
        if (err == ESP_OK) err = assemble(w->root, id, uid, v, w->b.format);
#if defined(ESP_PLATFORM)
        int64_t assembled=esp_timer_get_time();
#endif
        bool published = false;
        if (err == ESP_OK) {
            WR_GUARD;
            cJSON *m = cJSON_CreateObject();
            if (!m || !wr_set_owner(m) || !cJSON_AddStringToObject(m, "root", w->leaf) ||
                !cJSON_AddNumberToObject(m, "bookVersion", v) || !cJSON_AddNumberToObject(m, "chapterUid", uid)) err = ESP_ERR_NO_MEM;
            if (err == ESP_OK && cancelled()) err = ESP_ERR_INVALID_STATE;
            if (err == ESP_OK && common) {
                wr_cache_path(id, "resources.json", w->path, sizeof(w->path)); err = wr_write_json(w->path, m);
                published = err == ESP_OK;
            }
            if (err == ESP_OK) {
                char manifest_leaf[64]; snprintf(manifest_leaf, sizeof(manifest_leaf), "chapter-%u.json", (unsigned)uid);
                wr_cache_path(id, manifest_leaf, w->path, sizeof(w->path)); err = wr_write_json(w->path, m);
            }
            cJSON_Delete(m);
        }
#if defined(ESP_PLATFORM)
        ESP_LOGI("weread","chapter timing download=%u unpack=%u assemble=%u commit=%u result=%d preload=%d",
            (unsigned)((downloaded-started)/1000),(unsigned)((unpacked-downloaded)/1000),
            (unsigned)((assembled-unpacked)/1000),(unsigned)((esp_timer_get_time()-assembled)/1000),(int)err,preload);
#endif
        if (err == ESP_OK) return ESP_OK;
        if (!published) remove_tree(w->root, 0);
        if (err != WEREAD_ERR_VERSION || attempt) return err;
        book_weread_clear_prepared();
        err = weread_shelf_refresh_book(id); if (err == ESP_OK) err = weread_toc_sync_version(id,weread_shelf_book_version(id));
        if (err != ESP_OK) return err;
        // 原生拒绝旧版且新元数据不给版本时，必须重新开书核对，不能重标当前原文/待报坐标。
        // If the native endpoint rejects the held version and fresh metadata cannot name one, reopen instead of relabeling active text/pending coordinates.
        uint32_t held_version=0;
        if(!weread_shelf_book_version(id)&&book_weread_cached_version(id,&held_version)&&held_version)return WEREAD_ERR_VERSION;
    }
    return WEREAD_ERR_VERSION;
}
esp_err_t weread_chapter_load(const char *id, uint32_t uid, char **text, size_t *len) {
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

typedef enum {
    CACHE_KEEP = 0,
    CACHE_DEL_CHAPTER_DIR,
    CACHE_DEL_CHAPTER_FILE,
    CACHE_DEL_COVER_TEMP,
} cache_entry_kind_t;

static bool has_prefix_dot_or_end(const char *s, const char *base) {
    size_t len = strlen(base);
    return !strncmp(s, base, len) && (s[len] == '\0' || s[len] == '.');
}

static bool is_digits_followed_by(const char *p, const char *ext) {
    if (!p || !isdigit((unsigned char)*p)) return false;
    while (isdigit((unsigned char)*p)) ++p;
    return has_prefix_dot_or_end(p, ext);
}

static bool parse_chapter_manifest_uid(const char *name, uint32_t *out_uid) {
    if (!name || strncmp(name, "chapter-", 8) != 0) return false;
    const char *p = name + 8;
    if (!isdigit((unsigned char)*p)) return false;
    uint64_t val = 0;
    while (isdigit((unsigned char)*p)) {
        val = val * 10u + (uint64_t)(*p - '0');
        if (val > UINT32_MAX) return false;
        ++p;
    }
    if (strcmp(p, ".json") != 0 || val == 0) return false;
    *out_uid = (uint32_t)val;
    return true;
}

static cache_entry_kind_t classify_cache_entry(const char *name, bool is_dir) {
    if (!name || !*name) return CACHE_KEEP;
    if (is_dir) {
        if (!strcmp(name, "chapters") || !strcmp(name, "res")) return CACHE_DEL_CHAPTER_DIR;
        return CACHE_KEEP;
    }
    if (!strncmp(name, "chapter-", 8)) {
        if (is_digits_followed_by(name + 8, ".json")) return CACHE_DEL_CHAPTER_FILE;
        const char *dot = strrchr(name + 8, '.');
        if (dot && (!strcmp(dot, ".html") || !strcmp(dot, ".xhtml") || !strcmp(dot, ".part"))) {
            return CACHE_DEL_CHAPTER_FILE;
        }
        const char *h = strstr(name + 8, ".html");
        if (h && (h[5] == '\0' || h[5] == '.')) return CACHE_DEL_CHAPTER_FILE;
        const char *xh = strstr(name + 8, ".xhtml");
        if (xh && (xh[6] == '\0' || xh[6] == '.')) return CACHE_DEL_CHAPTER_FILE;
        const char *pt = strstr(name + 8, ".part");
        if (pt && (pt[5] == '\0' || pt[5] == '.')) return CACHE_DEL_CHAPTER_FILE;
        return CACHE_KEEP;
    }
    if (has_prefix_dot_or_end(name, "resources.json") ||
        !strncmp(name, "archive.", 8) ||
        !strncmp(name, "chapter_", 8)) {
        return CACHE_DEL_CHAPTER_FILE;
    }
    if (!strncmp(name, "cover-", 6)) {
        const char *dot = strchr(name + 6, '.');
        if (dot && (has_prefix_dot_or_end(dot, ".img") || has_prefix_dot_or_end(dot, ".failed"))) {
            return CACHE_DEL_COVER_TEMP;
        }
        return CACHE_KEEP;
    }
    if (has_prefix_dot_or_end(name, "cover.img") || has_prefix_dot_or_end(name, "cover.failed")) {
        return CACHE_DEL_COVER_TEMP;
    }
    return CACHE_KEEP;
}

static bool is_preserved_cover_file(const char *name) {
    if (!name || !*name) return false;
    if (!strcmp(name, "cover.gray")) return true;
    if (!strncmp(name, "cover-", 6)) {
        const char *dot = strchr(name + 6, '.');
        return dot && has_prefix_dot_or_end(dot, ".gray");
    }
    return false;
}

static esp_err_t stat_tree_iterative(const char *root_path, uint64_t *bytes_out,
    weread_cancel_fn cancel_fn, void *cancel_ctx) {
    if (!root_path || !*root_path || !bytes_out) return ESP_ERR_INVALID_ARG;
    if (cancel_fn && cancel_fn(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    struct stat st;
    if (lstat(root_path, &st)) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode)) {
        if (st.st_size > 0) *bytes_out += (uint64_t)st.st_size;
        return ESP_OK;
    }
    char *child = wr_alloc(1024);
    size_t stack_cap = 32, stack_top = 0;
    wr_dir_frame_t *stack = wr_alloc(stack_cap * sizeof(*stack));
    if (!child || !stack) {
        free(child); free(stack);
        return ESP_ERR_NO_MEM;
    }
    stack[0].path = wr_dup_str(root_path);
    stack[0].depth = 0;
    stack[0].visited = false;
    if (!stack[0].path) {
        free(child); free(stack);
        return ESP_ERR_NO_MEM;
    }
    stack_top = 1;
    esp_err_t err = ESP_OK;
    while (stack_top > 0) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        wr_dir_frame_t frame = stack[--stack_top];
        DIR *dir = opendir(frame.path);
        if (!dir) {
            if (errno != ENOENT) err = ESP_FAIL;
            free(frame.path);
            continue;
        }
        struct dirent *entry;
        while ((entry = readdir(dir))) {
            if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (join_path(frame.path, entry->d_name, child, 1024) != ESP_OK) {
                err = ESP_ERR_INVALID_SIZE;
                continue;
            }
            struct stat cst;
            if (lstat(child, &cst)) {
                if (errno != ENOENT) err = ESP_FAIL;
                continue;
            }
            if (!S_ISDIR(cst.st_mode)) {
                if (cst.st_size > 0) *bytes_out += (uint64_t)cst.st_size;
            } else if (frame.depth < 32) {
                if (stack_top == stack_cap) {
                    if (stack_cap >= 4096) { err = ESP_ERR_INVALID_SIZE; break; }
                    size_t next_cap = stack_cap * 2;
                    wr_dir_frame_t *bigger = wr_realloc(stack, next_cap * sizeof(*stack));
                    if (!bigger) { err = ESP_ERR_NO_MEM; break; }
                    stack = bigger;
                    stack_cap = next_cap;
                }
                char *sub = wr_dup_str(child);
                if (!sub) { err = ESP_ERR_NO_MEM; break; }
                stack[stack_top].path = sub;
                stack[stack_top].depth = (uint16_t)(frame.depth + 1);
                stack[stack_top].visited = false;
                ++stack_top;
            }
        }
        closedir(dir);
        free(frame.path);
        if (err != ESP_OK && err != ESP_FAIL) break;
    }
    while (stack_top > 0) free(stack[--stack_top].path);
    free(stack);
    free(child);
    return err;
}

typedef struct {
    char *path;
    bool chapter_dir;
} wr_subdir_item_t;

esp_err_t weread_chapter_inspect_book_cache(const char *book_id, weread_chapter_book_cache_info_t *out,
    weread_cancel_fn cancel_fn, void *cancel_ctx) {
    if (!wr_valid_id(book_id) || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (cancel_fn && cancel_fn(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    char *dir __attribute__((cleanup(wr_release))) = wr_alloc(512);
    char *child __attribute__((cleanup(wr_release))) = wr_alloc(1024);
    if (!dir || !child) return ESP_ERR_NO_MEM;
    esp_err_t err = wr_cache_book_dir(book_id, dir, 512);
    if (err != ESP_OK) return err;
    if (!sd_io_ready_for(dir)) return ESP_ERR_INVALID_STATE;
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    read_pico_sd_access_scope_t sd_access __attribute__((cleanup(read_pico_sd_access_scope_end))) =
        read_pico_sd_access_scope(dir);
    if (!sd_access.allowed) return ESP_ERR_INVALID_STATE;
#endif
    struct stat st;
    if (stat(dir, &st)) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode)) return ESP_FAIL;
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    size_t sub_count = 0, sub_cap = 0;
    wr_subdir_item_t *subs = NULL;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (join_path(dir, entry->d_name, child, 1024) != ESP_OK) { err = ESP_ERR_INVALID_SIZE; continue; }
        struct stat cst;
        if (lstat(child, &cst)) { if (errno != ENOENT) err = ESP_FAIL; continue; }
        bool is_dir = S_ISDIR(cst.st_mode);
        cache_entry_kind_t kind = classify_cache_entry(entry->d_name, is_dir);
        if (is_dir) {
            if (sub_count == sub_cap) {
                size_t next = sub_cap ? sub_cap * 2 : 4;
                wr_subdir_item_t *bigger = wr_realloc(subs, next * sizeof(*subs));
                if (!bigger) { err = ESP_ERR_NO_MEM; break; }
                subs = bigger;
                sub_cap = next;
            }
            char *p = wr_dup_str(child);
            if (!p) { err = ESP_ERR_NO_MEM; break; }
            subs[sub_count].path = p;
            subs[sub_count].chapter_dir = (kind == CACHE_DEL_CHAPTER_DIR);
            ++sub_count;
            continue;
        }
        uint64_t sz = cst.st_size > 0 ? (uint64_t)cst.st_size : 0;
        if (kind == CACHE_DEL_CHAPTER_FILE) {
            out->chapter_bytes += sz;
            out->reclaimable_bytes += sz;
            uint32_t uid = 0;
            if (parse_chapter_manifest_uid(entry->d_name, &uid)) ++out->chapter_manifest_count;
        } else if (kind == CACHE_DEL_COVER_TEMP) {
            out->cover_bytes += sz;
            out->reclaimable_bytes += sz;
        } else if (is_preserved_cover_file(entry->d_name)) {
            out->cover_bytes += sz;
        } else {
            out->meta_bytes += sz;
        }
    }
    closedir(d);
    for (size_t i = 0; i < sub_count; ++i) {
        if (err == ESP_OK) {
            uint64_t sub_bytes = 0;
            esp_err_t serr = stat_tree_iterative(subs[i].path, &sub_bytes, cancel_fn, cancel_ctx);
            if (serr != ESP_OK) err = serr;
            else if (subs[i].chapter_dir) {
                out->chapter_bytes += sub_bytes;
                out->reclaimable_bytes += sub_bytes;
            } else {
                out->meta_bytes += sub_bytes;
            }
        }
        free(subs[i].path);
    }
    free(subs);
    out->total_bytes = out->chapter_bytes + out->cover_bytes + out->meta_bytes;
    return err;
}

static esp_err_t collect_cache_book_ids(const char *cache_root, char (**ids_out)[WEREAD_BOOK_ID_CAP],
    size_t *count_out, weread_cancel_fn cancel_fn, void *cancel_ctx) {
    *ids_out = NULL;
    *count_out = 0;
    struct stat st;
    if (stat(cache_root, &st)) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode)) return ESP_FAIL;
    char *child __attribute__((cleanup(wr_release))) = wr_alloc(1024);
    if (!child) return ESP_ERR_NO_MEM;
    DIR *d = opendir(cache_root);
    if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    size_t cap = 0, count = 0;
    char (*ids)[WEREAD_BOOK_ID_CAP] = NULL;
    esp_err_t err = ESP_OK;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (!wr_valid_id(entry->d_name)) continue;
        if (join_path(cache_root, entry->d_name, child, 1024) != ESP_OK) continue;
        struct stat cst;
        if (stat(child, &cst) || !S_ISDIR(cst.st_mode)) continue;
        if (count == cap) {
            if (cap >= WEREAD_SHELF_LIMIT * 2u) { err = ESP_ERR_INVALID_SIZE; break; }
            size_t next = cap ? cap * 2 : 16;
            char (*bigger)[WEREAD_BOOK_ID_CAP] = wr_realloc(ids, next * sizeof(*ids));
            if (!bigger) { err = ESP_ERR_NO_MEM; break; }
            ids = bigger;
            cap = next;
        }
        memcpy(ids[count], entry->d_name, strlen(entry->d_name) + 1);
        ++count;
    }
    closedir(d);
    if (err != ESP_OK) {
        free(ids);
        return err;
    }
    *ids_out = ids;
    *count_out = count;
    return ESP_OK;
}

esp_err_t weread_chapter_inspect_cache_stats(weread_chapter_cache_stats_t *out,
    weread_cancel_fn cancel_fn, void *cancel_ctx) {
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (cancel_fn && cancel_fn(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = weread_chapter_storage_info(&out->free_bytes, &out->fs_total_bytes);
    if (err != ESP_OK) return err;
    char *root __attribute__((cleanup(wr_release))) = wr_alloc(512);
    if (!root) return ESP_ERR_NO_MEM;
    err = wr_cache_root(root, 512);
    if (err != ESP_OK) return err;
    if (!sd_io_ready_for(root)) return ESP_ERR_INVALID_STATE;
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    read_pico_sd_access_scope_t sd_access __attribute__((cleanup(read_pico_sd_access_scope_end))) =
        read_pico_sd_access_scope(root);
    if (!sd_access.allowed) return ESP_ERR_INVALID_STATE;
#endif
    char (*ids)[WEREAD_BOOK_ID_CAP] = NULL;
    size_t count = 0;
    err = collect_cache_book_ids(root, &ids, &count, cancel_fn, cancel_ctx);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < count; ++i) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        weread_chapter_book_cache_info_t info;
        esp_err_t berr = weread_chapter_inspect_book_cache(ids[i], &info, cancel_fn, cancel_ctx);
        if (berr != ESP_OK) { err = berr; break; }
        out->total_bytes += info.total_bytes;
        out->reclaimable_bytes += info.reclaimable_bytes;
        ++out->book_dir_count;
        if (info.chapter_manifest_count > 0 || info.chapter_bytes > 0) ++out->books_with_chapters;
    }
    free(ids);
    return err;
}

esp_err_t weread_chapter_clean_book_cache(const char *book_id, uint64_t *freed_bytes,
    weread_cancel_fn cancel_fn, void *cancel_ctx) {
    if (freed_bytes) *freed_bytes = 0;
    if (!wr_valid_id(book_id)) return ESP_ERR_INVALID_ARG;
    if (cancel_fn && cancel_fn(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    if (book_weread_id_in_use(book_id)) return ESP_ERR_INVALID_STATE;
    char *dir __attribute__((cleanup(wr_release))) = wr_alloc(512);
    char *child __attribute__((cleanup(wr_release))) = wr_alloc(1024);
    if (!dir || !child) return ESP_ERR_NO_MEM;
    esp_err_t err = wr_cache_book_dir(book_id, dir, 512);
    if (err != ESP_OK) return err;
    if (!sd_io_ready_for(dir)) return ESP_ERR_INVALID_STATE;
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    read_pico_sd_access_scope_t sd_access __attribute__((cleanup(read_pico_sd_access_scope_end))) =
        read_pico_sd_access_scope(dir);
    if (!sd_access.allowed) return ESP_ERR_INVALID_STATE;
#endif
    struct stat st;
    if (stat(dir, &st)) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    if (!S_ISDIR(st.st_mode)) return ESP_FAIL;
    uint64_t freed = 0;
    /* Pass 1: unlink chapter manifests, resources.json, temp cover artifacts, and legacy chapter files. */
    DIR *d = opendir(dir);
    if (!d) return errno == ENOENT ? ESP_OK : ESP_FAIL;
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (join_path(dir, entry->d_name, child, 1024) != ESP_OK) { err = ESP_ERR_INVALID_SIZE; continue; }
        struct stat cst;
        if (lstat(child, &cst)) { if (errno != ENOENT) err = ESP_FAIL; continue; }
        if (S_ISDIR(cst.st_mode)) continue;
        cache_entry_kind_t kind = classify_cache_entry(entry->d_name, false);
        if (kind == CACHE_DEL_CHAPTER_FILE || kind == CACHE_DEL_COVER_TEMP) {
            if (unlink(child) == 0) {
                if (cst.st_size > 0) freed += (uint64_t)cst.st_size;
            } else if (errno != ENOENT) {
                err = ESP_FAIL;
            }
        }
    }
    closedir(d);
    /* Pass 2: iteratively remove chapters/ and res/ directories after manifests are gone. */
    if (err == ESP_OK) {
        static const char *const k_dirs[2] = {"chapters", "res"};
        for (size_t i = 0; i < 2; ++i) {
            if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
            if (book_weread_id_in_use(book_id)) { err = ESP_ERR_INVALID_STATE; break; }
            if (join_path(dir, k_dirs[i], child, 1024) != ESP_OK) { err = ESP_ERR_INVALID_SIZE; break; }
            esp_err_t rerr = remove_tree_iterative(child, &freed, cancel_fn, cancel_ctx);
            if (rerr != ESP_OK) { err = rerr; break; }
        }
    }
    if (freed_bytes) *freed_bytes = freed;
    return err;
}

esp_err_t weread_chapter_clean_all_caches(uint64_t *freed_bytes, uint32_t *cleaned_books,
    weread_cancel_fn cancel_fn, void *cancel_ctx) {
    if (freed_bytes) *freed_bytes = 0;
    if (cleaned_books) *cleaned_books = 0;
    if (cancel_fn && cancel_fn(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    char *root __attribute__((cleanup(wr_release))) = wr_alloc(512);
    if (!root) return ESP_ERR_NO_MEM;
    esp_err_t err = wr_cache_root(root, 512);
    if (err != ESP_OK) return err;
    if (!sd_io_ready_for(root)) return ESP_ERR_INVALID_STATE;
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    read_pico_sd_access_scope_t sd_access __attribute__((cleanup(read_pico_sd_access_scope_end))) =
        read_pico_sd_access_scope(root);
    if (!sd_access.allowed) return ESP_ERR_INVALID_STATE;
#endif
    char (*ids)[WEREAD_BOOK_ID_CAP] = NULL;
    size_t count = 0;
    err = collect_cache_book_ids(root, &ids, &count, cancel_fn, cancel_ctx);
    if (err != ESP_OK) return err;
    uint64_t total_freed = 0;
    uint32_t cleaned = 0;
    for (size_t i = 0; i < count; ++i) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        if (book_weread_id_in_use(ids[i])) continue;
        uint64_t book_freed = 0;
        esp_err_t berr = weread_chapter_clean_book_cache(ids[i], &book_freed, cancel_fn, cancel_ctx);
        total_freed += book_freed;
        if (berr != ESP_OK) { err = berr; break; }
        if (book_freed > 0) ++cleaned;
    }
    free(ids);
    if (freed_bytes) *freed_bytes = total_freed;
    if (cleaned_books) *cleaned_books = cleaned;
    return err;
}

static int cmp_u32(const void *a, const void *b) {
    uint32_t va = *(const uint32_t *)a, vb = *(const uint32_t *)b;
    return va < vb ? -1 : (va > vb ? 1 : 0);
}

esp_err_t weread_chapter_count_cached(const char *book_id, uint16_t *cached_chapters,
    uint16_t *total_chapters, weread_cancel_fn cancel_fn, void *cancel_ctx) {
    if (cached_chapters) *cached_chapters = 0;
    if (total_chapters) *total_chapters = 0;
    if (!wr_valid_id(book_id)) return ESP_ERR_INVALID_ARG;
    if (cancel_fn && cancel_fn(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    weread_toc_t toc = {0};
    esp_err_t err = weread_toc_load(book_id, &toc);
    if (err != ESP_OK) return err;
    size_t toc_count = toc.count;
    uint16_t total_u16 = toc_count > UINT16_MAX ? UINT16_MAX : (uint16_t)toc_count;
    if (!toc_count) {
        weread_toc_free(&toc);
        return ESP_OK;
    }
    uint32_t *uids = wr_alloc(toc_count * sizeof(uint32_t));
    if (!uids) {
        weread_toc_free(&toc);
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < toc_count; ++i) uids[i] = toc.chapters[i].uid;
    weread_toc_free(&toc);
    qsort(uids, toc_count, sizeof(uint32_t), cmp_u32);
    char *dir __attribute__((cleanup(wr_release))) = wr_alloc(512);
    if (!dir) { free(uids); return ESP_ERR_NO_MEM; }
    err = wr_cache_book_dir(book_id, dir, 512);
    if (err != ESP_OK) { free(uids); return err; }
    if (!sd_io_ready_for(dir)) { free(uids); return ESP_ERR_INVALID_STATE; }
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    read_pico_sd_access_scope_t sd_access __attribute__((cleanup(read_pico_sd_access_scope_end))) =
        read_pico_sd_access_scope(dir);
    if (!sd_access.allowed) { free(uids); return ESP_ERR_INVALID_STATE; }
#endif
    struct stat st;
    if (stat(dir, &st)) {
        free(uids);
        if (errno == ENOENT) {
            if (total_chapters) *total_chapters = total_u16;
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    if (!S_ISDIR(st.st_mode)) { free(uids); return ESP_FAIL; }
    DIR *d = opendir(dir);
    if (!d) {
        free(uids);
        if (errno == ENOENT) {
            if (total_chapters) *total_chapters = total_u16;
            return ESP_OK;
        }
        return ESP_FAIL;
    }
    uint8_t *seen = wr_alloc((toc_count + 7u) / 8u);
    uint32_t *cands = NULL;
    size_t cand_count = 0, cand_cap = 0;
    if (!seen) { closedir(d); free(uids); return ESP_ERR_NO_MEM; }
    struct dirent *entry;
    while ((entry = readdir(d))) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        uint32_t uid = 0;
        if (!parse_chapter_manifest_uid(entry->d_name, &uid)) continue;
        uint32_t *found = bsearch(&uid, uids, toc_count, sizeof(uint32_t), cmp_u32);
        if (!found) continue;
        size_t idx = (size_t)(found - uids);
        if (seen[idx >> 3u] & (uint8_t)(1u << (idx & 7u))) continue;
        seen[idx >> 3u] |= (uint8_t)(1u << (idx & 7u));
        if (cand_count == cand_cap) {
            size_t next = cand_cap ? cand_cap * 2 : 16;
            uint32_t *bigger = wr_realloc(cands, next * sizeof(uint32_t));
            if (!bigger) { err = ESP_ERR_NO_MEM; break; }
            cands = bigger;
            cand_cap = next;
        }
        cands[cand_count++] = uid;
    }
    closedir(d);
    free(seen);
    free(uids);
    uint32_t cached = 0;
    for (size_t i = 0; err == ESP_OK && i < cand_count; ++i) {
        if (cancel_fn && cancel_fn(cancel_ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        if (weread_chapter_cached(book_id, cands[i])) ++cached;
    }
    free(cands);
    if (err == ESP_OK) {
        uint16_t cached_u16 = cached > total_u16 ? total_u16 : (uint16_t)cached;
        if (cached_chapters) *cached_chapters = cached_u16;
        if (total_chapters) *total_chapters = total_u16;
    }
    return err;
}
