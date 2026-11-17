/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_shelf.h"
#include "weread_auth.h"
#include "weread_store.h"
#include "weread_client.h"
#include <assert.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
static time_t clock_now=1800000000;
static time_t test_time(time_t *out){if(out)*out=clock_now;return clock_now;}
static int io_fault,write_scope,oom_after=-1;
static bool fat_mode;static unsigned rename_calls,rename_fail_mask;
static const char *detail_response="{\"books\":[]}";
static unsigned full_writes,check_writes,clones,requests,check_bytes,sorts;
static const char *response="{\"synckey\":7,\"bookIds\":[]}";
static void *json_alloc(size_t n){if(oom_after==0)return NULL;if(oom_after>0)--oom_after;return malloc(n);}
static bool fault(int n){if(!write_scope||io_fault!=n)return false;io_fault=0;errno=EIO;return true;}
static int test_open(const char*p,int f,...){va_list a;va_start(a,f);int m=va_arg(a,int);va_end(a);return fault(1)?-1:open(p,f,m);}
static size_t test_fwrite(const void*p,size_t n,size_t c,FILE*f){return fault(2)?0:fwrite(p,n,c,f);}
static int test_fflush(FILE*f){return fault(3)?EOF:fflush(f);}
static int test_fsync(int fd){return fault(4)?-1:fsync(fd);}
static int test_fclose(FILE*f){int r=fclose(f);return fault(5)?EOF:r;}
static int test_rename(const char*a,const char*b){
    if(write_scope&&fat_mode){++rename_calls;if(rename_fail_mask&(1u<<rename_calls)){errno=EIO;return -1;}
        if(strstr(a,".tmp")&&access(b,F_OK)==0){errno=EEXIST;return -1;}}
    return fault(6)?-1:rename(a,b);
}
#define open test_open
#define fwrite test_fwrite
#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
#define rename test_rename
#include "weread_store.c"
#undef open
#undef fwrite
#undef fflush
#undef fsync
#undef fclose
#undef rename
static esp_err_t counted_write(const char*p,const cJSON*j){bool check=strstr(p,"shelf-check.json")!=NULL;if(check)check_writes++;else if(strstr(p,"shelf.json"))full_writes++;write_scope=check;esp_err_t e=wr_write_json(p,j);write_scope=0;if(check&&e==ESP_OK){struct stat s;assert(!stat(p,&s));check_bytes=(unsigned)s.st_size;}return e;}
static void counted_sort(void *base,size_t n,size_t width,int(*cmp)(const void*,const void*)){++sorts;qsort(base,n,width,cmp);}
static cJSON *counted_duplicate(const cJSON*j,cJSON_bool r){++clones;return cJSON_Duplicate(j,r);}
#define wr_write_json counted_write
#define cJSON_Duplicate counted_duplicate
#define qsort counted_sort
#define time test_time
#include "weread_shelf.c"
#undef wr_write_json
#undef cJSON_Duplicate
#undef time
#undef qsort
static esp_err_t request(const char*u,const char*m,const char*h,const char*b,char**out,size_t*n,int*status,void*ctx){(void)m;(void)h;(void)b;(void)ctx;requests++;*out=strdup(strstr(u,"syncbook")?detail_response:response);assert(*out);*n=strlen(*out);*status=200;return ESP_OK;}
static char root[512];
static void account(const char*id){weread_auth_session_t s={.access_token="test-token",.refresh_token="test-refresh"};snprintf(s.vid,sizeof(s.vid),"%s",id);assert(weread_auth_save_session(&s)==ESP_OK&&weread_auth_load_session(&s)==ESP_OK);}
static void raw(const char*leaf,const char*data){char p[600];snprintf(p,sizeof(p),"%s/%s",root,leaf);FILE*f=fopen(p,"wb");assert(f);assert(fwrite(data,1,strlen(data),f)==strlen(data));assert(!fclose(f));}
static void stamp(const char*leaf,const char*owner,uint64_t key,uint64_t when){char s[200];snprintf(s,sizeof(s),"{\"vid\":\"%s\",\"synckey\":%llu,\"checkedAt\":%llu}",owner,(unsigned long long)key,(unsigned long long)when);raw(leaf,s);}
static uint64_t hash_file(const char*leaf){char p[600];snprintf(p,sizeof(p),"%s/%s",root,leaf);FILE*f=fopen(p,"rb");if(!f)return 0;uint64_t h=14695981039346656037ULL;int c;while((c=fgetc(f))!=EOF)h=(h^(unsigned)c)*1099511628211ULL;fclose(f);return h;}
static void counts(void){full_writes=check_writes=clones=requests=check_bytes=sorts=0;}
static void reset(uint64_t key,bool pending){weread_shelf_clear();clock_now=1800000000;account("123");char s[300];snprintf(s,sizeof(s),"{\"vid\":\"123\",\"synckey\":%llu,\"checkedAt\":1799999000,\"books\":[{\"bookId\":\"book\",\"title\":\"Original\",\"format\":\"epub\"}],\"syncPending\":%s}",(unsigned long long)key,pending?"true":"false");raw("shelf.json",s);raw("shelf-check.json","{}");raw("shelf-check.json.previous","{}");assert(weread_shelf_load()==ESP_OK);response="{\"synckey\":7,\"bookIds\":[]}";counts();}
int main(void){char temp[]="/tmp/weread-noop-regression-XXXXXX";assert(mkdtemp(temp));snprintf(root,sizeof(root),"%s",temp);assert(weread_auth_init(root)==ESP_OK);cJSON_Hooks h={.malloc_fn=json_alloc,.free_fn=free};cJSON_InitHooks(&h);weread_client_set_transport(request,NULL);
    reset(7,false);uint64_t before=hash_file("shelf.json");cJSON *old=s_shelf;assert(weread_shelf_sync(false)==ESP_OK);assert(s_shelf==old&&hash_file("shelf.json")==before&&requests==1&&clones==0&&sorts==0&&full_writes==0&&check_writes==1&&check_bytes<256&&!weread_shelf_sync_pending());assert(weread_shelf_cache_fresh((uint64_t)clock_now));weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&weread_shelf_cache_fresh((uint64_t)clock_now));printf("noop: GET=1 clones=0 sorts=0 full_writes=0 sidecar_bytes=%u; reload fresh\n",check_bytes);
    reset(7,false);assert(weread_shelf_sync_step(true)==ESP_OK&&clones==0&&full_writes==0&&check_writes==1&&!weread_shelf_sync_pending());
    for(int point=1;point<=6;point++){reset(7,false);before=hash_file("shelf.json");uint64_t stamp_before=hash_file("shelf-check.json");io_fault=point;assert(weread_shelf_sync(false)!=ESP_OK);assert(!io_fault&&hash_file("shelf.json")==before&&hash_file("shelf-check.json")==stamp_before&&!s_checked_at&&!s_checked_key&&!weread_shelf_sync_pending());}
    puts("six sidecar write failures preserve shelf, previous sidecar and memory stamp");
    const unsigned masks[]={0,1u<<2,1u<<3,(1u<<3)|(1u<<4)};
    for(unsigned i=0;i<sizeof(masks)/sizeof(*masks);i++){
        reset(7,false);stamp("shelf-check.json","123",7,1799999000);uint64_t old_stamp=hash_file("shelf-check.json");before=hash_file("shelf.json");fat_mode=true;rename_calls=0;rename_fail_mask=masks[i];int err=weread_shelf_sync(false);fat_mode=false;assert(hash_file("shelf.json")==before);
        if(!masks[i]){assert(err==ESP_OK&&s_checked_at==(uint64_t)clock_now);}
        else{assert(err!=ESP_OK&&!s_checked_at);assert(hash_file("shelf-check.json")==old_stamp||hash_file("shelf-check.json.previous")==old_stamp);weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&!weread_shelf_cache_fresh((uint64_t)clock_now));}
    }
    puts("FAT EEXIST commit, rollback and rollback-failure recovery preserve old stamp");
    unsigned failures=0,success=0;for(int nth=0;nth<90;nth++){reset(7,false);before=hash_file("shelf.json");uint64_t stamp_before=hash_file("shelf-check.json");old=s_shelf;oom_after=nth;int err=weread_shelf_sync(false);oom_after=-1;assert(s_shelf==old&&hash_file("shelf.json")==before&&!full_writes&&!weread_shelf_sync_pending());if(err==ESP_OK){++success;assert(s_checked_at==(uint64_t)clock_now);}else{++failures;assert(hash_file("shelf-check.json")==stamp_before&&!s_checked_at&&!s_checked_key);}}assert(failures&&success);printf("OOM sweep: %u failures/%u successes; old shelf remains exact\n",failures,success);
    reset(7,false);response="{\"synckey\":7}";assert(weread_shelf_sync(false)==ESP_OK&&!clones&&!full_writes&&check_writes==1);puts("all missing change fields allow same-key bypass");
    reset(7,false);stamp("shelf-check.json","123",7,1800000000);weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&weread_shelf_cache_fresh((uint64_t)clock_now));response="{\"synckey\":8,\"bookIds\":[\"book\"]}";assert(weread_shelf_sync_step(true)==ESP_OK);assert(weread_shelf_synckey()==7&&cJSON_IsTrue(cJSON_GetObjectItem(s_shelf,"syncPending"))&&!s_checked_at&&!weread_shelf_cache_fresh((uint64_t)clock_now));weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&!weread_shelf_cache_fresh((uint64_t)clock_now));puts("final skipped detail keeps key/pending and rejects old matching fresh stamp");
    reset(7,true);stamp("shelf-check.json","123",7,1800000000);weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&!weread_shelf_cache_fresh((uint64_t)clock_now));assert(weread_shelf_sync(false)==ESP_OK&&clones==1&&full_writes==1&&!check_writes);puts("pending snapshot rejects old matching sidecar and bypass");
    reset(7,false);response="{\"synckey\":8,\"bookIds\":[]}";assert(weread_shelf_sync(false)==ESP_OK&&weread_shelf_synckey()==8&&clones==1&&full_writes==1&&!check_writes&&!s_checked_at);assert(weread_shelf_count()==1);puts("changed key with empty records commits normal transaction");
    reset(0,false);assert(weread_shelf_sync(false)==ESP_OK&&!check_writes&&full_writes==1);reset(7,false);assert(weread_shelf_sync(true)==ESP_OK&&!check_writes&&full_writes==1);puts("key0 and explicit full sync do not bypass");
    const char *wrong[]={"{\"synckey\":7,\"books\":{}}","{\"synckey\":7,\"bookIds\":{}}","{\"synckey\":7,\"books\":[],\"bookProgress\":null}","{\"synckey\":7,\"books\":[],\"lectureBooks\":1}","{\"synckey\":7,\"books\":[],\"removed\":false}"};for(unsigned i=0;i<sizeof(wrong)/sizeof(*wrong);i++){reset(7,false);response=wrong[i];before=hash_file("shelf.json");assert(weread_shelf_sync(false)!=ESP_OK&&!clones&&!check_writes&&!full_writes&&hash_file("shelf.json")==before);}puts("invalid change-field types fail before clone/write");
    const char*changed[]={"{\"synckey\":7,\"books\":[{\"bookId\":\"book\",\"title\":\"Changed\"}]}","{\"synckey\":7,\"bookIds\":[\"book\"]}","{\"synckey\":7,\"books\":[],\"bookProgress\":[{\"bookId\":\"book\",\"progress\":25}]}","{\"synckey\":7,\"books\":[],\"lectureBooks\":[\"book\"]}","{\"synckey\":7,\"books\":[],\"removed\":[\"book\"]}"};
    for(unsigned i=0;i<sizeof(changed)/sizeof(*changed);i++){reset(7,false);response=changed[i];detail_response="{\"books\":[{\"bookId\":\"book\",\"title\":\"Changed\"}]}";assert(weread_shelf_sync(false)==ESP_OK&&clones==1&&full_writes==1&&!check_writes);detail_response="{\"books\":[]}";}puts("every nonempty change collection uses normal transaction");
    unsigned load_fail=0,load_ok=0,optional_oom=0;for(int nth=0;nth<70;nth++){reset(7,false);stamp("shelf-check.json","123",7,1800000000);oom_after=nth;int err=weread_shelf_load();oom_after=-1;if(err!=ESP_OK)load_fail++;else{load_ok++;if(!weread_shelf_cache_fresh((uint64_t)clock_now))optional_oom++;}assert(weread_shelf_count()==1);}assert(load_fail&&load_ok&&optional_oom);printf("load OOM sweep: failures=%u successful_loads=%u optional_stamp_oom=%u; cached books usable\n",load_fail,load_ok,optional_oom);
    reset(7,false);stamp("shelf-check.json","other",7,1800000000);stamp("shelf-check.json.previous","123",8,1800000000);weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&!weread_shelf_cache_fresh((uint64_t)clock_now));stamp("shelf-check.json","123",7,1800001000);stamp("shelf-check.json.previous","123",7,1800000000);weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&weread_shelf_cache_fresh((uint64_t)clock_now));puts("owner/key/future timestamps rejected; matching sane previous stamp accepted");
    reset(7,false);char huge[257];memset(huge,' ',sizeof(huge)-1);huge[sizeof(huge)-1]=0;raw("shelf-check.json",huge);weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&!s_checked_at);reset(7,false);clock_now=0;assert(weread_shelf_sync(false)==ESP_OK&&!check_writes&&!s_checked_at&&!weread_shelf_cache_fresh(0));puts("oversized sidecar and unset clock do not mark fresh");
    reset(7,false);clock_now=-1;assert(weread_shelf_sync(false)==ESP_OK&&!check_writes&&!s_checked_at&&!weread_shelf_cache_fresh((uint64_t)clock_now));
    reset(7,false);assert(weread_shelf_sync(false)==ESP_OK);account("456");assert(!weread_shelf_count()&&!weread_shelf_cache_fresh((uint64_t)clock_now));account("123");assert(weread_shelf_load()==ESP_OK&&weread_shelf_cache_fresh((uint64_t)clock_now));
    char second[]="/tmp/weread-noop-card-XXXXXX",first[512];assert(mkdtemp(second));snprintf(first,sizeof(first),"%s",root);snprintf(root,sizeof(root),"%s",second);assert(weread_auth_init(root)==ESP_OK);reset(7,false);assert(!s_checked_at&&!weread_shelf_cache_fresh((uint64_t)clock_now));snprintf(root,sizeof(root),"%s",first);assert(weread_auth_init(root)==ESP_OK);account("123");assert(weread_shelf_load()==ESP_OK);puts("account/card rebinding does not reuse old in-memory stamp");
    weread_shelf_clear();weread_auth_clear_session();puts("production shelf no-op regression: PASS");return 0;}
