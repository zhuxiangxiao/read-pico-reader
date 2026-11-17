/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：验证普通开书及下载自愈后的最终版本绑定，冷恢复继续校验持久化版本。
 * English: Bind normal/recovered opens to the final TOC while retaining cold-resume version checks.
 */
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_progress.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_position.h"
#include "cJSON.h"
#include "weread_store.h"
#include "book_source_weread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned version=100, detail_version=100, cloud_version=100, downloads;
static bool selfheal, missing_uid, bad_progress, append_chapter, omit_version, empty_hint;
static unsigned last_index,cloud_index=2;
static unsigned toc_requests;
static esp_err_t request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)body;(void)ctx;char text[2048];*status=200;
    if(selfheal&&downloads>=2&&strstr(url,"/shelf/syncbook")){detail_version=101;cloud_version=101;}
    if(bad_progress&&(strstr(url,"/shelf/syncbook")||strstr(url,"/book/getProgress"))){*status=503;strcpy(text,"{\"errcode\":-1}");}
    else if(strstr(url,"/book/chapterInfos"))snprintf(text,sizeof(text),"{\"data\":[{\"bookId\":\"test_book_101\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":%u,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"first\"},{\"chapterUid\":2,\"chapterIdx\":2,\"title\":\"second\"}]}]}",version);
    else if(strstr(url,"/shelf/syncbook"))snprintf(text,sizeof(text),"{\"books\":[{\"bookId\":\"test_book_101\",\"title\":\"fixture\",\"format\":\"epub\",\"bookVersion\":%u}],\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":2,\"chapterIdx\":2,\"chapterOffset\":20,\"progress\":86,\"bookVersion\":%u}]}",detail_version,cloud_version);
    else snprintf(text,sizeof(text),"{\"synckey\":1,\"books\":[{\"bookId\":\"test_book_101\",\"title\":\"fixture\",\"format\":\"epub\",\"bookVersion\":%u}]}",detail_version);
    if(strstr(url,"/book/chapterInfos")&&++toc_requests==1&&missing_uid)snprintf(text,sizeof(text),"{\"data\":[{\"bookId\":\"test_book_101\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":%u,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"first\"}]}]}",version);
    if(strstr(url,"/shelf/syncbook")){
        cJSON *r=cJSON_Parse(text);assert(r);cJSON *b=cJSON_GetArrayItem(cJSON_GetObjectItem(r,"books"),0);
        if(b){if(omit_version)cJSON_DeleteItemFromObject(b,"bookVersion");if(last_index)cJSON_AddNumberToObject(b,"lastChapterIdx",last_index);}
        cJSON *position=cJSON_GetArrayItem(cJSON_GetObjectItem(r,"bookProgress"),0);
        if(position&&empty_hint){
            const char *keys[]={"chapterUid","chapterIdx","chapterOffset","progress"};
            for(unsigned i=0;i<4;++i)cJSON_SetNumberValue(cJSON_GetObjectItem(position,keys[i]),0);
        }
        if(position&&!empty_hint)cJSON_SetNumberValue(cJSON_GetObjectItem(position,"chapterIdx"),cloud_index);
        *out=cJSON_PrintUnformatted(r);cJSON_Delete(r);
    }else if(strstr(url,"/book/chapterInfos")&&(append_chapter||cloud_index!=2||empty_hint)){
        cJSON *r=cJSON_Parse(text);assert(r);cJSON *chapters=cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r,"data"),0),"updated");
        if(empty_hint){while(cJSON_GetArraySize(chapters))cJSON_DeleteItemFromArray(chapters,0);}
        if(cloud_index!=2)cJSON_SetNumberValue(cJSON_GetObjectItem(cJSON_GetArrayItem(chapters,1),"chapterIdx"),cloud_index);
        if(append_chapter)assert(cJSON_AddItemToArray(chapters,cJSON_Parse("{\"chapterUid\":3,\"chapterIdx\":3,\"title\":\"new chapter\"}")));
        *out=cJSON_PrintUnformatted(r);cJSON_Delete(r);
    }else *out=strdup(text);
    assert(*out);*len=strlen(*out);return ESP_OK;
}
static esp_err_t download(const char *url, const char *headers, FILE *out, size_t limit,
    weread_cancel_fn cancel, void *arg, weread_download_info_t *info, void *ctx) {
    (void)url; (void)headers; (void)limit; (void)cancel; (void)arg; (void)ctx;
    memset(info, 0, sizeof(*info)); info->status = 200;
    if (downloads++ == 1 && selfheal) {
        const char *json = "{\"errcode\":-2063}"; info->bytes = strlen(json);
        assert(fwrite(json, 1, info->bytes, out) == info->bytes); return ESP_OK;
    }
    FILE *file = fopen("build/weread-tests/no-version.zip", "rb"); assert(file);
    strcpy(info->encrypt_key, "pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
    char buf[1024]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file))) { assert(fwrite(buf, 1, n, out) == n); info->bytes += n; }
    assert(!ferror(file)); fclose(file); return ESP_OK;
}
static weread_result_t finish(void){time_t started=time(NULL);while(weread_runtime_busy()){assert(time(NULL)-started<10);usleep(1000);}weread_result_t r;assert(weread_runtime_poll(&r));return r;}
int main(int argc,char **argv){
    assert(argc==3);missing_uid=!strcmp(argv[2],"missing-uid");assert(!setenv("WEREAD_STORAGE_ROOT",argv[1],1));assert(weread_auth_init(argv[1])==ESP_OK);
    weread_auth_session_t session={0};strcpy(session.vid,"123");strcpy(session.access_token,"test-token");strcpy(session.refresh_token,"fixture-refresh");
    assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    weread_client_set_transport(request,NULL);weread_client_set_download_transport(download,NULL);
    assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("test_book_101")==ESP_OK&&weread_chapter_download("test_book_101",2)==ESP_OK);
    assert(weread_progress_note("test_book_101",missing_uid?1:2,missing_uid?1:2,missing_uid?0:20,missing_uid?0:86,0,0,"fixture",1700000100)==ESP_OK);
    weread_progress_t p;assert(weread_progress_load("test_book_101",&p)==ESP_OK&&weread_progress_select("test_book_101",&p,true)==ESP_OK);
    bool should_fail=!strcmp(argv[2],"stale-progress-version")||!strncmp(argv[2],"split-",6)||!strcmp(argv[2],"known-no-fresh")||!strcmp(argv[2],"selfheal-split");
    if(!strcmp(argv[2],"hint-empty-unread")){empty_hint=true;last_index=3;}
    if(!strcmp(argv[2],"reordered-same-version"))cloud_index=3;
    if(!strcmp(argv[2],"append-same-version")){append_chapter=true;last_index=3;}
    if(!strcmp(argv[2],"stale-progress-version")){assert(weread_shelf_set_version("test_book_101",101)==ESP_OK);omit_version=true;}
    if(!strcmp(argv[2],"split-progress")){detail_version=101;cloud_version=100;}
    if(!strcmp(argv[2],"split-toc")){detail_version=cloud_version=101;}
    if(!strcmp(argv[2],"fresh-version")){detail_version=cloud_version=version=101;}
    if(!strcmp(argv[2],"known-no-fresh")){assert(weread_shelf_set_version("test_book_101",101)==ESP_OK);bad_progress=true;}
    if(!strcmp(argv[2],"selfheal-split")){
        selfheal=true;char path[512];assert(wr_cache_path("test_book_101","chapter-2.json",path,sizeof(path))==ESP_OK);assert(!unlink(path));
    }

    weread_runtime_init();assert(weread_runtime_start(WR_JOB_OPEN,"test_book_101",NULL));weread_result_t r=finish();
    if(r.conflict){weread_progress_t cloud=r.cloud;weread_runtime_result_free(&r);assert(weread_runtime_choose("test_book_101",&cloud,true));r=finish();}
    uint32_t cached=0;bool have=book_weread_cached_version("test_book_101",&cached);
    printf("mode=%s native-detail=%u native-cloud=%u chapterInfos=%u result=%d ready=%u shelf-version=%u prepared=%u/%u downloads=%u\n",argv[2],detail_version,cloud_version,version,r.error,r.open_ready,weread_shelf_book_version("test_book_101"),have,cached,downloads);
    if(empty_hint){
        assert(r.error==ESP_ERR_NOT_FOUND&&!r.open_ready);
        weread_progress_t saved;assert(weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.chapter_uid==2&&saved.chapter_offset==20);
        weread_runtime_result_free(&r);weread_runtime_stop();weread_shelf_clear();return 0;
    }
    if(should_fail){
        if(!strcmp(argv[2],"stale-progress-version"))assert(weread_shelf_book_version("test_book_101")==101);
        assert(r.error==WEREAD_ERR_VERSION&&!r.open_ready);weread_runtime_result_free(&r);weread_runtime_stop();
        if(!strcmp(argv[2],"selfheal-split")){
            const char *uri="weread://test_book_101";weread_runtime_load_local();
            assert(weread_reader_prepare_open(uri)==WEREAD_ERR_VERSION&&!book_weread_prepared("test_book_101"));
            assert(book_weread_open(uri)==WEREAD_ERR_VERSION);
            size_t chapter=42;uint32_t offset=99;
            assert(weread_reader_resume(uri,&chapter,&offset)!=ESP_OK&&chapter==42&&offset==99);
            book_weread_close();weread_runtime_stop();
        }
        weread_shelf_clear();return 0;
    }
    assert(r.error==ESP_OK&&r.open_ready);weread_runtime_result_free(&r);
    if(missing_uid||append_chapter||cloud_index!=2)assert(toc_requests==2);else if(!strcmp(argv[2],"warm"))assert(toc_requests==1);
    else if(!strcmp(argv[2],"fresh-version"))assert(cached==101&&toc_requests==2);
    const char *uri="weread://test_book_101";assert(book_weread_open(uri)==ESP_OK);size_t chapter=42;uint32_t offset=99;
    int resumed=weread_reader_resume(uri,&chapter,&offset);printf("resume=%d chapter=%zu offset=%u\n",resumed,chapter,offset);
    assert(resumed==ESP_OK);book_weread_close();weread_runtime_stop();weread_shelf_clear();return 0;
}
