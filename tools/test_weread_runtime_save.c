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
static unsigned progress_reads,note_jobs,report_jobs;
static uint32_t cloud_uid=11,cloud_index=1,cloud_offset,cloud_percent;
static esp_err_t counted_request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    if(strstr(url,"/shelf/syncbook")){
        ++progress_reads;char text[1024];
        snprintf(text,sizeof(text),"{\"books\":[{\"bookId\":\"window_book\",\"bookVersion\":%u}],\"bookProgress\":[{\"bookId\":\"window_book\",\"chapterUid\":%u,\"chapterIdx\":%u,\"chapterOffset\":%u,\"progress\":%u}]}",server_version,cloud_uid,cloud_index,cloud_offset,cloud_percent);
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
        local_navigation(0,4,2000,100);no_report();
        for(uint64_t now=3000;now<61000;now+=1000){
            reading_tick(0,4,now);assert(!weread_runtime_idle(now));no_report();
        }
        reading_tick(0,4,61000);reading_tick(0,4,61001);
        assert(note_jobs==1&&!report_jobs&&progress_posts==1&&progress_reads==1);
        reading_tick(0,4,62000);assert(note_jobs==1&&progress_posts==1);
    }else if(!strcmp(mode,"throttle-pages")||!strcmp(mode,"throttle-conflict")){
        for(unsigned page=1;page<=9;++page){
            local_navigation(0,page,1000+page*1000,100);assert(!weread_runtime_idle(1000+page*1000));no_report();
        }
        if(!strcmp(mode,"throttle-conflict"))cloud_offset=2;
        local_navigation(0,10,11000,100);reading_tick(0,10,12001);
        assert(note_jobs==1&&!report_jobs&&progress_reads==1);
        if(!strcmp(mode,"throttle-conflict")){
            assert(!progress_posts&&weread_reader_state()==WR_PROGRESS_CONFLICT);
            for(uint64_t now=13000;now<150000;now+=1000){reading_tick(0,10,now);assert(!weread_runtime_idle(now));}
            assert(note_jobs==1&&!report_jobs&&progress_reads==1&&!progress_posts&&weread_reader_state()==WR_PROGRESS_CONFLICT);
        }else{
            assert(progress_posts==1);reading_tick(0,10,13000);assert(note_jobs==1&&progress_posts==1);
        }
    }else if(!strcmp(mode,"throttle-chapter")){
        local_navigation(0,4,2000,100);no_report();html_text_t next={0};uint32_t version=0;
        adopt(1,&next,&version);local_navigation(1,4,3000,version);reading_tick(1,4,4001);
        assert(note_jobs==1&&!report_jobs&&progress_posts==1&&cloud_uid==27&&cloud_index==2);
        html_text_free(&next);adopt(2,&next,&version);local_navigation(2,4,5000,version);reading_tick(2,4,6001);
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
    html_text_t text={0};assert(book_weread_load(0,NULL,NULL,&text)==ESP_OK&&text.len>8);
    size_t chapter=0;uint32_t actual_version=0;assert(book_weread_commit_loaded(&chapter,&actual_version)==ESP_OK&&chapter==0&&actual_version==100);
    uint32_t raw=0;assert(book_weread_map(0,true,4,&raw)==ESP_OK);
    if(!strncmp(mode,"throttle-",9)){test_throttle(mode,&text);return 0;}
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
        if(strstr(mode,"newer"))assert(reported==WEREAD_ERR_PROGRESS_CONFLICT&&saved.book_version==100&&saved.sync_conflict&&!progress_posts);
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
        assert(r.error==WEREAD_ERR_PROGRESS_CONFLICT);weread_runtime_result_free(&r);
        weread_progress_t saved;assert(weread_progress_load("window_book",&saved)==ESP_OK&&saved.book_version==100&&saved.chapter_offset==raw&&saved.dirty&&saved.sync_conflict&&!progress_posts);
        assert(!weread_reader_save_failed());weread_reader_end(3001);weread_runtime_stop();
        puts("PASS: retained raw edition100 saves locally under metadata101 without relabel; fresh cloud conflict prevents any POST");return 0;
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
