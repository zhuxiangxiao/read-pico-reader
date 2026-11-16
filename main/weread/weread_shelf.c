/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：账号隔离的书架事务与离线读取；由单一微信读书工作线程调用。
 * English: Account-bound shelf transactions and offline reads, called by one WeRead worker.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "weread_shelf.h"
#include "weread_store.h"
#include "weread_auth.h"
#include "esp_log.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static cJSON *s_shelf;

static cJSON *books(cJSON *s) { return cJSON_GetObjectItemCaseSensitive(s, "books"); }
static const char *item_id(const cJSON *v) {
    return cJSON_IsString(v) ? v->valuestring : wr_string(v, "bookId");
}
static cJSON *find(cJSON *array, const char *id) {
    cJSON *v;
    cJSON_ArrayForEach(v, array) {
        const char *s = item_id(v);
        if (s && !strcmp(s, id)) return v;
    }
    return NULL;
}
static void remove_id(cJSON *array, const char *id) {
    cJSON *v = find(array, id);
    if (v) cJSON_Delete(cJSON_DetachItemViaPointer(array, v));
}
static bool readable(const cJSON *v) {
    const char *f = wr_string(v, "format");
    return !f || !*f || !strcasecmp(f, "epub") || !strcasecmp(f, "txt");
}
static esp_err_t merge_book(cJSON *array, const cJSON *v) {
    const char *id = item_id(v);
    if (!cJSON_IsObject(v) || !wr_valid_id(id)) return ESP_ERR_INVALID_RESPONSE;
    if (!readable(v)) { remove_id(array, id); return ESP_OK; }
    cJSON *old = find(array, id);
    if (!old) {
        if (cJSON_GetArraySize(array) >= WEREAD_SHELF_LIMIT) return ESP_ERR_INVALID_SIZE;
        cJSON *copy = cJSON_CreateObject();
        if (!copy) return ESP_ERR_NO_MEM;
        if (!cJSON_AddItemToArray(array, copy)) { cJSON_Delete(copy); return ESP_ERR_NO_MEM; }
        old=copy;
    }
    // 缓存阅读实际使用的完整字段，避免复制简介等巨大冗余字段。/ Keep complete reader fields without duplicating large unused introductions.
    const char *fields[]={"bookId","title","author","cover","format","bookVersion","totalWords","readUpdateTime","updateTime","readingProgress"};
    for(unsigned i=0;i<sizeof(fields)/sizeof(*fields);++i){
        cJSON *field=cJSON_GetObjectItemCaseSensitive(v,fields[i]);
        if(field&&!wr_set_copy(old,fields[i],field))return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static bool optional_array(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return !v || cJSON_IsArray(v);
}
static esp_err_t merge_progress(cJSON *array, const cJSON *r) {
    if (!optional_array(r, "bookProgress")) return ESP_ERR_INVALID_RESPONSE;
    cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(r, "bookProgress")) {
        const char *id = item_id(v);
        if (!wr_valid_id(id)) return ESP_ERR_INVALID_RESPONSE;
        cJSON *b = find(array, id);
        if (!b) continue;
        if (!wr_set_copy(b, "readingProgress", v)) return ESP_ERR_NO_MEM;
        cJSON *time = cJSON_GetObjectItemCaseSensitive(v, "updateTime");
        if (time && !wr_set_copy(b, "readUpdateTime", time)) return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static esp_err_t merge_response(cJSON *array, const cJSON *r) {
    if (!optional_array(r, "books")) return ESP_ERR_INVALID_RESPONSE;
    cJSON *v;
    cJSON_ArrayForEach(v, cJSON_GetObjectItemCaseSensitive(r, "books")) {
        esp_err_t err = merge_book(array, v);
        if (err != ESP_OK) return err;
    }
    return merge_progress(array, r);
}
static uint64_t number(const cJSON *o, const char *key) {
    uint64_t v = 0; wr_uint(o, key, &v); return v;
}
static int compare(const void *a, const void *b) {
    const cJSON *x = *(const cJSON *const *)a, *y = *(const cJSON *const *)b;
    uint64_t xt = number(x, "readUpdateTime"), yt = number(y, "readUpdateTime");
    if (xt != yt) return xt > yt ? -1 : 1;
    xt = number(x, "updateTime"); yt = number(y, "updateTime");
    return xt != yt ? (xt > yt ? -1 : 1) : strcmp(item_id(x), item_id(y));
}
static esp_err_t sort_shelf(cJSON *s) {
    cJSON *array = books(s);
    size_t n = (size_t)cJSON_GetArraySize(array);
    if (!n) return ESP_OK;
    cJSON **ptrs = wr_alloc(n * sizeof(*ptrs));
    if (!ptrs) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < n; ++i) ptrs[i] = cJSON_DetachItemFromArray(array, 0);
    qsort(ptrs, n, sizeof(*ptrs), compare);
    for (size_t i = 0; i < n; ++i) cJSON_AddItemToArray(array, ptrs[i]);
    free(ptrs);
    return ESP_OK;
}
static esp_err_t shelf_path(char *out, size_t cap) {
    int n = snprintf(out, cap, "%s/shelf.json", weread_auth_get_storage_root());
    return n >= 0 && (size_t)n < cap ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
static esp_err_t persist(cJSON *s) {
    char path[512];
    esp_err_t err = shelf_path(path, sizeof(path));
    return err == ESP_OK ? wr_write_json(path, s) : err;
}
void weread_shelf_clear(void) { WR_GUARD; weread_shelf_sync_discard();cJSON_Delete(s_shelf); s_shelf = NULL; }
size_t weread_shelf_count(void) { WR_GUARD;
    return wr_owner_matches(s_shelf) ? (size_t)cJSON_GetArraySize(books(s_shelf)) : 0;
}
uint64_t weread_shelf_synckey(void) { WR_GUARD;
    return wr_owner_matches(s_shelf) ? number(s_shelf, "synckey") : 0;
}
static bool decode(const cJSON *b, weread_book_t *out) {
    if (!b || !out) return false;
    memset(out, 0, sizeof(*out));
    wr_copy(out->book_id, sizeof(out->book_id), item_id(b));
    // 长标题只在展示副本截断；缓存保存服务端原文。/ Only the display copy truncates long titles; the cache retains originals.
    const char *keys[] = {"title", "author", "cover", "format"};
    char *dest[] = {out->title, out->author, out->cover, out->format};
    size_t caps[] = {sizeof(out->title), sizeof(out->author), sizeof(out->cover), sizeof(out->format)};
    for (size_t i = 0; i < 4; ++i) {
        const char *v = wr_string(b, keys[i]);
        if (v) wr_display_copy(dest[i], caps[i], v);
    }
    out->book_version = (uint32_t)number(b, "bookVersion");
    for (char *p = out->format; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    out->total_words = (uint32_t)number(b, "totalWords");
    out->read_update_time = number(b, "readUpdateTime");
    out->update_time = number(b, "updateTime");
    out->progress = (uint32_t)number(cJSON_GetObjectItemCaseSensitive(b, "readingProgress"), "progress");
    return true;
}
bool weread_shelf_get(size_t i, weread_book_t *out) { WR_GUARD;
    return i < weread_shelf_count() && decode(cJSON_GetArrayItem(books(s_shelf), (int)i), out);
}
bool weread_shelf_find(const char *id, weread_book_t *out) { WR_GUARD;
    return wr_valid_id(id) && wr_owner_matches(s_shelf) && decode(find(books(s_shelf), id), out);
}
uint32_t weread_shelf_book_version(const char *id){WR_GUARD;
    return wr_valid_id(id)&&wr_owner_matches(s_shelf)?(uint32_t)number(find(books(s_shelf),id),"bookVersion"):0;
}
esp_err_t weread_shelf_save(void) { WR_GUARD;
    return wr_owner_matches(s_shelf) ? persist(s_shelf) : ESP_ERR_INVALID_STATE;
}
esp_err_t weread_shelf_load(void) { WR_GUARD;
    char path[512];
    esp_err_t err = shelf_path(path, sizeof(path));
    cJSON *s = NULL;
    if (err == ESP_OK) err = wr_read_json(path, &s);
    cJSON *array = books(s), *v;
    if (err == ESP_OK && (!wr_owner_matches(s) || !cJSON_IsArray(array) ||
        !wr_uint(s, "synckey", NULL) || cJSON_GetArraySize(array) > WEREAD_SHELF_LIMIT)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK) cJSON_ArrayForEach(v, array) {
        if (!cJSON_IsObject(v) || !wr_valid_id(item_id(v)) || !readable(v) || find(array, item_id(v)) != v) {
            err = ESP_ERR_INVALID_RESPONSE; break;
        }
    }
    if (err == ESP_OK) err = sort_shelf(s);
    if (err == ESP_OK) { weread_shelf_clear(); s_shelf = s; }
    else cJSON_Delete(s);
    return err;
}

static struct {
    cJSON *next,*response,*pending;
    size_t cursor,count,skipped;
    uint64_t key,base_key;
} s_sync;
static size_t s_sync_done,s_sync_total,s_sync_skipped;
void weread_shelf_sync_discard(void){WR_GUARD;
    cJSON_Delete(s_sync.next);cJSON_Delete(s_sync.response);cJSON_Delete(s_sync.pending);
    memset(&s_sync,0,sizeof(s_sync));
}
bool weread_shelf_sync_pending(void){return s_sync.response!=NULL;}
void weread_shelf_sync_progress(size_t *done,size_t *total,size_t *skipped){
    if(done)*done=s_sync_done;
    if(total)*total=s_sync_total;
    if(skipped)*skipped=s_sync_skipped;
}
static esp_err_t sync_begin(bool full){
    weread_shelf_sync_discard();s_sync_done=s_sync_total=s_sync_skipped=0;
    if(!weread_auth_is_authenticated())return ESP_ERR_INVALID_STATE;
    if(!wr_owner_matches(s_shelf))full=true;
    s_sync.base_key=full?0:weread_shelf_synckey();
    s_sync.next=full?cJSON_CreateObject():cJSON_Duplicate(s_shelf,true);
    if(!s_sync.next)return ESP_ERR_NO_MEM;
    if(full&&!cJSON_AddArrayToObject(s_sync.next,"books"))return ESP_ERR_NO_MEM;
    char endpoint[160];snprintf(endpoint,sizeof(endpoint),"/shelf/sync?synckey=%" PRIu64 "&onlyBookid=1%s",s_sync.base_key,full?"&lectureSynckey=0":"");
    esp_err_t err=wr_request_json(endpoint,NULL,&s_sync.response);
    cJSON *r=s_sync.response,*array=books(s_sync.next),*v;
    if(err==ESP_OK&&(!wr_uint(r,"synckey",&s_sync.key)||!optional_array(r,"bookIds")||!optional_array(r,"lectureBooks")||!optional_array(r,"removed")||
        (!cJSON_HasObjectItem(r,"books")&&!cJSON_HasObjectItem(r,"bookIds"))))err=ESP_ERR_INVALID_RESPONSE;
    if(err==ESP_OK)err=merge_response(array,r);
    s_sync.pending=cJSON_CreateArray();if(!s_sync.pending)err=ESP_ERR_NO_MEM;
    if(err==ESP_OK)cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(r,"bookIds")){
        const char *id=item_id(v);if(!wr_valid_id(id)){err=ESP_ERR_INVALID_RESPONSE;break;}
        if(number(v,"type")!=0||find(cJSON_GetObjectItemCaseSensitive(r,"lectureBooks"),id)){remove_id(array,id);continue;}
        if(!find(s_sync.pending,id)){
            cJSON *value=cJSON_CreateString(id);
            if(!value||!cJSON_AddItemToArray(s_sync.pending,value)){cJSON_Delete(value);err=ESP_ERR_NO_MEM;break;}
        }
        if(cJSON_GetArraySize(s_sync.pending)>WEREAD_SHELF_LIMIT){err=ESP_ERR_INVALID_SIZE;break;}
    }
    s_sync.count=(size_t)cJSON_GetArraySize(s_sync.pending);s_sync_total=s_sync.count;
    ESP_LOGI("weread","shelf IDs result=%d ids=%u books=%d",(int)err,(unsigned)s_sync.count,cJSON_GetArraySize(array));
    return err;
}
static esp_err_t sync_advance(size_t batch_limit,bool publish){
    if(!s_sync.next)s_sync.next=cJSON_Duplicate(s_shelf,true);
    if(!s_sync.next)return ESP_ERR_NO_MEM;
    cJSON *array=books(s_sync.next),*detail=NULL,*req=NULL;
    esp_err_t err=ESP_OK;size_t end=s_sync.cursor+batch_limit;if(end>s_sync.count)end=s_sync.count;
    if(s_sync.cursor<end){
        req=cJSON_CreateObject();cJSON *batch=cJSON_AddArrayToObject(req,"bookIds");
        if(!req||!batch||!cJSON_AddArrayToObject(req,"albumIds"))err=ESP_ERR_NO_MEM;
        for(size_t j=s_sync.cursor;j<end&&err==ESP_OK;++j)
            if(!cJSON_AddItemReferenceToArray(batch,cJSON_GetArrayItem(s_sync.pending,(int)j)))err=ESP_ERR_NO_MEM;
        if(err==ESP_OK)err=wr_request_json("/shelf/syncbook",req,&detail);
        if(err==ESP_OK&&!cJSON_IsArray(books(detail)))err=ESP_ERR_INVALID_RESPONSE;
        if(err==ESP_OK)err=merge_response(array,detail);
        for(size_t j=s_sync.cursor;j<end&&err==ESP_OK;++j){
            const char *id=item_id(cJSON_GetArrayItem(s_sync.pending,(int)j));
            if(!find(array,id)&&!find(books(detail),id)){
                if(publish)++s_sync.skipped;else err=ESP_ERR_INVALID_RESPONSE;
            }
        }
        ESP_LOGI("weread","shelf batch=%u/%u result=%d returned=%d skipped=%u",(unsigned)end,(unsigned)s_sync.count,(int)err,cJSON_GetArraySize(books(detail)),(unsigned)s_sync.skipped);
    }
    cJSON_Delete(req);cJSON_Delete(detail);
    if(err==ESP_OK)err=merge_progress(array,s_sync.response);
    const char *remove_keys[]={"lectureBooks","removed"};
    for(unsigned k=0;k<2&&err==ESP_OK;++k){cJSON *v;
        cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(s_sync.response,remove_keys[k])){
            const char *id=item_id(v);if(!wr_valid_id(id)){err=ESP_ERR_INVALID_RESPONSE;break;}remove_id(array,id);
        }
    }
    if(err==ESP_OK){
        cJSON_DeleteItemFromObject(s_sync.next,"synckey");
        if(!cJSON_AddNumberToObject(s_sync.next,"synckey",(double)(end==s_sync.count&&!s_sync.skipped?s_sync.key:s_sync.base_key))||!wr_set_owner(s_sync.next))err=ESP_ERR_NO_MEM;
    }
    if(err==ESP_OK)err=sort_shelf(s_sync.next);
    if(err==ESP_OK&&(publish||end==s_sync.count)){
        err=persist(s_sync.next);
        if(err==ESP_OK){cJSON_Delete(s_shelf);s_shelf=s_sync.next;s_sync.next=NULL;}
    }
    if(err==ESP_OK){s_sync.cursor=end;s_sync_done=end;s_sync_skipped=s_sync.skipped;}
    if(err!=ESP_OK||end==s_sync.count)weread_shelf_sync_discard();
    return err;
}
esp_err_t weread_shelf_sync(bool full){WR_GUARD;
    esp_err_t err=sync_begin(full);
    if(err==ESP_OK)do{err=sync_advance(20,false);}while(err==ESP_OK&&weread_shelf_sync_pending());
    if(err!=ESP_OK)weread_shelf_sync_discard();
    return err;
}
esp_err_t weread_shelf_sync_step(bool restart){WR_GUARD;
    esp_err_t err=ESP_OK;
    if(restart||!weread_shelf_sync_pending())err=sync_begin(weread_shelf_count()==0);
    if(err==ESP_OK)err=sync_advance(s_sync.cursor?20:6,true);
    if(err!=ESP_OK)weread_shelf_sync_discard();
    return err;
}
esp_err_t weread_shelf_refresh_book(const char *id){WR_GUARD;
    if(!wr_valid_id(id)||!wr_owner_matches(s_shelf))return ESP_ERR_INVALID_ARG;
    cJSON *req=cJSON_CreateObject(),*ids=cJSON_AddArrayToObject(req,"bookIds"),*response=NULL,*next=NULL;
    esp_err_t err=ESP_ERR_NO_MEM;
    if(req&&ids&&cJSON_AddItemToArray(ids,cJSON_CreateString(id))&&cJSON_AddArrayToObject(req,"albumIds"))
        err=wr_request_json("/shelf/syncbook",req,&response);
    if(err==ESP_OK&&!find(books(response),id))err=ESP_ERR_INVALID_RESPONSE;
    if(err==ESP_OK){next=cJSON_Duplicate(s_shelf,true);if(!next)err=ESP_ERR_NO_MEM;}
    if(err==ESP_OK)err=merge_response(books(next),response);
    cJSON_Delete(req);cJSON_Delete(response);
    if(err==ESP_OK)err=persist(next);
    if(err==ESP_OK){cJSON_Delete(s_shelf);s_shelf=next;next=NULL;}
    cJSON_Delete(next);return err;
}

esp_err_t weread_cover_get_url(const char *raw, char *out, size_t cap) {
    if (!raw || !out || !cap || strpbrk(raw, "\r\n\\")) return ESP_ERR_INVALID_ARG;
    bool http = !strncmp(raw, "http://", 7), https = !strncmp(raw, "https://", 8);
    if (!http && !https) return ESP_ERR_INVALID_ARG;
    const char *host = raw + (http ? 7 : 8), *path = strchr(host, '/');
    size_t host_len = strcspn(host, "/?#");
    if (!host_len || memchr(host, '@', host_len)) return ESP_ERR_INVALID_ARG;
    const char *match = path ? strstr(path, "/s_") : NULL;
    const char *end = strpbrk(host, "?#");
    if (match && end && match >= end) match = NULL;
    size_t len = strlen(raw) + (http ? 1 : 0) + (match ? 1 : 0);
    if (len >= cap) return ESP_ERR_INVALID_SIZE;
    size_t pos = 0;
    memcpy(out, "https://", 8); pos = 8;
    const char *rest = host;
    if (match) {
        size_t n = (size_t)(match - rest);
        memcpy(out + pos, rest, n); pos += n;
        memcpy(out + pos, "/t6_", 4); pos += 4; rest = match + 3;
    }
    strcpy(out + pos, rest);
    return ESP_OK;
}
esp_err_t weread_cover_retry_url(const char *raw, const weread_cover_retry_t *r,
                               uint64_t now, char *out, size_t cap) {
    if (!r) return ESP_ERR_INVALID_ARG;
    if (now < r->not_before_ms) return ESP_ERR_TIMEOUT;
    if (!r->original) return weread_cover_get_url(raw, out, cap);
    // 原图回退仍保持 HTTPS。/ Original-size fallback still uses HTTPS.
    if (!raw) return ESP_ERR_INVALID_ARG;
    esp_err_t err = weread_cover_get_url(raw, out, cap);
    if (err != ESP_OK) return err;
    int n = snprintf(out, cap, "https://%s", raw + (!strncmp(raw, "http://", 7) ? 7 : 8));
    return n >= 0 && (size_t)n < cap ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
void weread_cover_retry_result(weread_cover_retry_t *r, int status, uint64_t now) {
    if (!r) return;
    if (status >= 200 && status < 300) { memset(r, 0, sizeof(*r)); return; }
    if (status == 404 && !r->original) { r->original = true; r->not_before_ms = now; return; }
    const unsigned delays[] = {5000, 15000, 45000};
    unsigned i = r->failures < 3 ? r->failures++ : 2;
    r->not_before_ms = now > UINT64_MAX - delays[i] ? UINT64_MAX : now + delays[i];
}
