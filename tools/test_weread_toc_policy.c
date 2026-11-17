/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：生产目录长期版本缓存与未知版本日边界、恢复副本和强刷回归。
 * English: Production version-cache longevity, daily unknown-version boundaries, recovery copies and forced refreshes.
 */
#include "weread_toc.h"
#include "weread_shelf.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"
#include "weread_chapter.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static time_t wall=1800000000;
static unsigned remote_version=100,toc_version=100,toc_requests;
time_t toc_test_time(time_t *out){if(out)*out=wall;return wall;}
static esp_err_t request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)body;(void)ctx;char text[1024];*status=200;
    if(strstr(url,"/book/chapterInfos")){
        ++toc_requests;
        snprintf(text,sizeof(text),"{\"data\":[{\"bookId\":\"policy_book\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":%u,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"one\"}]}]}",toc_version);
    }else snprintf(text,sizeof(text),"{\"synckey\":1,\"books\":[{\"bookId\":\"policy_book\",\"title\":\"fixture\",\"format\":\"epub\",\"bookVersion\":%u}]}",remote_version);
    *out=strdup(text);assert(*out);*len=strlen(text);return ESP_OK;
}
static void account(const char *id){
    weread_auth_session_t s={0};strcpy(s.vid,id);strcpy(s.access_token,"test-token");strcpy(s.refresh_token,"fixture-refresh");
    assert(weread_auth_save_session(&s)==ESP_OK&&weread_auth_load_session(&s)==ESP_OK);
}
static void stamp(time_t checked){
    char path[512];cJSON *o=NULL;assert(wr_cache_path("policy_book","chapters.json",path,sizeof(path))==ESP_OK&&wr_read_json(path,&o)==ESP_OK);
    cJSON_DeleteItemFromObject(o,"checkedAt");if(checked)assert(cJSON_AddNumberToObject(o,"checkedAt",(double)checked));
    assert(wr_write_json(path,o)==ESP_OK);cJSON_Delete(o);
}
int main(int argc,char **argv){
    assert(argc==2&&weread_auth_init(argv[1])==ESP_OK);account("123");weread_client_set_transport(request,NULL);
    assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("policy_book")==ESP_OK&&toc_requests==1);
    weread_toc_t toc={0};wall+=30*86400;unsigned before=toc_requests;
    assert(weread_toc_prepare("policy_book",false,100,&toc)==ESP_OK&&toc_requests==before&&toc.checked_at==1800000000);weread_toc_free(&toc);
    stamp(0);assert(weread_toc_prepare("policy_book",false,100,&toc)==ESP_OK&&toc_requests==before);weread_toc_free(&toc);
    // 没有本次可靠版本时，持久书架版本不能把旧目录变成永久新鲜。/ A persisted shelf version cannot make an old TOC permanently fresh without current metadata.
    assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before+1);weread_toc_free(&toc);
    before=toc_requests;wall+=86400-1;
    assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before);weread_toc_free(&toc);
    ++wall;assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before+1);weread_toc_free(&toc);
    before=toc_requests;assert(weread_toc_prepare("policy_book",true,100,&toc)==ESP_OK&&toc_requests==before+1);weread_toc_free(&toc);
    remote_version=toc_version=101;assert(weread_shelf_set_version("policy_book",101)==ESP_OK);before=toc_requests;
    assert(weread_toc_prepare("policy_book",false,101,&toc)==ESP_OK&&toc.book_version==101&&toc_requests==before+1);weread_toc_free(&toc);
    remote_version=toc_version=0;assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("policy_book")==ESP_OK);before=toc_requests;
    wall+=86400-1;assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before);weread_toc_free(&toc);
    ++wall;assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before+1);weread_toc_free(&toc);
    --wall;before=toc_requests;assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before+1);weread_toc_free(&toc);
    // 原子恢复的有效同版本副本可继续复用；无可用副本才重新请求。/ A validated same-version recovery copy remains usable; refresh when no valid copy survives.
    char path[512],previous[544];assert(wr_cache_path("policy_book","chapters.json",path,sizeof(path))==ESP_OK);snprintf(previous,sizeof(previous),"%s.previous",path);
    cJSON *saved=NULL;assert(wr_read_json(path,&saved)==ESP_OK&&wr_write_json(previous,saved)==ESP_OK);cJSON_Delete(saved);
    assert(wr_write_bytes(path,"{bad",4)==ESP_OK);before=toc_requests;
    assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before);weread_toc_free(&toc);
    assert(!unlink(previous));assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before+1);weread_toc_free(&toc);
    account("456");assert(weread_shelf_sync(true)==ESP_OK);before=toc_requests;
    assert(weread_toc_prepare("policy_book",false,0,&toc)==ESP_OK&&toc_requests==before+1);weread_toc_free(&toc);
    remote_version=101;toc_version=100;assert(weread_shelf_set_version("policy_book",101)==ESP_OK);
    assert(weread_toc_prepare("policy_book",false,101,&toc)==WEREAD_ERR_VERSION&&!toc.chapters);
    weread_shelf_clear();weread_auth_clear_session();puts("TOC policy: known-version longevity, daily unknown/fallback boundaries, force, identity, recovery and version refusal passed.");return 0;
}
