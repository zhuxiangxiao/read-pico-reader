/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：目录按章节 UID 合并、按 chapterIdx 排序，所有写入先验证再提交。
 * English: Merge chapter UIDs, order by chapterIdx and validate before committing.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Os")
#endif
#include "weread_toc.h"
#include "weread_auth.h"
#include "weread_chapter.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_store.h"
#include "esp_log.h"
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static int compare(const void *a, const void *b) {
    const weread_chapter_info_t *x = a, *y = b;
    if (x->index != y->index) return x->index < y->index ? -1 : 1;
    return x->uid == y->uid ? 0 : x->uid < y->uid ? -1 : 1;
}

static int compare_uid(const void *a, const void *b) {
    uint32_t x = ((const weread_chapter_info_t *)a)->uid, y = ((const weread_chapter_info_t *)b)->uid;
    return x == y ? 0 : x < y ? -1 : 1;
}

void weread_toc_free(weread_toc_t *toc) {
    if (toc) { free(toc->chapters); memset(toc, 0, sizeof(*toc)); }
}

/* ---- 有界轻量 JSON 切片扫描器（避免整棵 cJSON 树峰值） / Bounded lightweight JSON slice scanner ---- */

typedef struct {
    const char *s;
    size_t pos, end;
} js_iter_t;

static void js_skip_ws(const char *s, size_t len, size_t *pos) {
    size_t p = *pos;
    while (p < len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    *pos = p;
}

static void js_skip_bom(const char *s, size_t len, size_t *pos) {
    if (*pos + 3u <= len &&
        (unsigned char)s[*pos] == 0xEF &&
        (unsigned char)s[*pos + 1u] == 0xBB &&
        (unsigned char)s[*pos + 2u] == 0xBF) {
        *pos += 3u;
    }
}

static bool js_is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool js_skip_string(const char *s, size_t len, size_t *pos) {
    size_t p = *pos;
    if (p >= len || s[p] != '"') return false;
    ++p;
    while (p < len) {
        unsigned char c = (unsigned char)s[p++];
        if (c == '"') { *pos = p; return true; }
        if (c < 0x20u) return false;
        if (c == '\\') {
            if (p >= len) return false;
            unsigned char e = (unsigned char)s[p++];
            if (e == '"' || e == '\\' || e == '/' || e == 'b' || e == 'f' || e == 'n' || e == 'r' || e == 't') {
                continue;
            }
            if (e == 'u') {
                if (p + 4u > len) return false;
                for (unsigned i = 0; i < 4u; ++i) if (!js_is_hex(s[p + i])) return false;
                p += 4u;
                continue;
            }
            return false;
        }
    }
    return false;
}

static bool js_skip_number(const char *s, size_t len, size_t *pos) {
    size_t p = *pos;
    if (p < len && s[p] == '-') ++p;
    if (p >= len || s[p] < '0' || s[p] > '9') return false;
    if (s[p] == '0') {
        ++p;
        if (p < len && s[p] >= '0' && s[p] <= '9') return false;
    } else {
        while (p < len && s[p] >= '0' && s[p] <= '9') ++p;
    }
    if (p < len && s[p] == '.') {
        ++p;
        if (p >= len || s[p] < '0' || s[p] > '9') return false;
        while (p < len && s[p] >= '0' && s[p] <= '9') ++p;
    }
    if (p < len && (s[p] == 'e' || s[p] == 'E')) {
        ++p;
        if (p < len && (s[p] == '+' || s[p] == '-')) ++p;
        if (p >= len || s[p] < '0' || s[p] > '9') return false;
        while (p < len && s[p] >= '0' && s[p] <= '9') ++p;
    }
    *pos = p;
    return true;
}

static bool js_skip_value(const char *s, size_t len, size_t *pos, unsigned depth) {
    js_skip_ws(s, len, pos);
    size_t p = *pos;
    if (p >= len) return false;
    char ch = s[p];
    if (ch == '"') return js_skip_string(s, len, pos);
    if (ch == '-' || (ch >= '0' && ch <= '9')) return js_skip_number(s, len, pos);
    if (ch == 't') {
        if (p + 4u <= len && !memcmp(s + p, "true", 4u)) { *pos = p + 4u; return true; }
        return false;
    }
    if (ch == 'f') {
        if (p + 5u <= len && !memcmp(s + p, "false", 5u)) { *pos = p + 5u; return true; }
        return false;
    }
    if (ch == 'n') {
        if (p + 4u <= len && !memcmp(s + p, "null", 4u)) { *pos = p + 4u; return true; }
        return false;
    }
    if (ch == '{') {
        if (depth >= 32u) return false;
        ++p;
        js_skip_ws(s, len, &p);
        if (p < len && s[p] == '}') { *pos = p + 1u; return true; }
        for (;;) {
            if (!js_skip_string(s, len, &p)) return false;
            js_skip_ws(s, len, &p);
            if (p >= len || s[p] != ':') return false;
            ++p;
            if (!js_skip_value(s, len, &p, depth + 1u)) return false;
            js_skip_ws(s, len, &p);
            if (p >= len) return false;
            if (s[p] == ',') { ++p; js_skip_ws(s, len, &p); continue; }
            if (s[p] == '}') { *pos = p + 1u; return true; }
            return false;
        }
    }
    if (ch == '[') {
        if (depth >= 32u) return false;
        ++p;
        js_skip_ws(s, len, &p);
        if (p < len && s[p] == ']') { *pos = p + 1u; return true; }
        for (;;) {
            if (!js_skip_value(s, len, &p, depth + 1u)) return false;
            js_skip_ws(s, len, &p);
            if (p >= len) return false;
            if (s[p] == ',') { ++p; js_skip_ws(s, len, &p); continue; }
            if (s[p] == ']') { *pos = p + 1u; return true; }
            return false;
        }
    }
    return false;
}

static bool js_validate_root(const char *s, size_t len, size_t *root_start, size_t *root_end) {
    if (!s || !len || len > WEREAD_JSON_LIMIT) return false;
    size_t pos = 0;
    js_skip_bom(s, len, &pos);
    js_skip_ws(s, len, &pos);
    *root_start = pos;
    if (!js_skip_value(s, len, &pos, 0)) return false;
    *root_end = pos;
    js_skip_ws(s, len, &pos);
    return pos == len;
}

static bool js_obj_begin(const char *s, size_t start, size_t end, js_iter_t *it) {
    if (!s || start + 2u > end || s[start] != '{' || s[end - 1u] != '}') return false;
    it->s = s; it->pos = start + 1u; it->end = end - 1u;
    return true;
}

static bool js_obj_next(js_iter_t *it, size_t *kstart, size_t *kend, size_t *vstart, size_t *vend) {
    js_skip_ws(it->s, it->end, &it->pos);
    if (it->pos >= it->end) return false;
    if (it->s[it->pos] == ',') { ++it->pos; js_skip_ws(it->s, it->end, &it->pos); }
    if (it->pos >= it->end) return false;
    *kstart = it->pos;
    if (!js_skip_string(it->s, it->end, &it->pos)) return false;
    *kend = it->pos;
    js_skip_ws(it->s, it->end, &it->pos);
    if (it->pos >= it->end || it->s[it->pos] != ':') return false;
    ++it->pos;
    js_skip_ws(it->s, it->end, &it->pos);
    *vstart = it->pos;
    if (!js_skip_value(it->s, it->end, &it->pos, 0)) return false;
    *vend = it->pos;
    return true;
}

static bool js_arr_begin(const char *s, size_t start, size_t end, js_iter_t *it) {
    if (!s || start + 2u > end || s[start] != '[' || s[end - 1u] != ']') return false;
    it->s = s; it->pos = start + 1u; it->end = end - 1u;
    return true;
}

static bool js_arr_next(js_iter_t *it, size_t *vstart, size_t *vend) {
    js_skip_ws(it->s, it->end, &it->pos);
    if (it->pos >= it->end) return false;
    if (it->s[it->pos] == ',') { ++it->pos; js_skip_ws(it->s, it->end, &it->pos); }
    if (it->pos >= it->end) return false;
    *vstart = it->pos;
    if (!js_skip_value(it->s, it->end, &it->pos, 0)) return false;
    *vend = it->pos;
    return true;
}

static size_t js_arr_count(const char *s, size_t start, size_t end) {
    js_iter_t it;
    if (!js_arr_begin(s, start, end, &it)) return 0;
    size_t count = 0, vs = 0, ve = 0;
    while (js_arr_next(&it, &vs, &ve)) ++count;
    return count;
}

static esp_err_t js_parse_slice(const char *s, size_t start, size_t end, cJSON **out) {
    *out = NULL;
    if (!s || end <= start) return ESP_ERR_INVALID_RESPONSE;
    const char *parse_end = NULL;
    cJSON *item = cJSON_ParseWithLengthOpts(s + start, end - start, &parse_end, false);
    if (!item) return wr_memory_failed() ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_RESPONSE;
    if (parse_end != s + end) {
        cJSON_Delete(item);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out = item;
    return ESP_OK;
}

static bool js_has_escape(const char *s, size_t start, size_t end) {
    return end >= start + 2u && memchr(s + start + 1u, '\\', (end - start) - 2u) != NULL;
}

static bool js_str_equals(const char *s, size_t start, size_t end, const char *target, bool case_insensitive, bool *oom) {
    if (!s || !target || end < start + 2u || s[start] != '"' || s[end - 1u] != '"') return false;
    if (!js_has_escape(s, start, end)) {
        size_t raw_len = (end - start) - 2u, tlen = strlen(target);
        if (raw_len != tlen) return false;
        return case_insensitive ? strncasecmp(s + start + 1u, target, tlen) == 0
                                : memcmp(s + start + 1u, target, tlen) == 0;
    }
    cJSON *item = NULL;
    esp_err_t err = js_parse_slice(s, start, end, &item);
    if (err == ESP_ERR_NO_MEM) { if (oom) *oom = true; return false; }
    bool eq = err == ESP_OK && cJSON_IsString(item) &&
        (case_insensitive ? strcasecmp(item->valuestring, target) == 0 : strcmp(item->valuestring, target) == 0);
    cJSON_Delete(item);
    return eq;
}

static esp_err_t js_copy_string_slice(const char *s, size_t start, size_t end, char *out, size_t cap) {
    if (!s || !out || !cap || end < start + 2u || s[start] != '"' || s[end - 1u] != '"') return ESP_ERR_INVALID_RESPONSE;
    if (!js_has_escape(s, start, end)) {
        size_t n = (end - start) - 2u;
        const char *raw = s + start + 1u;
        if (n >= cap) {
            n = cap - 1u;
            while (n && ((unsigned char)raw[n] & 0xc0u) == 0x80u) --n;
        }
        memcpy(out, raw, n);
        out[n] = 0;
        return ESP_OK;
    }
    cJSON *item = NULL;
    esp_err_t err = js_parse_slice(s, start, end, &item);
    if (err != ESP_OK) return err;
    if (!cJSON_IsString(item)) { cJSON_Delete(item); return ESP_ERR_INVALID_RESPONSE; }
    wr_display_copy(out, cap, item->valuestring);
    cJSON_Delete(item);
    return ESP_OK;
}

static esp_err_t js_uint_slice(const char *s, size_t start, size_t end, uint64_t *out) {
    if (!s || end <= start) return ESP_ERR_INVALID_RESPONSE;
    char c = s[start];
    if (c != '-' && (c < '0' || c > '9')) return ESP_ERR_INVALID_RESPONSE;
    if (end - start <= 15u) {
        bool digits = true;
        uint64_t val = 0;
        for (size_t i = start; i < end; ++i) {
            if (s[i] < '0' || s[i] > '9') { digits = false; break; }
            val = val * 10u + (uint64_t)(s[i] - '0');
        }
        if (digits) {
            if (out) *out = val;
            return ESP_OK;
        }
    }
    cJSON *item = NULL;
    esp_err_t err = js_parse_slice(s, start, end, &item);
    if (err != ESP_OK) return err;
    bool ok = cJSON_IsNumber(item) && isfinite(item->valuedouble) && item->valuedouble >= 0.0 &&
              item->valuedouble <= 9007199254740991.0 && floor(item->valuedouble) == item->valuedouble;
    if (ok && out) *out = (uint64_t)item->valuedouble;
    cJSON_Delete(item);
    return ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t js_u32_slice(const char *s, size_t start, size_t end, uint32_t *out) {
    uint64_t n = 0;
    esp_err_t err = js_uint_slice(s, start, end, &n);
    if (err != ESP_OK) return err;
    if (n > UINT32_MAX) return ESP_ERR_INVALID_RESPONSE;
    if (out) *out = (uint32_t)n;
    return ESP_OK;
}

static void js_check_nonzero_number(const char *s, size_t start, size_t end, bool *is_num, bool *nonzero, bool *oom) {
    *is_num = false; *nonzero = false;
    if (!s || end <= start || (s[start] != '-' && (s[start] < '0' || s[start] > '9'))) return;
    cJSON *item = NULL;
    esp_err_t err = js_parse_slice(s, start, end, &item);
    if (err == ESP_ERR_NO_MEM) { if (oom) *oom = true; return; }
    if (err == ESP_OK && cJSON_IsNumber(item)) {
        *is_num = true;
        *nonzero = item->valuedouble != 0.0;
    }
    cJSON_Delete(item);
}

static esp_err_t parse_chapter_slice(const char *s, size_t start, size_t end, weread_chapter_info_t *out) {
    memset(out, 0, sizeof(*out));
    js_iter_t it;
    if (!js_obj_begin(s, start, end, &it)) return ESP_ERR_INVALID_RESPONSE;
    bool seen_uid = false, uid_ok = false;
    bool seen_idx = false, idx_ok = false;
    bool seen_title = false, title_ok = false;
    bool seen_level = false, seen_wc = false, seen_ut = false, oom = false;
    size_t ks = 0, ke = 0, vs = 0, ve = 0;
    while (js_obj_next(&it, &ks, &ke, &vs, &ve)) {
        if (!seen_uid && js_str_equals(s, ks, ke, "chapterUid", false, &oom)) {
            seen_uid = true;
            esp_err_t err = js_u32_slice(s, vs, ve, &out->uid);
            if (err == ESP_ERR_NO_MEM) return err;
            uid_ok = err == ESP_OK && out->uid != 0;
        } else if (!seen_idx && js_str_equals(s, ks, ke, "chapterIdx", false, &oom)) {
            seen_idx = true;
            esp_err_t err = js_u32_slice(s, vs, ve, &out->index);
            if (err == ESP_ERR_NO_MEM) return err;
            idx_ok = err == ESP_OK;
        } else if (!seen_title && js_str_equals(s, ks, ke, "title", false, &oom)) {
            seen_title = true;
            esp_err_t err = js_copy_string_slice(s, vs, ve, out->title, sizeof(out->title));
            if (err == ESP_ERR_NO_MEM) return err;
            title_ok = err == ESP_OK;
        } else if (!seen_level && js_str_equals(s, ks, ke, "level", false, &oom)) {
            seen_level = true;
            if (js_u32_slice(s, vs, ve, &out->level) == ESP_ERR_NO_MEM) return ESP_ERR_NO_MEM;
        } else if (!seen_wc && js_str_equals(s, ks, ke, "wordCount", false, &oom)) {
            seen_wc = true;
            if (js_u32_slice(s, vs, ve, &out->word_count) == ESP_ERR_NO_MEM) return ESP_ERR_NO_MEM;
        } else if (!seen_ut && js_str_equals(s, ks, ke, "updateTime", false, &oom)) {
            seen_ut = true;
            if (js_uint_slice(s, vs, ve, &out->update_time) == ESP_ERR_NO_MEM) return ESP_ERR_NO_MEM;
        }
        if (oom) return ESP_ERR_NO_MEM;
    }
    return uid_ok && idx_ok && title_ok ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t parse_removed_uid_slice(const char *s, size_t start, size_t end, uint32_t *out) {
    *out = 0;
    if (s[start] == '{') {
        js_iter_t it;
        if (!js_obj_begin(s, start, end, &it)) return ESP_ERR_INVALID_RESPONSE;
        size_t ks = 0, ke = 0, vs = 0, ve = 0;
        bool oom = false;
        while (js_obj_next(&it, &ks, &ke, &vs, &ve)) {
            if (js_str_equals(s, ks, ke, "chapterUid", false, &oom)) {
                esp_err_t err = js_u32_slice(s, vs, ve, out);
                if (err == ESP_ERR_NO_MEM) return err;
                return err == ESP_OK && *out != 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
            }
            if (oom) return ESP_ERR_NO_MEM;
        }
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint32_t n = 0;
    esp_err_t err = js_u32_slice(s, start, end, &n);
    if (err == ESP_ERR_NO_MEM) return err;
    if (err != ESP_OK || !n) return ESP_ERR_INVALID_RESPONSE;
    *out = n;
    return ESP_OK;
}

/* ---- 离线加载与恢复副本验证 / Offline load and .previous recovery ---- */

static esp_err_t read_valid_toc_file(const char *path, char **buf_out, size_t *len_out, size_t *root_start_out, size_t *root_end_out) {
    *buf_out = NULL; *len_out = 0; *root_start_out = 0; *root_end_out = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    struct stat st;
    if (fstat(fileno(f), &st)) { fclose(f); return ESP_FAIL; }
    if (st.st_size <= 0 || st.st_size > WEREAD_JSON_LIMIT) { fclose(f); return ESP_ERR_INVALID_SIZE; }
    size_t len = (size_t)st.st_size;
    char *buf = wr_alloc(len + 1u);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    bool ok = fread(buf, 1, len, f) == len;
    if (fclose(f)) ok = false;
    if (!ok) { free(buf); return ESP_FAIL; }
    buf[len] = 0;
    size_t root_start = 0, root_end = 0;
    if (!js_validate_root(buf, len, &root_start, &root_end)) {
        free(buf);
        return ESP_ERR_INVALID_RESPONSE;
    }
    *buf_out = buf;
    *len_out = len;
    *root_start_out = root_start;
    *root_end_out = root_end;
    return ESP_OK;
}

static esp_err_t decode_bytes(const char *s, size_t root_start, size_t root_end, const char *expected_id, weread_toc_t *out) {
    js_iter_t it;
    if (!js_obj_begin(s, root_start, root_end, &it)) return ESP_ERR_INVALID_RESPONSE;
    const weread_auth_session_t *session = weread_auth_get_current_session();
    bool seen_vid = false, vid_ok = false;
    bool seen_id = false, id_ok = false;
    bool seen_key = false, key_ok = false;
    bool seen_ver = false, ver_ok = false;
    bool seen_chapters = false, chapters_is_array = false;
    bool seen_ut = false, seen_checked = false, oom = false;
    size_t ch_start = 0, ch_end = 0;
    size_t ks = 0, ke = 0, vs = 0, ve = 0;
    while (js_obj_next(&it, &ks, &ke, &vs, &ve)) {
        if (!seen_vid && js_str_equals(s, ks, ke, "vid", false, &oom)) {
            seen_vid = true;
            vid_ok = session != NULL && js_str_equals(s, vs, ve, session->vid, false, &oom);
        } else if (!seen_id && js_str_equals(s, ks, ke, "bookId", false, &oom)) {
            seen_id = true;
            id_ok = js_str_equals(s, vs, ve, expected_id, false, &oom);
        } else if (!seen_key && js_str_equals(s, ks, ke, "synckey", false, &oom)) {
            seen_key = true;
            esp_err_t err = js_uint_slice(s, vs, ve, &out->synckey);
            if (err == ESP_ERR_NO_MEM) return err;
            key_ok = err == ESP_OK;
        } else if (!seen_ver && js_str_equals(s, ks, ke, "bookVersion", false, &oom)) {
            seen_ver = true;
            esp_err_t err = js_u32_slice(s, vs, ve, &out->book_version);
            if (err == ESP_ERR_NO_MEM) return err;
            ver_ok = err == ESP_OK;
        } else if (!seen_ut && js_str_equals(s, ks, ke, "updateTime", false, &oom)) {
            seen_ut = true;
            if (js_uint_slice(s, vs, ve, &out->update_time) == ESP_ERR_NO_MEM) return ESP_ERR_NO_MEM;
        } else if (!seen_checked && js_str_equals(s, ks, ke, "checkedAt", false, &oom)) {
            seen_checked = true;
            if (js_uint_slice(s, vs, ve, &out->checked_at) == ESP_ERR_NO_MEM) return ESP_ERR_NO_MEM;
        } else if (!seen_chapters && js_str_equals(s, ks, ke, "chapters", false, &oom)) {
            seen_chapters = true;
            chapters_is_array = s[vs] == '[';
            ch_start = vs; ch_end = ve;
        }
        if (oom) return ESP_ERR_NO_MEM;
    }
    if (!vid_ok || !id_ok || !key_ok || !ver_ok || !seen_chapters || !chapters_is_array) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    size_t n = js_arr_count(s, ch_start, ch_end);
    if (n > WEREAD_TOC_LIMIT) return ESP_ERR_INVALID_SIZE;
    if (!n) return ESP_OK;
    out->chapters = wr_alloc(n * sizeof(*out->chapters));
    if (!out->chapters) return ESP_ERR_NO_MEM;
    js_iter_t arr;
    if (!js_arr_begin(s, ch_start, ch_end, &arr)) return ESP_ERR_INVALID_RESPONSE;
    while (js_arr_next(&arr, &vs, &ve)) {
        esp_err_t err = parse_chapter_slice(s, vs, ve, &out->chapters[out->count]);
        if (err != ESP_OK) return err;
        ++out->count;
    }
    // 按UID查重不增加工作数组，大目录避免两两比较。/ Sort by UID to detect duplicates without another work array or quadratic scans.
    if (out->count > 1u) qsort(out->chapters, out->count, sizeof(*out->chapters), compare_uid);
    for (size_t i = 1u; i < out->count; ++i) {
        if (out->chapters[i - 1u].uid == out->chapters[i].uid) return ESP_ERR_INVALID_RESPONSE;
    }
    if (out->count > 1u) qsort(out->chapters, out->count, sizeof(*out->chapters), compare);
    return ESP_OK;
}

esp_err_t weread_toc_load(const char *id, weread_toc_t *out) { WR_GUARD;
    if (!out) return ESP_ERR_INVALID_ARG;
    char path[512];
    esp_err_t err = wr_cache_path(id, "chapters.json", path, sizeof(path));
    if (err != ESP_OK) return err;
    char *buf = NULL;
    size_t len = 0, root_start = 0, root_end = 0;
    err = read_valid_toc_file(path, &buf, &len, &root_start, &root_end);
    if (err != ESP_OK && err != ESP_ERR_NO_MEM) {
        size_t plen = strlen(path);
        if (plen + sizeof(".previous") <= 800u) {
            char prev[544];
            if (plen + sizeof(".previous") <= sizeof(prev)) {
                memcpy(prev, path, plen);
                memcpy(prev + plen, ".previous", sizeof(".previous"));
                if (read_valid_toc_file(prev, &buf, &len, &root_start, &root_end) == ESP_OK) err = ESP_OK;
            }
        }
    }
    if (err != ESP_OK) return err;
    weread_toc_t tmp = {0};
    err = decode_bytes(buf, root_start, root_end, id, &tmp);
    free(buf);
    if (err == ESP_OK) *out = tmp;
    else weread_toc_free(&tmp);
    return err;
}

/* ---- 合并去重与流式落盘 / Incremental merge, dedup and streaming disk commit ---- */

typedef struct {
    uint32_t uid;
    uint32_t pos;
} toc_uid_pos_t;

static int compare_uid_pos(const void *a, const void *b) {
    const toc_uid_pos_t *x = a, *y = b;
    if (x->uid != y->uid) return x->uid < y->uid ? -1 : 1;
    return x->pos < y->pos ? -1 : (x->pos > y->pos ? 1 : 0);
}

static esp_err_t dedup_tail(weread_toc_t *toc, size_t base_count) {
    if (!toc || toc->count <= base_count + 1u) return ESP_OK;
    size_t n = toc->count - base_count;
    toc_uid_pos_t *idx = wr_alloc(n * sizeof(*idx));
    if (!idx) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < n; ++i) {
        idx[i].uid = toc->chapters[base_count + i].uid;
        idx[i].pos = (uint32_t)i;
    }
    qsort(idx, n, sizeof(*idx), compare_uid_pos);
    for (size_t i = 0; i + 1u < n; ++i) {
        if (idx[i].uid == idx[i + 1u].uid) {
            toc->chapters[base_count + idx[i].pos].uid = 0;
        }
    }
    free(idx);
    size_t write = base_count;
    for (size_t i = base_count; i < toc->count; ++i) {
        if (toc->chapters[i].uid) {
            if (write != i) toc->chapters[write] = toc->chapters[i];
            ++write;
        }
    }
    toc->count = write;
    return ESP_OK;
}

typedef struct {
    const char *vid;
    const char *book_id;
    const weread_toc_t *toc;
    size_t bytes;
} toc_stream_ctx_t;

static esp_err_t stream_write(FILE *f, toc_stream_ctx_t *ctx, const void *data, size_t n) {
    if (n > WEREAD_JSON_LIMIT || ctx->bytes > WEREAD_JSON_LIMIT - n) return ESP_ERR_INVALID_SIZE;
    if (n && fwrite(data, 1, n, f) != n) return ESP_FAIL;
    ctx->bytes += n;
    return ESP_OK;
}
#define STREAM_LIT(f, ctx, lit) stream_write((f), (ctx), (lit), sizeof(lit) - 1u)

static esp_err_t stream_printf(FILE *f, toc_stream_ctx_t *ctx, const char *fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0 || (size_t)n >= sizeof(buf)) return ESP_FAIL;
    return stream_write(f, ctx, buf, (size_t)n);
}

static esp_err_t stream_json_str(FILE *f, toc_stream_ctx_t *ctx, const char *s) {
    esp_err_t err = STREAM_LIT(f, ctx, "\"");
    if (err != ESP_OK) return err;
    const unsigned char *p = (const unsigned char *)(s ? s : "");
    const unsigned char *run = p;
    for (; *p; ++p) {
        unsigned char c = *p;
        if (c >= 0x20u && c != '"' && c != '\\') continue;
        if (p > run) {
            err = stream_write(f, ctx, run, (size_t)(p - run));
            if (err != ESP_OK) return err;
        }
        char esc[7];
        size_t esc_len = 2u;
        esc[0] = '\\';
        switch (c) {
            case '"': esc[1] = '"'; break;
            case '\\': esc[1] = '\\'; break;
            case '\b': esc[1] = 'b'; break;
            case '\f': esc[1] = 'f'; break;
            case '\n': esc[1] = 'n'; break;
            case '\r': esc[1] = 'r'; break;
            case '\t': esc[1] = 't'; break;
            default:
                snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                esc_len = 6u;
                break;
        }
        err = stream_write(f, ctx, esc, esc_len);
        if (err != ESP_OK) return err;
        run = p + 1;
    }
    if (p > run) {
        err = stream_write(f, ctx, run, (size_t)(p - run));
        if (err != ESP_OK) return err;
    }
    return STREAM_LIT(f, ctx, "\"");
}

static esp_err_t write_toc_stream(FILE *f, void *arg) {
    toc_stream_ctx_t *ctx = arg;
    const weread_toc_t *toc = ctx->toc;
    esp_err_t err = STREAM_LIT(f, ctx, "{\"vid\":");
    if (err == ESP_OK) err = stream_json_str(f, ctx, ctx->vid);
    if (err == ESP_OK) err = STREAM_LIT(f, ctx, ",\"bookId\":");
    if (err == ESP_OK) err = stream_json_str(f, ctx, ctx->book_id);
    if (err == ESP_OK) err = stream_printf(f, ctx, ",\"synckey\":%llu,\"updateTime\":%llu,\"bookVersion\":%u",
        (unsigned long long)toc->synckey, (unsigned long long)toc->update_time, (unsigned)toc->book_version);
    if (err == ESP_OK && toc->checked_at) {
        err = stream_printf(f, ctx, ",\"checkedAt\":%llu", (unsigned long long)toc->checked_at);
    }
    if (err == ESP_OK) err = STREAM_LIT(f, ctx, ",\"chapters\":[");
    for (size_t i = 0; err == ESP_OK && i < toc->count; ++i) {
        const weread_chapter_info_t *c = &toc->chapters[i];
        err = stream_printf(f, ctx, "%s{\"chapterUid\":%u,\"chapterIdx\":%u,\"title\":",
            i ? "," : "", (unsigned)c->uid, (unsigned)c->index);
        if (err == ESP_OK) err = stream_json_str(f, ctx, c->title);
        if (err == ESP_OK && c->level) err = stream_printf(f, ctx, ",\"level\":%u", (unsigned)c->level);
        if (err == ESP_OK && c->word_count) err = stream_printf(f, ctx, ",\"wordCount\":%u", (unsigned)c->word_count);
        if (err == ESP_OK && c->update_time) err = stream_printf(f, ctx, ",\"updateTime\":%llu", (unsigned long long)c->update_time);
        if (err == ESP_OK) err = STREAM_LIT(f, ctx, "}");
    }
    if (err == ESP_OK) err = STREAM_LIT(f, ctx, "]}");
    return err;
}

static esp_err_t scan_toc_root(const char *s, size_t root_start, size_t root_end, const char *id,
    size_t *entry_start, size_t *entry_end) {
    js_iter_t root_it;
    if (!js_obj_begin(s, root_start, root_end, &root_it)) return ESP_ERR_INVALID_RESPONSE;
    bool has_errcode = false, errcode_nonzero = false, ret_nonzero = false;
    bool has_data = false, data_is_array = false, oom = false;
    size_t data_start = 0, data_end = 0;
    size_t ks = 0, ke = 0, vs = 0, ve = 0;
    while (js_obj_next(&root_it, &ks, &ke, &vs, &ve)) {
        if (js_str_equals(s, ks, ke, "errcode", true, &oom)) {
            bool is_num = false, nz = false;
            js_check_nonzero_number(s, vs, ve, &is_num, &nz, &oom);
            if (!has_errcode) { has_errcode = true; errcode_nonzero = is_num && nz; }
        } else if (js_str_equals(s, ks, ke, "ret", true, &oom)) {
            bool is_num = false, nz = false;
            js_check_nonzero_number(s, vs, ve, &is_num, &nz, &oom);
            if (is_num && nz) ret_nonzero = true;
        } else if (!has_data && js_str_equals(s, ks, ke, "data", false, &oom)) {
            has_data = true;
            data_is_array = s[vs] == '[';
            data_start = vs; data_end = ve;
        }
        if (oom) return ESP_ERR_NO_MEM;
    }
    if (has_errcode ? errcode_nonzero : ret_nonzero) return ESP_ERR_INVALID_RESPONSE;
    if (!has_data || !data_is_array) return ESP_ERR_INVALID_RESPONSE;

    js_iter_t data_it;
    if (!js_arr_begin(s, data_start, data_end, &data_it)) return ESP_ERR_INVALID_RESPONSE;
    bool found = false;
    size_t es = 0, ee = 0;
    while (js_arr_next(&data_it, &es, &ee)) {
        if (s[es] != '{') continue;
        js_iter_t ent_it;
        if (!js_obj_begin(s, es, ee, &ent_it)) continue;
        while (js_obj_next(&ent_it, &ks, &ke, &vs, &ve)) {
            if (js_str_equals(s, ks, ke, "bookId", false, &oom)) {
                if (oom) return ESP_ERR_NO_MEM;
                if (js_str_equals(s, vs, ve, id, false, &oom)) {
                    if (oom) return ESP_ERR_NO_MEM;
                    if (found) return ESP_ERR_INVALID_RESPONSE;
                    found = true;
                    *entry_start = es;
                    *entry_end = ee;
                }
                if (oom) return ESP_ERR_NO_MEM;
                break;
            }
            if (oom) return ESP_ERR_NO_MEM;
        }
    }
    return found ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t sync_toc(const char *id, uint32_t expected, weread_toc_t *out) { WR_MEMORY_SCOPE;
    if (!wr_valid_id(id)) return ESP_ERR_INVALID_ARG;
    char path[512], url[768], payload[192];
    weread_toc_t old_toc = {0}, merged = {0};
    char *raw_resp = NULL;
    size_t raw_len = 0;
    int status = 0;
    uint64_t key = 0, updated_at = 0, old_checked_at = 0;
    uint32_t old_version = 0;
    esp_err_t err = ESP_OK;
    {
        WR_GUARD;
        err = wr_cache_path(id, "chapters.json", path, sizeof(path));
        if (err != ESP_OK) return err;
        esp_err_t load_err = weread_toc_load(id, &old_toc);
        if (load_err == ESP_ERR_NO_MEM) return ESP_ERR_NO_MEM;
        if (load_err == ESP_OK && old_toc.count) {
            key = old_toc.synckey;
            updated_at = old_toc.update_time;
            old_version = old_toc.book_version;
            old_checked_at = old_toc.checked_at;
        } else {
            weread_toc_free(&old_toc);
        }
        int un = snprintf(url, sizeof(url), "%s/book/chapterInfos", weread_client_get_base_url());
        int pn = snprintf(payload, sizeof(payload),
            "{\"bookIds\":[\"%s\"],\"synckeys\":[%llu],\"updateTimes\":[%llu]}",
            id, (unsigned long long)key, (unsigned long long)updated_at);
        if (un < 0 || (size_t)un >= sizeof(url) || pn < 0 || (size_t)pn >= sizeof(payload)) {
            weread_toc_free(&old_toc);
            return ESP_ERR_INVALID_SIZE;
        }
    }
    err = weread_client_request(url, "POST", "Content-Type: application/json\r\n",
        payload, &raw_resp, &raw_len, &status);
    WR_GUARD;
    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && (!raw_resp || !raw_len || raw_len > WEREAD_JSON_LIMIT)) err = ESP_ERR_INVALID_SIZE;
    size_t root_start = 0, root_end = 0, entry_start = 0, entry_end = 0;
    if (err == ESP_OK && !js_validate_root(raw_resp, raw_len, &root_start, &root_end)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) err = scan_toc_root(raw_resp, root_start, root_end, id, &entry_start, &entry_end);

    uint64_t new_key = 0, clear = 0;
    size_t updated_start = 0, updated_end = 0, removed_start = 0, removed_end = 0;
    bool has_removed = false, removed_is_array = false;
    uint32_t version = 0;
    if (err == ESP_OK) {
        js_iter_t ent;
        if (!js_obj_begin(raw_resp, entry_start, entry_end, &ent)) err = ESP_ERR_INVALID_RESPONSE;
        bool has_key = false, has_clear = false, has_updated = false, has_ver = false, oom = false;
        size_t ks = 0, ke = 0, vs = 0, ve = 0;
        while (err == ESP_OK && js_obj_next(&ent, &ks, &ke, &vs, &ve)) {
            if (!has_key && js_str_equals(raw_resp, ks, ke, "synckey", false, &oom)) {
                esp_err_t e = js_uint_slice(raw_resp, vs, ve, &new_key);
                if (e == ESP_ERR_NO_MEM) err = e;
                else if (e == ESP_OK) has_key = true;
                else err = ESP_ERR_INVALID_RESPONSE;
            } else if (!has_clear && js_str_equals(raw_resp, ks, ke, "clearAll", false, &oom)) {
                esp_err_t e = js_uint_slice(raw_resp, vs, ve, &clear);
                if (e == ESP_ERR_NO_MEM) err = e;
                else if (e == ESP_OK && clear <= 1u) has_clear = true;
                else err = ESP_ERR_INVALID_RESPONSE;
            } else if (!has_updated && js_str_equals(raw_resp, ks, ke, "updated", false, &oom)) {
                if (raw_resp[vs] != '[') err = ESP_ERR_INVALID_RESPONSE;
                else { has_updated = true; updated_start = vs; updated_end = ve; }
            } else if (!has_removed && js_str_equals(raw_resp, ks, ke, "removed", false, &oom)) {
                has_removed = true;
                removed_is_array = raw_resp[vs] == '[';
                removed_start = vs; removed_end = ve;
            } else if (!has_ver && js_str_equals(raw_resp, ks, ke, "bookVersion", false, &oom)) {
                esp_err_t e = js_u32_slice(raw_resp, vs, ve, &version);
                if (e == ESP_ERR_NO_MEM) err = e;
                else if (e == ESP_OK) has_ver = true;
            }
            if (oom) err = ESP_ERR_NO_MEM;
        }
        if (err == ESP_OK && (!has_key || !has_clear || !has_updated || (has_removed && !removed_is_array))) {
            err = ESP_ERR_INVALID_RESPONSE;
        }
        if (err == ESP_OK && !has_ver) {
            weread_book_t book;
            if (weread_shelf_find(id, &book)) version = book.book_version;
            else if (!clear) version = old_version;
        }
    }

    if (err == ESP_OK) {
        if (clear) weread_toc_free(&old_toc);
        size_t base_count = old_toc.count;
        if (base_count > 1u) qsort(old_toc.chapters, base_count, sizeof(*old_toc.chapters), compare_uid);
        size_t updated_count = js_arr_count(raw_resp, updated_start, updated_end);
        size_t cap = base_count + updated_count;
        if (cap > WEREAD_TOC_LIMIT) cap = WEREAD_TOC_LIMIT;
        if (cap > 0u) {
            if (base_count > 0u && updated_count == 0u) {
                merged.chapters = old_toc.chapters;
                merged.count = base_count;
                old_toc.chapters = NULL;
                old_toc.count = 0;
            } else {
                merged.chapters = wr_alloc(cap * sizeof(*merged.chapters));
                if (!merged.chapters) err = ESP_ERR_NO_MEM;
                else if (base_count > 0u) {
                    memcpy(merged.chapters, old_toc.chapters, base_count * sizeof(*merged.chapters));
                    merged.count = base_count;
                    weread_toc_free(&old_toc);
                }
            }
        }

        js_iter_t up_it;
        if (err == ESP_OK && !js_arr_begin(raw_resp, updated_start, updated_end, &up_it)) err = ESP_ERR_INVALID_RESPONSE;
        uint32_t last_new_uid = 0;
        bool new_increasing = true;
        size_t vs = 0, ve = 0;
        while (err == ESP_OK && js_arr_next(&up_it, &vs, &ve)) {
            if (raw_resp[vs] != '{') { err = ESP_ERR_INVALID_RESPONSE; break; }
            weread_chapter_info_t parsed;
            err = parse_chapter_slice(raw_resp, vs, ve, &parsed);
            if (err != ESP_OK) break;
            if (parsed.update_time > updated_at) updated_at = parsed.update_time;
            weread_chapter_info_t *hit = base_count > 0u
                ? bsearch(&parsed, merged.chapters, base_count, sizeof(*merged.chapters), compare_uid)
                : NULL;
            if (hit) {
                *hit = parsed;
                continue;
            }
            if (merged.count == cap) {
                if (!new_increasing) {
                    err = dedup_tail(&merged, base_count);
                    if (err != ESP_OK) break;
                    new_increasing = true;
                    for (size_t j = base_count + 1u; j < merged.count; ++j) {
                        if (merged.chapters[j].uid <= merged.chapters[j - 1u].uid) {
                            new_increasing = false;
                            break;
                        }
                    }
                    if (merged.count > base_count) last_new_uid = merged.chapters[merged.count - 1u].uid;
                }
                if (merged.count == cap) {
                    bool replaced = false;
                    for (size_t j = base_count; j < merged.count; ++j) {
                        if (merged.chapters[j].uid == parsed.uid) {
                            merged.chapters[j] = parsed;
                            replaced = true;
                            break;
                        }
                    }
                    if (!replaced) { err = ESP_ERR_INVALID_SIZE; break; }
                    continue;
                }
            }
            if (merged.count > base_count && parsed.uid <= last_new_uid) new_increasing = false;
            last_new_uid = parsed.uid;
            merged.chapters[merged.count++] = parsed;
        }
        if (err == ESP_OK && !new_increasing && merged.count > base_count + 1u) {
            err = dedup_tail(&merged, base_count);
        }
        if (err == ESP_OK && merged.count > WEREAD_TOC_LIMIT) err = ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK && merged.count > 1u) {
            qsort(merged.chapters, merged.count, sizeof(*merged.chapters), compare_uid);
        }

        if (err == ESP_OK && has_removed) {
            size_t rem_count = js_arr_count(raw_resp, removed_start, removed_end);
            if (rem_count > 0u && merged.count > 0u) {
                uint8_t *dead = wr_alloc((merged.count + 7u) / 8u);
                if (!dead) err = ESP_ERR_NO_MEM;
                js_iter_t rem_it;
                if (err == ESP_OK && !js_arr_begin(raw_resp, removed_start, removed_end, &rem_it)) err = ESP_ERR_INVALID_RESPONSE;
                bool any_removed = false;
                while (err == ESP_OK && js_arr_next(&rem_it, &vs, &ve)) {
                    uint32_t rem_uid = 0;
                    err = parse_removed_uid_slice(raw_resp, vs, ve, &rem_uid);
                    if (err != ESP_OK) break;
                    weread_chapter_info_t key_item = { .uid = rem_uid };
                    const weread_chapter_info_t *hit = bsearch(&key_item, merged.chapters, merged.count,
                        sizeof(*merged.chapters), compare_uid);
                    if (hit) {
                        size_t idx = (size_t)(hit - merged.chapters);
                        dead[idx >> 3u] |= (uint8_t)(1u << (idx & 7u));
                        any_removed = true;
                    }
                }
                if (err == ESP_OK && any_removed) {
                    size_t write = 0;
                    for (size_t i = 0; i < merged.count; ++i) {
                        if (!(dead[i >> 3u] & (uint8_t)(1u << (i & 7u)))) {
                            if (write != i) merged.chapters[write] = merged.chapters[i];
                            ++write;
                        }
                    }
                    merged.count = write;
                }
                free(dead);
            } else if (rem_count > 0u) {
                js_iter_t rem_it;
                if (!js_arr_begin(raw_resp, removed_start, removed_end, &rem_it)) err = ESP_ERR_INVALID_RESPONSE;
                while (err == ESP_OK && js_arr_next(&rem_it, &vs, &ve)) {
                    uint32_t rem_uid = 0;
                    err = parse_removed_uid_slice(raw_resp, vs, ve, &rem_uid);
                }
            }
        }
    }
    // 解析完成后立即释放 HTTP 缓冲，再排序和流式写盘。/ Free HTTP buffer immediately before sorting and streaming to disk.
    free(raw_resp);
    raw_resp = NULL;

    if (err == ESP_OK) {
        if (merged.count > 1u) qsort(merged.chapters, merged.count, sizeof(*merged.chapters), compare);
        merged.synckey = new_key;
        merged.update_time = updated_at;
        merged.book_version = version;
        merged.checked_at = (!clear && old_checked_at) ? old_checked_at : 0;
        time_t now = time(NULL);
        if (now >= 1700000000) merged.checked_at = (uint64_t)now;
        if (expected && merged.book_version != expected) err = WEREAD_ERR_VERSION;
    }
    if (err == ESP_OK) {
        const weread_auth_session_t *session = weread_auth_get_current_session();
        if (!session) err = ESP_ERR_NO_MEM;
        else {
            toc_stream_ctx_t wctx = { .vid = session->vid, .book_id = id, .toc = &merged, .bytes = 0 };
            err = wr_write_stream(path, write_toc_stream, &wctx);
        }
    }
    if (err == ESP_OK && out) {
        *out = merged;
        memset(&merged, 0, sizeof(merged));
    }
    weread_toc_free(&merged);
    weread_toc_free(&old_toc);
    return err;
}

esp_err_t weread_toc_sync(const char *id) { return sync_toc(id, 0, NULL); }
esp_err_t weread_toc_sync_version(const char *id, uint32_t expected) { return sync_toc(id, expected, NULL); }

esp_err_t weread_toc_prepare(const char *id, bool force, uint32_t expected, weread_toc_t *out) { WR_MEMORY_SCOPE;
    if (!out) return ESP_ERR_INVALID_ARG;
    uint32_t version = 0;
    {
        WR_GUARD;
        version = expected ? expected : weread_shelf_book_version(id);
        uint64_t now = (uint64_t)time(NULL);
        esp_err_t cached = weread_toc_load(id, out);
        bool matching = cached == ESP_OK && out->count && (!version || version == out->book_version);
        // 已知版本匹配不按时间过期；未知版本用一天兜底发现目录变化。/ Matching known versions do not expire; unknown versions get a daily recheck for catalog changes.
        if (!force && matching && (expected || (out->checked_at >= 1700000000 && now >= out->checked_at && now - out->checked_at < 86400))) {
            return ESP_OK;
        }
        weread_toc_free(out);
    }
    esp_err_t err = sync_toc(id, version, out);
    if (err == ESP_OK) return err;
    // 瞬时离线可复用同版本旧缓存，已知版本变化不能回退旧正文。/ Offline fallback retains a matching version, never a known older text revision.
    WR_GUARD;
    if (err != WEREAD_ERR_VERSION && err != ESP_ERR_NO_MEM && err != ESP_ERR_INVALID_STATE &&
        weread_toc_load(id, out) == ESP_OK && out->count && (!version || out->book_version == version)) {
        return ESP_OK;
    }
    weread_toc_free(out);
    return err;
}
