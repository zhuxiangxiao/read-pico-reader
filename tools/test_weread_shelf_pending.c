/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_shelf.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
static time_t now=1800000000;
static time_t test_time(time_t *out){if(out)*out=now;return now;}
#define time test_time
#include "weread_shelf.c"
#undef time
static char root[]="/tmp/weread-pending-XXXXXX";
static const char *native_reply,*detail_reply;
static int fault_after=-1,arm_native=-1;
static unsigned detail_calls,requested_a,requested_b;
static char last_url[256];
static void *fault_alloc(size_t n){if(fault_after==0)return NULL;if(fault_after>0)--fault_after;return malloc(n);}
static esp_err_t transport(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *length,int *status,void *ctx){
    (void)method;(void)headers;(void)ctx;bool detail=strstr(url,"syncbook")!=NULL;
    if(detail){int remaining=fault_after;fault_after=-1;++detail_calls;cJSON *request=cJSON_Parse(body);assert(request);cJSON *id;
        cJSON_ArrayForEach(id,cJSON_GetObjectItem(request,"bookIds")){if(!strcmp(id->valuestring,"A"))++requested_a;if(!strcmp(id->valuestring,"B"))++requested_b;}cJSON_Delete(request);fault_after=remaining;
    }else snprintf(last_url,sizeof(last_url),"%s",url);
    *out=strdup(detail?detail_reply:native_reply);assert(*out);*length=strlen(*out);*status=200;
    if(!detail&&arm_native>=0){fault_after=arm_native;cJSON_Hooks hooks={.malloc_fn=fault_alloc,.free_fn=free};cJSON_InitHooks(&hooks);}
    return ESP_OK;
}
static void account(void){weread_auth_session_t a={.vid="123",.access_token="token",.refresh_token="refresh"};assert(weread_auth_save_session(&a)==ESP_OK&&weread_auth_load_session(&a)==ESP_OK);}
static void path(char *out,const char *leaf){snprintf(out,512,"%s/%s",root,leaf);}
static uint64_t hash(void){char p[512];path(p,"shelf.json");FILE *f=fopen(p,"rb");assert(f);uint64_t h=14695981039346656037ULL;int ch;while((ch=fgetc(f))!=EOF)h=(h^(unsigned char)ch)*1099511628211ULL;assert(!ferror(f));fclose(f);return h;}
static void seed(const char *text){weread_shelf_clear();char p[512];path(p,"shelf.json");cJSON *j=cJSON_Parse(text);assert(j&&wr_write_json(p,j)==ESP_OK);cJSON_Delete(j);assert(weread_shelf_load()==ESP_OK);}
static void reload(void){weread_shelf_clear();assert(weread_shelf_load()==ESP_OK);}
static const char *base="{\"vid\":\"123\",\"synckey\":10,\"detailPending\":[],\"books\":[{\"bookId\":\"A\",\"title\":\"Old\",\"format\":\"epub\",\"bookVersion\":10}]}";
static void queue_progress(void){
    seed(base);native_reply="{\"synckey\":20,\"bookIds\":[\"B\",\"C\"],\"bookProgress\":[{\"bookId\":\"B\",\"progress\":86,\"updateTime\":44}]}";detail_reply="{\"books\":[{\"bookId\":\"C\",\"format\":\"epub\"}]}";
    assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_synckey()==20&&weread_shelf_count()==2);
    cJSON *node=find(detail_queue(s_shelf),"B");assert(node&&number(cJSON_GetObjectItem(node,"readingProgress"),"progress")==86);
    reload();native_reply="{\"synckey\":21,\"bookProgress\":[{\"bookId\":\"B\",\"progress\":88,\"updateTime\":45}]}";detail_reply="{\"books\":[]}";unsigned before=requested_a;
    assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_synckey()==21&&requested_a==before);
    reload();node=find(detail_queue(s_shelf),"B");assert(node&&number(cJSON_GetObjectItem(node,"readingProgress"),"progress")==88);
    native_reply="{\"synckey\":21}";detail_reply="{\"books\":[{\"bookId\":\"B\",\"format\":\"epub\",\"bookVersion\":1}]}";
    assert(weread_shelf_sync_step(true)==ESP_OK);weread_book_t b;assert(weread_shelf_find("B",&b)&&b.progress==88&&b.read_update_time==45&&!detail_queue(s_shelf)->child);
    puts("PASS: missing metadata retains latest native progress across advancing cursor/cold load; bare unchanged cursor retries only queue");
    seed("{\"vid\":\"123\",\"synckey\":21,\"syncPending\":true,\"detailPending\":[{\"bookId\":\"B\",\"readingProgress\":{\"bookId\":\"B\",\"progress\":88}}],\"books\":[]}");
    detail_reply="{\"books\":[{\"bookId\":\"B\",\"format\":\"epub\",\"readingProgress\":{\"bookId\":\"B\",\"progress\":99}}]}";
    assert(weread_shelf_refresh_book("B")==ESP_OK&&weread_shelf_find("B",&b)&&b.progress==99&&!detail_queue(s_shelf)->child&&!cJSON_HasObjectItem(s_shelf,"syncPending"));reload();
    puts("PASS: fresh detail progress wins saved pending progress; direct refresh resolves durable pending entry");
}
static void removals(void){const char *pending="{\"vid\":\"123\",\"synckey\":10,\"syncPending\":true,\"detailPending\":[\"B\"],\"books\":[{\"bookId\":\"A\",\"format\":\"epub\"}]}";
    const char *responses[]={"{\"synckey\":11,\"removed\":[\"B\"]}","{\"synckey\":11,\"lectureBooks\":[\"B\"]}","{\"synckey\":11,\"bookIds\":[{\"bookId\":\"B\",\"type\":1}]}","{\"synckey\":11,\"books\":[{\"bookId\":\"B\",\"format\":\"audio\"}]}"};
    for(unsigned i=0;i<sizeof(responses)/sizeof(*responses);++i){seed(pending);native_reply=responses[i];unsigned before=detail_calls;assert(weread_shelf_sync_step(true)==ESP_OK&&detail_calls==before&&!detail_queue(s_shelf)->child&&weread_shelf_synckey()==11);reload();}
    seed(pending);native_reply="{\"synckey\":11,\"bookIds\":[]}";detail_reply="{\"books\":[{\"bookId\":\"B\",\"format\":\"pdf\"}]}";assert(weread_shelf_sync_step(true)==ESP_OK&&!detail_queue(s_shelf)->child&&weread_shelf_count()==1);
    puts("PASS: only explicit removal/lecture/type/unsupported metadata acknowledges an unavailable queue entry");
}
static void native_oom(void){unsigned failed=0,success=0;
    native_reply="{\"synckey\":11,\"bookIds\":[\"B\",\"C\"],\"bookProgress\":[{\"bookId\":\"B\",\"progress\":86}]}";detail_reply="{\"books\":[{\"bookId\":\"C\",\"format\":\"epub\"}]}";
    for(int nth=0;nth<500;++nth){seed(base);uint64_t before=hash();arm_native=nth;int err=weread_shelf_sync_step(true);arm_native=fault_after=-1;cJSON_InitHooks(NULL);
        if(err!=ESP_OK){++failed;assert(hash()==before&&weread_shelf_synckey()==10&&weread_shelf_count()==1);}else{++success;assert(weread_shelf_synckey()==11&&find(detail_queue(s_shelf),"B"));}reload();}
    assert(failed&&success);printf("PASS: native queue construction 500 JSON OOM sweep: %u unchanged durable hashes/%u complete commits\n",failed,success);
    seed(base);char p[512];path(p,"shelf.json.tmp");assert(!mkdir(p,0700));uint64_t before=hash();assert(weread_shelf_sync_step(true)!=ESP_OK&&hash()==before&&weread_shelf_synckey()==10);assert(!rmdir(p));reload();
    puts("PASS: failed queue/cursor file transaction never advances durable or in-memory cursor");
}
static char *large(bool native,unsigned start,unsigned count){cJSON *r=cJSON_CreateObject();assert(r);cJSON_AddNumberToObject(r,"synckey",30);if(!native){cJSON_AddStringToObject(r,"vid","123");assert(cJSON_AddArrayToObject(r,"detailPending"));}
    cJSON *a=cJSON_AddArrayToObject(r,native?"bookIds":"books");for(unsigned i=start;i<start+count;++i){char id[32];snprintf(id,sizeof(id),"book_%04u",i);cJSON *v=native?cJSON_CreateString(id):cJSON_CreateObject();if(!native){cJSON_AddStringToObject(v,"bookId",id);cJSON_AddStringToObject(v,"format","epub");}assert(cJSON_AddItemToArray(a,v));}
    char *s=cJSON_PrintUnformatted(r);assert(s);cJSON_Delete(r);return s;
}
static void limits(void){char *old=large(false,0,1024);seed(old);free(old);uint64_t before=hash();native_reply="{\"synckey\":31,\"bookIds\":[\"extra\"]}";detail_reply="{\"books\":[]}";
    assert(weread_shelf_sync_step(true)==ESP_ERR_INVALID_SIZE&&hash()==before&&weread_shelf_count()==1024);reload();
    char *replacement=large(true,1024,1024);native_reply=replacement;assert(weread_shelf_sync_step(true)==ESP_ERR_INVALID_SIZE); /* delta cannot replace unrelated membership */
    assert(weread_shelf_sync(true)==ESP_ERR_INVALID_RESPONSE&&hash()==before); /* strict full refuses missing metadata */
    weread_shelf_clear(); /* first staged full starts from key0 and prunes old cached membership */
    assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_count()==0&&cJSON_GetArraySize(detail_queue(s_shelf))==1024&&weread_shelf_synckey()==30);weread_shelf_sync_discard();reload();
    assert(cJSON_GetArraySize(detail_queue(s_shelf))==1024);free(replacement);
    replacement=large(true,0,1025);native_reply=replacement;before=hash();assert(weread_shelf_sync_step(true)==ESP_ERR_INVALID_SIZE&&hash()==before);free(replacement);
    puts("PASS: 1024 combined members/pending limit, strict full rollback and cold full replacement preserve queue and cursor");
}
int main(void){assert(mkdtemp(root));assert(weread_auth_init(root)==ESP_OK);account();weread_client_set_transport(transport,NULL);queue_progress();removals();native_oom();limits();weread_shelf_clear();weread_auth_clear_session();return 0;}
