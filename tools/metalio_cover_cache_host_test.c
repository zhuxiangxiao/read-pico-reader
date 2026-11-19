/* SPDX-License-Identifier: Apache-2.0
 * 中文：执行共享缓存，覆盖淘汰、版本、容量、内存失败、微信读书原始封面键族与并发复制；
 * Pico 配置另跑 16 槽/768 KiB 的 176x240 成品封面档位与开书前 128 KiB 回收。
 * English: Exercise production cache eviction, versions, capacity, OOM, WeRead raw-cover key
 * families and concurrent copies; the Pico profile also checks its 16-slot/768 KiB 176x240
 * raster budget and the 128 KiB trim before opening the reader.
 */
#include "metalio_cover_cache.h"
#include "metalio_memory.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static size_t available=16u*1024u*1024u;
static bool oom;
void *heap_caps_malloc(size_t n,unsigned caps) {assert(caps==3);return oom?NULL:malloc(n);}
void *heap_caps_calloc(size_t n,size_t size,unsigned caps) {assert(caps==3);return oom?NULL:calloc(n,size);}
size_t heap_caps_get_free_size(unsigned caps) {assert(caps==3);return available;}
size_t heap_caps_get_largest_free_block(unsigned caps) {assert(caps==3);return available;}
static metalio_cover_key_t key(unsigned n) {return (metalio_cover_key_t){{n+1,77,123,456,0,1,0,0}};}
// 与 app_weread.c 的微信读书 RAM 原始封面键同布局：家族标记按书号区分，field[2]=封面版本。
// Same layout as app_weread.c's WeRead raw-cover keys: per-book family marker and field[2]=cover version.
#define COVER_BYTES (176u*240u)
static metalio_cover_key_t raw_key(unsigned book,uint64_t cover,uint64_t epoch) {
    return (metalio_cover_key_t){{UINT64_C(0x5752435652310001)^(uint64_t)(book+1),
        (uint64_t)(book+1),cover,epoch,77,176,240,0}};
}
static void *parallel(void *arg) {
    unsigned id=(unsigned)(uintptr_t)arg;
    uint8_t input[64],out[64];memset(input,(int)id,sizeof(input));
    metalio_cover_key_t k=key(100+id);
    for(int i=0;i<1000;++i) {
        assert(metalio_cover_cache_put(&k,input,sizeof(input),(metalio_cover_meta_t){8,8,false}));
        assert(metalio_cover_cache_get(&k,out,sizeof(out),NULL));
        assert(!memcmp(input,out,sizeof(out)));
    }
    return NULL;
}
int main(void) {
    enum {CHUNK=512*1024};
    uint8_t *pixels=malloc(CHUNK),*out=malloc(CHUNK);assert(pixels&&out);
    memset(pixels,0x5a,CHUNK);
    metalio_cover_meta_t meta={176,240,true},got;
    metalio_cover_key_t k=key(0);
    oom=true;assert(!metalio_cover_cache_put(&k,pixels,CHUNK,meta));oom=false;
    assert(!metalio_cover_cache_get(&k,out,CHUNK,&got));
#if KIIKO_BOARD_METALIO47
    for(unsigned i=0;i<8;++i){k=key(i);assert(metalio_cover_cache_put(&k,pixels,CHUNK,meta));}
    assert(metalio_cover_cache_stats().bytes==4u*1024u*1024u);
    k=key(0);assert(metalio_cover_cache_get(&k,out,CHUNK,&got)&&got.width==176&&got.height==240&&got.white_edge);
    assert(!memcmp(pixels,out,CHUNK));out[0]=0;assert(metalio_cover_cache_get(&k,out,CHUNK,NULL)&&out[0]==0x5a);
    k=key(8);assert(metalio_cover_cache_put(&k,pixels,CHUNK,meta));
    k=key(1);assert(!metalio_cover_cache_get(&k,out,CHUNK,NULL));
    k=key(0);assert(metalio_cover_cache_get(&k,out,CHUNK,NULL));
    assert(!metalio_cover_cache_get(&k,out,CHUNK-1,NULL));
#else
    // Pico：微信读书书架把 176x240 成品灰阶（42,240 B）塞进 16 槽/768 KiB，开书前收到 128 KiB。
    // Pico: the WeRead shelf stores 176x240 gray rasters (42,240 B) in 16 slots/768 KiB and trims to 128 KiB before opening.
    uint8_t *cover=malloc(COVER_BYTES);assert(cover);memset(cover,0x3c,COVER_BYTES);
    const unsigned slots=KIIKO_BOARD_COVER_RAM_SLOTS;
    assert(slots<=16&&(size_t)slots*COVER_BYTES<=(size_t)KIIKO_BOARD_COVER_RAM_BUDGET_KB*1024u);
    for(unsigned i=0;i<slots;++i){k=key(i);assert(metalio_cover_cache_put(&k,cover,COVER_BYTES,meta));}
    assert(metalio_cover_cache_stats().entries==slots);
    assert(metalio_cover_cache_stats().bytes==(size_t)slots*COVER_BYTES);
    k=key(slots);assert(metalio_cover_cache_put(&k,cover,COVER_BYTES,meta));
    assert(metalio_cover_cache_stats().entries==slots&&metalio_cover_cache_stats().bytes==(size_t)slots*COVER_BYTES);
    k=key(0);assert(!metalio_cover_cache_get(&k,out,COVER_BYTES,NULL));
    k=key(slots);assert(metalio_cover_cache_get(&k,out,COVER_BYTES,&got)&&got.width==176&&got.height==240);
    metalio_cover_cache_trim(128u*1024u);
    assert(metalio_cover_cache_stats().bytes==(128u*1024u/COVER_BYTES)*COVER_BYTES);
    metalio_cover_cache_clear();free(cover);
#endif
    metalio_cover_cache_clear();assert(!metalio_cover_cache_stats().bytes);
    k=key(6);assert(metalio_cover_cache_put(&k,pixels,64,meta));
    metalio_cover_key_t style=k;style.field[4]=2;style.field[6]=91;
    assert(metalio_cover_cache_put(&style,pixels,64,meta));
    metalio_cover_key_t revised=k;revised.field[2]++;
    assert(metalio_cover_cache_put(&revised,pixels,64,meta));
    assert(!metalio_cover_cache_get(&k,out,CHUNK,NULL)&&!metalio_cover_cache_get(&style,out,CHUNK,NULL));
    assert(metalio_cover_cache_stats().entries==1);
    metalio_cover_cache_clear();k=key(6);
    assert(metalio_cover_cache_put(&k,pixels,64,meta));
    style=k;style.field[4]=2;assert(metalio_cover_cache_put(&style,pixels,64,meta));
    metalio_cover_key_t finished=k;finished.field[4]=3;
    assert(metalio_cover_cache_put(&finished,pixels,16,meta));
    assert(!metalio_cover_cache_get(&k,out,CHUNK,NULL) && !metalio_cover_cache_get(&style,out,CHUNK,NULL));
    assert(metalio_cover_cache_get(&finished,out,CHUNK,NULL));
    assert(metalio_cover_cache_stats().entries==1 && metalio_cover_cache_stats().bytes==16);
#if KIIKO_BOARD_METALIO47
    available=2560u*1024u+63;
    assert(!metalio_cover_cache_put(&k,pixels,64,meta));assert(!metalio_cover_cache_stats().bytes);
    available=16u*1024u*1024u;
    assert(metalio_cover_cache_put(&k,pixels,CHUNK,meta));
    metalio_cover_cache_trim(128*1024);assert(!metalio_cover_cache_stats().bytes);
    assert(metalio_cover_cache_put(&k,pixels,64,meta));oom=true;
    style=key(7);assert(!metalio_cover_cache_put(&style,pixels,64,meta));oom=false;
    assert(!metalio_cover_cache_stats().bytes);
#else
    metalio_cover_cache_clear();
#endif
    pthread_t threads[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&threads[i],NULL,parallel,(void *)(uintptr_t)i));
    for(unsigned i=0;i<4;++i)assert(!pthread_join(threads[i],NULL));
    assert(metalio_cover_cache_stats().entries==4);
    // 微信读书书架原始封面键（176x240）：同族不同书必须共存，同书换封面/代次才淘汰旧项。
    // WeRead shelf raw-cover keys (176x240): sibling books coexist; only the same book's new cover/epoch retires the old entry.
    metalio_cover_cache_clear();
    for(unsigned i=0;i<3;++i){k=raw_key(i,1,1);assert(metalio_cover_cache_put(&k,pixels,COVER_BYTES,meta));}
    assert(metalio_cover_cache_stats().entries==3);
    for(unsigned i=0;i<3;++i){k=raw_key(i,1,1);assert(metalio_cover_cache_get(&k,out,COVER_BYTES,&got)&&got.width==176&&got.height==240);}
    k=raw_key(1,2,1);assert(metalio_cover_cache_put(&k,pixels,COVER_BYTES,meta));
    k=raw_key(1,1,1);assert(!metalio_cover_cache_get(&k,out,COVER_BYTES,NULL));
    k=raw_key(0,1,1);assert(metalio_cover_cache_get(&k,out,COVER_BYTES,NULL));
    k=raw_key(2,1,2);assert(metalio_cover_cache_put(&k,pixels,COVER_BYTES,meta));
    k=raw_key(2,1,1);assert(!metalio_cover_cache_get(&k,out,COVER_BYTES,NULL));
    // 固定家族标记的旧布局会把同族其他书当旧版本淘汰（只剩一本）；这里锁住该语义，说明家族标记必须按书号区分。
    // The old fixed-family layout retires sibling books as stale versions (only one survives); lock that down to justify per-book markers.
    metalio_cover_cache_clear();
    for(unsigned i=0;i<3;++i){k=(metalio_cover_key_t){{UINT64_C(0x5752435652310001),1,(uint64_t)(i+1),1,77,176,240,0}};assert(metalio_cover_cache_put(&k,pixels,COVER_BYTES,meta));}
    assert(metalio_cover_cache_stats().entries==1);
    metalio_cover_cache_clear();free(pixels);free(out);
    uint8_t source[7*19],target[7*19];for(unsigned i=0;i<sizeof(source);++i)source[i]=(uint8_t)i;
    k=key(80);assert(metalio_cover_cache_put_rows(&k,source,19,11,7));memset(target,0xa5,sizeof(target));
    assert(metalio_cover_cache_get_rows(&k,target,19,11,7));
    for(unsigned y=0;y<7;++y)for(unsigned x=0;x<19;++x)assert(target[y*19+x]==(x<11?source[y*19+x]:0xa5));
    assert(!metalio_cover_cache_get_rows(&k,target,19,12,7));
    assert(!metalio_cover_cache_get_rows(&k,target,10,11,7));
    assert(!metalio_cover_cache_put_rows(&k,source,19,SIZE_MAX,7));
    assert(!metalio_cover_cache_put_rows(&k,source,SIZE_MAX,1,7));
    metalio_cover_cache_trim(0);assert(!metalio_cover_cache_get_rows(&k,target,19,11,7));
    metalio_cover_cache_clear();
#if KIIKO_BOARD_METALIO47
    puts("PASS: dynamic 4MiB cap, LRU recency, copy ownership, raster metadata, file-version invalidation, per-book raw-cover keys, 2.5MiB shared reserve, trim/OOM fallback and 4000 concurrent copy cycles");
#else
    puts("PASS: pico 16-slot/768KiB gray-cover budget, per-book raw-cover keys, LRU cap, 128KiB open trim, copy ownership and 4000 concurrent copy cycles");
#endif
    return 0;
}
