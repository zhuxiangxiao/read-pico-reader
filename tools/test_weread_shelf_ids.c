/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_shelf.h"
#include "weread_auth.h"
#include "weread_store.h"
#include "weread_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static size_t probes,array_walks,table_allocs,table_frees,details;
static bool fail_table,use_raw;static void *table_live;static char*raw_response;
static int json_fail=-1;
static void *json_allocate(size_t n){if(json_fail==0)return NULL;if(json_fail>0)--json_fail;return malloc(n);}
static void *counted_alloc(size_t n){if(n==4096*sizeof(const char*)){++table_allocs;if(fail_table)return NULL;assert(!table_live);table_live=wr_alloc(n);return table_live;}return wr_alloc(n);}
static void counted_free(void*p){if(p&&p==table_live){++table_frees;table_live=NULL;}free(p);}
static int counted_size(const cJSON*a){const cJSON*p=a?a->child:NULL;while(p){++array_walks;p=p->next;}return cJSON_GetArraySize(a);}
#define wr_alloc counted_alloc
#define free counted_free
#define cJSON_GetArraySize counted_size
#include "shelf_under_test.inc"
#undef wr_alloc
#undef free
#undef cJSON_GetArraySize
static cJSON*reply;static char received[6][64];
static esp_err_t transport(const char*u,const char*m,const char*h,const char*b,char**out,size_t*n,int*status,void*ctx){(void)m;(void)h;(void)ctx;*status=200;if(strstr(u,"syncbook")){assert(!table_live);details++;cJSON*req=cJSON_Parse(b),*ids=cJSON_GetObjectItem(req,"bookIds"),*r=cJSON_CreateObject(),*books=cJSON_AddArrayToObject(r,"books"),*id;int i=0;cJSON_ArrayForEach(id,ids){if(i<6)snprintf(received[i],sizeof(received[i]),"%s",id->valuestring);++i;cJSON*v=cJSON_CreateObject();cJSON_AddStringToObject(v,"bookId",id->valuestring);cJSON_AddStringToObject(v,"title",id->valuestring);cJSON_AddStringToObject(v,"format","epub");cJSON_AddItemToArray(books,v);}*out=cJSON_PrintUnformatted(r);cJSON_Delete(req);cJSON_Delete(r);}else *out=use_raw?strdup(raw_response):cJSON_PrintUnformatted(reply);assert(*out);*n=strlen(*out);return ESP_OK;}
static void baseline(void){weread_shelf_clear();s_shelf=cJSON_Parse("{\"vid\":\"123\",\"synckey\":4,\"books\":[{\"bookId\":\"retained\",\"title\":\"Retained\",\"format\":\"epub\"}]}");assert(s_shelf);probes=array_walks=table_allocs=table_frees=details=0;fail_table=false;memset(received,0,sizeof(received));}
static void response(size_t unique,unsigned repeat){cJSON_Delete(reply);reply=cJSON_CreateObject();cJSON_AddNumberToObject(reply,"synckey",5);cJSON*ids=cJSON_AddArrayToObject(reply,"bookIds");for(unsigned pass=0;pass<repeat;pass++)for(size_t i=0;i<unique;i++){char id[64];snprintf(id,sizeof(id),"book_%04zu",i);assert(cJSON_AddItemToArray(ids,cJSON_CreateString(id)));}}
static void verify_order(size_t n){cJSON*p=s_sync.pending->child;for(size_t i=0;i<n;i++,p=p->next){char id[64];snprintf(id,sizeof(id),"book_%04zu",i);assert(p&&!strcmp(p->valuestring,id));}assert(!p);}
int main(void){char path[]="/tmp/weread-id-hash-XXXXXX";assert(mkdtemp(path));assert(weread_auth_init(path)==ESP_OK);weread_auth_session_t session={.vid="123",.access_token="test-token",.refresh_token="test-refresh"};assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);weread_client_set_transport(transport,NULL);
    for(size_t unique=800;unique<=1024;unique+=224){baseline();if(unique==1024)cJSON_Delete(cJSON_DetachItemFromArray(books(s_shelf),0));response(unique,4);assert(sync_begin(false)==ESP_OK&&s_sync.count==unique&&table_allocs==1&&table_frees==1&&!table_live);verify_order(unique);printf("unique=%zu raw=%zu probes=%zu array_walks=%zu table_freed_before_details=1\n",unique,unique*4,probes,array_walks);assert(sync_advance(6,true)==ESP_OK&&weread_shelf_count()==(unique==1024?6:7));for(unsigned i=0;i<6;i++){char id[64];snprintf(id,sizeof(id),"book_%04u",i);assert(!strcmp(received[i],id));}}
    baseline();response(1025,1);cJSON*old=s_shelf;assert(weread_shelf_sync(false)==ESP_ERR_INVALID_SIZE&&s_shelf==old&&weread_shelf_count()==1&&!table_live&&table_frees==1&&!details&&!weread_shelf_sync_pending());puts("1025 unique rejected; old snapshot untouched");
    baseline();response(100,1);old=s_shelf;fail_table=true;assert(weread_shelf_sync(false)==ESP_ERR_NO_MEM&&s_shelf==old&&!table_live&&!details&&!weread_shelf_sync_pending());puts("table OOM rejects before publication");
    baseline();response(0,1);assert(sync_begin(false)==ESP_OK&&!table_allocs);weread_shelf_sync_discard();puts("empty ID list allocates no table");
    baseline();cJSON_Delete(reply);char*data=NULL;FILE*f=fopen("build/weread-tests/shelf-ids/collisions.json","rb");assert(f);assert(!fseek(f,0,SEEK_END));long len=ftell(f);rewind(f);data=malloc((size_t)len+1);assert(data);assert(fread(data,1,(size_t)len,f)==(size_t)len);fclose(f);data[len]=0;reply=cJSON_Parse(data);free(data);assert(reply);assert(sync_begin(false)==ESP_OK&&s_sync.count==80&&!table_live);cJSON*input=cJSON_GetObjectItem(reply,"bookIds")->child,*p=s_sync.pending->child;for(size_t i=0;i<80;i++){assert(input&&p&&!strcmp(input->valuestring,p->valuestring));input=input->next;p=p->next;}assert(!p);printf("80 bucket-2047 collisions + duplicates: probes=%zu; input order and wraparound retained\n",probes);weread_shelf_sync_discard();
    unsigned failed_after_table=0,complete=0;
    for(int nth=0;nth<210;nth++){
        baseline();response(20,2);raw_response=cJSON_PrintUnformatted(reply);assert(raw_response);use_raw=true;old=s_shelf;
        cJSON_Hooks hook={.malloc_fn=json_allocate,.free_fn=free};cJSON_InitHooks(&hook);json_fail=nth;int err=sync_begin(false);json_fail=-1;cJSON_InitHooks(NULL);use_raw=false;free(raw_response);raw_response=NULL;
        assert(s_shelf==old&&weread_shelf_count()==1&&!table_live);if(table_allocs)assert(table_frees==1);
        if(err==ESP_OK){complete++;assert(s_sync.count==20);verify_order(20);}else if(table_allocs)failed_after_table++;
        weread_shelf_sync_discard();
    }
    assert(complete&&failed_after_table);printf("JSON OOM sweep: %u failures after table allocation, %u successful preparations; table and snapshot safe\n",failed_after_table,complete);
    baseline();cJSON_Delete(reply);reply=cJSON_Parse("{\"synckey\":5,\"bookIds\":[{\"bookId\":\"retained\",\"type\":1},\"lecture\",\"book_0000\",\"book_0000\"],\"lectureBooks\":[\"lecture\"]}");assert(reply);assert(sync_begin(false)==ESP_OK&&s_sync.count==1&&!find(books(s_sync.next),"retained")&&!strcmp(s_sync.pending->child->valuestring,"book_0000")&&!table_live);weread_shelf_sync_discard();puts("lecture/type filtering and duplicate order unchanged");
    baseline();response(8,2);assert(sync_begin(true)==ESP_OK&&s_sync.count==8);assert(sync_advance(6,true)==ESP_OK&&weread_shelf_count()==6);for(unsigned i=0;i<6;i++){char id[64];snprintf(id,sizeof(id),"book_%04u",i);assert(!strcmp(received[i],id));}puts("full initial sync preserves exact first six IDs");
    cJSON_Delete(reply);weread_shelf_clear();weread_auth_clear_session();puts("ID hash production probe: PASS");return 0;}
