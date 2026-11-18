/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_store.h"
#include "book_source_weread.h"
#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>
static const char *fixtures;
static const uint32_t uids[]={11,27,50,103,404,777,900,1201};
static unsigned downloads,sequence[64];
static uint64_t space=512u*1024*1024;
static bool offline;
static uint32_t server_version=100;
static atomic_bool gated,entered;
int window_statvfs(const char *path,struct statvfs *out){(void)path;memset(out,0,sizeof(*out));out->f_frsize=4096;out->f_bavail=space/4096;return 0;}
static uint64_t millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static weread_result_t completed(void){
    uint64_t until=millis()+5000;while(weread_runtime_busy()){assert(millis()<until);usleep(1000);}
    weread_result_t r;assert(weread_runtime_poll(&r));return r;
}
static esp_err_t request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)body;(void)ctx;*status=200;
    char text[4096];const char *r;
    if(strstr(url,"chapterInfos")){
        size_t used=(size_t)snprintf(text,sizeof(text),"{\"data\":[{\"bookId\":\"window_book\",\"bookVersion\":%u,\"synckey\":1,\"clearAll\":1,\"updated\":[",server_version);
        for(unsigned i=0;i<8;++i){unsigned index=server_version==101?(i==1?7:i==6?2:i+1):i+1;
            used+=(size_t)snprintf(text+used,sizeof(text)-used,"%s{\"chapterUid\":%u,\"chapterIdx\":%u,\"title\":\"UID%u-v%u\"}",i?",":"",uids[i],index,uids[i],server_version);
        }
        snprintf(text+used,sizeof(text)-used,"]}]}");r=text;
    }else if(strstr(url,"/book/read"))r="{\"succ\":true}";
    else {
        snprintf(text,sizeof(text),"{\"synckey\":1,\"books\":[{\"bookId\":\"window_book\",\"title\":\"Window\",\"format\":\"epub\",\"bookVersion\":%u}],\"bookProgress\":[{\"bookId\":\"window_book\",\"chapterUid\":11,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":0}]}",server_version);r=text;
    }
    *out=strdup(r);assert(*out);*len=strlen(*out);return ESP_OK;
}
static esp_err_t download(const char *url,const char *headers,FILE *out,size_t limit,weread_cancel_fn cancel,void *cc,weread_download_info_t *info,void *ctx){
    (void)headers;(void)ctx;const char *p=strstr(url,"chapters=");assert(p);p+=9;if(!strncmp(p,"0,",2))p+=2;
    unsigned uid=(unsigned)strtoul(p,NULL,10);assert(downloads<64);sequence[downloads++]=uid;
    if(atomic_load(&gated)){
        atomic_store(&entered,true);while(!cancel||!cancel(cc))usleep(1000);
        return ESP_ERR_INVALID_STATE;
    }
    if(offline)return ESP_ERR_TIMEOUT;
    const char *v=strstr(url,"&bookVersion=");assert(v);
    if((uint32_t)strtoul(v+13,NULL,10)!=server_version){
        const char *error="{\"errcode\":-2063}";info->status=499;info->bytes=strlen(error);
        assert(fwrite(error,1,info->bytes,out)==info->bytes);return ESP_OK;
    }
    char path[1024];snprintf(path,sizeof(path),"%s/%u-v%u.zip",fixtures,uid,server_version);FILE *f=fopen(path,"rb");assert(f);
    info->status=200;strcpy(info->encrypt_key,"pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
    char buf[97];size_t n;while((n=fread(buf,1,sizeof(buf),f))){
        if(cancel&&cancel(cc)){fclose(f);return ESP_ERR_INVALID_STATE;}
        assert(n<=limit-info->bytes&&fwrite(buf,1,n,out)==n);info->bytes+=n;
    }
    fclose(f);return ESP_OK;
}
static weread_job_t step(size_t chapter,uint64_t now){
    weread_reader_tick("weread://window_book",chapter,0,0,"",now,true);
    uint64_t until=millis()+5000;
    while(weread_runtime_busy()){assert(millis()<until);usleep(1000);}
    weread_result_t r;if(!weread_runtime_poll(&r))return WR_JOB_INIT;
    weread_job_t job=r.job;weread_runtime_result_free(&r);return job;
}
static void init(const char *root){
    assert(!setenv("WEREAD_STORAGE_ROOT",root,1)&&weread_auth_init(root)==ESP_OK);
    weread_auth_session_t session={0};strcpy(session.vid,"123");strcpy(session.access_token,"test-token");strcpy(session.refresh_token,"test-refresh");
    assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    weread_client_set_transport(request,NULL);weread_client_set_download_transport(download,NULL);
    assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("window_book")==ESP_OK);
    assert(weread_chapter_download("window_book",11)==ESP_OK&&book_weread_open("weread://window_book")==ESP_OK);
    weread_progress_t p={.chapter_uid=11,.chapter_index=1,.book_version=100};
    assert(weread_progress_select("window_book",&p,true)==ESP_OK);
    size_t chapter=99;uint32_t offset=99;
    assert(weread_reader_resume("weread://window_book",&chapter,&offset)==ESP_OK&&chapter==0&&offset==0);
    weread_reader_begin("weread://window_book",1000);
    weread_reader_tick("weread://window_book",0,0,0,"",1000,false);
}
int main(int argc,char **argv){
    assert(argc==4);fixtures=argv[2];init(argv[1]);const char *mode=argv[3];uint64_t now=2500;
    if(!strcmp(mode,"binding")){
        html_text_t old={0},candidate={0};size_t current=0;uint32_t v=0,old_coordinate=0;
        assert(book_weread_load(0,NULL,NULL,&old)==ESP_OK&&book_weread_commit_loaded(&current,&v)==ESP_OK&&v==100);
        uint32_t old_identity=0;struct stat old_stat;
        assert(book_weread_content_identity("weread://window_book",&old_identity)&&old_identity);
        assert(book_weread_stat("weread://window_book",&old_stat)&&(uint32_t)old_stat.st_size==old_identity);
        uint32_t foreign_identity=1;assert(!book_weread_content_identity("weread://another_book",&foreign_identity)&&!foreign_identity);
        assert(book_weread_map_version(0,100,true,3,&old_coordinate)==ESP_OK);
        uint32_t cached_coordinate=0;assert(book_weread_map_cached(0,100,true,3,&cached_coordinate)==ESP_OK&&cached_coordinate==old_coordinate);
        assert(book_weread_map_cached(0,101,true,3,&cached_coordinate)==ESP_ERR_INVALID_STATE);
        assert(book_weread_map_cached(0,100,true,UINT32_MAX,&cached_coordinate)==WEREAD_ERR_POSITION);
        char old_root[512];assert(weread_chapter_directory("window_book",11,old_root,sizeof(old_root))==ESP_OK&&book_weread_directory_in_use(old_root));
        size_t old_map_bytes=book_weread_map_cache_bytes();assert(old_map_bytes>0&&old_map_bytes<=65536);
        assert(book_weread_prepare_open("window_book")==ESP_OK&&book_weread_prepared("window_book"));
        server_version=101;assert(weread_shelf_refresh_book("window_book")==ESP_OK&&weread_toc_sync_version("window_book",101)==ESP_OK);
        struct stat metadata_stat;assert(book_weread_stat("weread://window_book",&metadata_stat)&&(uint32_t)metadata_stat.st_size==old_identity);
        assert(book_weread_map_detached("window_book",11,100,false,3,&cached_coordinate)==ESP_OK&&cached_coordinate==old_coordinate);
        assert(book_weread_prepared("window_book")&&weread_reader_prepare("weread://window_book",1)==ESP_OK);
        assert(book_weread_load(1,NULL,NULL,&candidate)==ESP_OK&&strstr(candidate.utf8,"version101"));
        assert(book_weread_content_identity("weread://window_book",&foreign_identity)&&foreign_identity==old_identity);
        assert(book_weread_map_cached(1,101,true,3,&cached_coordinate)==ESP_ERR_INVALID_STATE);
        char title[256];assert(book_weread_title(1,title,sizeof(title))==ESP_OK&&!strcmp(title,"UID27-v101"));
        assert(book_weread_map_cache_bytes()>old_map_bytes&&book_weread_map_cache_bytes()<=131072);
        book_weread_discard_loaded();html_text_free(&candidate);assert(book_weread_map_cache_bytes()==old_map_bytes);
        assert(book_weread_cached_version("window_book",&v)&&v==100&&book_weread_uid(1)==27);
        uint32_t coordinate=0;assert(book_weread_map_version(0,100,true,3,&coordinate)==ESP_OK&&coordinate==old_coordinate);
        uint8_t *image=NULL;size_t length=0;bool png=false;
        assert(book_weread_image(0,"Images/p.png",&image,&length,&png)==ESP_OK&&length>8&&length>=18&&!memcmp(image+8,"version100",10));free(image);
        assert(book_weread_directory_in_use(old_root));
        weread_reader_navigated_version("weread://window_book",0,3,17,"old100",100);
        assert(weread_reader_save(now)==ESP_OK);
        weread_progress_t pending;assert(weread_progress_load("window_book",&pending)==ESP_OK&&pending.book_version==100&&pending.dirty&&pending.position_pending&&pending.chapter_offset==old_coordinate);
        current=1;assert(book_weread_load(1,NULL,NULL,&candidate)==ESP_OK&&book_weread_commit_loaded(&current,&v)==ESP_OK&&current==6&&v==101);
        assert(book_weread_uid(current)==27&&book_weread_cached_version("window_book",&v)&&v==101&&!book_weread_prepared("window_book"));
        uint32_t new_identity=0;struct stat new_stat;
        assert(book_weread_content_identity("weread://window_book",&new_identity)&&new_identity!=old_identity);
        assert(book_weread_stat("weread://window_book",&new_stat)&&(uint32_t)new_stat.st_size==new_identity);
        assert(book_weread_map_version(current,101,true,3,&coordinate)==ESP_OK);
        assert(book_weread_map_cached(current,101,true,3,&cached_coordinate)==ESP_OK&&cached_coordinate==coordinate);
        assert(book_weread_map_cached(1,101,true,3,&cached_coordinate)==ESP_ERR_INVALID_STATE);
        assert(book_weread_map_cached(current,100,true,3,&cached_coordinate)==ESP_ERR_INVALID_STATE);
        uint32_t checked=0;assert(book_weread_validate_prepared("window_book",27,7,coordinate,&checked)==ESP_OK&&checked==101);
        assert(book_weread_image(current,"Images/p.png",&image,&length,&png)==ESP_OK&&length>8&&length>=18&&!memcmp(image+8,"version101",10));free(image);
        assert(weread_runtime_start(WR_JOB_OPEN,"window_book",NULL));weread_result_t reopen=completed();assert(reopen.error==ESP_OK&&!reopen.conflict&&reopen.open_ready);weread_runtime_result_free(&reopen);
        assert(!book_weread_directory_in_use(old_root));html_text_free(&old);html_text_free(&candidate);
        puts("PASS: layout discard preserves actual100 text/map/image/identity under metadata101; commit adopts UID27 at its fresh ordinal7, actual101 identity and exact cached coordinates");
    }else if(!strcmp(mode,"version-zero")){
        assert(weread_progress_note("window_book",11,1,20,17,4,1,"old pending",1700000100)==ESP_OK);
        weread_progress_t before,after;assert(weread_progress_load("window_book",&before)==ESP_OK&&before.book_version==100&&before.dirty);
        char old_root[512];assert(weread_chapter_directory("window_book",11,old_root,sizeof(old_root))==ESP_OK);
        unsigned old_downloads=downloads;server_version=0;
        assert(weread_reader_prepare("weread://window_book",1)==WEREAD_ERR_VERSION&&downloads==old_downloads+1);
        assert(weread_progress_load("window_book",&after)==ESP_OK&&!memcmp(&before,&after,sizeof(before)));
        uint32_t version=999;assert(book_weread_cached_version("window_book",&version)&&version==100);
        char *body=NULL;size_t length=0;assert(weread_chapter_load_cached("window_book",11,&body,&length)==ESP_OK&&strstr(body,"Chapter 11"));free(body);
        book_weread_close();
        char old_manifest[512];assert(wr_cache_path("window_book","chapter-11.json",old_manifest,sizeof(old_manifest))==ESP_OK);
        assert(!unlink(old_manifest));
        assert(weread_runtime_start(WR_JOB_OPEN,"window_book",NULL));weread_result_t r=completed();
        assert(r.error==ESP_OK&&!r.conflict&&r.open_ready);weread_runtime_result_free(&r);
        assert(book_weread_open("weread://window_book")==ESP_OK&&book_weread_cached_version("window_book",&version)&&version==0);
        assert(weread_progress_load("window_book",&after)==ESP_OK&&after.book_version==0&&after.position_pending&&after.chapter_offset==20);
        html_text_t parsed={0};assert(book_weread_load(0,NULL,NULL,&parsed)==ESP_OK&&strstr(parsed.utf8,"Chapter 11"));html_text_free(&parsed);
        char old_body[600];snprintf(old_body,sizeof(old_body),"%s/body.html",old_root);assert(!access(old_body,R_OK));
        puts("PASS: rejected held version100 with fresh unknown0 does not repeat stale request or relabel old text/pending; explicit reopen verifies fresh cloud and downloads0");
    }else if(!strcmp(mode,"window")){
        assert(step(0,now++)==WR_JOB_PREFETCH&&downloads==2&&sequence[1]==27&&weread_reader_offline()==1);
        assert(step(0,now++)==WR_JOB_COVER);
        assert(step(0,now++)==WR_JOB_PREFETCH&&sequence[2]==50&&weread_reader_offline()==2);
        assert(step(0,now++)==WR_JOB_PREFETCH&&sequence[3]==103&&weread_reader_offline()==3);
        unsigned before=downloads;offline=true;
        for(size_t i=1;i<=3;++i){html_text_t t={0};assert(book_weread_load(i,NULL,NULL,&t)==ESP_OK&&t.len);html_text_free(&t);}
        assert(downloads==before&&!weread_chapter_cached("window_book",404));
        assert(weread_reader_prepare("weread://window_book",4)==ESP_ERR_TIMEOUT&&downloads==before+1);
        for(unsigned i=0;i<8;++i){(void)step(0,now++);}assert(downloads==before+2);
        weread_reader_end(now);book_weread_close();weread_runtime_stop();weread_shelf_clear();weread_auth_forget_session();
        assert(book_weread_open("weread://window_book")==ESP_OK);
        for(size_t i=1;i<=3;++i){html_text_t t={0};assert(book_weread_load(i,NULL,NULL,&t)==ESP_OK&&t.len);html_text_free(&t);}
        assert(downloads==before+2);
        puts("PASS: non-contiguous real UIDs; next chapter then cover then farther chapters; three READY open offline even after RAM/auth/shelf reload and fourth remains missing");
    }else if(!strcmp(mode,"space")){
        space=0;assert(step(0,now++)==WR_JOB_PREFETCH&&weread_reader_storage_blocked()&&downloads==1);
        for(unsigned i=0;i<12;++i){now+=16000;(void)step(0,now);}assert(downloads==1);
        puts("PASS: insufficient space pauses automatic window without touching cached chapter");
    }else if(!strcmp(mode,"fail")){
        offline=true;for(unsigned i=0;i<3;++i){
            while(step(0,now++)!=WR_JOB_PREFETCH){assert(now<2500+(i+1)*20000);}
            assert(downloads==2+i);now+=16000;
        }
        for(unsigned i=0;i<12;++i){now+=16000;(void)step(0,now);}assert(downloads==4);
        puts("PASS: only three actual download failures per unchanged window");
    }else if(!strcmp(mode,"cancel")){
        for(unsigned i=0;i<4;++i){
            atomic_store(&gated,true);atomic_store(&entered,false);
            weread_reader_tick("weread://window_book",0,0,0,"",now,true);
            uint64_t until=millis()+3000;while(!atomic_load(&entered)){assert(millis()<until);usleep(1000);}
            weread_runtime_cancel();weread_result_t r=completed();assert(r.job==WR_JOB_PREFETCH&&r.error==ESP_ERR_INVALID_STATE);weread_runtime_result_free(&r);
            now+=16000;
        }
        atomic_store(&gated,false);assert(step(0,now)==WR_JOB_PREFETCH&&weread_reader_offline()==1);
        puts("PASS: four foreground cancellations do not consume the three-failure allowance");
    }else if(!strcmp(mode,"jump")){
        assert(weread_chapter_download("window_book",404)==ESP_OK);
        atomic_store(&gated,true);weread_reader_tick("weread://window_book",0,0,0,"",now,true);
        uint64_t until=millis()+3000;while(!atomic_load(&entered)){assert(millis()<until);usleep(1000);}
        weread_runtime_cancel();while(weread_runtime_busy()){assert(millis()<until);usleep(1000);}
        weread_reader_tick("weread://window_book",4,0,0,"",now+100,false);
        assert(!weread_reader_offline());atomic_store(&gated,false);
        assert(step(4,now+1600)==WR_JOB_PREFETCH&&sequence[downloads-1]==777&&weread_reader_offline()==1);
        puts("PASS: late cancelled old-window result cannot populate new window; next target uses jumped chapter UID");
    }else assert(false);
    weread_reader_end(now);book_weread_close();weread_runtime_stop();return 0;
}
