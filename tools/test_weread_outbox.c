/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_progress.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"
#include "weread_shelf.h"
#include "weread_chapter.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *id="outbox_book";
static weread_progress_t cloud={.chapter_uid=1,.chapter_index=1,.chapter_offset=10,.progress=10,.book_version=100};
static unsigned writes,reads,fail_write,write_count;
static bool armed,network_fail,empty_ack,cancelled,cloud_incomplete;
static bool cancel_query(void *unused){(void)unused;return cancelled;}
static unsigned block_stage,entered;
static bool release_request;
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready=PTHREAD_COND_INITIALIZER;
static weread_progress_t result;
static esp_err_t report_result;
static int oom_after=-1;
static void *fault_allocate(size_t bytes){if(oom_after==0){oom_after=-1;return NULL;}if(oom_after>0)--oom_after;return malloc(bytes);}
static uint64_t file_hash(const char *path){FILE *file=fopen(path,"rb");assert(file);uint64_t hash=14695981039346656037ULL;int c;while((c=fgetc(file))!=EOF)hash=(hash^(unsigned)c)*1099511628211ULL;assert(!fclose(file));return hash;}
esp_err_t actual_write_json(const char *,const cJSON *);
esp_err_t wr_write_json(const char *path,const cJSON *json){
    if(armed&&strstr(path,"progress.json")&&++write_count==fail_write)return ESP_FAIL;
    return actual_write_json(path,json);
}
static uint64_t millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void block(unsigned stage){
    if(block_stage!=stage)return;
    pthread_mutex_lock(&gate);entered=stage;pthread_cond_broadcast(&ready);
    while(!release_request)pthread_cond_wait(&ready,&gate);
    pthread_mutex_unlock(&gate);
}
static esp_err_t transport(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)ctx;*status=200;
    if(network_fail)return ESP_ERR_TIMEOUT;
    char response[512];
    if(strstr(url,"/shelf/syncbook")){
        ++reads;
        snprintf(response,sizeof(response),"{\"books\":[{\"bookId\":\"%s\",\"bookVersion\":%u}],\"bookProgress\":[{\"bookId\":\"%s\",\"chapterUid\":%u,\"chapterIdx\":%u,\"chapterOffset\":%u,\"progress\":%u}]}",id,cloud.book_version,id,cloud.chapter_uid,cloud.chapter_index,cloud.chapter_offset,cloud.progress);
        if(cloud_incomplete)snprintf(response,sizeof(response),"{\"books\":[{\"bookId\":\"%s\",\"bookVersion\":100}],\"bookProgress\":[]}",id);
        block(1);
    }else if(strstr(url,"/book/getProgress")){
        assert(cloud_incomplete);snprintf(response,sizeof(response),"{\"bookId\":\"%s\",\"book\":{}}",id);
    }else{
        assert(strstr(url,"/book/read"));++writes;
        cJSON *o=cJSON_Parse(body);assert(o);
        assert(!cJSON_HasObjectItem(o,"sync")&&!cJSON_HasObjectItem(o,"dirty")&&!cJSON_HasObjectItem(o,"positionPending"));
        uint32_t offset=(uint32_t)cJSON_GetObjectItemCaseSensitive(o,"chapterOffset")->valuedouble;
        cJSON_Delete(o);block(2);
        if(!empty_ack)cloud.chapter_offset=offset;
        snprintf(response,sizeof(response),"%s",empty_ack?"{}":"{\"succ\":true}");
    }
    *out=strdup(response);assert(*out);*len=strlen(*out);return ESP_OK;
}
static void login(const char *root,const char *vid,const char *token){
    assert(weread_auth_init(root)==ESP_OK);
    weread_auth_session_t s={0};snprintf(s.vid,sizeof(s.vid),"%s",vid);snprintf(s.access_token,sizeof(s.access_token),"%s",token);
    strcpy(s.refresh_token,"refresh-fixture");assert(weread_auth_save_session(&s)==ESP_OK&&weread_auth_load_session(&s)==ESP_OK);
}
static void seed(void){assert(weread_progress_select(id,&cloud,true)==ESP_OK);}
static void shelf_version(uint32_t version){
    char path[512],text[512];snprintf(path,sizeof(path),"%s/shelf.json",weread_auth_get_storage_root());
    snprintf(text,sizeof(text),"{\"vid\":\"123\",\"synckey\":1,\"books\":[{\"bookId\":\"%s\",\"title\":\"fixture\",\"format\":\"epub\",\"bookVersion\":%u}]}",id,version);
    cJSON *o=cJSON_Parse(text);assert(o&&wr_write_json(path,o)==ESP_OK);cJSON_Delete(o);
    assert(weread_shelf_load()==ESP_OK&&weread_shelf_book_version(id)==version);
}
static void move(uint32_t offset,unsigned seconds){
    weread_progress_t delta={.chapter_uid=1,.chapter_index=1,.chapter_offset=offset,.progress=20,.reading_time=seconds,.pending_pages=1};
    strcpy(delta.summary,"fixture");bool noted=false;
    assert(weread_progress_store(id,&delta,true,1700000100,&noted)==ESP_OK&&noted&&delta.dirty&&delta.position_pending);
}
static void *report(void *unused){(void)unused;report_result=weread_progress_outbox_report(id,90000,1700000200,&result);return NULL;}
static pthread_t delayed(unsigned stage){
    block_stage=stage;entered=0;release_request=false;pthread_t thread;assert(!pthread_create(&thread,NULL,report,NULL));
    pthread_mutex_lock(&gate);while(!entered)pthread_cond_wait(&ready,&gate);pthread_mutex_unlock(&gate);return thread;
}
static void complete(pthread_t thread){pthread_mutex_lock(&gate);release_request=true;pthread_cond_broadcast(&ready);pthread_mutex_unlock(&gate);assert(!pthread_join(thread,NULL));block_stage=0;}
int main(int argc,char **argv){
    assert(argc==3);login(argv[1],"123","access-fixture");weread_client_set_transport(transport,NULL);
    if(!strcmp(argv[2],"produce")){seed();move(20,5);puts("pending sample persisted for a fresh process");return 0;}
    weread_progress_t p;
    if(!strcmp(argv[2],"recover")){
        char next[64];bool found=false;
        for(unsigned i=0;i<10&&!found;++i)found=weread_progress_outbox_next(next,sizeof(next),1700000200)==ESP_OK;
        assert(found&&!strcmp(next,id));assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_OK&&!p.dirty&&writes==1);
        assert(weread_progress_state(&p)==WR_PROGRESS_SYNCED);puts("fresh process discovers and acknowledges the pending book");return 0;
    }
    seed();move(20,5);
    if(!strcmp(argv[2],"merge")){
        pthread_t thread=delayed(2);uint64_t started=millis();move(30,2);assert(millis()-started<100);
        complete(thread);assert(report_result==ESP_OK&&result.dirty&&result.position_pending&&result.chapter_offset==30&&result.reading_time==2);
        assert(weread_progress_load(id,&p)==ESP_OK&&p.chapter_offset==30&&p.base_offset==20&&p.pending_pages==1);
        assert(weread_progress_outbox_report(id,100000,1700000201,&p)==ESP_OK&&!p.dirty&&writes==2);
        puts("delayed ACK preserves newer navigation, seconds and pages; local save remains below100ms");
    }else if(!strcmp(argv[2],"fetch-race")){
        pthread_t thread=delayed(1);move(30,2);complete(thread);
        assert(report_result==ESP_ERR_INVALID_STATE&&!writes&&weread_progress_load(id,&p)==ESP_OK&&p.chapter_offset==30&&p.dirty);
    }else if(!strcmp(argv[2],"session")){
        pthread_t thread=delayed(2);login(argv[1],"123","replacement-token");complete(thread);
        assert(report_result==ESP_ERR_INVALID_STATE&&weread_progress_load(id,&p)==ESP_OK&&p.dirty&&p.chapter_offset==20);
    }else if(!strcmp(argv[2],"owner")){
        pthread_t thread=delayed(1);login(argv[1],"999","other-owner");complete(thread);
        assert(report_result==ESP_ERR_INVALID_STATE&&!writes&&weread_progress_load(id,&p)!=ESP_OK);
        char next[64];for(unsigned i=0;i<10;++i)assert(weread_progress_outbox_next(next,sizeof(next),1700000300)!=ESP_OK);
    }else if(!strcmp(argv[2],"media")){
        pthread_t thread=delayed(2);char other[1024];snprintf(other,sizeof(other),"%s/other-card",argv[1]);login(other,"123","access-fixture");complete(thread);
        assert(report_result==ESP_ERR_INVALID_STATE&&weread_progress_load(id,&p)==ESP_ERR_NOT_FOUND);
        login(argv[1],"123","access-fixture");assert(weread_progress_load(id,&p)==ESP_OK&&p.dirty&&p.chapter_offset==20);
    }else if(!strcmp(argv[2],"cancel")){
        weread_client_set_cancel(cancel_query,NULL);cancelled=true;
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_ERR_INVALID_STATE&&p.dirty&&!p.retry_count&&!writes&&!reads);
        cancelled=false;assert(weread_progress_outbox_report(id,90001,1700000201,&p)==ESP_OK&&!p.dirty);
    }else if(!strcmp(argv[2],"conflict")){
        cloud.chapter_offset=50;assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_OK);
        assert(!writes&&!p.sync_conflict&&!p.dirty&&p.chapter_offset==50);
        assert(weread_progress_note(id,1,1,80,35,5,1,"local ahead",1700000250)==ESP_OK);
        cloud.chapter_offset=40;
        assert(weread_progress_outbox_report(id,90001,1700000251,&p)==ESP_OK&&!p.dirty&&!p.sync_conflict&&writes==1);
        weread_progress_t c;bool conflict=false;assert(weread_progress_sync(id,&p,&c,&conflict)==ESP_OK&&!conflict&&!c.book_version&&c.base_version==100&&p.chapter_offset==80);
    }else if(!strcmp(argv[2],"version")){
        p=cloud;p.chapter_offset=20;p.book_version=100;assert(weread_progress_select(id,&p,false)==ESP_OK);
        cloud.book_version=101;assert(weread_progress_outbox_report(id,90000,1700000200,&p)==WEREAD_ERR_VERSION&&!writes&&p.dirty&&!p.sync_conflict);
    }else if(!strcmp(argv[2],"unknown-version")){
        p=cloud;p.chapter_offset=20;p.book_version=100;assert(weread_progress_select(id,&p,false)==ESP_OK);
        cloud.book_version=0;assert(weread_progress_outbox_report(id,90000,1700000200,&p)==WEREAD_ERR_VERSION&&!writes&&p.dirty&&!p.sync_conflict);
    }else if(!strcmp(argv[2],"reselect")){
        network_fail=true;assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_ERR_TIMEOUT&&p.retry_count==1);
        uint32_t revision=p.revision;assert(weread_progress_select(id,&p,false)==ESP_OK);
        assert(weread_progress_load(id,&p)==ESP_OK&&p.revision!=revision&&!p.retry_count&&!p.retry_after&&p.dirty);
        network_fail=false;assert(weread_progress_outbox_report(id,90001,1700000201,&p)==ESP_OK&&!p.dirty);
    }else if(!strcmp(argv[2],"backoff-cap")){
        network_fail=true;uint64_t at=1700000200;unsigned delays[]={15,30,60,120,240,240,240};
        for(unsigned i=0;i<7;++i){assert(weread_progress_outbox_report(id,90000+i,at,&p)==ESP_ERR_TIMEOUT&&p.dirty);assert(p.retry_after==at+delays[i]&&p.retry_count<=5);at=p.retry_after;}
    }else if(!strcmp(argv[2],"backoff")){
        network_fail=true;assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_ERR_TIMEOUT&&p.retry_count==1&&p.retry_after==1700000215);
        weread_progress_outbox_reset();char next[64];for(unsigned i=0;i<10;++i)assert(weread_progress_outbox_next(next,sizeof(next),1700000214)!=ESP_OK);
        unsigned old_reads=reads;assert(weread_progress_outbox_report(id,90001,1700000214,&p)==ESP_ERR_TIMEOUT&&reads==old_reads);
        network_fail=false;empty_ack=true;assert(weread_progress_outbox_report(id,90002,1700000215,&p)==ESP_ERR_INVALID_RESPONSE&&p.dirty&&p.retry_count==2&&p.retry_after==1700000245);
        old_reads=reads;assert(weread_progress_outbox_report(id,90002,1700000220,&p)==ESP_ERR_TIMEOUT&&reads==old_reads);
        empty_ack=false;assert(weread_progress_outbox_report_ex(id,90003,1700000220,true,&p)==ESP_OK&&!p.dirty&&!p.retry_count&&reads>old_reads);
    }else if(!strcmp(argv[2],"ack-write")){
        armed=true;fail_write=1;assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_FAIL&&p.dirty);
        assert(weread_progress_load(id,&p)==ESP_OK&&p.dirty&&p.reading_time==5);
        armed=false;assert(weread_progress_outbox_report(id,90001,1700000201,&p)==ESP_OK&&!p.dirty&&writes==2);
    }else if(!strcmp(argv[2],"local-write")){
        armed=true;fail_write=1;weread_progress_t delta={.chapter_uid=1,.chapter_index=1,.chapter_offset=30,.progress=30};bool noted=true;
        assert(weread_progress_store(id,&delta,true,1700000200,&noted)==ESP_FAIL&&!noted&&!writes);
        assert(weread_progress_load(id,&p)==ESP_OK&&p.chapter_offset==20&&p.dirty);
    }else if(!strncmp(argv[2],"oom-",4)){
        char path[512];assert(wr_cache_path(id,"progress.json",path,sizeof(path))==ESP_OK);uint64_t before=file_hash(path);
        login(argv[1],"999","second-account");oom_after=(int)strtol(argv[2]+4,NULL,10);
        cJSON_Hooks hooks={.malloc_fn=fault_allocate,.free_fn=free};cJSON_InitHooks(&hooks);
        esp_err_t stored=weread_progress_select(id,&cloud,true);cJSON_InitHooks(NULL);
        if(stored!=ESP_OK)assert(file_hash(path)==before);
        login(argv[1],"123","access-fixture");assert(weread_progress_load(id,&p)==ESP_OK&&p.dirty&&p.chapter_offset==20&&p.reading_time==5);
    }else if(!strncmp(argv[2],"bound",5)){
        shelf_version(101);uint32_t actual=!strcmp(argv[2],"bound-zero")?0:!strcmp(argv[2],"bound-old")?100:101;
        cloud.book_version=101;if(actual==101)assert(weread_progress_select(id,&cloud,true)==ESP_OK);
        weread_progress_t delta={.chapter_uid=1,.chapter_index=1,.chapter_offset=30,.progress=30,.pending_pages=1};bool noted=false;
        assert(weread_progress_store_version(id,&delta,true,actual,1700000200,&noted)==ESP_OK&&noted&&delta.book_version==actual&&delta.dirty);
        assert(weread_shelf_book_version(id)==101);
        esp_err_t expected=actual==101?ESP_OK:WEREAD_ERR_VERSION;
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==expected);
        assert(p.book_version==actual&&(actual==101?(!p.dirty&&writes==1):(p.dirty&&!writes&&!p.sync_conflict)));
    }else if(!strcmp(argv[2],"incomplete-cloud")){
        assert(weread_progress_load(id,&p)==ESP_OK);p.base_uid=p.base_index=p.base_offset=0;
        assert(weread_progress_checkpoint(id,&p)==ESP_OK);cloud_incomplete=true;
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_ERR_INVALID_RESPONSE&&!writes&&p.dirty&&!p.sync_conflict);
        weread_progress_t legacy;bool conflict=false;
        assert(weread_progress_sync(id,&p,&legacy,&conflict)==ESP_OK&&!legacy.chapter_uid&&!conflict);
    }else if(!strncmp(argv[2],"corrupt-",8)||!strncmp(argv[2],"load-oom-",9)){
        login(argv[1],"999","second-account");assert(weread_progress_select(id,&cloud,true)==ESP_OK);
        login(argv[1],"123","access-fixture");move(30,2);
        char path[512],archive_a[512],archive_b[512],previous[600];
        assert(wr_cache_path(id,"progress.json",path,sizeof(path))==ESP_OK&&
            wr_cache_path(id,"progress.json.123",archive_a,sizeof(archive_a))==ESP_OK&&
            wr_cache_path(id,"progress.json.999",archive_b,sizeof(archive_b))==ESP_OK);
        cJSON *o=NULL;assert(wr_read_json_primary(path,&o)==ESP_OK);
        if(!strcmp(argv[2],"corrupt-json")||!strcmp(argv[2],"corrupt-previous")||!strcmp(argv[2],"corrupt-missing")){
            if(!strcmp(argv[2],"corrupt-json")){cJSON_Delete(o);o=NULL;assert(wr_read_json(archive_b,&o)==ESP_OK);}
            snprintf(previous,sizeof(previous),"%s.previous",path);assert(wr_write_json(previous,o)==ESP_OK);
            if(!strcmp(argv[2],"corrupt-missing"))assert(!unlink(path));
            else {FILE *file=fopen(path,"wb");assert(file&&fputs("{",file)>=0&&!fclose(file));}
        }else{
            bool partial=!strcmp(argv[2],"corrupt-owned")||!strcmp(argv[2],"corrupt-both")||!strncmp(argv[2],"load-oom-",9);
            if(!strncmp(argv[2],"load-oom-",9)){snprintf(previous,sizeof(previous),"%s.previous",path);assert(wr_write_json(previous,o)==ESP_OK);}
            cJSON_DeleteItemFromObject(o,partial?"progress":"bookId");
            assert(partial?cJSON_AddNumberToObject(o,"progress",101)!=NULL:cJSON_AddStringToObject(o,"bookId","other-book")!=NULL);
            assert(wr_write_json(path,o)==ESP_OK);
            if(!strcmp(argv[2],"corrupt-both")){snprintf(previous,sizeof(previous),"%s.previous",path);assert(wr_write_json(previous,o)==ESP_OK);}
        }
        cJSON_Delete(o);
        if(!strcmp(argv[2],"corrupt-foreign")){
            login(argv[1],"999","second-account");assert(weread_progress_select(id,&cloud,true)==ESP_ERR_INVALID_RESPONSE);
            assert(wr_read_json(archive_a,&o)==ESP_OK&&cJSON_GetObjectItemCaseSensitive(o,"chapterOffset")->valueint==20);cJSON_Delete(o);
        }else if(!strcmp(argv[2],"corrupt-previous")||!strcmp(argv[2],"corrupt-missing")){
            assert(weread_progress_load(id,&p)==ESP_OK&&p.chapter_offset==30);
            move(40,1);assert(weread_progress_load(id,&p)==ESP_OK&&p.chapter_offset==40);
        }else if(!strncmp(argv[2],"load-oom-",9)){
            oom_after=(int)strtol(argv[2]+9,NULL,10);cJSON_Hooks hooks={.malloc_fn=fault_allocate,.free_fn=free};cJSON_InitHooks(&hooks);
            memset(&p,0xa5,sizeof(p));esp_err_t loaded=weread_progress_load(id,&p);cJSON_InitHooks(NULL);
            weread_progress_t zero={0};assert(loaded==ESP_OK?p.chapter_offset==30:!memcmp(&p,&zero,sizeof(p)));
        }else {weread_progress_t zero={0};assert(weread_progress_load(id,&p)!=ESP_OK&&!memcmp(&p,&zero,sizeof(p)));}
    }else if(!strcmp(argv[2],"unsafe-owner")){
        char path[512];cJSON *stored=NULL;
        assert(wr_cache_path(id,"progress.json",path,sizeof(path))==ESP_OK&&wr_read_json(path,&stored)==ESP_OK);
        cJSON_DeleteItemFromObject(stored,"vid");assert(cJSON_AddStringToObject(stored,"vid","../../outside"));
        assert(wr_write_json(path,stored)==ESP_OK);cJSON_Delete(stored);
        login(argv[1],"999","second-account");
        assert(weread_progress_select(id,&cloud,true)==ESP_ERR_INVALID_RESPONSE);
        assert(wr_read_json(path,&stored)==ESP_OK&&!strcmp(wr_string(stored,"vid"),"../../outside"));cJSON_Delete(stored);
    }else if(!strcmp(argv[2],"accounts")||!strcmp(argv[2],"archive-write")||!strcmp(argv[2],"active-write")){
        login(argv[1],"999","second-account");
        if(strcmp(argv[2],"accounts")){
            armed=true;fail_write=!strcmp(argv[2],"archive-write")?1:2;assert(weread_progress_select(id,&cloud,true)==ESP_FAIL);
            armed=false;login(argv[1],"123","access-fixture");
            assert(weread_progress_load(id,&p)==ESP_OK&&p.dirty&&p.chapter_offset==20);
            login(argv[1],"999","second-account");
        }
        assert(weread_progress_select(id,&cloud,true)==ESP_OK);
        assert(weread_progress_load(id,&p)==ESP_OK&&!p.dirty&&p.chapter_offset==10);
        login(argv[1],"123","access-fixture");weread_progress_outbox_reset();
        assert(weread_progress_load(id,&p)==ESP_OK&&p.dirty&&p.chapter_offset==20&&p.reading_time==5);
        char next[64];bool found=false;
        for(unsigned i=0;i<10&&!found;++i)found=weread_progress_outbox_next(next,sizeof(next),1700000200)==ESP_OK;
        assert(found&&!strcmp(next,id)&&weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_OK&&!p.dirty);
        login(argv[1],"999","second-account");
        assert(weread_progress_load(id,&p)==ESP_OK&&!p.dirty&&p.chapter_offset==10);
    }else if(!strcmp(argv[2],"legacy")){
        assert(weread_progress_load(id,&p)==ESP_OK);p.base_known=false;assert(weread_progress_checkpoint(id,&p)==ESP_OK);
        assert(weread_progress_outbox_report(id,90000,1700000200,&p)==ESP_OK&&writes==1&&!p.dirty&&!p.sync_conflict);
    }else assert(false);
    weread_progress_outbox_reset();weread_shelf_clear();puts(argv[2]);return 0;
}
