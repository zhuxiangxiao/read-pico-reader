/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_store.h"
#include "book_source_weread.h"
#include <assert.h>
#include <dirent.h>
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
static bool capture_stages;
static unsigned stage_log[16],stage_log_count;
static unsigned fail_mode,fail_uid,fail_attempts;
static unsigned adopt_test_uid;
static unsigned toc_requests;
static bool fail_toc_request;
static size_t last_job_idx;
static esp_err_t last_job_error;
int window_statvfs(const char *path,struct statvfs *out){(void)path;memset(out,0,sizeof(*out));out->f_frsize=4096;out->f_bavail=space/4096;return 0;}
static void record_stage(void){
    if(!capture_stages)return;
    weread_stage_t stage;uint64_t bytes;
    if(!weread_reader_stage(&stage,&bytes))return;
    if(stage_log_count&&stage_log[stage_log_count-1]==(unsigned)stage)return;
    if(stage_log_count<sizeof(stage_log)/sizeof(*stage_log))stage_log[stage_log_count++]=(unsigned)stage;
}
static uint64_t millis(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void rm_rf(const char *path){
    DIR *dir=opendir(path);
    if(!dir){unlink(path);return;}
    struct dirent *entry;
    while((entry=readdir(dir))){
        if(!strcmp(entry->d_name,".")||!strcmp(entry->d_name,".."))continue;
        char child[600];snprintf(child,sizeof(child),"%s/%s",path,entry->d_name);rm_rf(child);
    }
    closedir(dir);rmdir(path);
}
static weread_result_t completed(void){
    uint64_t until=millis()+5000;while(weread_runtime_busy()){assert(millis()<until);usleep(1000);}
    weread_result_t r;assert(weread_runtime_poll(&r));return r;
}
static esp_err_t request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    (void)method;(void)headers;(void)body;(void)ctx;*status=200;
    if(capture_stages&&strstr(url,"chapterInfos")){
        weread_stage_t stage;uint64_t bytes;char label[64];
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_TOC&&!bytes);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在获取目录…"));
        record_stage();
    }else if(capture_stages&&strstr(url,"/shelf/syncbook")){
        weread_stage_t stage;uint64_t bytes;char label[64];
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_PROGRESS&&!bytes);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在同步进度…"));
        record_stage();
    }
    if(strstr(url,"chapterInfos")){
        ++toc_requests;
        if(fail_toc_request){*out=NULL;*len=0;return ESP_ERR_TIMEOUT;}
    }
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
    // 瞬态超时/硬错误注入：超时验证静默重试一次，硬错误必须原样返回且不重试。
    // Transient-timeout / hard-error injection: a timeout must replay once, a hard error must return unchanged.
    if(uid==fail_uid){
        if(fail_mode==2)return ESP_ERR_INVALID_RESPONSE;
        if(fail_mode==1&&fail_attempts++==0)return ESP_ERR_TIMEOUT;
    }
    if(capture_stages&&uid==fail_uid&&fail_mode==1&&fail_attempts>1){
        weread_stage_t stage;uint64_t bytes;char label[64];
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_RETRY&&!bytes);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"网络超时，正在重试…"));
        record_stage();
        // 重试已重新进入字节流：KB 进度覆盖“正在重试”，大包不会全程停在重试文案。
        // The replay is streaming again: KB progress replaces the retry label instead of staying frozen.
        weread_client_report_download_progress(64u*1024u,false);
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_CHAPTER&&bytes==64u*1024u);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在下载章节 64 KB…"));
    }else if(capture_stages&&uid==11&&!fail_uid){
        weread_stage_t stage;uint64_t bytes;char label[64];
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_CHAPTER&&!bytes);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在下载章节…"));
        record_stage();
        weread_client_report_download_progress(200u*1024u,false);
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_CHAPTER&&bytes==200u*1024u);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在下载章节 200 KB…"));
        weread_client_report_download_progress(2u*1024u*1024u,false);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在下载章节 2 MB…"));
        // 正文传输完成后进入解包校验阶段。
        // Body completion moves the notice to the unpack/verify phase.
        weread_client_report_download_progress(0,false);
        weread_client_report_download_progress(0,true);
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_PARSE);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在校验章节…"));
        record_stage();
    }
    const char *v=strstr(url,"&bookVersion=");assert(v);
    if((uint32_t)strtoul(v+13,NULL,10)!=server_version){
        const char *error="{\"errcode\":-2063}";info->status=499;info->bytes=strlen(error);
        assert(fwrite(error,1,info->bytes,out)==info->bytes);return ESP_OK;
    }
    if(adopt_test_uid==uid){
        // 模拟阅读页在预取下载中接管：字节回调必须发布 CHAPTER 阶段与 KB 文案。
        // Simulate the reader adopting an in-flight prefetch: the byte callback must publish CHAPTER + KB.
        assert(weread_runtime_prefetch_active("window_book",uid));
        weread_runtime_prefetch_adopt(uid);
        weread_client_report_download_progress(200u*1024u,false);
        weread_stage_t stage;uint64_t bytes;char label[64];
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_CHAPTER&&bytes==200u*1024u);
        assert(weread_reader_stage_text(label,sizeof(label))&&!strcmp(label,"正在下载章节 200 KB…"));
        weread_client_report_download_progress(0,true);
        assert(weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_PARSE);
        adopt_test_uid=0;
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
    weread_result_t r;if(!weread_runtime_poll(&r)){last_job_idx=SIZE_MAX;last_job_error=ESP_OK;return WR_JOB_INIT;}
    weread_job_t job=r.job;last_job_idx=r.chapter_idx;last_job_error=r.error;weread_runtime_result_free(&r);return job;
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
        // 冷章超时静默重试一次：仍失败时返回同一超时，但多一次下载尝试。
        // A cold-chapter timeout replays once: it still returns the timeout after a second download attempt.
        assert(weread_reader_prepare("weread://window_book",4)==ESP_ERR_TIMEOUT&&downloads==before+2);
        for(unsigned i=0;i<8;++i){(void)step(0,now++);}assert(downloads==before+3);
        weread_reader_end(now);book_weread_close();weread_runtime_stop();weread_shelf_clear();weread_auth_forget_session();
        assert(book_weread_open("weread://window_book")==ESP_OK);
        for(size_t i=1;i<=3;++i){html_text_t t={0};assert(book_weread_load(i,NULL,NULL,&t)==ESP_OK&&t.len);html_text_free(&t);}
        assert(downloads==before+3);
        puts("PASS: non-contiguous real UIDs; next chapter then cover then farther chapters; three READY open offline even after RAM/auth/shelf reload and fourth remains missing");
    }else if(!strcmp(mode,"space")){
        space=0;assert(step(0,now++)==WR_JOB_PREFETCH&&weread_reader_storage_blocked()&&downloads==1);
        for(unsigned i=0;i<12;++i){now+=16000;(void)step(0,now);}assert(downloads==1);
        // 自动预取已被空间暂停：开关仍启用但处于 stalled 态，一次“恢复”即重新派发（再次被空间拦下）。
        // Automatic prefetch is space-stalled: still enabled but stalled, and one resume retriggers it (blocked again).
        assert(weread_reader_cache_fill_enabled()&&weread_reader_cache_forward_stalled());
        weread_reader_cache_forward_set(true);
        assert(weread_reader_cache_fill_enabled()&&!weread_reader_cache_forward_stalled());
        assert(step(0,now++)==WR_JOB_PREFETCH&&weread_reader_storage_blocked()&&downloads==1);
        puts("PASS: insufficient space pauses automatic window without touching cached chapter and one resume retriggers a blocked fill");
    }else if(!strcmp(mode,"fail")){
        offline=true;for(unsigned i=0;i<3;++i){
            while(step(0,now++)!=WR_JOB_PREFETCH){assert(now<2500+(i+1)*20000);}
            assert(downloads==2+i);now+=16000;
        }
        for(unsigned i=0;i<12;++i){now+=16000;(void)step(0,now);}assert(downloads==4);
        // 连续三次实际失败后自动补齐处于 stalled；一次恢复清空失败计数并重新派发。
        // After three actual failures the automatic fill is stalled; one resume clears the counter and dispatches again.
        assert(weread_reader_cache_fill_enabled()&&weread_reader_cache_forward_stalled());
        weread_reader_cache_forward_set(true);
        assert(!weread_reader_cache_forward_stalled());
        assert(step(0,now++)==WR_JOB_PREFETCH&&downloads==5);
        puts("PASS: only three actual download failures per unchanged window and one resume clears the failure latch");
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
    }else if(!strcmp(mode,"stage")){
        // 删除目录缓存与章节目录，让开书走 进度→目录→下载→校验 的完整阶段链。
        // Drop the TOC and chapter caches so the open job walks the full progress→TOC→download→verify chain.
        book_weread_close();
        char root[512];
        assert(weread_chapter_directory("window_book",11,root,sizeof(root))==ESP_OK);
        rm_rf(root);
        char toc_path[512];
        assert(wr_cache_path("window_book","chapters.json",toc_path,sizeof(toc_path))==ESP_OK&&!unlink(toc_path));
        capture_stages=true;stage_log_count=0;
        assert(weread_runtime_start(WR_JOB_OPEN,"window_book",NULL));
        weread_result_t r=completed();
        weread_stage_t stage=WEREAD_STAGE_RETRY;uint64_t bytes=1;char label[64];
        assert(!weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_NONE&&!bytes);
        assert(!weread_reader_stage_text(label,sizeof(label))&&!label[0]);
        capture_stages=false;
        assert(r.error==ESP_OK&&r.open_ready);weread_runtime_result_free(&r);
        const unsigned expected[]={WEREAD_STAGE_PROGRESS,WEREAD_STAGE_TOC,WEREAD_STAGE_CHAPTER,WEREAD_STAGE_PARSE};
        assert(stage_log_count==4);
        for(unsigned i=0;i<4;++i)assert(stage_log[i]==expected[i]);
        // 目录任务：缓存删除后联网重取目录，阶段为获取目录。
        // The TOC job re-fetches a deleted TOC and reports the TOC stage.
        book_weread_clear_prepared();
        assert(!unlink(toc_path));
        capture_stages=true;stage_log_count=0;
        assert(weread_reader_prepare_open("weread://window_book")==ESP_OK);
        capture_stages=false;
        assert(stage_log_count==1&&stage_log[0]==WEREAD_STAGE_TOC);
        assert(!weread_reader_stage(&stage,&bytes)&&!weread_reader_stage_text(label,sizeof(label)));
        puts("PASS: foreground open stages progress->TOC->chapter(+bytes)->verify and the TOC job reports fetching; idle clears the notice");
    }else if(!strcmp(mode,"retry")){
        const char *uri="weread://window_book";
        // 超时一次后静默重试成功：重试期间保持「网络超时，正在重试…」。
        // One transient timeout replays silently and the retry notice stays visible during the replay.
        fail_uid=404;fail_mode=1;fail_attempts=0;capture_stages=true;stage_log_count=0;
        unsigned before=downloads;
        assert(weread_reader_prepare(uri,4)==ESP_OK);
        capture_stages=false;
        assert(downloads==before+2&&weread_chapter_cached("window_book",404));
        assert(stage_log_count==1&&stage_log[0]==WEREAD_STAGE_RETRY);
        weread_stage_t stage=WEREAD_STAGE_RETRY;uint64_t bytes=1;char label[64];
        assert(!weread_reader_stage(&stage,&bytes)&&!weread_reader_stage_text(label,sizeof(label)));
        // 非超时错误不重试：只下载一次并原样上报。
        // A hard error is reported unchanged after one download attempt.
        fail_uid=777;fail_mode=2;fail_attempts=0;
        before=downloads;
        assert(weread_reader_prepare(uri,5)==ESP_ERR_INVALID_RESPONSE&&downloads==before+1);
        assert(!weread_reader_stage(&stage,&bytes));
        fail_uid=0;fail_mode=0;
        // 目录任务在UI线程同步等待，超时不重试：只发一次目录请求并快速失败。
        // The TOC job joins the UI thread synchronously, so its timeout never replays: one TOC request and a fast failure.
        book_weread_clear_prepared();
        char toc_path[512];
        assert(wr_cache_path("window_book","chapters.json",toc_path,sizeof(toc_path))==ESP_OK&&!unlink(toc_path));
        fail_toc_request=true;toc_requests=0;capture_stages=true;stage_log_count=0;
        assert(weread_reader_prepare_open(uri)==ESP_ERR_TIMEOUT);
        capture_stages=false;fail_toc_request=false;
        assert(toc_requests==1&&stage_log_count==1&&stage_log[0]==WEREAD_STAGE_TOC);
        assert(!weread_reader_stage(&stage,&bytes)&&!weread_reader_stage_text(label,sizeof(label)));
        puts("PASS: timeout retries the chapter body exactly once with the retry notice; hard errors and the UI-joined TOC job never replay");
    }else if(!strcmp(mode,"manual")){
        const char *uri="weread://window_book";
        assert(weread_chapter_download("window_book",50)==ESP_OK);
        unsigned before=downloads;
        uint64_t t=now;
        // 真实翻页后，自动下一章预读保留；手动关闭时1秒静止窗口内不向后派发。
        // A real chapter move keeps the automatic next-chapter pre-read; manual off dispatches nothing inside the1s idle window.
        weread_reader_tick(uri,1,0,0,"",t,true);
        assert(!weread_reader_cache_forward());
        // 默认自动整本补齐处于启用态（工具栏圆点亮），一次点击即暂停。
        // The automatic whole-book fill is enabled by default (toolbar dot lit) and one tap pauses it.
        assert(weread_reader_cache_fill_enabled());
        assert(step(1,t+400)==WR_JOB_PREFETCH&&last_job_error==ESP_OK&&last_job_idx==2&&downloads==before);
        assert(step(1,t+700)==WR_JOB_INIT);
        // 手动开启后不等1秒静止：封面预热后继续向后补齐下一章。
        // Manual on skips the1s idle window: after the cover warm it keeps filling the next chapter.
        weread_reader_cache_forward_set(true);
        assert(weread_reader_cache_forward());
        assert(step(1,t+800)==WR_JOB_COVER);
        assert(step(1,t+900)==WR_JOB_PREFETCH&&last_job_error==ESP_OK&&last_job_idx==3&&downloads==before+1);
        // 关闭只停止新派发：已启动章节照常完成，窗口内不再启动下一章。
        // Off stops only new dispatches: the in-flight chapter still finishes and no further chapter starts inside the window.
        weread_reader_cache_forward_set(true);
        weread_reader_tick(uri,1,0,0,"",t+950,true);
        assert(weread_runtime_busy());
        weread_reader_cache_forward_set(false);
        assert(!weread_reader_cache_fill_enabled());
        {
            uint64_t until=millis()+5000;while(weread_runtime_busy()){assert(millis()<until);usleep(1000);}
            weread_result_t r;assert(weread_runtime_poll(&r)&&r.job==WR_JOB_PREFETCH&&r.error==ESP_OK&&r.chapter_idx==4);
            weread_runtime_result_free(&r);
        }
        assert(downloads==before+2);
        assert(step(1,t+960)==WR_JOB_INIT);
        // 前台输入冷却仍暂停手动缓存；冷却结束后按开关继续。
        // Recent foreground input still pauses manual caching; it resumes once the cooldown expires.
        weread_reader_cache_forward_set(true);
        assert(weread_reader_cache_fill_enabled());
        weread_reader_input(false);
        assert(step(1,t+970)==WR_JOB_INIT);
        usleep(850000);
        assert(step(1,t+980)==WR_JOB_PREFETCH&&last_job_error==ESP_OK&&last_job_idx==5&&downloads==before+3);
        weread_reader_cache_forward_set(false);
        assert(step(1,t+990)==WR_JOB_INIT);
        // 显式关闭也抑制原有的自动空闲整本补齐：静止超过1秒不再派发 forward。
        // Explicit off also suppresses the pre-existing automatic idle whole-book fill: no forward dispatch even after1s idle.
        assert(step(1,t+2000)==WR_JOB_INIT&&downloads==before+3);
        // 自动下一章预读仍保留：翻到第6章后未缓存的第7章照常预读。
        // The urgent next-chapter pre-read remains: after moving to chapter6 the uncached chapter7 is still pre-read.
        assert(step(6,t+2400)==WR_JOB_INIT);
        assert(step(6,t+2800)==WR_JOB_PREFETCH&&last_job_error==ESP_OK&&last_job_idx==7&&downloads==before+4);
        puts("PASS: manual forward caching ignores the1s idle window, keeps the automatic pre-read, pauses on input, and explicit off suppresses the whole-book fill only");
    }else if(!strcmp(mode,"adopt-stage")){
        // 阅读页接管下一章预取时，下载进度以 CHAPTER 阶段 + KB 文案呈现；完成后回到 PARSE 并被任务结束清除。
        // Adopting the next-chapter prefetch presents CHAPTER + KB from byte callbacks, then PARSE, cleared at job end.
        adopt_test_uid=27;
        assert(step(0,now++)==WR_JOB_PREFETCH&&downloads==2);
        assert(adopt_test_uid==0);
        weread_stage_t stage=WEREAD_STAGE_CHAPTER;uint64_t bytes=1;
        assert(!weread_reader_stage(&stage,&bytes)&&stage==WEREAD_STAGE_NONE);
        puts("PASS: adopting an in-flight prefetch publishes CHAPTER byte progress and clears the stage at job end");
    }else if(!strcmp(mode,"chapter-cache")){
        const char *uri="weread://window_book";
        unsigned cached=0,total=0;
        assert(weread_reader_chapter_cached(uri,0));
        assert(!weread_reader_chapter_cached(uri,1));
        assert(!weread_reader_chapter_cached(uri,SIZE_MAX));
        assert(!weread_reader_chapter_cached("local.epub",0));
        weread_reader_cached_progress(&cached,&total);
        assert(total==8&&cached==1);
        assert(weread_chapter_download("window_book",27)==ESP_OK);
        assert(weread_reader_chapter_cached(uri,1));
        weread_reader_cached_progress(&cached,&total);
        assert(cached==2);
        assert(weread_reader_chapter_cached(uri,1));
        weread_reader_cached_progress(&cached,&total);
        assert(cached==2);
        assert(!weread_reader_chapter_cached(uri,2));
        // 离开阅读后位图释放；单章探测仍返回有界结果且不启动整书扫描。
        // Leaving the reader releases the bitmap; the probe stays bounded and never starts a full scan.
        uint16_t mc = 0, mt = 0; bool chg = false;
        assert(weread_shelf_set_chapter_counts_mem("window_book", 2, 8, &chg) == ESP_OK);
        assert(weread_shelf_get_chapter_counts("window_book", &mc, &mt) && mc == 2 && mt == 8);
        weread_reader_end(now);
        assert(weread_shelf_load() == ESP_OK && weread_shelf_get_chapter_counts("window_book", &mc, &mt) && mc == 2 && mt == 8);
        assert(weread_reader_chapter_cached(uri,0)&&!weread_reader_chapter_cached(uri,2));
        assert(!weread_reader_chapter_cached("weread://missing_book",0));
        puts("PASS: TOC chapter probe marks bitmap hits while ready and falls back to one bounded check outside the reader");
    }else assert(false);
    weread_reader_end(now);book_weread_close();weread_runtime_stop();return 0;
}
