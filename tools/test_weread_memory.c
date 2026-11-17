/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 双内存池、真实生产JSON与线程隔离；不是设备峰值测量。
 * Two memory pools, production JSON and thread isolation; not device peak measurements.
 */
#undef malloc
#undef calloc
#undef realloc
#undef free
#include "weread_store.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdatomic.h>

#include "weread_memory_test.h"

static atomic_bool check_other;
static void *unrelated(void *ctx){(void)ctx;while(!atomic_load(&check_other))usleep(100);
    cJSON *o=cJSON_CreateObject();assert(o&&(weread_test_memory_capability(o)&MALLOC_CAP_INTERNAL));cJSON_Delete(o);return NULL;}
static void thread_routing(void){
    pthread_t thread;assert(!pthread_create(&thread,NULL,unrelated,NULL));
    {WR_GUARD;cJSON *o=cJSON_CreateObject();assert(o&&(weread_test_memory_capability(o)&MALLOC_CAP_SPIRAM));atomic_store(&check_other,true);
        pthread_join(thread,NULL);cJSON_Delete(o);}
}
static esp_err_t transport(const char *url,const char *method,const char *headers,const char *body,
    char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)ctx;cJSON *r=cJSON_CreateObject();assert(r);
    if(strstr(url,"/shelf/syncbook")){
        cJSON *request=cJSON_Parse(body),*ids=cJSON_GetObjectItem(request,"bookIds"),*books=cJSON_AddArrayToObject(r,"books"),*v;
        cJSON_ArrayForEach(v,ids){cJSON *b=cJSON_CreateObject();cJSON_AddStringToObject(b,"bookId",v->valuestring);
            cJSON_AddStringToObject(b,"title","Memory stress book");cJSON_AddStringToObject(b,"format","epub");
            cJSON_AddNumberToObject(b,"bookVersion",1);char intro[2049];memset(intro,'X',2048);intro[2048]=0;
            cJSON_AddStringToObject(b,"intro",intro);cJSON_AddItemToArray(books,b);}
        cJSON_Delete(request);
    }else if(strstr(url,"/shelf/sync?")){
        assert(strstr(url,"onlyBookid=1"));cJSON *ids=cJSON_AddArrayToObject(r,"bookIds");
        for(unsigned i=0;i<800;++i){char id[32];snprintf(id,sizeof(id),"memory_%u",i);cJSON_AddItemToArray(ids,cJSON_CreateString(id));}
        cJSON_AddNumberToObject(r,"synckey",17);
    }else if(strstr(url,"/book/chapterInfos")){
        cJSON *data=cJSON_AddArrayToObject(r,"data"),*entry=cJSON_CreateObject();cJSON_AddItemToArray(data,entry);
        cJSON_AddStringToObject(entry,"bookId","memory_0");cJSON_AddNumberToObject(entry,"synckey",12);
        cJSON_AddNumberToObject(entry,"clearAll",1);cJSON_AddNumberToObject(entry,"bookVersion",1);
        cJSON *updated=cJSON_AddArrayToObject(entry,"updated");for(unsigned i=1;i<=3000;++i){cJSON *c=cJSON_CreateObject();
            cJSON_AddNumberToObject(c,"chapterUid",i);cJSON_AddNumberToObject(c,"chapterIdx",i);
            cJSON_AddStringToObject(c,"title","Chapter memory stress");cJSON_AddItemToArray(updated,c);}
    }else assert(false);
    *out=cJSON_PrintUnformatted(r);assert(*out);*len=strlen(*out);*status=200;cJSON_Delete(r);return ESP_OK;
}
int main(void){
    wr_memory_init();thread_routing();assert(!weread_test_memory_used(MALLOC_CAP_INTERNAL)&&!weread_test_memory_used(MALLOC_CAP_SPIRAM));
    {WR_GUARD;char nested[80];memset(nested,'[',33);nested[33]='0';memset(nested+34,']',33);nested[67]=0;
        assert(!wr_parse_json(nested,68));
        nested[32]='0';memset(nested+33,']',32);nested[65]=0;
        cJSON *bounded=wr_parse_json(nested,66);assert(bounded);cJSON_Delete(bounded);
        const char escaped[]="{\"text\":\"[[[[{\\\"}]]]]\"}";
        bounded=wr_parse_json(escaped,sizeof(escaped));assert(bounded);cJSON_Delete(bounded);}
    char root[]="/tmp/weread-memory-XXXXXX";assert(mkdtemp(root));assert(weread_auth_init(root)==ESP_OK);
    weread_auth_session_t session={0};strcpy(session.vid,"123");strcpy(session.access_token,"test-token");strcpy(session.refresh_token,"test-refresh");
    assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    weread_client_set_transport(transport,NULL);
    assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_count()==6&&weread_shelf_sync_pending());
    size_t visible=6;
    while(weread_shelf_sync_pending()){
        assert(weread_shelf_sync_step(false)==ESP_OK&&weread_shelf_count()>=visible);
        visible=weread_shelf_count();
    }
    assert(visible==800);
    weread_shelf_item_t *items=NULL;size_t item_count=0;
    assert(weread_shelf_snapshot(&items,&item_count)==ESP_OK&&item_count==800&&sizeof(*items)<512);
    weread_book_t first;assert(weread_shelf_get(0,&first)&&!strcmp(items[0].book_id,first.book_id)&&!strcmp(items[0].title,first.title));
    assert(items[0].cover_key==weread_cover_key(first.book_id,first.cover));heap_caps_free(items);
    assert(weread_shelf_sync(false)==ESP_OK&&weread_shelf_load()==ESP_OK);
    assert(weread_toc_sync("memory_0")==ESP_OK);weread_toc_t toc={0};
    assert(weread_toc_load("memory_0",&toc)==ESP_OK&&toc.count==3000);weread_toc_free(&toc);
    size_t baseline=weread_test_memory_used(MALLOC_CAP_SPIRAM);weread_test_memory_fail_after(2);
    assert(weread_shelf_load()==ESP_ERR_NO_MEM&&weread_shelf_count()==800&&weread_shelf_synckey()==17);
    assert(weread_test_memory_used(MALLOC_CAP_SPIRAM)==baseline);weread_test_memory_fail_after(-1);
    assert(weread_shelf_load()==ESP_OK&&weread_shelf_count()==800);
    weread_shelf_clear();weread_auth_clear_session();assert(!weread_test_memory_used(MALLOC_CAP_INTERNAL)&&!weread_test_memory_used(MALLOC_CAP_SPIRAM));
    puts("Memory pools passed: 800 books, 3000 chapters, task isolation, JSON OOM rollback/retry.");weread_test_memory_report();
    return 0;
}
