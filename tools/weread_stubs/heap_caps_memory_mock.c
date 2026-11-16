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

typedef struct {void *pointer;size_t bytes;uint32_t caps;} allocation_t;
static allocation_t allocations[65536];
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static size_t internal,external,internal_peak,external_peak;
static int fail_after=-1;
static const size_t internal_limit=64u*1024u,external_limit=6u*1024u*1024u;
static allocation_t *find(void *p){
    unsigned slot=(unsigned)((uintptr_t)p>>4)&65535;
    for(unsigned i=0;i<65536;++i){allocation_t *r=&allocations[(slot+i)&65535];if(r->pointer==p)return r;if(!r->pointer)return NULL;}
    return NULL;
}
static allocation_t *empty(void *p){
    unsigned slot=(unsigned)((uintptr_t)p>>4)&65535;
    for(unsigned i=0;i<65536;++i){allocation_t *r=&allocations[(slot+i)&65535];if(!r->pointer||r->pointer==(void *)1)return r;}
    return NULL;
}
static void *allocate(size_t bytes,uint32_t caps){
    pthread_mutex_lock(&mutex);bool ext=(caps&MALLOC_CAP_SPIRAM)!=0;
    size_t used=ext?external:internal,limit=ext?external_limit:internal_limit;
    if(bytes>limit-used||(ext&&fail_after==0)){pthread_mutex_unlock(&mutex);return NULL;}
    if(ext&&fail_after>0)--fail_after;
    void *p=malloc(bytes?bytes:1);if(p){allocation_t *record=empty(p);assert(record);*record=(allocation_t){p,bytes,caps};
        if(ext){external+=bytes;if(external>external_peak)external_peak=external;}
        else{internal+=bytes;if(internal>internal_peak)internal_peak=internal;}}
    pthread_mutex_unlock(&mutex);return p;
}
void weread_test_free(void *pointer){
    if(!pointer)return;pthread_mutex_lock(&mutex);allocation_t *r=find(pointer);
    if(r){if(r->caps&MALLOC_CAP_SPIRAM)external-=r->bytes;else internal-=r->bytes;memset(r,0,sizeof(*r));r->pointer=(void *)1;}
    pthread_mutex_unlock(&mutex);free(pointer);
}
void *weread_test_malloc(size_t bytes){return allocate(bytes,MALLOC_CAP_INTERNAL);}
void *weread_test_calloc(size_t n,size_t bytes){if(bytes&&n>SIZE_MAX/bytes)return NULL;void *p=weread_test_malloc(n*bytes);if(p)memset(p,0,n*bytes);return p;}
void *heap_caps_malloc(size_t bytes,uint32_t caps){return allocate(bytes,caps);}
void *heap_caps_calloc(size_t n,size_t bytes,uint32_t caps){if(bytes&&n>SIZE_MAX/bytes)return NULL;void *p=allocate(n*bytes,caps);if(p)memset(p,0,n*bytes);return p;}
void heap_caps_free(void *p){weread_test_free(p);}
size_t heap_caps_get_free_size(uint32_t caps){return caps&MALLOC_CAP_SPIRAM?external_limit-external:internal_limit-internal;}
size_t heap_caps_get_largest_free_block(uint32_t caps){return heap_caps_get_free_size(caps);}
void *heap_caps_realloc(void *p,size_t bytes,uint32_t caps){
    if(!p)return allocate(bytes,caps);allocation_t *r=find(p);assert(r);size_t old=r->bytes;
    void *next=allocate(bytes,caps);if(next){memcpy(next,p,old<bytes?old:bytes);weread_test_free(p);}return next;
}
void *weread_test_realloc(void *p,size_t bytes){return heap_caps_realloc(p,bytes,MALLOC_CAP_INTERNAL);}
uint32_t weread_test_memory_capability(void *p){allocation_t *r=find(p);assert(r);return r->caps;}

size_t weread_test_memory_used(uint32_t caps){return caps&MALLOC_CAP_SPIRAM?external:internal;}
void weread_test_memory_fail_after(int count){fail_after=count;}
void weread_test_memory_report(void){assert(!internal&&!external);printf("Tracked memory released; internal peak=%zu, PSRAM peak=%zu.\n",internal_peak,external_peak);}
