/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：进度写卡先于联网；Web 回退必须收到 succ=1，空对象不算成功。
 * English: Persist before networking; web fallback requires succ=1 and never accepts an empty object.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Os")
#endif
#include "weread_progress.h"
#include "weread_store.h"
#include "weread_shelf.h"
#include "weread_client.h"
#include "weread_auth.h"
#include "weread_chapter.h"
#include "book_weread_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <stddef.h>
#include <zlib.h>
#if defined(ESP_PLATFORM)
#include "esp_log.h"
#include "nvs.h"
#include "read_pico_sd.h"
_Static_assert(WEREAD_MEDIA_ID_BYTES==READ_PICO_SD_MEDIA_ID_BYTES,"media identity ABI");
#endif
static WR_EXT_BSS char s_retry_id[WEREAD_BOOK_ID_CAP];
static uint64_t s_retry_at, s_retry_started;
static WR_EXT_BSS struct { char id[WEREAD_BOOK_ID_CAP]; uint32_t session; weread_progress_t cloud; } s_observed;
static WR_EXT_BSS struct { DIR *dir; uint32_t session; uint64_t next_scan; char hint[WEREAD_BOOK_ID_CAP]; } s_outbox;
static uint32_t session_stamp(void) {
    const weread_auth_session_t *session=weread_auth_get_current_session();
    if(!session)return 0;
    uint32_t hash=2166136261u;
    const char *parts[]={weread_auth_get_storage_root(),session->vid,session->access_token,session->refresh_token};
    for(size_t i=0;i<sizeof(parts)/sizeof(*parts);++i)for(const unsigned char *c=(const unsigned char*)parts[i];*c;++c)hash=(hash^*c)*16777619u;
    return hash?hash:1;
}
typedef struct {
    weread_client_identity_t client;
    uint8_t media[WEREAD_MEDIA_ID_BYTES];
    bool media_valid;
} progress_identity_t;
static bool rescue_enabled(void);
static bool identity_read(progress_identity_t *identity){
    memset(identity,0,sizeof(*identity));
    if(!weread_client_identity_get(&identity->client))return false;
    identity->media_valid=weread_progress_capture_media(identity->media);
    return identity->media_valid||!rescue_enabled();
}
static bool identity_matches(const progress_identity_t *identity){
    const weread_auth_session_t *s=weread_auth_get_current_session();
    const weread_client_identity_t *client=&identity->client;uint8_t media[WEREAD_MEDIA_ID_BYTES];
    return s&&!strcmp(client->root,weread_auth_get_storage_root())&&!strcmp(client->vid,s->vid)&&
        !strcmp(client->token,s->access_token)&&!strcmp(client->refresh,s->refresh_token)&&!strcmp(client->device,s->device_id)&&
        (!identity->media_valid||(weread_progress_capture_media(media)&&!memcmp(identity->media,media,sizeof(media))));
}
static void baseline(weread_progress_t *p,const weread_progress_t *cloud) {
    p->base_uid=cloud->chapter_uid;p->base_index=cloud->chapter_index;p->base_offset=cloud->chapter_offset;
    p->base_version=cloud->book_version;p->base_known=true;
}
static uint32_t next_revision(uint32_t revision) { return revision==UINT32_MAX?1:revision+1; }

// 仅写卡故障时写内部flash；两个槽绝不淘汰另一份未恢复的位置。/ Internal flash is written only after TF failures; neither slot evicts an unrelated position.
typedef struct {
    uint32_t magic,size,crc;
    uint8_t media[WEREAD_MEDIA_ID_BYTES];
    char root[256],vid[32],device[64],id[WEREAD_BOOK_ID_CAP];
    weread_progress_t expected,desired;
} rescue_record_t;
_Static_assert(sizeof(rescue_record_t)<=1280,"bounded rescue record");
static bool s_rescue_choice;
static bool s_rescue_empty_known;
static uint8_t s_rescue_empty_card[WEREAD_MEDIA_ID_BYTES];
static char s_rescue_empty_key[360];
static inline void rescue_cache_invalidate(void) { s_rescue_empty_known = false; }
#if !defined(ESP_PLATFORM)
static weread_progress_rescue_backend_t s_rescue_backend;
void weread_progress_rescue_backend(const weread_progress_rescue_backend_t *backend){WR_GUARD;
    rescue_cache_invalidate();
    if(backend)s_rescue_backend=*backend;else memset(&s_rescue_backend,0,sizeof(s_rescue_backend));
}
#endif
static bool media_id(uint8_t out[WEREAD_MEDIA_ID_BYTES]){
    memset(out,0,WEREAD_MEDIA_ID_BYTES);
#if defined(ESP_PLATFORM)
    return read_pico_sd_media_id(out);
#else
    return s_rescue_backend.media&&s_rescue_backend.media(out,s_rescue_backend.ctx);
#endif
}
bool weread_progress_capture_media(uint8_t out[WEREAD_MEDIA_ID_BYTES]){
    return out&&media_id(out);
}
static esp_err_t rescue_io(unsigned slot,void *data,unsigned operation){
#if defined(ESP_PLATFORM)
    nvs_handle_t handle;esp_err_t err=nvs_open("wr_rescue",operation?NVS_READWRITE:NVS_READONLY,&handle);
    if(err!=ESP_OK)return err==ESP_ERR_NVS_NOT_FOUND?ESP_ERR_NOT_FOUND:err;
    const char *key=slot?"s1":"s0";size_t bytes=sizeof(rescue_record_t);
    if(operation==0)err=nvs_get_blob(handle,key,data,&bytes);
    else if(operation==1)err=nvs_set_blob(handle,key,data,bytes);
    else err=nvs_erase_key(handle,key);
    if(err==ESP_OK&&operation)err=nvs_commit(handle);
    nvs_close(handle);
    if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_ERR_NOT_FOUND;
    if(err==ESP_OK&&operation==0&&bytes!=sizeof(rescue_record_t))return ESP_ERR_INVALID_SIZE;
    return err;
#else
    if(operation==0)return s_rescue_backend.read?s_rescue_backend.read(slot,data,sizeof(rescue_record_t),s_rescue_backend.ctx):ESP_ERR_NOT_FOUND;
    if(operation==1)return s_rescue_backend.write?s_rescue_backend.write(slot,data,sizeof(rescue_record_t),s_rescue_backend.ctx):ESP_ERR_NOT_SUPPORTED;
    return s_rescue_backend.erase?s_rescue_backend.erase(slot,s_rescue_backend.ctx):ESP_ERR_NOT_SUPPORTED;
#endif
}
static uint32_t rescue_crc(const rescue_record_t *record){
    return (uint32_t)crc32(0,(const Bytef*)record+offsetof(rescue_record_t,media),sizeof(*record)-offsetof(rescue_record_t,media));
}
static esp_err_t rescue_read(unsigned slot,rescue_record_t *record){
    memset(record,0,sizeof(*record));esp_err_t err=rescue_io(slot,record,0);if(err!=ESP_OK)return err;
    if(record->magic!=0x57525231u||record->size!=sizeof(*record)||record->crc!=rescue_crc(record))return ESP_ERR_INVALID_CRC;
    if(!memchr(record->root,0,sizeof(record->root))||!memchr(record->vid,0,sizeof(record->vid))||
       !memchr(record->device,0,sizeof(record->device))||!memchr(record->id,0,sizeof(record->id))||
       !wr_valid_id(record->id)||!wr_valid_id(record->vid)||!record->desired.chapter_uid||
       record->desired.progress>100||!memchr(record->desired.summary,0,sizeof(record->desired.summary)))return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}
static bool rescue_owner(const rescue_record_t *record,const uint8_t *card){
    const weread_auth_session_t *session=weread_auth_get_current_session();
    return session&&card&&!memcmp(record->media,card,WEREAD_MEDIA_ID_BYTES)&&
        !strcmp(record->root,weread_auth_get_storage_root())&&!strcmp(record->vid,session->vid)&&!strcmp(record->device,session->device_id);
}
static esp_err_t rescue_find(const char *id,const uint8_t *card,rescue_record_t *record,unsigned *slot){
    const weread_auth_session_t *session=weread_auth_get_current_session();
    const char *root=weread_auth_get_storage_root();
    char key[sizeof(s_rescue_empty_key)];
    int klen = (session && root && card)
        ? snprintf(key, sizeof(key), "%s\n%s\n%s", root, session->vid, session->device_id) : -1;
    bool have_key = klen > 0 && (size_t)klen < sizeof(key);
    if (s_rescue_empty_known && have_key &&
        !memcmp(s_rescue_empty_card, card, sizeof(s_rescue_empty_card)) &&
        !strcmp(s_rescue_empty_key, key)) {
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t failure=ESP_ERR_NOT_FOUND;
    bool all_empty=true;
    for(unsigned i=0;i<2;++i){
        esp_err_t err=rescue_read(i,record);
        if(err!=ESP_ERR_NOT_FOUND){all_empty=false;rescue_cache_invalidate();}
        if(err==ESP_OK&&rescue_owner(record,card)&&(!id||!strcmp(record->id,id))){*slot=i;return ESP_OK;}
        if(err!=ESP_OK&&err!=ESP_ERR_NOT_FOUND)failure=err;
    }
    if (all_empty && have_key) {
        memcpy(s_rescue_empty_key, key, (size_t)klen + 1u);
        memcpy(s_rescue_empty_card, card, sizeof(s_rescue_empty_card));
        s_rescue_empty_known = true;
    }
    return failure;
}
static bool rescue_enabled(void){
#if defined(ESP_PLATFORM)
    return true;
#else
    return s_rescue_backend.read!=NULL;
#endif
}
static esp_err_t rescue_write(unsigned slot,rescue_record_t *record){
    rescue_cache_invalidate();
    record->magic=0x57525231u;record->size=sizeof(*record);record->crc=rescue_crc(record);
    return rescue_io(slot,record,1);
}
static bool progress_equal(const weread_progress_t *a,const weread_progress_t *b){
    return a->chapter_uid==b->chapter_uid&&a->chapter_index==b->chapter_index&&a->chapter_offset==b->chapter_offset&&
        a->progress==b->progress&&a->book_version==b->book_version&&a->reading_time==b->reading_time&&
        a->updated_at==b->updated_at&&a->last_report_ms==b->last_report_ms&&a->pending_pages==b->pending_pages&&
        a->dirty==b->dirty&&a->position_pending==b->position_pending&&a->revision==b->revision&&
        a->base_uid==b->base_uid&&a->base_index==b->base_index&&a->base_offset==b->base_offset&&a->base_version==b->base_version&&
        a->base_known==b->base_known&&a->sync_conflict==b->sync_conflict&&a->retry_after==b->retry_after&&
        a->retry_count==b->retry_count&&!strcmp(a->summary,b->summary);
}
static esp_err_t load_tf_ex(const char *id,weread_progress_t *out,bool *primary_owned);
static esp_err_t save_tf_ex(const char *id,const weread_progress_t *p,bool owner_verified);
static inline esp_err_t load_tf(const char *id,weread_progress_t *out){return load_tf_ex(id,out,NULL);}
static inline esp_err_t save_tf(const char *id,const weread_progress_t *p){return save_tf_ex(id,p,false);}


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
    const cJSON *sync=cJSON_GetObjectItemCaseSensitive(o,"sync");
    if(sync){
        if(!cJSON_IsObject(sync)||!field(sync,"revision",&p->revision,true)||
           !field(sync,"uid",&p->base_uid,true)||!field(sync,"index",&p->base_index,true)||
           !field(sync,"offset",&p->base_offset,true)||!field(sync,"version",&p->base_version,true))return false;
        if(!cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(sync,"known"))||
           !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(sync,"conflict"))||!wr_uint(sync,"retryAfter",&p->retry_after))return false;
        p->base_known=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sync,"known"));
        p->sync_conflict=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sync,"conflict"));
        p->retry_after=number(sync,"retryAfter");
        uint32_t attempts=0;if(!field(sync,"attempts",&attempts,false)||attempts>5)return false;p->retry_count=attempts;
    }
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
static esp_err_t owner_path(const char *id,const char *vid,char out[512]){
    if(!wr_valid_id(vid)||strlen(vid)>=WEREAD_VID_MAX_LEN)return ESP_ERR_INVALID_RESPONSE;
    char leaf[64];snprintf(leaf,sizeof(leaf),"progress.json.%s",vid);
    return wr_cache_path(id,leaf,out,512);
}
typedef struct {
    weread_progress_t record;
    char archive[512];
} foreign_owner_work_t;
typedef struct {
    rescue_record_t record;
    weread_progress_t current;
} rescue_save_work_t;
typedef struct {
    char path[512], recovery[600];
    weread_progress_t foreign;
} load_tf_work_t;
typedef struct {
    rescue_record_t record;
    weread_progress_t delta, old, next;
} rescue_store_work_t;
// 共享同一书号的账号切换不能覆盖另一账号的待报位置。/ Switching accounts on the same book must retain the previous owner's pending position.
static esp_err_t retain_foreign_owner(const char *id,const char *path){
    cJSON *old=NULL;esp_err_t err=wr_read_json_primary(path,&old);
    if(err==ESP_ERR_NOT_FOUND)err=wr_read_json(path,&old);
    if(err==ESP_ERR_NOT_FOUND)return ESP_OK;
    if(err==ESP_ERR_NO_MEM)return err;
    if(err!=ESP_OK){
        cJSON *backup=NULL;
        if(wr_read_json(path,&backup)==ESP_OK){
            foreign_owner_work_t *w __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*w));
            const char *saved_id=wr_string(backup,"bookId");
            if(!w)err=ESP_ERR_NO_MEM;
            else if(wr_owner_matches(backup)&&saved_id&&!strcmp(saved_id,id)&&decode(backup,&w->record,false))err=ESP_OK;
        }
        cJSON_Delete(backup);return err;
    }
    if(!wr_owner_matches(old)){
        foreign_owner_work_t *w __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*w));
        const char *saved_id=wr_string(old,"bookId");
        if(!w)err=ESP_ERR_NO_MEM;
        else{
            err=saved_id&&!strcmp(saved_id,id)&&decode(old,&w->record,false)?
                owner_path(id,wr_string(old,"vid"),w->archive):ESP_ERR_INVALID_RESPONSE;
            if(err==ESP_OK)err=wr_write_json(w->archive,old);
        }
    }
    cJSON_Delete(old);return err;
}
static esp_err_t save_tf_ex(const char *id, const weread_progress_t *p, bool owner_verified) {
    char *path __attribute__((cleanup(wr_release))) = wr_alloc(512);
    if (!path) return ESP_ERR_NO_MEM;
    esp_err_t err = wr_cache_path(id, "progress.json", path, 512);
    if (err != ESP_OK) return err;
    if(!owner_verified){err=retain_foreign_owner(id,path);if(err!=ESP_OK)return err;}
    cJSON *o = encode(p);
    cJSON *sync=o?cJSON_AddObjectToObject(o,"sync"):NULL;
    if (!sync || !cJSON_AddNumberToObject(sync,"revision",p->revision)||
        !cJSON_AddNumberToObject(sync,"uid",p->base_uid)||!cJSON_AddNumberToObject(sync,"index",p->base_index)||
        !cJSON_AddNumberToObject(sync,"offset",p->base_offset)||!cJSON_AddNumberToObject(sync,"version",p->base_version)||
        !cJSON_AddBoolToObject(sync,"known",p->base_known)||!cJSON_AddBoolToObject(sync,"conflict",p->sync_conflict)||
        !cJSON_AddNumberToObject(sync,"retryAfter",(double)p->retry_after)||!cJSON_AddNumberToObject(sync,"attempts",p->retry_count)||
        !wr_set_owner(o) || !cJSON_AddStringToObject(o, "bookId", id)) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = wr_write_json(path, o);
    cJSON_Delete(o);
    if(err==ESP_OK&&p->dirty&&p->position_pending){s_outbox.next_scan=0;wr_copy(s_outbox.hint,sizeof(s_outbox.hint),id);}
    return err;
}
static esp_err_t save(const char *id,const weread_progress_t *p){
    uint8_t card[WEREAD_MEDIA_ID_BYTES];
    if(!rescue_enabled())return save_tf(id,p);
    if(!media_id(card))return ESP_ERR_INVALID_STATE;
    rescue_save_work_t *w __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*w));
    if(!w)return ESP_ERR_NO_MEM;
    unsigned slot;esp_err_t err=rescue_find(id,card,&w->record,&slot);
    if(err==ESP_ERR_NOT_FOUND)return save_tf(id,p);
    if(err!=ESP_OK)return err;
    err=load_tf(id,&w->current);
    bool safe=err==ESP_ERR_NOT_FOUND||(err==ESP_OK&&(progress_equal(&w->current,&w->record.expected)||progress_equal(&w->current,&w->record.desired)));
    if(err==ESP_OK&&safe)w->record.expected=w->current;
    w->record.desired=*p;
    err=rescue_write(slot,&w->record);if(err!=ESP_OK)return err;
    if(safe||s_rescue_choice){
        err=save_tf(id,&w->record.desired);
        // 删除失败时已落卡和NVS内容相同，继续保留应急槽，下次幂等恢复。/ An erase failure retains an identical durable rescue copy for idempotent recovery.
        if(err==ESP_OK)(void)rescue_io(slot,NULL,2);
    }
    return ESP_OK;
}
static esp_err_t recover_owned(const char *path,const char *id,weread_progress_t *out,bool archive){
    cJSON *o=NULL;esp_err_t err=(archive?wr_read_json:wr_read_json_primary)(path,&o);
    const char *saved_id=wr_string(o,"bookId");
    if(err==ESP_OK&&(!wr_owner_matches(o)||!saved_id||strcmp(saved_id,id)||!decode(o,out,false)))err=ESP_ERR_INVALID_RESPONSE;
    cJSON_Delete(o);return err;
}
static esp_err_t load_tf_ex(const char *id, weread_progress_t *out, bool *primary_owned) { WR_GUARD;
    if (primary_owned) *primary_owned = false;
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    load_tf_work_t *w __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*w));
    if (!w) return ESP_ERR_NO_MEM;
    cJSON *o = NULL;
    esp_err_t err = wr_cache_path(id, "progress.json", w->path, sizeof(w->path));
    if(err!=ESP_OK)return err;
    err=wr_read_json_primary(w->path,&o);
    const char *saved_id = wr_string(o, "bookId");
    const char *vid=wr_string(o,"vid");
    bool foreign_valid=err==ESP_OK&&!wr_owner_matches(o)&&saved_id&&!strcmp(saved_id,id)&&
        wr_valid_id(vid)&&strlen(vid)<WEREAD_VID_MAX_LEN&&decode(o,&w->foreign,false);
    bool allow_archive=err==ESP_ERR_NOT_FOUND||foreign_valid;
    if (err == ESP_OK && (!wr_owner_matches(o) || !saved_id || strcmp(saved_id, id) || !decode(o, out, false))) err = ESP_ERR_INVALID_RESPONSE;
    cJSON_Delete(o);
    // 临时IO或OOM不能用旧副本冒充当前事务。/ Transient I/O and OOM must not turn a stale backup into the current transaction.
    if(err!=ESP_OK&&err!=ESP_ERR_NOT_FOUND&&err!=ESP_ERR_INVALID_RESPONSE&&err!=ESP_ERR_INVALID_SIZE){memset(out,0,sizeof(*out));return err;}
    if(err==ESP_OK){if(primary_owned)*primary_owned=true;return ESP_OK;}
    if(err==ESP_ERR_NO_MEM){memset(out,0,sizeof(*out));return err;}
    // 主文件缺失/损坏时优先本账号事务副本；明确切换账号时优先该账号归档。/ Prefer the same-owner transaction backup after interrupted commits, and owner archives after an explicit account switch.
    for(unsigned pass=0;pass<2;++pass){
        bool archive=pass? !foreign_valid:foreign_valid;
        if(archive){
            const weread_auth_session_t *session=weread_auth_get_current_session();
            if(!allow_archive||!session||owner_path(id,session->vid,w->recovery)!=ESP_OK)continue;
        }else{
            int length=snprintf(w->recovery,sizeof(w->recovery),"%s.previous",w->path);
            if(length<0||(size_t)length>=sizeof(w->recovery))continue;
        }
        esp_err_t recovered=recover_owned(w->recovery,id,out,archive);
        if(recovered==ESP_OK||recovered==ESP_ERR_NO_MEM){if(recovered!=ESP_OK)memset(out,0,sizeof(*out));return recovered;}
    }
    if(err!=ESP_OK)memset(out,0,sizeof(*out));
    return err;
}
esp_err_t weread_progress_load(const char *id,weread_progress_t *out){WR_GUARD;
    if(!out||!wr_valid_id(id))return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));uint8_t card[WEREAD_MEDIA_ID_BYTES];
    if(rescue_enabled()){
        if(!media_id(card))return ESP_ERR_INVALID_STATE;
        rescue_record_t *record __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*record));
        if(!record)return ESP_ERR_NO_MEM;
        unsigned slot;esp_err_t err=rescue_find(id,card,record,&slot);
        if(err==ESP_OK){*out=record->desired;return ESP_OK;}
        if(err!=ESP_ERR_NOT_FOUND)return err;
    }
    return load_tf(id,out);
}
static esp_err_t rescue_restore(const char *id,weread_progress_t *out){
    uint8_t card[WEREAD_MEDIA_ID_BYTES];
    if(!rescue_enabled()||!media_id(card))return ESP_ERR_NOT_FOUND;
    rescue_save_work_t *w __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*w));
    if(!w)return ESP_ERR_NO_MEM;
    unsigned slot;esp_err_t err=rescue_find(id,card,&w->record,&slot);if(err!=ESP_OK)return err;
    *out=w->record.desired;err=load_tf(id,&w->current);
    if(err!=ESP_OK&&err!=ESP_ERR_NOT_FOUND)return err;
    if(err==ESP_OK&&!progress_equal(&w->current,&w->record.expected)&&!progress_equal(&w->current,&w->record.desired))return WEREAD_ERR_PROGRESS_CONFLICT;
    if(err!=ESP_OK||!progress_equal(&w->current,&w->record.desired)){
        err=save_tf(id,&w->record.desired);if(err!=ESP_OK)return err;
    }
    return rescue_io(slot,NULL,2);
}
static int error_code(const cJSON *r) {
    const cJSON *v = cJSON_GetObjectItem(r, "errcode");
    if (!v) v = cJSON_GetObjectItem(r, "errCode");
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
static esp_err_t request_mode(const char *url, const char *body, cJSON **out, int *status, progress_identity_t *identity) {
    char *text = NULL; size_t len = 0; *out = NULL;
    if(identity){WR_GUARD;if(!identity_matches(identity)){*status=0;return ESP_ERR_INVALID_STATE;}}
    const char *headers=body?"Content-Type: application/json\r\n":NULL;
    bool skip_renew=(strstr(url,"/book/getProgress")!=NULL&&!strstr(url,"/web/book/getProgress"))||
        (strstr(url,"/book/read")!=NULL&&!strstr(url,"/web/book/read"));
    esp_err_t err=identity?(skip_renew?weread_client_request_bound(url,body?"POST":"GET",headers,body,&text,&len,status,&identity->client):
        weread_client_request_bound_renew(url,body?"POST":"GET",headers,body,&text,&len,status,&identity->client)):
        weread_client_request(url,body?"POST":"GET",headers,body,&text,&len,status);
    if(identity){WR_GUARD;if(!identity_matches(identity)){free(text);*status=0;return ESP_ERR_INVALID_STATE;}}
    if (text && len && len <= 65536) { WR_GUARD; *out = wr_parse_json(text,len+1); }
    free(text);
    if(err==ESP_OK && !*out && wr_memory_failed())err=ESP_ERR_NO_MEM;
    if (err == ESP_OK && (*status != 200 || !cJSON_IsObject(*out) || error_code(*out))) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}
static bool fallback(const cJSON *r, int status) { return status == 401 || error_code(r) == -2012; }
static esp_err_t fetch_progress(const char *id,weread_progress_t *cloud,uint32_t *version,uint32_t *last, progress_identity_t *identity, bool allow_unread){
    memset(cloud,0,sizeof(*cloud));
    // 原生syncbook同时返回完整进度，避免不兼容getProgress引发续期/重放/Web链。
    // Native syncbook supplies complete positions without the unsupported getProgress renewal/replay/web chain.
    char body[128], url[768];
    snprintf(body,sizeof(body),"{\"bookIds\":[\"%s\"],\"albumIds\":[]}",id);
    snprintf(url,sizeof(url),"%s/shelf/syncbook",weread_client_get_base_url());
    int status = 0; cJSON *r = NULL;
    esp_err_t err = request_mode(url,body,&r,&status,identity);
    if(err!=ESP_OK && (!status || err==ESP_ERR_NO_MEM)){cJSON_Delete(r);return err;}
    const cJSON *book = NULL;uint32_t fresh_version=0,last_index=0;
    bool unread_syncbook = false, has_detail = false;
    if(err==ESP_OK){
        const cJSON *detail=NULL,*entry;
        cJSON_ArrayForEach(entry,cJSON_GetObjectItemCaseSensitive(r,"books")){
            const char *saved_id=wr_string(entry,"bookId");
            if(saved_id&&!strcmp(saved_id,id)){if(detail){cJSON_Delete(r);return ESP_ERR_INVALID_RESPONSE;}detail=entry;}
        }
        has_detail = (detail != NULL);
        if(detail&&!field(detail,"bookVersion",&fresh_version,false)){cJSON_Delete(r);return ESP_ERR_INVALID_RESPONSE;}
        if(detail&&!field(detail,"lastChapterIdx",&last_index,false))last_index=0;
        const cJSON *positions=cJSON_GetObjectItemCaseSensitive(r,"bookProgress"),*p;
        if(cJSON_IsArray(positions))cJSON_ArrayForEach(p,positions){
            const char *saved_id=wr_string(p,"bookId");
            if(saved_id&&!strcmp(saved_id,id)){if(book){cJSON_Delete(r);return ESP_ERR_INVALID_RESPONSE;}book=p;}
        }
        if(book){
            bool has_all=true;
            const char *required[]={"chapterUid","chapterIdx","chapterOffset","progress"};
            for(unsigned i=0;i<sizeof(required)/sizeof(*required);++i)
                if(!cJSON_GetObjectItemCaseSensitive(book,required[i]))has_all=false;
            // 原生syncbook对未读书籍可只给chapterUid=0或空进度数组，避免回退不兼容的getProgress/Web链。
            // Native syncbook may report unread books with chapterUid=0 or an empty bookProgress entry; avoid falling back to getProgress/web.
            if(has_all){
                if(!decode(book,cloud,true)){memset(cloud,0,sizeof(*cloud));book=NULL;}
            }else if(allow_unread&&detail&&(!positions||cJSON_IsArray(positions))&&
                     !cJSON_HasObjectItem(book,"chapterIdx")&&!cJSON_HasObjectItem(book,"chapterOffset")&&
                     decode(book,cloud,true)&&!cloud->chapter_uid){
                unread_syncbook=true;
            }else{
                memset(cloud,0,sizeof(*cloud));book=NULL;
            }
        }
#if defined(ESP_PLATFORM)
        else if(allow_unread&&detail&&(!positions||cJSON_IsArray(positions))){
            unread_syncbook=true;
        }
#else
        else if(allow_unread&&detail&&cJSON_IsArray(positions)){
            unread_syncbook=true;
        }
#endif
    }
    if (!book && !unread_syncbook) {
        cJSON_Delete(r); r = NULL;
        snprintf(url, sizeof(url), "%s/book/getProgress?bookId=%s", weread_client_get_base_url(), id);
        err = request_mode(url,NULL,&r,&status,identity);
        if (fallback(r, status)) {
            cJSON_Delete(r); r = NULL;
            snprintf(url, sizeof(url), "%s/web/book/getProgress?bookId=%s", web_base(), id);
            err = request_mode(url,NULL,&r,&status,identity);
        }
        const cJSON *explicit_book = cJSON_GetObjectItemCaseSensitive(r, "book");
        book = explicit_book ? explicit_book : r;
        const char *server_id=wr_string(r,"bookId"),*nested_id=wr_string(book,"bookId");
        bool wrong_id=(cJSON_HasObjectItem(r,"bookId")&&(!server_id||strcmp(server_id,id)))||
            (cJSON_HasObjectItem(book,"bookId")&&(!nested_id||strcmp(nested_id,id)));
        bool empty_unread = allow_unread && has_detail && cJSON_IsObject(r) && !r->child;
        if(!empty_unread && (!allow_unread || (identity && !cJSON_IsObject(explicit_book)))){
            const char *required[]={"chapterUid","chapterIdx","chapterOffset","progress"};
            for(size_t i=0;i<sizeof(required)/sizeof(*required);++i)if(!cJSON_HasObjectItem(book,required[i]))wrong_id=true;
        }
        if(err==ESP_OK&&(wrong_id||error_code(book)||!decode(book,cloud,true)))err=ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(r);
    if (err != ESP_OK) return err;
    if(version)*version=fresh_version;
    if(last)*last=last_index;
    return ESP_OK;
}
bool weread_progress_ahead(const weread_progress_t *a, const weread_progress_t *b) {
    if (!a || !b || !a->chapter_uid) return false;
    if (!b->chapter_uid) return true;
    if (a->chapter_index != b->chapter_index) return a->chapter_index > b->chapter_index;
    return a->chapter_offset > b->chapter_offset;
}
esp_err_t weread_progress_fetch_version(const char *id, weread_progress_t *cloud, uint32_t *known_version, uint32_t *last_chapter) {
    if (!wr_valid_id(id) || !cloud) return ESP_ERR_INVALID_ARG;
    if (known_version) *known_version = 0;
    if (last_chapter) *last_chapter = 0;
    memset(cloud, 0, sizeof(*cloud));
    progress_identity_t session;
    { WR_GUARD;
        if (!identity_read(&session)) return ESP_ERR_INVALID_STATE;
    }
    uint32_t fresh_version = 0, last_index = 0;
    esp_err_t err = fetch_progress(id, cloud, &fresh_version, &last_index, &session, true);
    if (err != ESP_OK) return err;
    { WR_GUARD;
        if (!identity_matches(&session)) return ESP_ERR_INVALID_STATE;
        if (known_version) *known_version = fresh_version;
        if (last_chapter) *last_chapter = last_index;
        if (fresh_version) {
            esp_err_t updated = weread_shelf_set_version(id, fresh_version);
            if (updated != ESP_OK && updated != ESP_ERR_NOT_FOUND) return updated;
        }
        uint32_t version = fresh_version ? fresh_version : weread_shelf_book_version(id);
        if (version && cloud->book_version && version != cloud->book_version) return WEREAD_ERR_VERSION;
        baseline(cloud, cloud);
        if (fresh_version) cloud->base_version = fresh_version;
        strcpy(s_observed.id, id); s_observed.session = session_stamp(); s_observed.cloud = *cloud;
        if (fresh_version) s_observed.cloud.book_version = fresh_version;
    }
    return ESP_OK;
}
esp_err_t weread_progress_rebase(const char *id, const weread_progress_t *cloud, uint32_t fresh_version, weread_progress_t *out) { WR_GUARD;
    if (!wr_valid_id(id) || !cloud) return ESP_ERR_INVALID_ARG;
    weread_progress_t local = {0};
    esp_err_t err = weread_progress_load(id, &local);
    if (err != ESP_OK) return err;
    if (weread_progress_ahead(cloud, &local)) {
        if (out) *out = local;
        return ESP_OK;
    }
    baseline(&local, cloud);
    local.base_version = fresh_version ? fresh_version : cloud->base_version;
    local.sync_conflict = false;
    local.retry_after = 0; local.retry_count = 0;
    if (local.chapter_uid) err = weread_progress_checkpoint(id, &local);
    if (err == ESP_OK && out) *out = local;
    return err;
}
esp_err_t weread_progress_sync_version(const char *id, weread_progress_t *local, weread_progress_t *cloud, bool *conflict,uint32_t *known_version,uint32_t *last_chapter) { WR_GUARD;
    if (!wr_valid_id(id) || !local || !cloud || !conflict) return ESP_ERR_INVALID_ARG;
    if(known_version)*known_version=0;
    if(last_chapter)*last_chapter=0;
    bool have_local = weread_progress_load(id, local) == ESP_OK;
    memset(cloud, 0, sizeof(*cloud)); *conflict = false;
    uint32_t fresh_version=0,last_index=0;
    esp_err_t err=fetch_progress(id,cloud,&fresh_version,&last_index,NULL,true);
    if(err!=ESP_OK)return err;
    // 只有本次书籍元数据可证明当前版本；阅读位置快照不能回写旧版。/ Only fresh book metadata proves the current version; progress snapshots cannot restore an older version.
    if(known_version)*known_version=fresh_version;
    if(last_chapter)*last_chapter=last_index;
    if(fresh_version){esp_err_t updated=weread_shelf_set_version(id,fresh_version);
        if(updated!=ESP_OK&&updated!=ESP_ERR_NOT_FOUND)return updated;}
    uint32_t version=fresh_version?fresh_version:weread_shelf_book_version(id);
    if(version&&cloud->book_version&&version!=cloud->book_version)return WEREAD_ERR_VERSION;
    baseline(cloud,cloud);if(fresh_version)cloud->base_version=fresh_version;
    strcpy(s_observed.id,id);s_observed.session=session_stamp();s_observed.cloud=*cloud;
    if(fresh_version)s_observed.cloud.book_version=fresh_version;
    if (!have_local) {
        *local = *cloud; local->reading_time = 0; local->pending_pages = 0;
        local->dirty = local->position_pending = local->sync_conflict = false;
        if (local->chapter_uid) err = save(id, local);
    } else if (weread_progress_ahead(cloud, local)) {
        // 云端进度严格领先：自动采用云端位置，取消二选一冲突弹窗。/ Cloud is strictly ahead: adopt cloud automatically without prompting.
        uint32_t rev = next_revision(local->revision);
        *local = *cloud;
        local->reading_time = 0; local->pending_pages = 0;
        local->dirty = local->position_pending = local->sync_conflict = false;
        local->retry_after = 0; local->retry_count = 0; local->revision = rev;
        err = save(id, local);
    } else {
        // 本机领先或一致：保留本机位置并静默对齐云端基线。/ Local is ahead or tied: keep local position and rebase cloud baseline silently.
        bool same = local->chapter_uid == cloud->chapter_uid && local->chapter_offset == cloud->chapter_offset &&
            local->chapter_index == cloud->chapter_index;
        bool had_conflict = local->sync_conflict;
        bool base_changed = !local->base_known || local->base_uid != cloud->base_uid ||
            local->base_index != cloud->base_index || local->base_offset != cloud->base_offset ||
            local->base_version != cloud->base_version;
        baseline(local,cloud);local->base_version=cloud->base_version;local->sync_conflict=false;
        local->retry_after=0;local->retry_count=0;
        if(same&&!local->dirty&&!local->position_pending)local->progress=cloud->progress;
        if(local->chapter_uid&&(had_conflict||base_changed||(same&&!local->dirty&&!local->position_pending)))
            err=weread_progress_checkpoint(id,local);
    }
    return err;
}
esp_err_t weread_progress_sync(const char *id,weread_progress_t *local,weread_progress_t *cloud,bool *conflict){
    return weread_progress_sync_version(id,local,cloud,conflict,NULL,NULL);
}
esp_err_t weread_progress_checkpoint(const char *id,const weread_progress_t *p){WR_GUARD;
    if (!p || !p->chapter_uid || p->progress > 100 || !memchr(p->summary, 0, sizeof(p->summary))) return ESP_ERR_INVALID_ARG;
    weread_progress_t *old __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*old));
    if (!old) return ESP_ERR_NO_MEM;
    if(weread_progress_load(id,old)==ESP_OK&&old->chapter_uid==p->chapter_uid&&old->chapter_index==p->chapter_index&&
       old->chapter_offset==p->chapter_offset&&old->progress==p->progress&&old->book_version==p->book_version&&
       old->reading_time==p->reading_time&&old->pending_pages==p->pending_pages&&old->updated_at==p->updated_at&&
       old->last_report_ms==p->last_report_ms&&old->dirty==p->dirty&&old->position_pending==p->position_pending&&
       old->base_known==p->base_known&&old->base_uid==p->base_uid&&old->base_index==p->base_index&&old->base_offset==p->base_offset&&
       old->base_version==p->base_version&&old->sync_conflict==p->sync_conflict&&old->revision==p->revision&&
       old->retry_after==p->retry_after&&old->retry_count==p->retry_count&&!strcmp(old->summary,p->summary))return ESP_OK;
    return save(id,p);
}
esp_err_t weread_progress_select(const char *id, const weread_progress_t *p, bool from_cloud) { WR_GUARD;
    if(!p)return ESP_ERR_INVALID_ARG;
    weread_progress_t *selected __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*selected));
    weread_progress_t *old __attribute__((cleanup(wr_release))) = wr_alloc(sizeof(*old));
    if (!selected || !old) return ESP_ERR_NO_MEM;
    *selected = *p;
    if(weread_progress_load(id,old)==ESP_OK)selected->revision=next_revision(old->revision);
    selected->dirty = !from_cloud;
    selected->sync_conflict=false;selected->retry_after=0;selected->retry_count=0;
    if(from_cloud){baseline(selected,p);if(p->base_known)selected->base_version=p->base_version;}
    else if(!strcmp(s_observed.id,id)&&s_observed.session==session_stamp())baseline(selected,&s_observed.cloud);
    selected->position_pending=!from_cloud;
    if (from_cloud) selected->pending_pages = selected->reading_time = 0;
    s_rescue_choice=true;
    esp_err_t err=weread_progress_checkpoint(id,selected);
    s_rescue_choice=false;return err;
}
esp_err_t weread_progress_note_time(const char *id,uint32_t seconds,uint64_t timestamp){WR_GUARD;
    if(timestamp>9007199254740991ULL)return ESP_ERR_INVALID_ARG;
    weread_progress_t p;esp_err_t err=weread_progress_load(id,&p);if(err!=ESP_OK)return err;
    p.reading_time=seconds>UINT32_MAX-p.reading_time?UINT32_MAX:p.reading_time+seconds;
    if(seconds){p.dirty=true;p.updated_at=timestamp;p.revision=next_revision(p.revision);}
    return save(id,&p);
}
esp_err_t weread_progress_note(const char *id, uint32_t uid, uint32_t index, uint32_t offset,
    uint32_t percent, uint32_t seconds, unsigned pages, const char *summary, uint64_t timestamp) { WR_GUARD;
    if (!uid || percent > 100 || timestamp > 9007199254740991ULL) return ESP_ERR_INVALID_ARG;
    weread_progress_t p = {0}; (void)weread_progress_load(id, &p);
    p.chapter_uid = uid; p.chapter_index = index; p.chapter_offset = offset; p.progress = percent;
    p.reading_time = seconds > UINT32_MAX - p.reading_time ? UINT32_MAX : p.reading_time + seconds;
    p.pending_pages = pages > UINT32_MAX - p.pending_pages ? UINT32_MAX : p.pending_pages + pages;
    p.updated_at = timestamp; p.dirty = p.position_pending = true;p.revision=next_revision(p.revision);
    wr_display_copy(p.summary, sizeof(p.summary), summary ? summary : "");
    weread_book_t book; if (weread_shelf_find(id, &book)) p.book_version = book.book_version;
    return save(id, &p);
}
static uint32_t s_prefer_web_read_session;
static inline bool resp_succ(const cJSON *r) {
    return number(r, "succ") == 1 || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(r, "succ"));
}
static esp_err_t try_web_read(const char *id, const weread_progress_t *p, progress_identity_t *identity,
    char *url, size_t url_cap, cJSON **r, int *status) {
    char *signed_body __attribute__((cleanup(wr_release))) = wr_alloc(4096);
    if (!signed_body) return ESP_ERR_NO_MEM;
    if (!book_weread_build_web_read_body(id, p->chapter_uid, p->chapter_index, p->chapter_offset,
        p->progress, p->summary, (int64_t)time(NULL), WEREAD_DEFAULT_USER_AGENT, signed_body, 4096)) return ESP_ERR_INVALID_SIZE;
    int n = snprintf(url, url_cap, "%s/web/book/read", web_base());
    if (n < 0 || (size_t)n >= url_cap) return ESP_ERR_INVALID_SIZE;
    esp_err_t err = request_mode(url, signed_body, r, status, identity);
    if (err == ESP_OK && !resp_succ(*r)) err = ESP_ERR_INVALID_RESPONSE;
    return err;
}
static esp_err_t send_progress(const char *id,const weread_progress_t *p, progress_identity_t *identity){
    esp_err_t err=ESP_OK;
    cJSON *body=NULL,*r=NULL;char *text=NULL;
    char url[768];
    uint32_t sess = 0;
    bool prefer_web = false;
    { WR_GUARD;
        sess = session_stamp();
        prefer_web = sess != 0 && s_prefer_web_read_session == sess;
    }
    int status = 0;
    if (prefer_web) {
        err = try_web_read(id, p, identity, url, sizeof(url), &r, &status);
        if (err == ESP_OK) { cJSON_Delete(r); return ESP_OK; }
        bool authed = false;
        { WR_GUARD;
            s_prefer_web_read_session = 0;
            authed = weread_auth_is_authenticated();
        }
        cJSON_Delete(r); r = NULL;
        if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_TIMEOUT || err == ESP_ERR_NO_MEM || err == ESP_ERR_INVALID_SIZE) return err;
        if (!authed) return ESP_ERR_INVALID_STATE;
    }
    { WR_GUARD; body=encode(p);
    if (!body || !cJSON_AddStringToObject(body, "bookId", id)) { cJSON_Delete(body); return ESP_ERR_NO_MEM; }
    // 只发送协议字段，不把本地状态混入 API。/ Send protocol fields without local bookkeeping.
    const char *keys[] = {"dirty", "pendingPages", "lastReportMs", "updateTime", "positionPending"};
    for (size_t i = 0; i < 5; ++i) cJSON_DeleteItemFromObject(body, keys[i]);
    text = cJSON_PrintUnformatted(body); cJSON_Delete(body); }
    if (!text) return ESP_ERR_NO_MEM;
    snprintf(url, sizeof(url), "%s/book/read", weread_client_get_base_url());
    status = 0; err = request_mode(url,text,&r,&status,identity); cJSON_free(text);
    bool learned_web = false;
    if (fallback(r, status)) {
        cJSON_Delete(r); r = NULL;
        err = try_web_read(id, p, identity, url, sizeof(url), &r, &status);
        learned_web = (err == ESP_OK);
    } else if (err == ESP_OK && !resp_succ(r)) {
        err = ESP_ERR_INVALID_RESPONSE;
    }
    if (learned_web && sess) { WR_GUARD; s_prefer_web_read_session = sess; }
#if defined(ESP_PLATFORM)
    if (err != ESP_OK) ESP_LOGW("weread", "report fail id=%s uid=%u idx=%u off=%u p=%u st=%d err=%d",
        id, (unsigned)p->chapter_uid, (unsigned)p->chapter_index,
        (unsigned)p->chapter_offset, (unsigned)p->progress, status, (int)err);
#endif
    cJSON_Delete(r);
    return err;
}
static esp_err_t report_saved(const char *id,weread_progress_t *saved,bool force,uint64_t now){
    if (!wr_valid_id(id)) return ESP_ERR_INVALID_ARG;
    // 旧兼容API不能绕开应急恢复与新鲜云基线；统一交给outbox。/ Legacy callers must not bypass rescue restoration and fresh-cloud validation.
    if(rescue_enabled()){
        uint8_t card[WEREAD_MEDIA_ID_BYTES];if(!media_id(card))return ESP_ERR_INVALID_STATE;
        rescue_record_t *record __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*record));
        if(!record)return ESP_ERR_NO_MEM;
        unsigned slot;esp_err_t found=rescue_find(id,card,record,&slot);
        if(found==ESP_OK)return ESP_ERR_NOT_FINISHED;
        if(found!=ESP_ERR_NOT_FOUND)return found;
    }
    weread_progress_t p=*saved;esp_err_t err=ESP_OK;
    if (!p.dirty || !p.position_pending) return ESP_OK;
    if (!force && !strcmp(id, s_retry_id) && now >= s_retry_started && now < s_retry_at) return ESP_ERR_TIMEOUT;
    if (!force && p.pending_pages < 10 && now >= p.last_report_ms && now - p.last_report_ms < 60000) return ESP_OK;
    err=send_progress(id,&p,NULL);
    if (err != ESP_OK) {
        wr_copy(s_retry_id, sizeof(s_retry_id), id); s_retry_started = now;
        s_retry_at = now > UINT64_MAX - 15000 ? UINT64_MAX : now + 15000;
        return err;
    }
    s_retry_id[0] = 0;
    p.dirty = p.position_pending = false; p.reading_time = 0; p.pending_pages = 0; p.last_report_ms = now;baseline(&p,&p);
    err=save(id, &p);if(err==ESP_OK)*saved=p;return err;
}
esp_err_t weread_progress_report(const char *id,bool force,uint64_t now){WR_GUARD;
    weread_progress_t p;esp_err_t err=weread_progress_load(id,&p);
    return err==ESP_OK?report_saved(id,&p,force,now):err;
}
static esp_err_t apply_progress_delta(weread_progress_t *dst, const weread_progress_t *delta, bool moved,
    bool bound, uint32_t version, const char *id, uint64_t timestamp) {
    if (moved) {
        if (!delta->chapter_uid || delta->progress > 100 || !memchr(delta->summary, 0, sizeof(delta->summary))) return ESP_ERR_INVALID_ARG;
        dst->chapter_uid = delta->chapter_uid;
        dst->chapter_index = delta->chapter_index;
        dst->chapter_offset = delta->chapter_offset;
        dst->progress = delta->progress;
        wr_display_copy(dst->summary, sizeof(dst->summary), delta->summary);
        dst->position_pending = true;
        uint32_t v = bound ? version : weread_shelf_book_version(id);
        if (bound || v) dst->book_version = v;
        dst->pending_pages = delta->pending_pages > UINT32_MAX - dst->pending_pages
            ? UINT32_MAX : dst->pending_pages + delta->pending_pages;
    }
    dst->reading_time = delta->reading_time > UINT32_MAX - dst->reading_time
        ? UINT32_MAX : dst->reading_time + delta->reading_time;
    if (moved || delta->reading_time) {
        dst->updated_at = timestamp;
        dst->dirty = true;
        dst->revision = next_revision(dst->revision);
    }
    return ESP_OK;
}
static esp_err_t store_sample(const char *id,weread_progress_t *sample,bool moved,bool bound,uint32_t content_version,uint64_t timestamp,bool *noted){WR_GUARD;
    if(!sample||!noted||timestamp>9007199254740991ULL)return ESP_ERR_INVALID_ARG;
    *noted=false;
    weread_progress_t *p __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*p));
    if(!p)return ESP_ERR_NO_MEM;
    esp_err_t err=weread_progress_load(id,p);if(err!=ESP_OK&&(!moved||err!=ESP_ERR_NOT_FOUND))return err;
    err=apply_progress_delta(p,sample,moved,bound,content_version,id,timestamp);
    if(err!=ESP_OK)return err;
    if(moved||sample->reading_time){err=save(id,p);if(err!=ESP_OK)return err;}
    *noted=true;*sample=*p;
    return ESP_OK;
}
esp_err_t weread_progress_store(const char *id,weread_progress_t *sample,bool moved,uint64_t timestamp,bool *noted){
    return store_sample(id,sample,moved,false,0,timestamp,noted);
}
esp_err_t weread_progress_store_version(const char *id,weread_progress_t *sample,bool moved,uint32_t content_version,uint64_t timestamp,bool *noted){
    return store_sample(id,sample,moved,true,content_version,timestamp,noted);
}
esp_err_t weread_progress_store_version_resilient(const char *id,weread_progress_t *sample,bool moved,uint32_t version,uint64_t timestamp,bool *noted,const weread_progress_t *before,const uint8_t captured[WEREAD_MEDIA_ID_BYTES],bool *rescued){WR_GUARD;
    if(!sample||!noted||!rescued||!wr_valid_id(id)||timestamp>9007199254740991ULL)return ESP_ERR_INVALID_ARG;
    *noted=*rescued=false;
    if(!rescue_enabled())return store_sample(id,sample,moved,true,version,timestamp,noted);
    if(!captured)return ESP_ERR_INVALID_STATE;
    uint8_t card[WEREAD_MEDIA_ID_BYTES];bool present=media_id(card),nonzero=false;
    for(unsigned i=0;i<WEREAD_MEDIA_ID_BYTES;++i)nonzero|=captured[i]!=0;
    if(!nonzero||(present&&memcmp(card,captured,sizeof(card))))return ESP_ERR_INVALID_STATE;
    rescue_store_work_t *w __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*w));
    if(!w)return ESP_ERR_NO_MEM;
    w->delta=*sample;
    unsigned slot=0;esp_err_t found=rescue_find(id,captured,&w->record,&slot);
    if(found==ESP_OK){
        esp_err_t err=present?store_sample(id,sample,moved,true,version,timestamp,noted):ESP_ERR_INVALID_STATE;
        if(err==ESP_OK){
            if(rescue_find(id,captured,&w->record,&slot)==ESP_OK)*rescued=true;
            return ESP_OK;
        }
        if(!moved&&!w->delta.reading_time)return err;
        w->old=w->record.desired;
    }else if(found!=ESP_ERR_NOT_FOUND){
        return (!present&&!moved&&!w->delta.reading_time)?ESP_ERR_INVALID_STATE:found;
    }else{
        esp_err_t loaded=ESP_ERR_INVALID_STATE,err=ESP_ERR_INVALID_STATE;
        if(present){
            bool primary_owned=false;
            loaded=load_tf_ex(id,&w->old,&primary_owned);
            if(loaded==ESP_OK||(moved&&loaded==ESP_ERR_NOT_FOUND)){
                w->next=w->old;
                err=apply_progress_delta(&w->next,&w->delta,moved,true,version,id,timestamp);
                if(err!=ESP_OK)return err;
                if(moved||w->delta.reading_time)err=save_tf_ex(id,&w->next,primary_owned);
                if(err==ESP_OK){*noted=true;*sample=w->next;return ESP_OK;}
            }else err=loaded;
        }
        if(!moved&&!w->delta.reading_time)return err;
        if(loaded!=ESP_OK){if(!before||!before->chapter_uid)return err;w->old=*before;}
        bool empty=false;
        for(slot=0;slot<2;++slot)if(rescue_read(slot,&w->record)==ESP_ERR_NOT_FOUND){empty=true;break;}
        if(!empty)return ESP_ERR_NO_MEM;
        memset(&w->record,0,sizeof(w->record));const weread_auth_session_t *session=weread_auth_get_current_session();
        if(!session||!wr_copy(w->record.root,sizeof(w->record.root),weread_auth_get_storage_root())||
           !wr_copy(w->record.vid,sizeof(w->record.vid),session->vid)||!wr_copy(w->record.device,sizeof(w->record.device),session->device_id)||
           !wr_copy(w->record.id,sizeof(w->record.id),id))return ESP_ERR_INVALID_STATE;
        memcpy(w->record.media,captured,sizeof(w->record.media));w->record.expected=w->old;
    }
    w->next=w->old;
    esp_err_t err=apply_progress_delta(&w->next,&w->delta,moved,true,version,id,timestamp);
    if(err!=ESP_OK)return err;
    w->record.desired=w->next;
    err=rescue_write(slot,&w->record);if(err!=ESP_OK)return err;
    *sample=w->next;*noted=*rescued=true;s_outbox.next_scan=0;wr_copy(s_outbox.hint,sizeof(s_outbox.hint),id);return ESP_OK;
}
esp_err_t weread_progress_update(const char *id,weread_progress_t *sample,bool moved,bool force,uint64_t now,uint64_t timestamp,bool *noted){WR_GUARD;
    esp_err_t err=weread_progress_store(id,sample,moved,timestamp,noted);
    return err==ESP_OK?report_saved(id,sample,force,now):err;
}


weread_progress_state_t weread_progress_state(const weread_progress_t *p) {
    if(!p)return WR_PROGRESS_LOCAL;
    if(p->sync_conflict)return WR_PROGRESS_CONFLICT;
    if(p->dirty&&p->position_pending)return WR_PROGRESS_PENDING;
    return p->last_report_ms?WR_PROGRESS_SYNCED:WR_PROGRESS_LOCAL;
}
void weread_progress_outbox_reset(void) { WR_GUARD;
    rescue_cache_invalidate();
    s_prefer_web_read_session = 0;
    if(s_outbox.dir)closedir(s_outbox.dir);
    memset(&s_outbox,0,sizeof(s_outbox));
    memset(&s_observed,0,sizeof(s_observed));
}
esp_err_t weread_progress_outbox_next(char *id,size_t cap,uint64_t now) { WR_GUARD;
    if(!id||cap<WEREAD_BOOK_ID_CAP)return ESP_ERR_INVALID_ARG;
    id[0]=0;uint32_t session=session_stamp();
    if(!session)return ESP_ERR_INVALID_STATE;
    uint8_t mounted[WEREAD_MEDIA_ID_BYTES];if(rescue_enabled()&&!media_id(mounted))return ESP_ERR_INVALID_STATE;
    if(s_outbox.session!=session){
        if(s_outbox.dir)closedir(s_outbox.dir);
        char hint[WEREAD_BOOK_ID_CAP];memcpy(hint,s_outbox.hint,sizeof(hint));
        memset(&s_outbox,0,sizeof(s_outbox));s_outbox.session=session;memcpy(s_outbox.hint,hint,sizeof(hint));
    }
    uint8_t card[WEREAD_MEDIA_ID_BYTES];
    if(rescue_enabled()&&media_id(card)){
        rescue_record_t *record __attribute__((cleanup(wr_release)))=wr_alloc(sizeof(*record));
        if(!record)return ESP_ERR_NO_MEM;
        for(unsigned slot=0;slot<2;++slot)if(rescue_read(slot,record)==ESP_OK&&rescue_owner(record,card)&&
           !record->desired.sync_conflict&&!(record->desired.retry_after>now&&record->desired.retry_after-now<=300)){
            wr_copy(id,cap,record->id);return ESP_OK;
        }
    }
    if(s_outbox.hint[0]){
        weread_progress_t pending;
        if(weread_progress_load(s_outbox.hint,&pending)==ESP_OK&&pending.dirty&&pending.position_pending&&!pending.sync_conflict&&
           !(pending.retry_after>now&&pending.retry_after-now<=300)){
            wr_copy(id,cap,s_outbox.hint);s_outbox.hint[0]=0;return ESP_OK;
        }
        s_outbox.hint[0]=0;
    }
    if(!s_outbox.dir){
        if(now<s_outbox.next_scan)return ESP_ERR_NOT_FOUND;
        char path[512];int len=snprintf(path,sizeof(path),"%s/cache",weread_auth_get_storage_root());
        if(len<0||(size_t)len>=sizeof(path))return ESP_ERR_INVALID_SIZE;
        s_outbox.dir=opendir(path);
        if(!s_outbox.dir){s_outbox.next_scan=now+30;return ESP_ERR_NOT_FOUND;}
    }
    // 每次只看少量条目；不在UI tick遍历完整书架。/ Bound each worker slice instead of scanning the whole shelf per UI tick.
    for(unsigned scanned=0;scanned<8;++scanned){
        struct dirent *entry=readdir(s_outbox.dir);
        if(!entry){closedir(s_outbox.dir);s_outbox.dir=NULL;s_outbox.next_scan=now+30;return ESP_ERR_NOT_FOUND;}
        if(!wr_valid_id(entry->d_name))continue;
        weread_progress_t p;
        if(weread_progress_load(entry->d_name,&p)!=ESP_OK||!p.dirty||!p.position_pending||p.sync_conflict)continue;
        if(p.retry_after>now&&p.retry_after-now<=300)continue;
        wr_copy(id,cap,entry->d_name);return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
static bool same_baseline(const weread_progress_t *a,const weread_progress_t *b){
    return a->base_known==b->base_known&&a->base_uid==b->base_uid&&a->base_index==b->base_index&&
        a->base_offset==b->base_offset&&a->base_version==b->base_version;
}
static bool cloud_matches_base(const weread_progress_t *cloud,const weread_progress_t *p){
    return p->base_known&&cloud->chapter_uid==p->base_uid&&cloud->chapter_index==p->base_index&&cloud->chapter_offset==p->base_offset&&
        p->base_version==cloud->book_version;
}
static esp_err_t outbox_failure(const char *id,const progress_identity_t *session,const weread_progress_t *sent,esp_err_t err,uint64_t now,weread_progress_t *out){WR_GUARD;
    if(!identity_matches(session))return ESP_ERR_INVALID_STATE;
    weread_progress_t latest;if(weread_progress_load(id,&latest)!=ESP_OK)return err;
    if(out)*out=latest;
    if(latest.revision!=sent->revision)return err;
    if(err==WEREAD_ERR_PROGRESS_CONFLICT)latest.sync_conflict=true;
    else if(err!=ESP_ERR_INVALID_STATE){
        if(latest.retry_count<5)++latest.retry_count;
        unsigned delay=15u<<(latest.retry_count-1);if(delay>300)delay=300;
        latest.retry_after=now>9007199254740991ULL-delay?9007199254740991ULL:now+delay;
    }else return err; // 前台抢占不消耗重试。/ Foreground cancellation does not consume a retry.
    esp_err_t stored=save(id,&latest);if(stored==ESP_OK&&out)*out=latest;
    return stored==ESP_OK?err:stored;
}
esp_err_t weread_progress_outbox_report(const char *id,uint64_t now_ms,uint64_t now,weread_progress_t *out){
    return weread_progress_outbox_report_ex(id,now_ms,now,false,out);
}
esp_err_t weread_progress_outbox_report_ex(const char *id,uint64_t now_ms,uint64_t now,bool ignore_backoff,weread_progress_t *out){
    if(!wr_valid_id(id)||!out||now>9007199254740991ULL)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));weread_progress_t sent;progress_identity_t session;
    { WR_GUARD;
        if(!identity_read(&session))return ESP_ERR_INVALID_STATE;
        uint8_t mounted[WEREAD_MEDIA_ID_BYTES];if(rescue_enabled()&&!media_id(mounted))return ESP_ERR_INVALID_STATE;
        esp_err_t err=weread_progress_load(id,&sent);if(err!=ESP_OK)return err;*out=sent;
        err=rescue_restore(id,&sent);if(err!=ESP_ERR_NOT_FOUND&&err!=ESP_OK)
            return outbox_failure(id,&session,&sent,err,now,out);
        if(!sent.dirty||!sent.position_pending)return ESP_OK;
        if(sent.sync_conflict)return WEREAD_ERR_PROGRESS_CONFLICT;
        if(!ignore_backoff&&sent.retry_after>now&&sent.retry_after-now<=300)return ESP_ERR_TIMEOUT;
    }
    weread_progress_t cloud;uint32_t version=0;
    esp_err_t err=fetch_progress(id,&cloud,&version,NULL,&session,false);
    if(err!=ESP_OK)return outbox_failure(id,&session,&sent,err,now,out);
    if(version&&cloud.book_version&&version!=cloud.book_version)
        return outbox_failure(id,&session,&sent,WEREAD_ERR_VERSION,now,out);
    if(version)cloud.book_version=version;
    bool resolved=false;
    { WR_GUARD;
        if(!identity_matches(&session))return ESP_ERR_INVALID_STATE;
        weread_progress_t latest;err=weread_progress_load(id,&latest);if(err!=ESP_OK)return err;*out=latest;
        if(latest.revision!=sent.revision)return ESP_ERR_INVALID_STATE;
        if((!sent.base_known||!cloud_matches_base(&cloud,&sent))&&weread_progress_ahead(&cloud,&latest)){
            // 云端严格领先：自动采用云端位置，不再锁定冲突状态。/ Cloud is strictly ahead: adopt cloud automatically without locking conflict.
            uint32_t rev=next_revision(latest.revision);
            baseline(&cloud,&cloud);if(version)cloud.base_version=version;
            latest=cloud;latest.reading_time=0;latest.pending_pages=0;
            latest.dirty=latest.position_pending=latest.sync_conflict=false;
            latest.retry_after=0;latest.retry_count=0;latest.last_report_ms=now_ms;
            latest.revision=rev;
            err=save(id,&latest);
            if(err==ESP_OK){*out=latest;resolved=true;}
            else return err;
        }else{
            bool wrong_version=sent.book_version!=version||(cloud.book_version&&sent.book_version!=cloud.book_version);
            if(wrong_version)return outbox_failure(id,&session,&sent,WEREAD_ERR_VERSION,now,out);
            if(!sent.base_known||!cloud_matches_base(&cloud,&sent)){
                // 本机领先或持平但基线缺失/过期：对齐云端基线后再上行，不触发二选一冲突。
                // Local is ahead or tied with missing/stale baseline: rebase to cloud before POSTing without flagging a conflict.
                baseline(&cloud,&cloud);if(version)cloud.base_version=version;
                baseline(&sent,&cloud);sent.base_version=cloud.base_version;
                baseline(&latest,&cloud);latest.base_version=cloud.base_version;
                latest.sync_conflict=false;
                err=save(id,&latest);if(err!=ESP_OK)return err;
                *out=latest;
            }
        }
    }
    if(resolved)return ESP_OK;
    err=send_progress(id,&sent,&session);
    if(err!=ESP_OK)return outbox_failure(id,&session,&sent,err,now,out);
    { WR_GUARD;
        if(!identity_matches(&session))return ESP_ERR_INVALID_STATE;
        weread_progress_t latest;err=weread_progress_load(id,&latest);if(err!=ESP_OK)return err;*out=latest;
        if(!latest.position_pending||latest.sync_conflict||!same_baseline(&latest,&sent))return ESP_ERR_INVALID_STATE;
        if(latest.revision==sent.revision){
            latest.dirty=latest.position_pending=false;latest.reading_time=latest.pending_pages=0;
        }else{
            // 只核销已确认的样本；较新的位置和累计读秒继续排队。/ Acknowledge only the sent sample, retaining newer navigation and time.
            latest.reading_time=latest.reading_time>=sent.reading_time?latest.reading_time-sent.reading_time:0;
            latest.pending_pages=latest.pending_pages>=sent.pending_pages?latest.pending_pages-sent.pending_pages:0;
        }
        baseline(&latest,&sent);latest.last_report_ms=now_ms;latest.retry_after=0;latest.retry_count=0;
        err=save(id,&latest);if(err==ESP_OK)*out=latest;
    }
    return err;
}
