/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
// Reuse the small encrypted-book fixture setup, exercising the production runtime rather than a reimplemented state machine.
#define main window_fixture_main
#include "test_weread_window.c"
#undef main
#include "test_weread_rescue_backend.h"
#ifdef WEREAD_RUNTIME_TEST_FILE_IO
#undef fopen
extern FILE *fopen(const char *path,const char *mode);
static bool forbid_tf_reads;
static unsigned forbidden_tf_opens;
static const char *watched_tf_root;
FILE *runtime_test_fopen(const char *path,const char *mode){
    if(forbid_tf_reads&&watched_tf_root&&!strncmp(path,watched_tf_root,strlen(watched_tf_root))){++forbidden_tf_opens;errno=EIO;return NULL;}
    return fopen(path,mode);
}
#endif
static unsigned progress_posts;
static unsigned progress_reads,note_jobs,report_jobs,purchase_uid;
static uint32_t cloud_uid=11,cloud_index=1,cloud_offset,cloud_percent;
static unsigned sync_fail_times;
static esp_err_t sync_fail_err=ESP_FAIL;
static atomic_bool note_sync_gate,note_sync_entered,note_sync_release;
static bool clean_guards_shelf;
static unsigned cache_heal_toc_limit;
static uint32_t cache_heal_bump_uid1,cache_heal_bump_ver1,cache_heal_bump_uid2,cache_heal_bump_ver2;
static uint32_t cache_heal_toc_bump_after,cache_heal_toc_bump_ver;
static esp_err_t counted_download(const char *url,const char *headers,FILE *out,size_t limit,weread_cancel_fn cancel,void *cc,weread_download_info_t *info,void *ctx){
    const char *p=strstr(url,"chapters=");assert(p);p+=9;if(!strncmp(p,"0,",2))p+=2;
    unsigned uid=(unsigned)strtoul(p,NULL,10);
    if(cache_heal_bump_uid1&&uid==cache_heal_bump_uid1&&server_version!=cache_heal_bump_ver1){
        server_version=cache_heal_bump_ver1;
        cache_heal_bump_uid1=0;
    }else if(cache_heal_bump_uid2&&uid==cache_heal_bump_uid2&&server_version!=cache_heal_bump_ver2){
        server_version=cache_heal_bump_ver2;
        cache_heal_bump_uid2=0;
    }
    if(strstr(url,"bookId=finished_book")){
        memset(info,0,sizeof(*info));
        char path[1024];snprintf(path,sizeof(path),"%s/finished-11-v100.zip",fixtures);
        FILE *f=fopen(path,"rb");assert(f);
        info->status=200;strcpy(info->encrypt_key,"pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
        char buf[97];size_t n;while((n=fread(buf,1,sizeof(buf),f))){assert(n<=limit-info->bytes&&fwrite(buf,1,n,out)==n);info->bytes+=n;}
        fclose(f);return ESP_OK;
    }
    if(purchase_uid&&uid==purchase_uid){
        memset(info,0,sizeof(*info));
        const char *msg="{\"errcode\":-1,\"errmsg\":\"pay\"}";
        info->status=402;info->bytes=strlen(msg);
        assert(fwrite(msg,1,info->bytes,out)==info->bytes);
        return ESP_OK;
    }
    return download(url,headers,out,limit,cancel,cc,info,ctx);
}
static esp_err_t counted_request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    if(strstr(url,"/shelf/syncbook")){
        if(atomic_load(&note_sync_gate)){
            atomic_store(&note_sync_entered,true);
            while(!atomic_load(&note_sync_release))usleep(1000);
        }
        ++progress_reads;
        if(sync_fail_times){--sync_fail_times;*out=NULL;*len=0;*status=0;return sync_fail_err;}
        char text[1024];
        if(clean_guards_shelf&&body&&strstr(body,"finished_book")){
            snprintf(text,sizeof(text),"{\"books\":[{\"bookId\":\"window_book\",\"title\":\"Window\",\"format\":\"epub\",\"bookVersion\":%u},{\"bookId\":\"finished_book\",\"title\":\"Finished\",\"format\":\"epub\",\"bookVersion\":%u}],\"bookProgress\":[{\"bookId\":\"window_book\",\"chapterUid\":11,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":100},{\"bookId\":\"finished_book\",\"chapterUid\":11,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":100}]}",server_version,server_version);
        }else{
            snprintf(text,sizeof(text),"{\"books\":[{\"bookId\":\"window_book\",\"bookVersion\":%u}],\"bookProgress\":[{\"bookId\":\"window_book\",\"chapterUid\":%u,\"chapterIdx\":%u,\"chapterOffset\":%u,\"progress\":%u}]}",server_version,cloud_uid,cloud_index,cloud_offset,cloud_percent);
        }
        *out=strdup(text);assert(*out);*len=strlen(text);*status=200;return ESP_OK;
    }
    if(cache_heal_toc_limit&&strstr(url,"chapterInfos")&&(!body||!strstr(body,"finished_book"))){
        ++toc_requests;
        if(cache_heal_toc_bump_after&&toc_requests>=cache_heal_toc_bump_after){
            server_version=cache_heal_toc_bump_ver;
            cache_heal_toc_bump_after=0;
        }
        char text[2048];
        size_t used=(size_t)snprintf(text,sizeof(text),"{\"data\":[{\"bookId\":\"window_book\",\"bookVersion\":%u,\"synckey\":1,\"clearAll\":1,\"updated\":[",server_version);
        for(unsigned i=0;i<cache_heal_toc_limit;++i){
            used+=(size_t)snprintf(text+used,sizeof(text)-used,"%s{\"chapterUid\":%u,\"chapterIdx\":%u,\"title\":\"UID%u-v%u\"}",i?",":"",uids[i],i+1,uids[i],server_version);
        }
        snprintf(text+used,sizeof(text)-used,"]}]}");
        *out=strdup(text);assert(*out);*len=strlen(text);*status=200;return ESP_OK;
    }
    if(clean_guards_shelf&&strstr(url,"chapterInfos")&&body&&strstr(body,"finished_book")){
        const char *text="{\"data\":[{\"bookId\":\"finished_book\",\"bookVersion\":100,\"synckey\":1,\"clearAll\":1,\"updated\":[{\"chapterUid\":11,\"chapterIdx\":1,\"title\":\"F11\"}]}]}";
        *out=strdup(text);assert(*out);*len=strlen(text);*status=200;return ESP_OK;
    }
    if(clean_guards_shelf&&strstr(url,"/shelf/sync")){
        const char *text="{\"synckey\":2,\"books\":[{\"bookId\":\"window_book\",\"title\":\"Window\",\"format\":\"epub\",\"bookVersion\":100},{\"bookId\":\"finished_book\",\"title\":\"Finished\",\"format\":\"epub\",\"bookVersion\":100}],\"bookProgress\":[{\"bookId\":\"window_book\",\"chapterUid\":11,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":100},{\"bookId\":\"finished_book\",\"chapterUid\":11,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":100}]}";
        *out=strdup(text);assert(*out);*len=strlen(text);*status=200;return ESP_OK;
    }
    if(strstr(url,"/book/read")){
        ++progress_posts;cJSON *json=cJSON_Parse(body);assert(json);
        cloud_uid=(uint32_t)cJSON_GetObjectItemCaseSensitive(json,"chapterUid")->valuedouble;
        cloud_index=(uint32_t)cJSON_GetObjectItemCaseSensitive(json,"chapterIdx")->valuedouble;
        cloud_offset=(uint32_t)cJSON_GetObjectItemCaseSensitive(json,"chapterOffset")->valuedouble;
        cloud_percent=(uint32_t)cJSON_GetObjectItemCaseSensitive(json,"progress")->valuedouble;cJSON_Delete(json);
    }
    return request(url,method,headers,body,out,len,status,ctx);
}
static weread_job_t observe_result(void){
    uint64_t until=millis()+5000;while(weread_runtime_busy()){assert(millis()<until);usleep(1000);}
    weread_result_t r;if(!weread_runtime_poll(&r))return WR_JOB_INIT;
    if(r.job==WR_JOB_NOTE)++note_jobs;
    if(r.job==WR_JOB_REPORT)++report_jobs;
    weread_job_t job=r.job;weread_runtime_result_free(&r);return job;
}
static void reading_tick(size_t chapter,uint32_t offset,uint64_t now){
    weread_reader_tick("weread://window_book",chapter,offset,17,"fixture",now,true);
    (void)observe_result();
}
static void local_navigation(size_t chapter,uint32_t offset,uint64_t now,uint32_t version){
    weread_reader_navigated_version("weread://window_book",chapter,offset,17,"fixture",version);
    reading_tick(chapter,offset,now);
    assert(!weread_reader_pending_save());
}
static void no_report(void){assert(!note_jobs&&!report_jobs&&!progress_posts&&!progress_reads);}
static void adopt(size_t chapter,html_text_t *text,uint32_t *version){
    assert(weread_chapter_download("window_book",book_weread_uid(chapter))==ESP_OK);
    assert(book_weread_load(chapter,NULL,NULL,text)==ESP_OK&&text->len>10);
    size_t actual=chapter;assert(book_weread_commit_loaded(&actual,version)==ESP_OK&&actual==chapter);
}
static void test_throttle(const char *mode,html_text_t *first){
    if(!strcmp(mode,"throttle-time")){
        assert(weread_chapter_download("window_book",27)==ESP_OK);
        weread_reader_cache_forward_set(false);
        local_navigation(0,4,2000,100);no_report();
        for(uint64_t now=3000;now<61000;now+=1000){
            reading_tick(0,4,now);assert(!weread_runtime_idle(now));no_report();
        }
        reading_tick(0,4,61000);reading_tick(0,4,61001);
        assert(note_jobs==1&&!report_jobs&&progress_posts==1&&progress_reads==1);
        reading_tick(0,4,62000);assert(note_jobs==1&&progress_posts==1);
    }else if(!strcmp(mode,"throttle-pause-online")){
        assert(weread_runtime_connect(NULL)==ESP_OK&&weread_runtime_network_active());
        assert(weread_chapter_download("window_book",27)==ESP_OK);
        weread_reader_cache_forward_set(false);
        local_navigation(0,4,2000,100);no_report();
        reading_tick(0,4,9999);no_report();
        reading_tick(0,4,10000);
        assert(note_jobs==1&&!report_jobs&&progress_posts==1);
        local_navigation(0,5,11000,100);
        reading_tick(0,5,19000);
        assert(note_jobs==1&&progress_posts==1);
        reading_tick(0,5,25000);
        assert(note_jobs==2&&!report_jobs&&progress_posts==2);
    }else if(!strcmp(mode,"throttle-pause-offline")){
        assert(weread_chapter_download("window_book",27)==ESP_OK);
        weread_reader_cache_forward_set(false);
        local_navigation(0,4,2000,100);no_report();
        for(uint64_t now=3000;now<60000;now+=1000){
            reading_tick(0,4,now);no_report();
        }
        assert(!weread_runtime_network_active());
    }else if(!strcmp(mode,"exit-note-online")){
        assert(weread_runtime_connect(NULL)==ESP_OK);
        local_navigation(0,4,2000,100);no_report();
        weread_reader_end(2500);book_weread_close();
        assert(observe_result()==WR_JOB_NOTE);
        assert(note_jobs==1&&!report_jobs&&progress_posts==1);
        weread_progress_t p;assert(weread_progress_load("window_book",&p)==ESP_OK&&!p.dirty&&p.chapter_offset==cloud_offset);
        weread_runtime_stop();html_text_free(first);
        printf("PASS actual runtime %s NOTE=%u POST=%u\n",mode,note_jobs,progress_posts);
        return;
    }else if(!strcmp(mode,"cloud-turn-rebase")){
        assert(!weread_runtime_reader_has_turned("weread://window_book"));
        local_navigation(0,4,2000,100);
        assert(weread_runtime_reader_has_turned("weread://window_book"));
        cloud_uid=27;cloud_index=2;cloud_offset=20;cloud_percent=40;
        assert(weread_runtime_start(WR_JOB_CLOUD_SYNC,"window_book",NULL));
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        size_t jch=99;uint32_t joff=99;
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==1);
        assert(weread_runtime_cloud_jump_rebase("weread://window_book")==ESP_OK);
        assert(!weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff));
        weread_progress_t p;assert(weread_progress_load("window_book",&p)==ESP_OK&&p.chapter_uid==11&&
            p.base_known&&p.base_uid==27&&p.base_index==2&&p.base_offset==20&&p.dirty&&p.position_pending);
        assert(weread_runtime_start(WR_JOB_NOTE,"window_book",NULL));
        assert(observe_result()==WR_JOB_NOTE);
        assert(weread_progress_load("window_book",&p)==ESP_OK&&!p.dirty&&p.chapter_uid==11&&cloud_uid==11);
        cloud_uid=50;cloud_index=3;cloud_offset=10;cloud_percent=60;
        assert(weread_runtime_start(WR_JOB_CLOUD_SYNC,"window_book",NULL));
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==2);
        assert(weread_runtime_cloud_jump_rebase("weread://window_book")==ESP_OK);
        weread_reader_commit_cloud_jump("weread://window_book",true);
        assert(weread_progress_load("window_book",&p)==ESP_OK&&p.chapter_uid==50&&!p.dirty);
        weread_reader_end(3000);book_weread_close();weread_runtime_stop();html_text_free(first);
        printf("PASS actual runtime %s\n",mode);
        return;
    }else if(!strcmp(mode,"throttle-pages")||!strcmp(mode,"throttle-conflict")){
        for(unsigned page=1;page<=9;++page){
            local_navigation(0,page,1000+page*1000,100);assert(!weread_runtime_idle(1000+page*1000));no_report();
        }
        if(!strcmp(mode,"throttle-conflict"))cloud_offset=2;
        local_navigation(0,10,11000,100);
        // 300ms先预取下一章，进度上报让位到预取完成且满1s后。/ Next-chapter prefetch leads at 300ms; progress reporting follows once prefetch is ready and 1s elapsed.
        reading_tick(0,10,11350);no_report();
        reading_tick(0,10,12001);
        assert(note_jobs==1&&!report_jobs&&progress_reads==1);
        if(!strcmp(mode,"throttle-conflict")){
            assert(progress_posts==1&&weread_reader_state()==WR_PROGRESS_SYNCED);
        }else{
            assert(progress_posts==1);reading_tick(0,10,13000);assert(note_jobs==1&&progress_posts==1);
        }
    }else if(!strcmp(mode,"throttle-chapter")){
        local_navigation(0,4,2000,100);no_report();html_text_t next={0};uint32_t version=0;
        adopt(1,&next,&version);local_navigation(1,4,3000,version);reading_tick(1,4,3350);reading_tick(1,4,4001);
        assert(note_jobs==1&&!report_jobs&&progress_posts==1&&cloud_uid==27&&cloud_index==2);
        html_text_free(&next);adopt(2,&next,&version);local_navigation(2,4,5000,version);reading_tick(2,4,5350);reading_tick(2,4,6001);
        // Each successful chapter handoff is urgent, including shortly after a successful previous ACK.
        assert(note_jobs==2&&!report_jobs&&progress_posts==2&&cloud_uid==50&&cloud_index==3);
        html_text_free(&next);
    }else{
        assert(!strcmp(mode,"throttle-closed"));local_navigation(0,4,2000,100);assert(!weread_runtime_idle(2500));no_report();
        weread_reader_end(2600);book_weread_close();assert(weread_runtime_idle(3000));assert(observe_result()==WR_JOB_REPORT);
        assert(report_jobs==1&&!note_jobs&&progress_posts==1);
        weread_progress_t p;assert(weread_progress_load("window_book",&p)==ESP_OK&&!p.dirty&&p.chapter_offset==cloud_offset);
    }
    printf("PASS actual runtime %s NOTE=%u REPORT=%u cloud-GET=%u POST=%u\n",mode,note_jobs,report_jobs,progress_reads,progress_posts);
    weread_reader_end(160000);book_weread_close();weread_runtime_stop();html_text_free(first);
}

int main(int argc,char **argv){
    assert(argc==4);fixtures=argv[2];const char *mode=argv[3];bool rescue_mode=!strncmp(mode,"rescue-",7);
    if(rescue_mode){
        assert(getenv("WEREAD_RESCUE_FIXTURE"));snprintf(rescue_root,sizeof(rescue_root),"%s",getenv("WEREAD_RESCUE_FIXTURE"));
        weread_progress_rescue_backend_t backend={read_slot,write_slot,erase_slot,fixture_card,NULL};weread_progress_rescue_backend(&backend);
    }
    init(argv[1]);
    weread_client_set_transport(counted_request,NULL);
    weread_client_set_download_transport(counted_download,NULL);
    html_text_t text={0};assert(book_weread_load(0,NULL,NULL,&text)==ESP_OK&&text.len>8);
    size_t chapter=0;uint32_t actual_version=0;assert(book_weread_commit_loaded(&chapter,&actual_version)==ESP_OK&&chapter==0&&actual_version==100);
    uint32_t raw=0;assert(book_weread_map(0,true,4,&raw)==ESP_OK);
    if(!strncmp(mode,"throttle-",9)||!strcmp(mode,"exit-note-online")||!strcmp(mode,"cloud-turn-rebase")){test_throttle(mode,&text);return 0;}
    if(!strcmp(mode,"cloud-sync-priority-wake")){
        weread_reader_end(1500);book_weread_close();
        // 1. Cached chapter open ("open local-fast") runs WR_JOB_CLOUD_SYNC before WR_JOB_PREFETCH("1") and scan.
        cloud_uid=50;cloud_index=3;cloud_offset=0;cloud_percent=38;
        assert(weread_runtime_start(WR_JOB_OPEN,"window_book",NULL));
        assert(observe_result()==WR_JOB_OPEN&&progress_reads==0);
        assert(book_weread_open("weread://window_book")==ESP_OK);
        size_t rch=99;uint32_t rso=99;
        assert(weread_reader_resume_chapter("weread://window_book",&rch,&rso)==ESP_OK&&rch==0);
        weread_reader_begin("weread://window_book",2000);
        weread_reader_tick("weread://window_book",0,0,1,"start",2000,true);
        weread_reader_tick("weread://window_book",0,0,1,"start",2300,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC&&progress_reads==1);
        size_t jch=99;uint32_t joff=99;
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==2);
        weread_reader_commit_cloud_jump("weread://window_book",true);
        weread_reader_tick("weread://window_book",2,0,38,"ch2",2350,false);

        // 2. Transient ESP_ERR_TIMEOUT on WR_JOB_CLOUD_SYNC silently replays once inside perform_job without setting WEREAD_STAGE_RETRY.
        cloud_uid=103;cloud_index=4;cloud_offset=0;cloud_percent=50;
        unsigned reads_before=progress_reads;
        sync_fail_times=1;sync_fail_err=ESP_ERR_TIMEOUT;
        weread_reader_wake(3000);
        weread_reader_tick("weread://window_book",2,0,38,"ch2",3300,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC&&progress_reads==reads_before+2&&sync_fail_times==0);
        weread_stage_t st=WEREAD_STAGE_RETRY;uint64_t bytes=0;
        assert(!weread_reader_stage(&st,&bytes)&&st==WEREAD_STAGE_NONE);
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==3);
        weread_reader_commit_cloud_jump("weread://window_book",true);
        weread_reader_tick("weread://window_book",3,0,50,"ch3",3350,false);

        // 3. Bounded backoff retry (+5s, +10s, +15s, max 3 retries) and clean stop offline.
        cloud_uid=404;cloud_index=5;cloud_offset=0;cloud_percent=63;
        reads_before=progress_reads;sync_fail_times=2;sync_fail_err=ESP_FAIL;
        weread_reader_wake(10000);
        weread_reader_tick("weread://window_book",3,0,50,"ch3",10300,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC&&progress_reads==reads_before+1);
        assert(!weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff));
        weread_reader_tick("weread://window_book",3,0,50,"ch3",15200,true);
        assert(observe_result()!=WR_JOB_CLOUD_SYNC&&progress_reads==reads_before+1);
        weread_reader_tick("weread://window_book",3,0,50,"ch3",15300,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC&&progress_reads==reads_before+2);
        weread_reader_tick("weread://window_book",3,0,50,"ch3",25200,true);
        assert(observe_result()!=WR_JOB_CLOUD_SYNC&&progress_reads==reads_before+2);
        weread_reader_tick("weread://window_book",3,0,50,"ch3",25300,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC&&progress_reads==reads_before+3);
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==4);
        weread_reader_commit_cloud_jump("weread://window_book",true);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",25350,false);

        cloud_uid=777;cloud_index=6;cloud_offset=0;cloud_percent=75;
        reads_before=progress_reads;sync_fail_times=10;sync_fail_err=ESP_FAIL;
        weread_reader_wake(30000);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",30300,true);assert(observe_result()==WR_JOB_CLOUD_SYNC);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",35300,true);assert(observe_result()==WR_JOB_CLOUD_SYNC);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",45300,true);assert(observe_result()==WR_JOB_CLOUD_SYNC);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",60300,true);assert(observe_result()==WR_JOB_CLOUD_SYNC);
        assert(progress_reads==reads_before+4);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",120000,true);
        assert(observe_result()!=WR_JOB_CLOUD_SYNC&&progress_reads==reads_before+4);
        sync_fail_times=0;

        // 4. Cold resume via weread_reader_resume_chapter() (deep-sleep / boot path) triggers WR_JOB_CLOUD_SYNC.
        weread_reader_end(121000);book_weread_close();weread_runtime_stop();
        cloud_uid=900;cloud_index=7;cloud_offset=0;cloud_percent=88;
        assert(weread_reader_prepare_open("weread://window_book")==ESP_OK);
        assert(book_weread_open("weread://window_book")==ESP_OK);
        rch=99;rso=99;
        assert(weread_reader_resume_chapter("weread://window_book",&rch,&rso)==ESP_OK&&rch==4);
        weread_reader_begin("weread://window_book",122000);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",122000,true);
        weread_reader_tick("weread://window_book",4,0,63,"ch4",122300,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==6);
        weread_reader_commit_cloud_jump("weread://window_book",true);

        // 5. Light-sleep flush(true) + wake(): auto-jumps when idle after wake, or rebases with WR-UX-04 if user turns first.
        html_text_t c6={0};uint32_t v6=0;adopt(6,&c6,&v6);
        weread_reader_tick("weread://window_book",6,4,88,c6.utf8+4,129000,false);
        weread_reader_navigated_version("weread://window_book",6,4,88,c6.utf8+4,v6);
        assert(weread_runtime_reader_has_turned("weread://window_book"));
        weread_reader_flush(130000,true);
        cloud_uid=1201;cloud_index=8;cloud_offset=0;cloud_percent=100;
        weread_reader_wake(140000);
        weread_reader_tick("weread://window_book",6,6,89,c6.utf8+6,140050,false);
        weread_reader_navigated_version("weread://window_book",6,6,89,c6.utf8+6,v6);
        assert(weread_runtime_reader_has_turned("weread://window_book"));
        weread_reader_tick("weread://window_book",6,6,89,c6.utf8+6,140400,true);
        assert(!weread_reader_pending_save());
        weread_reader_tick("weread://window_book",6,6,89,c6.utf8+6,140450,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==7);
        assert(weread_runtime_cloud_jump_rebase("weread://window_book")==ESP_OK);

        weread_reader_flush(145000,true);
        weread_reader_wake(150000);
        assert(!weread_runtime_reader_has_turned("weread://window_book"));
        weread_reader_tick("weread://window_book",6,6,89,c6.utf8+6,150300,true);
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==7);
        weread_reader_commit_cloud_jump("weread://window_book",true);
        html_text_free(&c6);
        weread_reader_end(160000);book_weread_close();weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime cloud-sync-priority-wake");
        return 0;
    }
    if(!strcmp(mode,"m2-regressions")){
        // 1. weread_runtime_cloud_jump_rebase() save failure: no tick spin and s_cloud_jump.rebased == false
        local_navigation(0,4,2000,100);
        cloud_uid=27;cloud_index=2;cloud_offset=20;cloud_percent=40;
        assert(weread_runtime_start(WR_JOB_CLOUD_SYNC,"window_book",NULL));
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        size_t jch=99;uint32_t joff=99;
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==1);
        weread_reader_navigated_version("weread://window_book",0,5,18,"unsaved",100);
        assert(weread_reader_pending_save());
        char blk[1024];snprintf(blk,sizeof(blk),"%s/cache/window_book/progress.json.tmp",argv[1]);
        assert(!mkdir(blk,0700));
        assert(weread_runtime_cloud_jump_rebase("weread://window_book")!=ESP_OK);
        assert(!weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff));
        weread_reader_commit_cloud_jump("weread://window_book",true);
        weread_progress_t p;assert(weread_progress_load("window_book",&p)==ESP_OK&&p.chapter_uid==11);
        assert(!rmdir(blk));
        assert(weread_reader_save(3000)==ESP_OK);
        weread_reader_end(3000);book_weread_close();
        (void)observe_result();
        // 2. WR_JOB_CACHE_BOOK mid-job -2063 + 402 recounts actual v101 cached chapters
        assert(weread_chapter_download("window_book",27)==ESP_OK);
        assert(weread_chapter_cached("window_book",11)&&weread_chapter_cached("window_book",27));
        server_version=101;
        purchase_uid=103;
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        weread_result_t cr={0};
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==WEREAD_ERR_PURCHASE&&
               cr.shelf_done==3&&cr.shelf_total==8);
        weread_runtime_result_free(&cr);
        purchase_uid=0;
        assert(weread_chapter_cached("window_book",11)&&weread_chapter_cached("window_book",900)&&
               weread_chapter_cached("window_book",50)&&!weread_chapter_cached("window_book",27));
        weread_shelf_item_t *items=NULL;size_t cnt=0;
        assert(weread_shelf_snapshot(&items,&cnt)==ESP_OK&&cnt==1&&
               items[0].cached_chapters==3&&items[0].total_chapters==8);
        free(items);
        // 3. choose == true (WR_JOB_CLOUD) post-heal updates s_result.local.book_version
        cloud_uid=11;cloud_index=1;cloud_offset=0;cloud_percent=0;
        server_version=100;
        assert(weread_shelf_refresh_book("window_book")==ESP_OK&&weread_toc_sync_version("window_book",100)==ESP_OK);
        assert(weread_progress_note("window_book",11,1,0,10,0,1,"pre-open",1700000150)==ESP_OK);
        assert(weread_chapter_download("window_book",11)==ESP_OK);
        assert(weread_runtime_start(WR_JOB_OPEN,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.error==ESP_OK&&cr.open_ready);
        weread_runtime_result_free(&cr);
        char mf[512];assert(wr_cache_path("window_book","chapter-11.json",mf,sizeof(mf))==ESP_OK&&!unlink(mf));
        server_version=101;
        assert(weread_runtime_start(WR_JOB_CLOUD,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CLOUD&&cr.error==ESP_OK&&cr.open_ready&&
               cr.local.book_version==101&&!cr.local.sync_conflict);
        weread_runtime_result_free(&cr);
        assert(weread_progress_load("window_book",&p)==ESP_OK&&p.book_version==101);
        weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime m2-regressions");
        return 0;
    }
    if(!strcmp(mode,"shelf-cache-ops")){
        weread_reader_end(1500);book_weread_close();
        char gray[512],img[512],prog_p[512];
        assert(weread_cover_cache_path("window_book","gray",gray,sizeof(gray))==ESP_OK);
        assert(wr_write_bytes(gray,"GRAY",4)==ESP_OK);
        assert(weread_cover_cache_path("window_book","img",img,sizeof(img))==ESP_OK);
        assert(wr_write_bytes(img,"IMG",3)==ESP_OK);
        const char *probe_ids[]={"window_book","missing_book"};
        assert(weread_runtime_probe_shelf(probe_ids,2));
        assert(observe_result()==WR_JOB_PROBE_SHELF);
        weread_shelf_item_t *items=NULL;size_t cnt=0;
        assert(weread_shelf_snapshot(&items,&cnt)==ESP_OK&&cnt==1&&items[0].cached_chapters==1&&items[0].total_chapters==8);
        free(items);
        // Preemption mid-cache_book retains partial counts
        atomic_store(&gated,true);atomic_store(&entered,false);
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        uint64_t until=millis()+3000;while(!atomic_load(&entered)){assert(millis()<until);usleep(1000);}
        weread_shelf_cache_status_t st={0};
        assert(weread_runtime_shelf_cache_status(&st)&&st.active&&!strcmp(st.book_id,"window_book")&&st.total_chapters==8);
        weread_runtime_cancel();
        assert(observe_result()==WR_JOB_CACHE_BOOK);
        assert(!weread_runtime_shelf_cache_status(&st));
        atomic_store(&gated,false);
        // 104 MiB storage guard stops WR_JOB_CACHE_BOOK cleanly with storage_blocked
        space=10u*1024u*1024u;
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        weread_result_t cr={0};assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==WEREAD_ERR_STORAGE);
        weread_runtime_result_free(&cr);
        assert(!weread_runtime_shelf_cache_status(&st)&&st.storage_blocked);
        space=512u*1024u*1024u;
        // Full WR_JOB_CACHE_BOOK downloads remaining chapters and updates shelf counts
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==ESP_OK&&cr.shelf_done==8&&cr.shelf_total==8);
        weread_runtime_result_free(&cr);
        assert(weread_shelf_snapshot(&items,&cnt)==ESP_OK&&cnt==1&&items[0].cached_chapters==8&&items[0].total_chapters==8);
        free(items);
        // WR_JOB_CACHE_STATS populates zero-IO stats
        assert(weread_runtime_start(WR_JOB_CACHE_STATS,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_STATS);
        weread_runtime_cache_stats_t cstats={0};
        assert(weread_runtime_cache_stats(&cstats)&&cstats.valid&&cstats.books_with_chapters==1&&cstats.reclaimable_bytes>0);
        // WR_JOB_CACHE_CLEAN_FINISHED skips <100% progress, cleans >=100% preserving cover-*.gray and progress.json
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_FINISHED,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_CLEAN_FINISHED);
        assert(weread_chapter_cached("window_book",11));
        assert(weread_progress_note("window_book",11,1,4,100,1,1,"done",1700000200)==ESP_OK);
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_FINISHED,NULL,NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_CLEAN_FINISHED&&cr.error==ESP_OK&&cr.shelf_done==1);
        weread_runtime_result_free(&cr);
        assert(!weread_chapter_cached("window_book",11)&&!access(gray,F_OK)&&access(img,F_OK)!=0);
        assert(wr_cache_path("window_book","progress.json",prog_p,sizeof(prog_p))==ESP_OK&&!access(prog_p,F_OK));
        assert(weread_shelf_snapshot(&items,&cnt)==ESP_OK&&cnt==1&&items[0].cached_chapters==0&&items[0].total_chapters==8);
        free(items);
        // Re-download one chapter, verify WR_JOB_CLEAR selective cleanup & WR_JOB_CACHE_CLEAN_ALL
        assert(weread_chapter_download("window_book",11)==ESP_OK);
        assert(weread_runtime_start(WR_JOB_CLEAR,"window_book",NULL));
        assert(observe_result()==WR_JOB_CLEAR);
        assert(!weread_chapter_cached("window_book",11)&&!access(gray,F_OK)&&!access(prog_p,F_OK));
        assert(weread_chapter_download("window_book",11)==ESP_OK);
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_CLEAN_ALL);
        assert(!weread_chapter_cached("window_book",11)&&!access(gray,F_OK)&&!access(prog_p,F_OK));
        weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime shelf-cache-ops");
        return 0;
    }
    if(!strcmp(mode,"ux04-rebase-race")){
        uint32_t raw4=0;assert(book_weread_map(0,true,4,&raw4)==ESP_OK);
        local_navigation(0,4,2000,100);
        cloud_uid=27;cloud_index=2;cloud_offset=20;cloud_percent=40;
        assert(weread_runtime_start(WR_JOB_CLOUD_SYNC,"window_book",NULL));
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        size_t jch=99;uint32_t joff=99;
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==1);
        atomic_store(&note_sync_entered,false);
        atomic_store(&note_sync_release,false);
        atomic_store(&note_sync_gate,true);
        assert(weread_runtime_start(WR_JOB_NOTE,"window_book",NULL));
        uint64_t until=millis()+3000;
        while(!atomic_load(&note_sync_entered)){assert(millis()<until);usleep(1000);}
        assert(weread_runtime_cloud_jump_rebase("weread://window_book")==ESP_OK);
        atomic_store(&note_sync_release,true);
        assert(observe_result()==WR_JOB_NOTE);
        atomic_store(&note_sync_gate,false);
        weread_progress_t p;
        assert(weread_progress_load("window_book",&p)==ESP_OK&&p.chapter_uid==11&&
               p.chapter_offset==raw4&&p.dirty&&p.base_known&&p.base_uid==27&&progress_posts==0);
        assert(weread_runtime_start(WR_JOB_NOTE,"window_book",NULL));
        assert(observe_result()==WR_JOB_NOTE);
        assert(weread_progress_load("window_book",&p)==ESP_OK&&!p.dirty&&p.chapter_uid==11&&cloud_uid==11&&progress_posts==1);
        weread_reader_end(3000);book_weread_close();weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime ux04-rebase-race");
        return 0;
    }
    if(!strcmp(mode,"ux04-auto-undo-note")){
        assert(!weread_runtime_reader_has_turned("weread://window_book"));
        cloud_uid=27;cloud_index=2;cloud_offset=20;cloud_percent=40;
        assert(weread_runtime_start(WR_JOB_CLOUD_SYNC,"window_book",NULL));
        assert(observe_result()==WR_JOB_CLOUD_SYNC);
        size_t jch=99;uint32_t joff=99;
        assert(weread_reader_poll_cloud_jump("weread://window_book",&jch,&joff)&&jch==1);
        weread_reader_commit_cloud_jump("weread://window_book",true);
        weread_progress_t p;
        assert(weread_progress_load("window_book",&p)==ESP_OK&&p.chapter_uid==27&&!p.dirty);
        local_navigation(0,4,2500,100);
        assert(weread_progress_load("window_book",&p)==ESP_OK&&p.chapter_uid==11&&p.base_known&&p.base_uid==27&&p.dirty&&p.position_pending);
        assert(weread_runtime_start(WR_JOB_NOTE,"window_book",NULL));
        assert(observe_result()==WR_JOB_NOTE);
        assert(weread_progress_load("window_book",&p)==ESP_OK&&!p.dirty&&p.chapter_uid==11&&cloud_uid==11&&progress_posts==1);
        weread_reader_end(3000);book_weread_close();weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime ux04-auto-undo-note");
        return 0;
    }
    if(!strcmp(mode,"ux06-end-while-prefetch")){
        assert(weread_runtime_connect(NULL)==ESP_OK);
        local_navigation(0,4,2000,100);
        weread_progress_t p;assert(weread_progress_load("window_book",&p)==ESP_OK&&p.dirty&&p.chapter_uid==11);
        atomic_store(&gated,true);atomic_store(&entered,false);
        weread_reader_tick("weread://window_book",0,4,17,"fixture",2400,true);
        uint64_t until=millis()+3000;while(!atomic_load(&entered)){assert(millis()<until);usleep(1000);}
        uint64_t t0=millis();
        weread_reader_end(2500);
        assert(millis()-t0<500);
        assert(observe_result()==WR_JOB_PREFETCH);
        atomic_store(&gated,false);
        assert(!note_jobs&&!progress_posts);
        assert(weread_progress_load("window_book",&p)==ESP_OK&&p.dirty&&p.chapter_uid==11);
        book_weread_close();weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime ux06-end-while-prefetch");
        return 0;
    }
    if(!strcmp(mode,"ux09-clean-guards")){
        clean_guards_shelf=true;
        assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("finished_book")==ESP_OK);
        assert(weread_chapter_download("finished_book",11)==ESP_OK);
        assert(weread_chapter_cached("window_book",11)&&weread_chapter_cached("finished_book",11));
        assert(weread_progress_note("window_book",11,1,4,5,1,1,"reread",1700000200)==ESP_OK);
        assert(weread_progress_note("finished_book",11,1,4,100,1,1,"done",1700000200)==ESP_OK);
        assert(book_weread_id_in_use("window_book")&&!book_weread_id_in_use("finished_book"));
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_FINISHED,NULL,NULL));
        while(weread_runtime_busy())usleep(1000);
        weread_result_t cr={0};
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_CLEAN_FINISHED&&cr.error==ESP_OK&&cr.shelf_done==1);
        weread_runtime_result_free(&cr);
        assert(weread_chapter_cached("window_book",11)&&!weread_chapter_cached("finished_book",11));
        assert(weread_progress_note("window_book",11,1,4,100,1,1,"open-done",1700000300)==ESP_OK);
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_FINISHED,NULL,NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_CLEAN_FINISHED&&cr.error==ESP_OK&&cr.shelf_done==0);
        weread_runtime_result_free(&cr);
        assert(weread_chapter_cached("window_book",11));
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL,NULL,NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_CLEAN_ALL&&cr.error==ESP_OK&&cr.shelf_done==0);
        weread_runtime_result_free(&cr);
        assert(weread_chapter_cached("window_book",11));
        weread_reader_end(3000);book_weread_close();
        assert(weread_progress_note("window_book",11,1,4,5,1,1,"reread-closed",1700000400)==ESP_OK);
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_FINISHED,NULL,NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_CLEAN_FINISHED&&cr.error==ESP_OK&&cr.shelf_done==0);
        weread_runtime_result_free(&cr);
        assert(weread_chapter_cached("window_book",11));
        assert(weread_progress_note("window_book",11,1,4,100,1,1,"done-closed",1700000500)==ESP_OK);
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_FINISHED,NULL,NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_CLEAN_FINISHED&&cr.error==ESP_OK&&cr.shelf_done==1);
        weread_runtime_result_free(&cr);
        assert(!weread_chapter_cached("window_book",11));
        weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime ux09-clean-guards");
        return 0;
    }
    if(!strcmp(mode,"cache-book-2063-heal")){
        weread_reader_end(1500);book_weread_close();
        weread_result_t cr={0};
        weread_shelf_item_t *items=NULL;size_t cnt=0;
        char toc_p[512];

        // 1. Mid-job -2063 -> ESP_OK heal: ch11 & ch27 cached at v100, ch50 bumps server to v101 ->
        //    cache_book_job restarts, reloads v101 TOC, and caches all 3 v101 chapters (3/3).
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_CLEAN_ALL);
        cache_heal_toc_limit=3;
        server_version=100;
        assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("window_book")==ESP_OK);
        assert(weread_chapter_download("window_book",11)==ESP_OK&&weread_chapter_download("window_book",27)==ESP_OK);
        assert(weread_chapter_cached("window_book",11)&&weread_chapter_cached("window_book",27));
        cache_heal_bump_uid1=50;cache_heal_bump_ver1=101;
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==ESP_OK&&cr.shelf_done==3&&cr.shelf_total==3);
        weread_runtime_result_free(&cr);
        assert(weread_shelf_book_version("window_book")==101);
        assert(weread_chapter_cached("window_book",11)&&weread_chapter_cached("window_book",27)&&weread_chapter_cached("window_book",50));
        assert(weread_shelf_snapshot(&items,&cnt)==ESP_OK&&cnt==1&&items[0].cached_chapters==3&&items[0].total_chapters==3);
        free(items);

        // 2. Mid-job -2063 -> ESP_OK heal with newly added TOC chapter in v101 (v100 had {11,27}, v101 adds 50)
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_CLEAN_ALL);
        cache_heal_toc_limit=2;
        server_version=100;
        assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("window_book")==ESP_OK);
        assert(weread_chapter_download("window_book",11)==ESP_OK);
        cache_heal_toc_limit=3;
        cache_heal_bump_uid1=27;cache_heal_bump_ver1=101;
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==ESP_OK&&cr.shelf_done==3&&cr.shelf_total==3);
        weread_runtime_result_free(&cr);
        assert(weread_chapter_cached("window_book",11)&&weread_chapter_cached("window_book",27)&&weread_chapter_cached("window_book",50));

        // 3. Pre-loop missing-TOC v100 -> v101 WEREAD_ERR_VERSION single auto-heal to completion
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_CLEAN_ALL);
        cache_heal_toc_limit=3;
        server_version=100;
        assert(weread_shelf_sync(true)==ESP_OK);
        assert(wr_cache_path("window_book","chapters.json",toc_p,sizeof(toc_p))==ESP_OK);
        (void)unlink(toc_p);
        server_version=101;
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==ESP_OK&&cr.shelf_done==3&&cr.shelf_total==3);
        weread_runtime_result_free(&cr);
        assert(weread_shelf_book_version("window_book")==101);
        assert(weread_chapter_cached("window_book",11)&&weread_chapter_cached("window_book",27)&&weread_chapter_cached("window_book",50));

        // 4a. Bounded abort if pre-loop version keeps changing twice during heal_open_version
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_CLEAN_ALL);
        server_version=100;
        assert(weread_shelf_sync(true)==ESP_OK);
        (void)unlink(toc_p);
        server_version=101;
        toc_requests=0;cache_heal_toc_bump_after=2;cache_heal_toc_bump_ver=102;
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==WEREAD_ERR_VERSION);
        weread_runtime_result_free(&cr);

        // 4b. Bounded abort if mid-job version bumps a second time after healed == true (v100 -> v101 -> v102)
        assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL,NULL,NULL));
        assert(observe_result()==WR_JOB_CACHE_CLEAN_ALL);
        server_version=100;
        assert(weread_shelf_sync(true)==ESP_OK&&weread_toc_sync("window_book")==ESP_OK);
        assert(weread_chapter_download("window_book",11)==ESP_OK&&weread_chapter_download("window_book",27)==ESP_OK);
        cache_heal_bump_uid1=50;cache_heal_bump_ver1=101;
        cache_heal_bump_uid2=11;cache_heal_bump_ver2=102;
        assert(weread_runtime_start(WR_JOB_CACHE_BOOK,"window_book",NULL));
        while(weread_runtime_busy())usleep(1000);
        assert(weread_runtime_poll(&cr)&&cr.job==WR_JOB_CACHE_BOOK&&cr.error==WEREAD_ERR_VERSION&&cr.shelf_done==1&&cr.shelf_total==3);
        weread_runtime_result_free(&cr);
        assert(weread_shelf_book_version("window_book")==102);

        weread_runtime_stop();html_text_free(&text);
        puts("PASS actual runtime cache-book-2063-heal");
        return 0;
    }
    weread_reader_navigated("weread://window_book",0,4,17,text.utf8+4);
    char block[1024];snprintf(block,sizeof(block),"%s/cache/window_book/progress.json.tmp",argv[1]);
    assert(!mkdir(block,0700));
    weread_progress_t old;assert(weread_progress_load("window_book",&old)==ESP_OK);
    if(rescue_mode){
        if(!strncmp(mode,"rescue-eject-large",18)){
            assert(book_weread_map_cache_bytes()==0);unsigned old_downloads=downloads;
#ifdef WEREAD_RUNTIME_TEST_FILE_IO
            watched_tf_root=argv[1];forbid_tf_reads=true;
#endif
            absent_card=true;weread_runtime_media_lost();assert(weread_reader_save_failed()&&weread_reader_pending_save()&&!internal_writes);
#ifdef WEREAD_RUNTIME_TEST_FILE_IO
            forbid_tf_reads=false;assert(!forbidden_tf_opens);
#endif
            book_weread_close();assert(!book_weread_is_open());absent_card=false;
            weread_auth_session_t session;assert(weread_auth_init(argv[1])==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
            if(strstr(mode,"newer")){
                assert(weread_shelf_load()==ESP_OK&&weread_shelf_set_version("window_book",101)==ESP_OK);
                server_version=101;assert(weread_toc_sync_version("window_book",101)==ESP_OK);
            }
            assert(weread_reader_save(2001)==ESP_OK&&!weread_reader_pending_save()&&internal_writes>0);
            assert(!book_weread_is_open()&&downloads==old_downloads);
        }else if(!strcmp(mode,"rescue-wrong-card")){
            assert(book_weread_map_cache_bytes()==0);current_card[0]=2;
#ifdef WEREAD_RUNTIME_TEST_FILE_IO
            watched_tf_root=argv[1];forbid_tf_reads=true;
#endif
            assert(weread_reader_save(2000)==ESP_ERR_INVALID_STATE&&weread_reader_pending_save()&&!internal_writes);
#ifdef WEREAD_RUNTIME_TEST_FILE_IO
            forbid_tf_reads=false;assert(!forbidden_tf_opens);
#endif
            current_card[0]=1;assert(weread_reader_save(2001)==ESP_OK&&!weread_reader_pending_save()&&internal_writes>0);
        }else if(!strcmp(mode,"rescue-logout")){
            internal_fail=true;assert(weread_reader_save(2000)!=ESP_OK&&weread_reader_pending_save());
            absent_card=true;weread_runtime_media_lost();book_weread_close();absent_card=false;
            weread_auth_session_t session;assert(weread_auth_init(argv[1])==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
            assert(!weread_runtime_start(WR_JOB_LOGOUT,NULL,NULL)&&weread_auth_is_authenticated()&&weread_reader_pending_save());
            assert(!weread_runtime_start(WR_JOB_CLEAR,"window_book",NULL)&&!weread_runtime_busy()&&weread_reader_pending_save());
            internal_fail=false;assert(weread_reader_save(2001)==ESP_OK&&!weread_reader_pending_save()&&internal_writes>0);
        }else if(!strcmp(mode,"rescue-internal")){
            internal_fail=true;assert(weread_reader_save(2000)!=ESP_OK&&weread_reader_save_failed()&&weread_reader_pending_save());
            weread_reader_end(2001);book_weread_close();assert(weread_reader_pending_save());internal_fail=false;
            assert(weread_reader_save(2002)==ESP_OK&&!weread_reader_pending_save()&&internal_writes>0);
        }else if(!strcmp(mode,"rescue-eject")){
            absent_card=true;weread_runtime_media_lost();assert(internal_writes>0);absent_card=false;
        }else{
            assert(!strcmp(mode,"rescue-leave"));assert(weread_reader_save(2000)==ESP_OK&&!weread_reader_save_failed()&&!weread_reader_pending_save());
            weread_reader_end(2001);book_weread_close();assert(!weread_reader_pending_save()&&internal_writes>0);
        }
        // media_lost intentionally forgets auth; rebind the original card only after it returns.
        if(!strcmp(mode,"rescue-eject")){
            weread_auth_session_t session;assert(weread_auth_init(argv[1])==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
        }
        weread_progress_t saved;assert(weread_progress_load("window_book",&saved)==ESP_OK&&saved.chapter_offset==raw&&saved.progress==17&&saved.dirty);
        assert(!rmdir(block));esp_err_t reported=weread_progress_outbox_report("window_book",90000,1700000300,&saved);
        if(strstr(mode,"newer"))assert(reported==WEREAD_ERR_VERSION&&saved.book_version==100&&!saved.sync_conflict&&!progress_posts);
        else assert(reported==ESP_OK&&!saved.dirty);
        assert(internal_erases>0);weread_runtime_stop();html_text_free(&text);
        if(strstr(mode,"newer"))printf("PASS actual runtime %s retains exact edition100 raw under metadata101; fresh cloud conflict prevents every POST\n",mode);
        else printf("PASS actual runtime %s retains the verified raw position through TF failure and restores before cloud ACK\n",mode);
        return 0;
    }
    assert(weread_reader_save(2000)!=ESP_OK&&weread_reader_save_failed()&&weread_reader_pending_save());
    weread_reader_end(2001);book_weread_close();html_text_free(&text);
    assert(weread_reader_save_failed()&&weread_reader_pending_save());
    if(!strcmp(mode,"begin")){
        weread_reader_begin("weread://window_book",2100);
        assert(weread_reader_save_failed()&&weread_reader_pending_save());
    }
    assert(!rmdir(block));
    if(!strcmp(mode,"owner")){
        weread_auth_session_t session=*weread_auth_get_current_session();strcpy(session.vid,"999");
        assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
        assert(!weread_runtime_idle(3000)&&weread_reader_save_failed());
        weread_progress_t untouched;assert(weread_progress_load("window_book",&untouched)!=ESP_OK);
        strcpy(session.vid,"123");assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    }else if(!strcmp(mode,"media")){
        char other[1024];snprintf(other,sizeof(other),"%s/other-card",argv[1]);
        weread_auth_session_t session=*weread_auth_get_current_session();assert(weread_auth_init(other)==ESP_OK);
        assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
        assert(!weread_runtime_idle(3000)&&weread_reader_save_failed());
        weread_progress_t untouched;assert(weread_progress_load("window_book",&untouched)==ESP_ERR_NOT_FOUND);
        assert(weread_auth_init(argv[1])==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    }else if(!strcmp(mode,"version")){
        assert(weread_shelf_set_version("window_book",101)==ESP_OK);
        server_version=101;assert(weread_runtime_idle(3000));weread_result_t r=completed();
        assert(r.error==WEREAD_ERR_VERSION);weread_runtime_result_free(&r);
        weread_progress_t saved;assert(weread_progress_load("window_book",&saved)==ESP_OK&&saved.book_version==100&&saved.chapter_offset==raw&&saved.dirty&&!saved.sync_conflict&&!progress_posts);
        assert(!weread_reader_save_failed());weread_reader_end(3001);weread_runtime_stop();
        puts("PASS: retained raw edition100 saves locally under metadata101 without relabel; version mismatch prevents any POST without locking conflict");return 0;
    }else if(!strcmp(mode,"renew")){
        weread_auth_session_t session=*weread_auth_get_current_session();strcpy(session.access_token,"renewed-token");strcpy(session.refresh_token,"renewed-refresh");
        assert(weread_auth_save_session(&session)==ESP_OK&&weread_auth_load_session(&session)==ESP_OK);
    }else assert(!strcmp(mode,"recover")||!strcmp(mode,"begin"));
    assert(weread_runtime_idle(4000));weread_result_t r=completed();assert(r.error==ESP_OK);weread_runtime_result_free(&r);
    weread_progress_t saved;assert(weread_progress_load("window_book",&saved)==ESP_OK&&saved.chapter_uid==11&&saved.chapter_offset==raw&&saved.progress==17&&!saved.dirty);
    assert(!weread_reader_save_failed()&&!weread_reader_pending_save());
    weread_reader_end(4001);weread_runtime_stop();weread_progress_outbox_reset();
    printf("PASS: failed local commit survives end/close and recovers without the original map; %s identity guard\n",mode);
    return 0;
}
