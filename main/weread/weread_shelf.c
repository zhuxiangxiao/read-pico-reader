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
#include <time.h>

static cJSON *s_shelf;
static uint64_t s_checked_at,s_checked_key;
static size_t id_slot(const char **seen,const char *id);

static cJSON *books(cJSON *s) { return cJSON_GetObjectItemCaseSensitive(s, "books"); }
static cJSON *detail_queue(cJSON *s) { return cJSON_GetObjectItemCaseSensitive(s,"detailPending"); }
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
        if (!cJSON_IsObject(v)||!wr_valid_id(id)) return ESP_ERR_INVALID_RESPONSE;
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
static int compare_id(const void *a,const void *b){return strcmp(item_id(*(const cJSON *const *)a),item_id(*(const cJSON *const *)b));}
static esp_err_t validate_books(cJSON *array){
    size_t count=(size_t)cJSON_GetArraySize(array),i=0;
    if(count>WEREAD_SHELF_LIMIT)return ESP_ERR_INVALID_RESPONSE;
    if(!count)return ESP_OK;
    cJSON **items=wr_alloc(count*sizeof(*items));if(!items)return ESP_ERR_NO_MEM;
    esp_err_t err=ESP_OK;cJSON *v;
    cJSON_ArrayForEach(v,array){
        if(!cJSON_IsObject(v)||!wr_valid_id(item_id(v))||!readable(v)){err=ESP_ERR_INVALID_RESPONSE;break;}
        items[i++]=v;
    }
    if(err==ESP_OK){qsort(items,count,sizeof(*items),compare_id);
        for(i=1;i<count;++i)if(!compare_id(&items[i-1],&items[i])){err=ESP_ERR_INVALID_RESPONSE;break;}}
    free(items);return err;
}
static esp_err_t validate_detail_queue(cJSON *s){
    cJSON *pending=detail_queue(s);if(!pending)return ESP_OK;
    if(!cJSON_IsArray(pending)||cJSON_GetArraySize(pending)>WEREAD_SHELF_LIMIT||
       (!!pending->child!=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s,"syncPending"))))return ESP_ERR_INVALID_RESPONSE;
    if(!pending->child)return ESP_OK;
    const char **seen=wr_alloc(2048*sizeof(*seen));if(!seen)return ESP_ERR_NO_MEM;
    esp_err_t err=ESP_OK;size_t unique=0;cJSON *entry;
    cJSON_ArrayForEach(entry,pending){
        const char *id=item_id(entry);cJSON *progress=cJSON_GetObjectItemCaseSensitive(entry,"readingProgress");
        if(!wr_valid_id(id)||(!cJSON_IsString(entry)&&!cJSON_IsObject(entry))||
           (progress&&(!cJSON_IsObject(progress)||!item_id(progress)||strcmp(item_id(progress),id)))){err=ESP_ERR_INVALID_RESPONSE;break;}
        size_t slot=id_slot(seen,id);if(seen[slot]){err=ESP_ERR_INVALID_RESPONSE;break;}
        seen[slot]=id;++unique;
    }
    if(err==ESP_OK)cJSON_ArrayForEach(entry,books(s)){
        const char *id=item_id(entry);size_t slot=id_slot(seen,id);
        if(!seen[slot]){seen[slot]=id;if(++unique>WEREAD_SHELF_LIMIT){err=ESP_ERR_INVALID_SIZE;break;}}
    }
    free(seen);return err;
}
static esp_err_t pending_progress(cJSON *s,const cJSON *response){
    cJSON *entry,*pending=detail_queue(s),*array=books(s);
    cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(response,"bookProgress")){
        const char *id=item_id(entry);if(!wr_valid_id(id))return ESP_ERR_INVALID_RESPONSE;
        cJSON *node=find(pending,id);if(!node||find(array,id))continue;
        if(cJSON_IsString(node)){
            cJSON *replacement=cJSON_CreateObject();
            if(!replacement||!cJSON_AddStringToObject(replacement,"bookId",id)||!wr_set_copy(replacement,"readingProgress",entry)){
                cJSON_Delete(replacement);return ESP_ERR_NO_MEM;
            }
            cJSON_ReplaceItemViaPointer(pending,node,replacement);
        }else if(!wr_set_copy(node,"readingProgress",entry))return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
static esp_err_t resolve_pending(cJSON *s,const cJSON *response){
    cJSON *entry,*pending=detail_queue(s),*array=books(s);
    cJSON_ArrayForEach(entry,books((cJSON *)response)){
        const char *id=item_id(entry);cJSON *node=find(pending,id),*cached=find(array,id);
        const cJSON *progress=cJSON_GetObjectItemCaseSensitive(node,"readingProgress");
        if(cached&&progress&&!cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(entry,"readingProgress"))&&
           !find(cJSON_GetObjectItemCaseSensitive(response,"bookProgress"),id)){
            if(!wr_set_copy(cached,"readingProgress",progress))return ESP_ERR_NO_MEM;
            cJSON *stamp=cJSON_GetObjectItemCaseSensitive(progress,"updateTime");
            if(stamp&&!cJSON_GetObjectItemCaseSensitive(entry,"readUpdateTime")&&!wr_set_copy(cached,"readUpdateTime",stamp))return ESP_ERR_NO_MEM;
        }
        remove_id(pending,id);
    }
    return ESP_OK;
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
void weread_shelf_clear(void) { WR_GUARD; weread_shelf_sync_discard();cJSON_Delete(s_shelf); s_shelf = NULL;s_checked_at=s_checked_key=0; }
size_t weread_shelf_count(void) { WR_GUARD;
    return wr_owner_matches(s_shelf) ? (size_t)cJSON_GetArraySize(books(s_shelf)) : 0;
}
uint64_t weread_shelf_synckey(void) { WR_GUARD;
    return wr_owner_matches(s_shelf) ? number(s_shelf, "synckey") : 0;
}
bool weread_shelf_cache_fresh(uint64_t now){WR_GUARD;
    if(now>INT64_MAX||!wr_owner_matches(s_shelf))return false;
    if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s_shelf,"syncPending"))){
        uint64_t retry=number(s_shelf,"syncRetryAt");
        return (detail_queue(s_shelf)?weread_shelf_synckey():number(s_shelf,"syncTarget"))&&retry>now&&retry-now<=300;
    }
    uint64_t checked=number(s_shelf,"checkedAt");
    if(checked>now)checked=0;
    if(s_checked_key==weread_shelf_synckey()&&s_checked_at<=now&&s_checked_at>checked)checked=s_checked_at;
    return checked>=1700000000&&now>=checked&&now-checked<300;
}
static esp_err_t checked_path(char *path,size_t cap){
    int n=snprintf(path,cap,"%s/shelf-check.json",weread_auth_get_storage_root());
    return n>=0&&(size_t)n<cap?ESP_OK:ESP_ERR_INVALID_SIZE;
}
static void load_checked(void){
    char path[544];if(checked_path(path,sizeof(path)-10)!=ESP_OK)return;
    time_t current=time(NULL);if(current<1700000000)return;
    size_t base=strlen(path);uint64_t now=(uint64_t)current,key=weread_shelf_synckey();
    if(!key||cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s_shelf,"syncPending")))return;
    for(unsigned previous=0;previous<2;++previous){
        if(previous)strcpy(path+base,".previous");
        FILE *f=fopen(path,"rb");if(!f)continue;
        char text[256];size_t n=fread(text,1,sizeof(text)-1,f);
        bool ok=n&&fgetc(f)==EOF&&!ferror(f);if(fclose(f))ok=false;
        if(!ok)continue;
        text[n]=0;cJSON *o=wr_parse_json(text,n+1);
        uint64_t stamp=number(o,"checkedAt");
        if(wr_owner_matches(o)&&number(o,"synckey")==key&&stamp>=1700000000&&stamp<=now&&stamp>s_checked_at){s_checked_at=stamp;s_checked_key=key;}
        cJSON_Delete(o);
    }
}
static esp_err_t save_checked(uint64_t key){
    time_t current=time(NULL);if(current<1700000000)return ESP_OK;
    uint64_t now=(uint64_t)current;
    cJSON *o=cJSON_CreateObject();char path[512];esp_err_t err=ESP_ERR_NO_MEM;
    if(o&&wr_set_owner(o)&&cJSON_AddNumberToObject(o,"synckey",(double)key)&&cJSON_AddNumberToObject(o,"checkedAt",(double)now)){
        err=checked_path(path,sizeof(path));if(err==ESP_OK)err=wr_write_json(path,o);
    }
    cJSON_Delete(o);if(err==ESP_OK){s_checked_at=now;s_checked_key=key;}return err;
}
uint64_t weread_cover_key(const char *id,const char *url){WR_GUARD;
    const weread_auth_session_t *session=weread_auth_get_current_session();
    uint64_t key=UINT64_C(14695981039346656037);
    const char *parts[]={session?session->vid:"",id?id:"",url?url:"","gray-v1-176x240"};
    for(unsigned i=0;i<sizeof(parts)/sizeof(*parts);++i){
        const unsigned char *p=(const unsigned char *)parts[i];
        do{key=(key^*p)*UINT64_C(1099511628211);}while(*p++);
    }
    return key;
}
esp_err_t weread_cover_cache_path(const char *id,const char *kind,char *path,size_t cap){WR_GUARD;
    if(!wr_owner_matches(s_shelf)||!wr_valid_id(id)||!kind||
       (strcmp(kind,"img")&&strcmp(kind,"gray")&&strcmp(kind,"failed")))return ESP_ERR_INVALID_ARG;
    const cJSON *book=find(books(s_shelf),id);if(!book)return ESP_ERR_NOT_FOUND;
    char leaf[64];snprintf(leaf,sizeof(leaf),"cover-%016" PRIx64 ".%s",weread_cover_key(id,wr_string(book,"cover")),kind);
    return wr_cache_path(id,leaf,path,cap);
}
bool weread_cover_is_original(const char *id,const char *url){WR_GUARD;
    const char *raw=wr_owner_matches(s_shelf)?wr_string(find(books(s_shelf),id),"cover"):NULL;
    if(!raw||!url||strncmp(url,"https://",8))return false;
    if(!strncmp(raw,"http://",7))raw+=7;
    else if(!strncmp(raw,"https://",8))raw+=8;
    else return false;
    return !strcmp(raw,url+8);
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
esp_err_t weread_shelf_snapshot(weread_shelf_item_t **out,size_t *count){WR_GUARD;
    if(!out||!count)return ESP_ERR_INVALID_ARG;
    *out=NULL;*count=weread_shelf_count();if(!*count)return ESP_OK;
    weread_shelf_item_t *items=wr_alloc(*count*sizeof(*items));
    if(!items){*count=0;return ESP_ERR_NO_MEM;}
    size_t i=0;cJSON *book;
    cJSON_ArrayForEach(book,books(s_shelf)){
        weread_shelf_item_t *item=&items[i++];const char *id=item_id(book),*url=wr_string(book,"cover");
        if(!wr_valid_id(id)){free(items);*count=0;return ESP_ERR_INVALID_RESPONSE;}
        wr_copy(item->book_id,sizeof(item->book_id),id);wr_display_copy(item->title,sizeof(item->title),wr_string(book,"title"));
        wr_display_copy(item->author,sizeof(item->author),wr_string(book,"author"));item->has_cover=url&&*url;
        item->read_update_time=number(book,"readUpdateTime");item->update_time=number(book,"updateTime");
        item->chapter_index=(uint32_t)number(cJSON_GetObjectItemCaseSensitive(book,"readingProgress"),"chapterIdx");
        item->cover_key=weread_cover_key(id,url);item->progress=(uint32_t)number(cJSON_GetObjectItemCaseSensitive(book,"readingProgress"),"progress");
    }
    *out=items;return ESP_OK;
}
bool weread_shelf_find(const char *id, weread_book_t *out) { WR_GUARD;
    return wr_valid_id(id) && wr_owner_matches(s_shelf) && decode(find(books(s_shelf), id), out);
}
uint32_t weread_shelf_book_version(const char *id){WR_GUARD;
    return wr_valid_id(id)&&wr_owner_matches(s_shelf)?(uint32_t)number(find(books(s_shelf),id),"bookVersion"):0;
}
esp_err_t weread_shelf_set_version(const char *id,uint32_t version){WR_GUARD;
    cJSON *book=wr_valid_id(id)&&wr_owner_matches(s_shelf)?find(books(s_shelf),id):NULL;
    if(!book)return ESP_ERR_NOT_FOUND;
    if(!version||number(book,"bookVersion")==version)return ESP_OK;
    cJSON *value=cJSON_CreateNumber(version);if(!value)return ESP_ERR_NO_MEM;
    cJSON *old=cJSON_DetachItemFromObjectCaseSensitive(book,"bookVersion");
    cJSON_AddItemToObjectCS(book,"bookVersion",value);
    esp_err_t err=persist(s_shelf);
    // 持续OOM时恢复旧节点也不能分配key。/ Restoring the old node must not allocate a key during persistent OOM.
    if(err!=ESP_OK){cJSON_DeleteItemFromObject(book,"bookVersion");if(old)cJSON_AddItemToObjectCS(book,"bookVersion",old);}
    else cJSON_Delete(old);
    return err;
}
esp_err_t weread_shelf_save(void) { WR_GUARD;
    return wr_owner_matches(s_shelf) ? persist(s_shelf) : ESP_ERR_INVALID_STATE;
}
esp_err_t weread_shelf_load(void) { WR_GUARD;
    char path[512];
    esp_err_t err = shelf_path(path, sizeof(path));
    cJSON *s = NULL;
    if (err == ESP_OK) err = wr_read_json(path, &s);
    cJSON *array = books(s);
    const cJSON *pending=cJSON_GetObjectItemCaseSensitive(s,"syncPending");
    if (err == ESP_OK && (!wr_owner_matches(s) || !cJSON_IsArray(array) ||
        !wr_uint(s, "synckey", NULL) || (pending&&!cJSON_IsBool(pending)) || cJSON_GetArraySize(array) > WEREAD_SHELF_LIMIT)) err = ESP_ERR_INVALID_RESPONSE;
    if(err==ESP_OK)err=validate_books(array);
    if(err==ESP_OK)err=validate_detail_queue(s);
    if (err == ESP_OK) err = sort_shelf(s);
    if (err == ESP_OK) { weread_shelf_clear(); s_shelf = s;load_checked(); }
    else cJSON_Delete(s);
    return err;
}

static struct {
    cJSON *next,*response,*pending,*at;
    size_t cursor,count,skipped,reused;
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
static size_t id_slot(const char **seen,const char *id){
    uint32_t hash=2166136261u;for(const unsigned char *p=(const unsigned char *)id;*p;++p)hash=(hash^*p)*16777619u;
    size_t slot=hash&2047u;while(seen[slot]&&strcmp(seen[slot],id))slot=(slot+1)&2047u;
    return slot;
}
static bool native_removed(const cJSON *response,const char *id){
    return find(cJSON_GetObjectItemCaseSensitive(response,"removed"),id)||find(cJSON_GetObjectItemCaseSensitive(response,"lectureBooks"),id);
}
static bool native_readable(const cJSON *response,const cJSON *entry){
    const char *id=item_id(entry);const cJSON *detail=find(books((cJSON *)response),id);
    return !number(entry,"type")&&!native_removed(response,id)&&(!detail||readable(detail));
}
static esp_err_t sync_begin(bool full){
    char endpoint[160] = "";
    bool forced = full, legacy = false;
    uint64_t base_key = 0;
    {
        WR_GUARD;
        weread_shelf_sync_discard();s_sync_done=s_sync_total=s_sync_skipped=0;
        if(!weread_auth_is_authenticated())return ESP_ERR_INVALID_STATE;
        legacy=wr_owner_matches(s_shelf)&&!detail_queue(s_shelf)&&cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s_shelf,"syncPending"));
        if(!weread_shelf_synckey()||(legacy&&(!number(s_shelf,"syncTarget")||number(s_shelf,"syncTarget")==weread_shelf_synckey())))full=true;
        base_key=full?0:weread_shelf_synckey();
        s_sync.base_key=base_key;
        snprintf(endpoint,sizeof(endpoint),"/shelf/sync?synckey=%" PRIu64 "&onlyBookid=1%s",base_key,full?"&lectureSynckey=0":"");
    }
    cJSON *resp = NULL;
    esp_err_t err=wr_request_json(endpoint,NULL,&resp);
    WR_GUARD;
    if(!weread_auth_is_authenticated()){cJSON_Delete(resp);return ESP_ERR_INVALID_STATE;}
    s_sync.response=resp;
    cJSON *r=s_sync.response,*v;
    bool has_books=cJSON_HasObjectItem(r,"books")||cJSON_HasObjectItem(r,"bookIds");
    if(err==ESP_OK&&(!wr_uint(r,"synckey",&s_sync.key)||!optional_array(r,"bookIds")||!optional_array(r,"lectureBooks")||!optional_array(r,"removed")))err=ESP_ERR_INVALID_RESPONSE;
    const char *changes[]={"books","bookIds","bookProgress","lectureBooks","removed"};
    bool unchanged=err==ESP_OK&&!full&&s_sync.base_key&&s_sync.base_key==s_sync.key&&wr_owner_matches(s_shelf)&&!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(s_shelf,"syncPending"));
    for(unsigned i=0;i<sizeof(changes)/sizeof(*changes)&&err==ESP_OK;++i){
        cJSON *change=cJSON_GetObjectItemCaseSensitive(r,changes[i]);
        if(change&&!cJSON_IsArray(change)){err=ESP_ERR_INVALID_RESPONSE;break;}
        if(change&&change->child)unchanged=false;
    }
    if(err==ESP_OK&&unchanged){err=save_checked(s_sync.key);weread_shelf_sync_discard();return err;}
    if(!has_books&&err==ESP_OK&&(full||(!(detail_queue(s_shelf)&&s_sync.base_key==s_sync.key)&&!cJSON_HasObjectItem(r,"bookProgress")&&!cJSON_HasObjectItem(r,"removed")&&!cJSON_HasObjectItem(r,"lectureBooks"))))err=ESP_ERR_INVALID_RESPONSE;
    if(err!=ESP_OK)return err;
    bool resume=!forced&&legacy&&s_sync.key&&number(s_shelf,"syncTarget")==s_sync.key;
    s_sync.next=wr_owner_matches(s_shelf)?cJSON_Duplicate(s_shelf,true):cJSON_CreateObject();
    if(!s_sync.next)return ESP_ERR_NO_MEM;
    if(!books(s_sync.next)&&!cJSON_AddArrayToObject(s_sync.next,"books"))return ESP_ERR_NO_MEM;
    if(!detail_queue(s_sync.next)&&!cJSON_AddArrayToObject(s_sync.next,"detailPending"))return ESP_ERR_NO_MEM;
    cJSON *array=books(s_sync.next),*pending=detail_queue(s_sync.next);
    const char *removals[]={"lectureBooks","removed"};
    for(unsigned k=0;k<2&&err==ESP_OK;++k)cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(r,removals[k])){
        const char *id=item_id(v);if(!wr_valid_id(id)){err=ESP_ERR_INVALID_RESPONSE;break;}remove_id(array,id);remove_id(pending,id);
    }
    if(err==ESP_OK)cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(r,"bookIds")){
        const char *id=item_id(v);if(!wr_valid_id(id)){err=ESP_ERR_INVALID_RESPONSE;break;}
        if(!native_readable(r,v)){remove_id(array,id);remove_id(pending,id);}
    }
    if(err==ESP_OK)cJSON_ArrayForEach(v,books(r)){
        const char *id=item_id(v);if(!cJSON_IsObject(v)||!wr_valid_id(id)){err=ESP_ERR_INVALID_RESPONSE;break;}
        if(!readable(v)||native_removed(r,id)){remove_id(array,id);remove_id(pending,id);}
    }
    // 第一半索引去重持久队列，第二半是本次完整成员集合；均为临时PSRAM。
    // Temporary PSRAM indexes separately track queued IDs and native membership.
    bool has_members=cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(r,"bookIds"))||cJSON_GetArraySize(books(r));
    const char **indices=err==ESP_OK&&has_members?wr_alloc(4096*sizeof(*indices)):NULL;
    if(err==ESP_OK&&has_members&&!indices)err=ESP_ERR_NO_MEM;
    const char **queued=indices,**members=indices?indices+2048:NULL;size_t membership=0;
    const char *sets[]={"bookIds","books"};
    for(unsigned k=0;k<2&&err==ESP_OK;++k)cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(r,sets[k])){
        if(k?(!readable(v)||native_removed(r,item_id(v))):!native_readable(r,v))continue;
        const char *id=item_id(v);size_t slot=id_slot(members,id);
        if(!members[slot]){if(membership++==WEREAD_SHELF_LIMIT){err=ESP_ERR_INVALID_SIZE;break;}members[slot]=id;}
    }
    if(full&&err==ESP_OK){
        cJSON *arrays[]={array,pending};
        for(unsigned k=0;k<2;++k){cJSON *entry=arrays[k]->child;
            while(entry){cJSON *next=entry->next;const char *id=item_id(entry);
                if(!members||!members[id_slot(members,id)])cJSON_Delete(cJSON_DetachItemViaPointer(arrays[k],entry));
                entry=next;
            }
        }
    }
    size_t queued_count=(size_t)cJSON_GetArraySize(pending);
    if(err==ESP_OK&&queued)cJSON_ArrayForEach(v,pending){const char *id=item_id(v);queued[id_slot(queued,id)]=id;}
    if(err==ESP_OK)cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(r,"bookIds")){
        if(!native_readable(r,v))continue;
        const char *id=item_id(v);size_t slot=id_slot(queued,id);
        if(queued[slot]||(resume&&number(find(array,id),"syncBatch")==s_sync.key))continue;
        if(queued_count++==WEREAD_SHELF_LIMIT){err=ESP_ERR_INVALID_SIZE;break;}
        cJSON *value=cJSON_CreateString(id);
        if(!value||!cJSON_AddItemToArray(pending,value)){cJSON_Delete(value);err=ESP_ERR_NO_MEM;break;}
        queued[slot]=value->valuestring;
    }
    free(indices);
    if(err==ESP_OK)cJSON_ArrayForEach(v,books(r)){
        if(native_removed(r,item_id(v)))continue;
        err=merge_book(array,v);if(err!=ESP_OK)break;
    }
    if(err==ESP_OK)err=merge_progress(array,r);
    if(err==ESP_OK)err=resolve_pending(s_sync.next,r);
    if(err==ESP_OK)err=pending_progress(s_sync.next,r);
    s_sync.pending=cJSON_CreateArray();if(!s_sync.pending)err=ESP_ERR_NO_MEM;
    if(err==ESP_OK)cJSON_ArrayForEach(v,pending){
        cJSON *value=cJSON_CreateString(item_id(v));
        if(!value||!cJSON_AddItemToArray(s_sync.pending,value)){cJSON_Delete(value);err=ESP_ERR_NO_MEM;break;}
        ++s_sync.count;
    }
    s_sync_total=s_sync.count;s_sync.at=s_sync.pending?s_sync.pending->child:NULL;
    return err;
}
static esp_err_t sync_advance(size_t batch_limit,bool publish){
    cJSON *detail=NULL,*req=NULL,*batch=NULL;
    esp_err_t err=ESP_OK;size_t end=0,requested=0;
    cJSON *next=NULL;
    {
        WR_GUARD;
        if(!s_sync.response)return ESP_ERR_INVALID_STATE;
        if(!s_sync.next)s_sync.next=cJSON_Duplicate(s_shelf,true);
        if(!s_sync.next)return ESP_ERR_NO_MEM;
        cJSON *pending=detail_queue(s_sync.next);
        end=s_sync.cursor;next=s_sync.at;
        if(next){
            req=cJSON_CreateObject();batch=req?cJSON_AddArrayToObject(req,"bookIds"):NULL;
            if(!req||!batch||!cJSON_AddArrayToObject(req,"albumIds"))err=ESP_ERR_NO_MEM;
            while(next&&requested<batch_limit&&err==ESP_OK){
                cJSON *item=next;next=next->next;++end;
                if(!find(pending,item_id(item)))continue;
                cJSON *dup=cJSON_CreateString(item_id(item));
                if(!dup||!cJSON_AddItemToArray(batch,dup)){cJSON_Delete(dup);err=ESP_ERR_NO_MEM;}
                else ++requested;
            }
        }
    }
    if(err==ESP_OK&&requested)err=wr_request_json("/shelf/syncbook",req,&detail);
    WR_GUARD;
    if(!s_sync.response||!s_sync.next){cJSON_Delete(req);cJSON_Delete(detail);return ESP_ERR_INVALID_STATE;}
    cJSON *array=books(s_sync.next),*pending=detail_queue(s_sync.next);
    if(requested){
        if(err==ESP_OK&&!cJSON_IsArray(books(detail)))err=ESP_ERR_INVALID_RESPONSE;
        if(err==ESP_OK)err=merge_response(array,detail);
        if(err==ESP_OK)err=resolve_pending(s_sync.next,detail);
        cJSON *entry;
        cJSON_ArrayForEach(entry,batch){
            if(err!=ESP_OK)break;
            if(!find(books(detail),item_id(entry))){if(publish)++s_sync.skipped;else err=ESP_ERR_INVALID_RESPONSE;}
        }
    }
    cJSON_Delete(req);cJSON_Delete(detail);
    const char *remove_keys[]={"lectureBooks","removed"};
    for(unsigned k=0;k<2&&err==ESP_OK;++k){cJSON *v;
        cJSON_ArrayForEach(v,cJSON_GetObjectItemCaseSensitive(s_sync.response,remove_keys[k])){
            const char *id=item_id(v);if(!wr_valid_id(id)){err=ESP_ERR_INVALID_RESPONSE;break;}remove_id(array,id);remove_id(pending,id);
        }
    }
    if(err==ESP_OK){
        size_t unresolved=(size_t)cJSON_GetArraySize(pending);
        if(!unresolved){end=s_sync.count;next=NULL;}
        if(end==s_sync.count)s_sync.skipped=unresolved;
        cJSON_DeleteItemFromObject(s_sync.next,"syncPending");
        cJSON_DeleteItemFromObject(s_sync.next,"syncTarget");
        cJSON_DeleteItemFromObject(s_sync.next,"syncRetryAt");
        if(unresolved&&!cJSON_AddBoolToObject(s_sync.next,"syncPending",true))err=ESP_ERR_NO_MEM;
        cJSON *cached;cJSON_ArrayForEach(cached,array)cJSON_DeleteItemFromObject(cached,"syncBatch");
        // 成员变化、进度和完整待补队列同事务提交后，原生游标不再被单本缺详情拖住。
        // Native membership/progress and the complete unresolved queue commit together, independently of missing details.
        cJSON_DeleteItemFromObject(s_sync.next,"synckey");
        if(!cJSON_AddNumberToObject(s_sync.next,"synckey",(double)s_sync.key)||!wr_set_owner(s_sync.next))err=ESP_ERR_NO_MEM;
        time_t now=time(NULL);
        if(err==ESP_OK&&end==s_sync.count&&unresolved&&now>=1700000000&&!cJSON_AddNumberToObject(s_sync.next,"syncRetryAt",(double)now+300))err=ESP_ERR_NO_MEM;
        if(err==ESP_OK&&end==s_sync.count&&!unresolved&&now>=1700000000){
            cJSON_DeleteItemFromObject(s_sync.next,"checkedAt");
            if(!cJSON_AddNumberToObject(s_sync.next,"checkedAt",(double)now))err=ESP_ERR_NO_MEM;
        }
    }
    if(err==ESP_OK)err=validate_detail_queue(s_sync.next);
    if(err==ESP_OK)err=sort_shelf(s_sync.next);
    if(err==ESP_OK&&(publish||end==s_sync.count)){
        err=persist(s_sync.next);
        if(err==ESP_OK){cJSON_Delete(s_shelf);s_shelf=s_sync.next;s_sync.next=NULL;s_checked_at=s_checked_key=0;}
    }
    if(err==ESP_OK){s_sync.cursor=end;s_sync.at=next;s_sync_done=end;s_sync_skipped=s_sync.skipped;}
    if(err!=ESP_OK||end==s_sync.count)weread_shelf_sync_discard();
    return err;
}
esp_err_t weread_shelf_sync(bool full){ WR_MEMORY_SCOPE;
    esp_err_t err=sync_begin(full);
    while(err==ESP_OK&&weread_shelf_sync_pending())err=sync_advance(20,false);
    if(err!=ESP_OK)weread_shelf_sync_discard();
    return err;
}
esp_err_t weread_shelf_sync_step(bool restart){ WR_MEMORY_SCOPE;
    esp_err_t err=ESP_OK;
    if(restart||!weread_shelf_sync_pending())err=sync_begin(false);
    if(err==ESP_OK&&weread_shelf_sync_pending())err=sync_advance(s_sync.cursor?20:6,true);
    if(err!=ESP_OK)weread_shelf_sync_discard();
    return err;
}
esp_err_t weread_shelf_refresh_book(const char *id){ WR_MEMORY_SCOPE;
    cJSON *req=NULL,*ids=NULL,*response=NULL,*next=NULL;
    esp_err_t err=ESP_ERR_NO_MEM;
    {
        WR_GUARD;
        if(!wr_valid_id(id)||!wr_owner_matches(s_shelf))return ESP_ERR_INVALID_ARG;
        req=cJSON_CreateObject();ids=cJSON_AddArrayToObject(req,"bookIds");
        if(!req||!ids||!cJSON_AddItemToArray(ids,cJSON_CreateString(id))||!cJSON_AddArrayToObject(req,"albumIds")){
            cJSON_Delete(req);return ESP_ERR_NO_MEM;
        }
    }
    err=wr_request_json("/shelf/syncbook",req,&response);
    WR_GUARD;
    if(!wr_owner_matches(s_shelf))err=ESP_ERR_INVALID_STATE;
    if(err==ESP_OK&&!find(books(response),id))err=ESP_ERR_INVALID_RESPONSE;
    if(err==ESP_OK){next=cJSON_Duplicate(s_shelf,true);if(!next)err=ESP_ERR_NO_MEM;}
    if(err==ESP_OK)err=merge_response(books(next),response);
    if(err==ESP_OK)err=resolve_pending(next,response);
    if(err==ESP_OK&&detail_queue(next)&&!detail_queue(next)->child){
        cJSON_DeleteItemFromObject(next,"syncPending");cJSON_DeleteItemFromObject(next,"syncRetryAt");
    }
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
