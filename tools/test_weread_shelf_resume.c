/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * Execute production cache transactions across interrupted and incomplete syncs.
 */
#include "weread_shelf.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
static time_t clock_now=1794900000;
time_t resume_test_time(time_t *out){if(out)*out=clock_now;return clock_now;}
static unsigned requested[64],get_calls,last_batch,total_ids=45,remote_key=100;
static bool missing=true,fail_detail,reverse_ids,remove_three,progress_only;
static int arm_fault=-1,fault_after=-1;
static void *fault_alloc(size_t n){if(fault_after==0)return NULL;if(fault_after>0)--fault_after;return malloc(n);}
static char root[]="/tmp/weread-resume-XXXXXX";
static unsigned index_of(const char *id){unsigned n=99;assert(sscanf(id,"book_%u",&n)==1&&n<64);return n;}
static esp_err_t transport(const char *url,const char *method,const char *headers,
    const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)headers;(void)ctx;*status=200;cJSON *r=cJSON_CreateObject();assert(r);
    if(strstr(url,"/shelf/syncbook")){
        assert(!strcmp(method,"POST"));cJSON *req=cJSON_Parse(body),*ids=cJSON_GetObjectItem(req,"bookIds");
        last_batch=(unsigned)cJSON_GetArraySize(ids);assert(last_batch&&last_batch<=20);
        if(fail_detail){cJSON_Delete(req);cJSON_Delete(r);return ESP_ERR_INVALID_STATE;}
        cJSON *array=cJSON_AddArrayToObject(r,"books"),*v;
        cJSON_ArrayForEach(v,ids){unsigned n=index_of(v->valuestring);++requested[n];if(n==2&&missing)continue;
            cJSON *b=cJSON_CreateObject();assert(b);cJSON_AddStringToObject(b,"bookId",v->valuestring);
            cJSON_AddStringToObject(b,"title",v->valuestring);cJSON_AddStringToObject(b,"format","epub");
            cJSON_AddNumberToObject(b,"bookVersion",remote_key);cJSON_AddItemToArray(array,b);
        }cJSON_Delete(req);
    }else{
        assert(strstr(url,"/shelf/sync?")&&!strcmp(method,"GET"));++get_calls;
        const char *p=strstr(url,"synckey=");assert(p);unsigned base=(unsigned)strtoul(p+8,NULL,10);
        cJSON_AddNumberToObject(r,"synckey",remote_key);cJSON *ids=progress_only?NULL:cJSON_AddArrayToObject(r,"bookIds");
        if(progress_only){cJSON *positions=cJSON_AddArrayToObject(r,"bookProgress"),*position=cJSON_CreateObject();
            cJSON_AddStringToObject(position,"bookId","book_00");cJSON_AddNumberToObject(position,"progress",77);cJSON_AddItemToArray(positions,position);}
        if(!progress_only&&base!=remote_key)for(unsigned j=0;j<total_ids;++j){unsigned n=reverse_ids?total_ids-1-j:j;if(remove_three&&n==3)continue;
            char id[24];snprintf(id,sizeof(id),"book_%02u",n);cJSON_AddItemToArray(ids,cJSON_CreateString(id));
        }
        if(remove_three){cJSON *removed=cJSON_AddArrayToObject(r,"removed");cJSON_AddItemToArray(removed,cJSON_CreateString("book_03"));}
    }
    *out=cJSON_PrintUnformatted(r);cJSON_Delete(r);assert(*out);*len=strlen(*out);
    if(arm_fault>=0&&strstr(url,"/shelf/syncbook")){cJSON_Hooks hooks={.malloc_fn=fault_alloc,.free_fn=free};fault_after=arm_fault;cJSON_InitHooks(&hooks);}
    return ESP_OK;
}
static void account(const char *vid){weread_auth_session_t a={0};snprintf(a.vid,sizeof(a.vid),"%s",vid);
    strcpy(a.access_token,"test-token");strcpy(a.refresh_token,"test-refresh");
    assert(weread_auth_save_session(&a)==ESP_OK&&weread_auth_load_session(&a)==ESP_OK);
}
static void reload(void){weread_shelf_clear();assert(weread_shelf_load()==ESP_OK);}
static void finish(void){while(weread_shelf_sync_pending())assert(weread_shelf_sync_step(false)==ESP_OK);}
static cJSON *saved(void){char path[512];snprintf(path,sizeof(path),"%s/shelf.json",root);cJSON *s=NULL;
    assert(wr_read_json(path,&s)==ESP_OK);return s;
}
static uint64_t file_hash(void){char path[512];snprintf(path,sizeof(path),"%s/shelf.json",root);FILE *f=fopen(path,"rb");assert(f);uint64_t h=14695981039346656037ULL;int c;while((c=fgetc(f))!=EOF)h=(h^(unsigned char)c)*1099511628211ULL;assert(!ferror(f));fclose(f);return h;}
static void clean_checkpoint(void){cJSON *s=saved();assert(!cJSON_HasObjectItem(s,"syncPending")&&!cJSON_HasObjectItem(s,"syncTarget")&&!cJSON_HasObjectItem(s,"syncRetryAt"));
    cJSON *b;cJSON_ArrayForEach(b,cJSON_GetObjectItem(s,"books"))assert(!cJSON_HasObjectItem(b,"syncBatch"));cJSON_Delete(s);
}
int main(void){assert(mkdtemp(root));assert(weread_auth_init(root)==ESP_OK);account("123");weread_client_set_transport(transport,NULL);
    assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_count()==5&&last_batch==6);
    unsigned old=requested[0];reload();assert(weread_shelf_sync_step(true)==ESP_OK&&requested[0]==old&&weread_shelf_count()==10);
    fail_detail=true;assert(weread_shelf_sync_step(false)==ESP_ERR_INVALID_STATE&&weread_shelf_count()==10);fail_detail=false;
    reload();reverse_ids=true;assert(weread_shelf_sync_step(true)==ESP_OK&&requested[0]==old&&weread_shelf_count()>=10);finish();
    assert(weread_shelf_count()==44&&weread_shelf_synckey()==100&&weread_shelf_cache_fresh((uint64_t)clock_now));
    for(unsigned i=0;i<45;++i)if(i!=2)assert(requested[i]==1);
    reload();assert(weread_shelf_cache_fresh((uint64_t)clock_now));clock_now+=301;assert(!weread_shelf_cache_fresh((uint64_t)clock_now));
    assert(weread_shelf_sync_step(true)==ESP_OK&&last_batch==1&&!weread_shelf_sync_pending());
    for(unsigned i=0;i<45;++i)if(i!=2)assert(requested[i]==1);
    puts("PASS: interruption/cold reload/reordered IDs retain cached details; only missing ID retries; partial cooldown survives reload");
    progress_only=true;remote_key=101;clock_now+=301;reload();unsigned stable=requested[0];
    assert(weread_shelf_sync_step(true)==ESP_OK&&last_batch==1&&requested[0]==stable&&weread_shelf_synckey()==101);
    cJSON *pending_state=saved();assert(cJSON_GetArraySize(cJSON_GetObjectItem(pending_state,"detailPending"))==1);cJSON_Delete(pending_state);
    puts("PASS: progress-only native cursor changes do not refetch successful details; durable queue retries only the missing ID");
    progress_only=false;
    missing=false;clock_now+=301;reload();assert(weread_shelf_sync_step(true)==ESP_OK&&last_batch==1&&weread_shelf_synckey()==101&&weread_shelf_count()==45);clean_checkpoint();
    unsigned before_get=get_calls;assert(weread_shelf_sync_step(true)==ESP_OK&&get_calls==before_get+1);for(unsigned i=0;i<45;++i)if(i!=2)assert(requested[i]==1);
    puts("PASS: recovered missing detail completes the cursor; final checkpoints are removed; unchanged sync reuses all metadata");
    remote_key=102;total_ids=46;remove_three=true;reverse_ids=false;
    assert(weread_shelf_sync_step(true)==ESP_OK);assert(requested[0]==2);reload();reverse_ids=true;
    assert(weread_shelf_sync_step(true)==ESP_OK&&requested[0]==2);finish();
    weread_book_t b;assert(weread_shelf_synckey()==102&&weread_shelf_count()==45&&!weread_shelf_find("book_03",&b));
    for(unsigned i=0;i<46;++i)if(i!=3){char id[24];snprintf(id,sizeof(id),"book_%02u",i);assert(weread_shelf_find(id,&b)&&b.book_version==102);}
    clean_checkpoint();puts("PASS: a new cloud revision refreshes stale details while same-revision restarts skip confirmed IDs and reconcile membership");
    account("456");weread_shelf_clear();assert(weread_shelf_load()!=ESP_OK&&!weread_shelf_count());
    unsigned previous=requested[0];reverse_ids=false;assert(weread_shelf_sync_step(true)==ESP_OK&&last_batch==6&&requested[0]==previous+1);
    finish();assert(weread_shelf_count()==45);clean_checkpoint();
    puts("PASS: another account cannot reuse previous checkpoints or metadata");
    remote_key=103;assert(weread_shelf_sync_step(true)==ESP_OK);cJSON *seed=saved();char path[512];snprintf(path,sizeof(path),"%s/shelf.json",root);
    unsigned failed=0,succeeded=0;
    for(int nth=0;nth<500;++nth){weread_shelf_clear();assert(wr_write_json(path,seed)==ESP_OK&&weread_shelf_load()==ESP_OK);uint64_t before=file_hash();unsigned prefix=requested[0];
        arm_fault=nth;int err=weread_shelf_sync_step(true);arm_fault=fault_after=-1;cJSON_InitHooks(NULL);
        assert(requested[0]==prefix&&weread_shelf_count()==45&&weread_shelf_find("book_07",&b));
        if(err!=ESP_OK){++failed;assert(file_hash()==before&&b.book_version==102);}else{++succeeded;assert(b.book_version==103);}
    }assert(failed&&succeeded);printf("PASS: resume JSON OOM sweep: %u failures, %u commits; old durable metadata stays exact on failure\n",failed,succeeded);
    weread_shelf_clear();assert(wr_write_json(path,seed)==ESP_OK&&weread_shelf_load()==ESP_OK);unsigned prefix=requested[0];assert(weread_shelf_sync(true)==ESP_OK&&requested[0]==prefix+1);clean_checkpoint();
    cJSON_DeleteItemFromObject(seed,"syncTarget");cJSON_DeleteItemFromObject(seed,"detailPending");cJSON *entry;cJSON_ArrayForEach(entry,cJSON_GetObjectItem(seed,"books"))cJSON_DeleteItemFromObject(entry,"syncBatch");
    weread_shelf_clear();assert(wr_write_json(path,seed)==ESP_OK&&weread_shelf_load()==ESP_OK);prefix=requested[0];assert(weread_shelf_sync_step(true)==ESP_OK&&requested[0]==prefix+1);cJSON_Delete(seed);
    puts("PASS: explicit full sync revalidates details; legacy caches lacking confirmation are never falsely declared current");
    weread_shelf_clear();weread_auth_clear_session();return 0;
}
