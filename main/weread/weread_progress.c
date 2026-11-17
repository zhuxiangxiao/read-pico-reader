/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：进度写卡先于联网；Web 回退必须收到 succ=1，空对象不算成功。
 * English: Persist before networking; web fallback requires succ=1 and never accepts an empty object.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "weread_progress.h"
#include "weread_store.h"
#include "weread_shelf.h"
#include "weread_client.h"
#include "book_weread_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static char s_retry_id[WEREAD_BOOK_ID_CAP];
static uint64_t s_retry_at, s_retry_started;

static uint64_t number(const cJSON *o, const char *key) { uint64_t n = 0; wr_uint(o, key, &n); return n; }
static bool field(const cJSON *o, const char *key, uint32_t *out, bool required) {
    uint64_t n = 0;
    if (!wr_uint(o, key, &n)) return !required && !cJSON_HasObjectItem(o, key);
    if (n > UINT32_MAX) return false;
    *out = (uint32_t)n; return true;
}
static bool decode(const cJSON *o, weread_progress_t *p, bool unread) {
    memset(p, 0, sizeof(*p));
    if (!cJSON_IsObject(o) || !field(o, "chapterUid", &p->chapter_uid, !unread) ||
        !field(o, "chapterIdx", &p->chapter_index, !unread || p->chapter_uid) || !field(o, "chapterOffset", &p->chapter_offset, false) ||
        !field(o, "progress", &p->progress, !unread || p->chapter_uid) || p->progress > 100 ||
        !field(o, "readingTime", &p->reading_time, false) || !field(o, "bookVersion", &p->book_version, false)) return false;
    if (!p->chapter_uid && (!unread || p->chapter_index || p->chapter_offset || p->progress)) return false;
    p->updated_at = number(o, "updateTime");
    p->last_report_ms = number(o, "lastReportMs");
    uint32_t pages = 0; if (!field(o, "pendingPages", &pages, false)) return false;
    p->pending_pages = pages;
    p->dirty = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "dirty"));
    const cJSON *pending=cJSON_GetObjectItemCaseSensitive(o,"positionPending");
    if(pending&&!cJSON_IsBool(pending))return false;
    p->position_pending=pending?cJSON_IsTrue(pending):p->dirty;
    const char *summary = wr_string(o, "summary");
    if (summary) wr_display_copy(p->summary, sizeof(p->summary), summary);
    return true;
}
static cJSON *encode(const weread_progress_t *p) {
    cJSON *o = cJSON_CreateObject();
    if (!o) return NULL;
    if (!cJSON_AddBoolToObject(o,"positionPending",p->position_pending) ||
        !cJSON_AddNumberToObject(o, "chapterUid", p->chapter_uid) ||
        !cJSON_AddNumberToObject(o, "chapterIdx", p->chapter_index) ||
        !cJSON_AddNumberToObject(o, "chapterOffset", p->chapter_offset) ||
        !cJSON_AddNumberToObject(o, "progress", p->progress) ||
        !cJSON_AddNumberToObject(o, "readingTime", p->reading_time) ||
        !cJSON_AddNumberToObject(o, "bookVersion", p->book_version) ||
        !cJSON_AddNumberToObject(o, "updateTime", (double)p->updated_at) ||
        !cJSON_AddNumberToObject(o, "lastReportMs", (double)p->last_report_ms) ||
        !cJSON_AddNumberToObject(o, "pendingPages", p->pending_pages) ||
        !cJSON_AddBoolToObject(o, "dirty", p->dirty) ||
        !cJSON_AddStringToObject(o, "summary", p->summary)) { cJSON_Delete(o); return NULL; }
    return o;
}
static esp_err_t save(const char *id, const weread_progress_t *p) {
    char path[512]; esp_err_t err = wr_cache_path(id, "progress.json", path, sizeof(path));
    if (err != ESP_OK) return err;
    cJSON *o = encode(p);
    if (!o || !wr_set_owner(o) || !cJSON_AddStringToObject(o, "bookId", id)) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = wr_write_json(path, o);
    cJSON_Delete(o); return err;
}
esp_err_t weread_progress_load(const char *id, weread_progress_t *out) { WR_GUARD;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    char path[512]; cJSON *o = NULL;
    esp_err_t err = wr_cache_path(id, "progress.json", path, sizeof(path));
    if (err == ESP_OK) err = wr_read_json(path, &o);
    const char *saved_id = wr_string(o, "bookId");
    if (err == ESP_OK && (!wr_owner_matches(o) || !saved_id || strcmp(saved_id, id) || !decode(o, out, false))) err = ESP_ERR_INVALID_RESPONSE;
    cJSON_Delete(o); return err;
}
static int error_code(const cJSON *r) {
    const cJSON *v = cJSON_GetObjectItem(r, "errcode");
    if (!v) v = cJSON_GetObjectItem(r, "ret");
    return cJSON_IsNumber(v) ? v->valueint : 0;
}
static const char *web_base(void) {
#if !defined(ESP_PLATFORM)
    const char *base = getenv("WEREAD_WEB_BASE");
    if (base && *base) return base;
#endif
    return "https://weread.qq.com";
}
static esp_err_t request(const char *url, const char *body, cJSON **out, int *status) {
    char *text = NULL; size_t len = 0; *out = NULL;
    esp_err_t err = weread_client_request(url, body ? "POST" : "GET",
        body ? "Content-Type: application/json\r\n" : NULL, body, &text, &len, status);
    if (text && len && len <= 65536) *out = wr_parse_json(text,len+1);
    free(text);
    if(err==ESP_OK && !*out && wr_memory_failed())err=ESP_ERR_NO_MEM;
    if (err == ESP_OK && (*status != 200 || !cJSON_IsObject(*out) || error_code(*out))) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}
static bool fallback(const cJSON *r, int status) { return status == 401 || error_code(r) == -2012; }
esp_err_t weread_progress_sync(const char *id, weread_progress_t *local, weread_progress_t *cloud, bool *conflict) { WR_GUARD;
    if (!wr_valid_id(id) || !local || !cloud || !conflict) return ESP_ERR_INVALID_ARG;
    bool have_local = weread_progress_load(id, local) == ESP_OK;
    memset(cloud, 0, sizeof(*cloud)); *conflict = false;
    // 原生syncbook同时返回完整进度，避免不兼容getProgress引发续期/重放/Web链。
    // Native syncbook supplies complete positions without the unsupported getProgress renewal/replay/web chain.
    char body[128], url[768];
    snprintf(body,sizeof(body),"{\"bookIds\":[\"%s\"],\"albumIds\":[]}",id);
    snprintf(url,sizeof(url),"%s/shelf/syncbook",weread_client_get_base_url());
    int status = 0; cJSON *r = NULL;
    esp_err_t err = request(url, body, &r, &status);
    if(err!=ESP_OK && (!status || err==ESP_ERR_NO_MEM)){cJSON_Delete(r);return err;}
    const cJSON *book = NULL;
    if(err==ESP_OK){
        const cJSON *positions=cJSON_GetObjectItemCaseSensitive(r,"bookProgress"),*p;
        if(cJSON_IsArray(positions))cJSON_ArrayForEach(p,positions){
            const char *saved_id=wr_string(p,"bookId");
            if(saved_id&&!strcmp(saved_id,id)){if(book){cJSON_Delete(r);return ESP_ERR_INVALID_RESPONSE;}book=p;}
        }
        const char *required[]={"chapterUid","chapterIdx","chapterOffset","progress"};
        for(unsigned i=0;i<sizeof(required)/sizeof(*required)&&book;++i)
            if(!cJSON_GetObjectItemCaseSensitive(book,required[i]))book=NULL;
        if(book&&!decode(book,cloud,true))book=NULL;
    }
    if (!book) {
        cJSON_Delete(r); r = NULL;
        snprintf(url, sizeof(url), "%s/book/getProgress?bookId=%s", weread_client_get_base_url(), id);
        err = request(url, NULL, &r, &status);
        if (fallback(r, status)) {
            cJSON_Delete(r); r = NULL;
            snprintf(url, sizeof(url), "%s/web/book/getProgress?bookId=%s", web_base(), id);
            err = request(url, NULL, &r, &status);
        }
        book = cJSON_GetObjectItemCaseSensitive(r, "book");
        if (!book) book = r;
        const char *server_id = wr_string(r, "bookId");
        if (err == ESP_OK && ((server_id && strcmp(server_id, id)) || error_code(book) || !decode(book, cloud, true))) err = ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(r);
    if (err != ESP_OK) return err;
    bool different = have_local && (local->chapter_uid != cloud->chapter_uid || local->chapter_offset != cloud->chapter_offset ||
        local->chapter_index != cloud->chapter_index || (local->book_version && cloud->book_version && local->book_version != cloud->book_version));
    if(have_local&&!different)local->progress=cloud->progress;
    *conflict = different;
    if (!have_local) {
        *local = *cloud; local->reading_time = 0;
        if (local->chapter_uid) err = save(id, local);
    }
    return err;
}
esp_err_t weread_progress_checkpoint(const char *id,const weread_progress_t *p){WR_GUARD;
    if (!p || !p->chapter_uid || p->progress > 100 || !memchr(p->summary, 0, sizeof(p->summary))) return ESP_ERR_INVALID_ARG;
    return save(id,p);
}
esp_err_t weread_progress_select(const char *id, const weread_progress_t *p, bool from_cloud) { WR_GUARD;
    if(!p)return ESP_ERR_INVALID_ARG;
    weread_progress_t selected = *p; selected.dirty = !from_cloud;
    selected.position_pending=!from_cloud;
    if (from_cloud) selected.pending_pages = selected.reading_time = 0;
    return weread_progress_checkpoint(id, &selected);
}
esp_err_t weread_progress_note_time(const char *id,uint32_t seconds,uint64_t timestamp){WR_GUARD;
    if(timestamp>9007199254740991ULL)return ESP_ERR_INVALID_ARG;
    weread_progress_t p;esp_err_t err=weread_progress_load(id,&p);if(err!=ESP_OK)return err;
    p.reading_time=seconds>UINT32_MAX-p.reading_time?UINT32_MAX:p.reading_time+seconds;
    if(seconds){p.dirty=true;p.updated_at=timestamp;}
    return save(id,&p);
}
esp_err_t weread_progress_note(const char *id, uint32_t uid, uint32_t index, uint32_t offset,
    uint32_t percent, uint32_t seconds, unsigned pages, const char *summary, uint64_t timestamp) { WR_GUARD;
    if (!uid || percent > 100 || timestamp > 9007199254740991ULL) return ESP_ERR_INVALID_ARG;
    weread_progress_t p = {0}; (void)weread_progress_load(id, &p);
    p.chapter_uid = uid; p.chapter_index = index; p.chapter_offset = offset; p.progress = percent;
    p.reading_time = seconds > UINT32_MAX - p.reading_time ? UINT32_MAX : p.reading_time + seconds;
    p.pending_pages = pages > UINT32_MAX - p.pending_pages ? UINT32_MAX : p.pending_pages + pages;
    p.updated_at = timestamp; p.dirty = p.position_pending = true;
    wr_display_copy(p.summary, sizeof(p.summary), summary ? summary : "");
    weread_book_t book; if (weread_shelf_find(id, &book)) p.book_version = book.book_version;
    return save(id, &p);
}
esp_err_t weread_progress_report(const char *id, bool force, uint64_t now) { WR_GUARD;
    if (!wr_valid_id(id)) return ESP_ERR_INVALID_ARG;
    if (!force && !strcmp(id, s_retry_id) && now >= s_retry_started && now < s_retry_at) return ESP_ERR_TIMEOUT;
    weread_progress_t p;
    esp_err_t err = weread_progress_load(id, &p);
    if (err != ESP_OK || !p.dirty || !p.position_pending) return err;
    if (!force && p.pending_pages < 10 && now >= p.last_report_ms && now - p.last_report_ms < 60000) return ESP_OK;
    cJSON *body = encode(&p), *r = NULL;
    if (!body || !cJSON_AddStringToObject(body, "bookId", id)) { cJSON_Delete(body); return ESP_ERR_NO_MEM; }
    // 只发送协议字段，不把本地状态混入 API。/ Send protocol fields without local bookkeeping.
    const char *keys[] = {"dirty", "pendingPages", "lastReportMs", "updateTime", "positionPending"};
    for (size_t i = 0; i < 5; ++i) cJSON_DeleteItemFromObject(body, keys[i]);
    char *text = cJSON_PrintUnformatted(body); cJSON_Delete(body);
    if (!text) return ESP_ERR_NO_MEM;
    char url[768]; snprintf(url, sizeof(url), "%s/book/read", weread_client_get_base_url());
    int status = 0; err = request(url, text, &r, &status); cJSON_free(text);
    if (fallback(r, status)) {
        cJSON_Delete(r); r = NULL;
        char *signed_body __attribute__((cleanup(wr_release)))=wr_alloc(4096);
        if(!signed_body)return ESP_ERR_NO_MEM;
        if (!book_weread_build_web_read_body(id, p.chapter_uid, p.chapter_index, p.chapter_offset,
            p.progress, p.summary, (int64_t)time(NULL), WEREAD_DEFAULT_USER_AGENT, signed_body, 4096)) return ESP_ERR_INVALID_SIZE;
        snprintf(url, sizeof(url), "%s/web/book/read", web_base());
        err = request(url, signed_body, &r, &status);
    }
    if (err == ESP_OK && number(r, "succ") != 1 && !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "succ"))) err = ESP_ERR_INVALID_RESPONSE;
    cJSON_Delete(r);
    if (err != ESP_OK) {
        wr_copy(s_retry_id, sizeof(s_retry_id), id); s_retry_started = now;
        s_retry_at = now > UINT64_MAX - 15000 ? UINT64_MAX : now + 15000;
        return err;
    }
    s_retry_id[0] = 0;
    p.dirty = p.position_pending = false; p.reading_time = 0; p.pending_pages = 0; p.last_report_ms = now;
    return save(id, &p);
}
