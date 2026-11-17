/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：目录按章节 UID 合并、按 chapterIdx 排序，所有写入先验证再提交。
 * English: Merge chapter UIDs, order by chapterIdx and validate before committing.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "weread_toc.h"
#include "weread_shelf.h"
#include "weread_store.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool u32(const cJSON *v, const char *key, uint32_t *out) {
    uint64_t n;
    if (!wr_uint(v, key, &n) || n > UINT32_MAX) return false;
    *out = (uint32_t)n; return true;
}
static bool chapter(const cJSON *v, weread_chapter_info_t *out) {
    memset(out, 0, sizeof(*out));
    if (!u32(v, "chapterUid", &out->uid) || !out->uid || !u32(v, "chapterIdx", &out->index)) return false;
    const char *title = wr_string(v, "title");
    if (!title) return false;
    wr_display_copy(out->title, sizeof(out->title), title);
    u32(v, "level", &out->level); u32(v, "wordCount", &out->word_count);
    wr_uint(v, "updateTime", &out->update_time);
    return true;
}
static int compare(const void *a, const void *b) {
    const weread_chapter_info_t *x = a, *y = b;
    if (x->index != y->index) return x->index < y->index ? -1 : 1;
    return x->uid == y->uid ? 0 : x->uid < y->uid ? -1 : 1;
}
void weread_toc_free(weread_toc_t *toc) {
    if (toc) { free(toc->chapters); memset(toc, 0, sizeof(*toc)); }
}
static esp_err_t decode(const cJSON *root, weread_toc_t *out) {
    const cJSON *array = cJSON_GetObjectItemCaseSensitive(root, "chapters");
    if (!wr_owner_matches(root) || !cJSON_IsArray(array) || !wr_uint(root, "synckey", &out->synckey) ||
        !u32(root, "bookVersion", &out->book_version)) return ESP_ERR_INVALID_RESPONSE;
    wr_uint(root, "updateTime", &out->update_time);
    int n = cJSON_GetArraySize(array);
    if (n > WEREAD_TOC_LIMIT) return ESP_ERR_INVALID_SIZE;
    if (!n) return ESP_OK;
    out->chapters = wr_alloc((size_t)n * sizeof(*out->chapters));
    if (!out->chapters) return ESP_ERR_NO_MEM;
    cJSON *v;
    cJSON_ArrayForEach(v, array) {
        weread_chapter_info_t *c = &out->chapters[out->count];
        if (!chapter(v, c)) return ESP_ERR_INVALID_RESPONSE;
        for (size_t i = 0; i < out->count; ++i)
            if (out->chapters[i].uid == c->uid) return ESP_ERR_INVALID_RESPONSE;
        ++out->count;
    }
    qsort(out->chapters, out->count, sizeof(*out->chapters), compare);
    return ESP_OK;
}
esp_err_t weread_toc_load(const char *id, weread_toc_t *out) { WR_GUARD;
    if (!out) return ESP_ERR_INVALID_ARG;
    char path[512];
    cJSON *root = NULL;
    weread_toc_t tmp = {0};
    esp_err_t err = wr_cache_path(id, "chapters.json", path, sizeof(path));
    if (err == ESP_OK) err = wr_read_json(path, &root);
    const char *saved_id = wr_string(root, "bookId");
    if (err == ESP_OK && (!saved_id || strcmp(saved_id, id))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) err = decode(root, &tmp);
    if (err == ESP_OK) *out = tmp;
    else weread_toc_free(&tmp);
    cJSON_Delete(root);
    return err;
}
static uint32_t uid(const cJSON *v) {
    uint32_t n = 0;
    if (cJSON_IsNumber(v) && v->valuedouble >= 1 && v->valuedouble <= UINT32_MAX &&
        v->valuedouble == (double)(uint32_t)v->valuedouble) n = (uint32_t)v->valuedouble;
    else u32(v, "chapterUid", &n);
    return n;
}
static cJSON *find(cJSON *a, uint32_t n) {
    cJSON *v;
    cJSON_ArrayForEach(v, a) if (uid(v) == n) return v;
    return NULL;
}
esp_err_t weread_toc_sync(const char *id) { WR_GUARD;
    if (!wr_valid_id(id)) return ESP_ERR_INVALID_ARG;
    char path[512];
    esp_err_t err = wr_cache_path(id, "chapters.json", path, sizeof(path));
    if (err != ESP_OK) return err;
    cJSON *old = NULL, *req = cJSON_CreateObject(), *response = NULL, *next = NULL;
    if (!req) return ESP_ERR_NO_MEM;
    uint64_t key = 0, time = 0;
    weread_toc_t check = {0};
    if (wr_read_json(path, &old) != ESP_OK || decode(old, &check) != ESP_OK || !check.count ||
        !wr_string(old, "bookId") || strcmp(wr_string(old, "bookId"), id)) {
        cJSON_Delete(old); old = NULL;
    } else { key = check.synckey; time = check.update_time; }
    weread_toc_free(&check);
    const char *ids[] = {id};
    cJSON *id_array = cJSON_CreateStringArray(ids, 1);
    cJSON *keys = cJSON_CreateArray(), *times = cJSON_CreateArray();
    if (!id_array || !keys || !times) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && (!cJSON_AddItemToArray(keys, cJSON_CreateNumber((double)key)) ||
        !cJSON_AddItemToArray(times, cJSON_CreateNumber((double)time)))) err = ESP_ERR_NO_MEM;
    if (id_array) cJSON_AddItemToObject(req, "bookIds", id_array);
    if (keys) cJSON_AddItemToObject(req, "synckeys", keys);
    if (times) cJSON_AddItemToObject(req, "updateTimes", times);
    if (err == ESP_OK) err = wr_request_json("/book/chapterInfos", req, &response);
    cJSON *entry = NULL, *v;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(response, "data");
    if (err == ESP_OK && !cJSON_IsArray(data)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) cJSON_ArrayForEach(v, data) {
        const char *s = wr_string(v, "bookId");
        if (s && !strcmp(s, id)) { if (entry) { err = ESP_ERR_INVALID_RESPONSE; break; } entry = v; }
    }
    uint64_t new_key = 0, clear = 0;
    cJSON *updated = cJSON_GetObjectItemCaseSensitive(entry, "updated");
    cJSON *removed = cJSON_GetObjectItemCaseSensitive(entry, "removed");
    if (err == ESP_OK && (!entry || !wr_uint(entry, "synckey", &new_key) ||
        !wr_uint(entry, "clearAll", &clear) || clear > 1 || !cJSON_IsArray(updated) ||
        (removed && !cJSON_IsArray(removed)))) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) {
        if(old&&!clear){next=old;old=NULL;}else {cJSON_Delete(old);old=NULL;next=cJSON_CreateObject();}
        if (!next) err = ESP_ERR_NO_MEM;
        else if (!cJSON_HasObjectItem(next, "chapters") && !cJSON_AddArrayToObject(next, "chapters")) err = ESP_ERR_NO_MEM;
    }
    cJSON *array = cJSON_GetObjectItemCaseSensitive(next, "chapters");
    if (err == ESP_OK) cJSON_ArrayForEach(v, updated) {
        weread_chapter_info_t parsed;
        if (!chapter(v, &parsed)) { err = ESP_ERR_INVALID_RESPONSE; break; }
        cJSON *existing = find(array, parsed.uid);
        cJSON *copy = cJSON_Duplicate(v, true);
        if (!copy) { err = ESP_ERR_NO_MEM; break; }
        if (existing) cJSON_ReplaceItemViaPointer(array, existing, copy);
        else cJSON_AddItemToArray(array, copy);
        if (parsed.update_time > time) time = parsed.update_time;
        if (cJSON_GetArraySize(array) > WEREAD_TOC_LIMIT) { err = ESP_ERR_INVALID_SIZE; break; }
    }
    if (err == ESP_OK) cJSON_ArrayForEach(v, removed) {
        uint32_t n = uid(v);
        if (!n) { err = ESP_ERR_INVALID_RESPONSE; break; }
        cJSON *existing = find(array, n);
        if (existing) cJSON_Delete(cJSON_DetachItemViaPointer(array, existing));
    }
    if (err == ESP_OK) {
        weread_book_t book;
        uint32_t version = 0;
        if (!u32(entry, "bookVersion", &version) && weread_shelf_find(id, &book)) version = book.book_version;
        if (!version && old) u32(old, "bookVersion", &version);
        cJSON_DeleteItemFromObject(next, "synckey"); cJSON_DeleteItemFromObject(next, "updateTime");
        cJSON_DeleteItemFromObject(next, "bookVersion"); cJSON_DeleteItemFromObject(next, "bookId");
        if (!cJSON_AddNumberToObject(next, "synckey", (double)new_key) ||
            !cJSON_AddNumberToObject(next, "updateTime", (double)time) ||
            !cJSON_AddNumberToObject(next, "bookVersion", version) ||
            !cJSON_AddStringToObject(next, "bookId", id) || !wr_set_owner(next)) err = ESP_ERR_NO_MEM;
    }
    cJSON_Delete(response);response=NULL;
    if (err == ESP_OK) err = decode(next, &check);
    if (err == ESP_OK) err = wr_write_json(path, next);
    weread_toc_free(&check);
    cJSON_Delete(old); cJSON_Delete(req); cJSON_Delete(response); cJSON_Delete(next);
    return err;
}
