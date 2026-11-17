/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
// Reuse the small encrypted-book fixture setup, exercising the production runtime rather than a reimplemented state machine.
#define main window_fixture_main
#include "test_weread_window.c"
#undef main
#include "test_weread_rescue_backend.h"
static unsigned progress_posts;
static esp_err_t counted_request(const char *url,const char *method,const char *headers,const char *body,char **out,size_t *len,int *status,void *ctx){
    if(strstr(url,"/book/read"))++progress_posts;
    return request(url,method,headers,body,out,len,status,ctx);
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
    weread_reader_navigated("weread://window_book",0,4,17,text.utf8+4);
    char block[1024];snprintf(block,sizeof(block),"%s/cache/window_book/progress.json.tmp",argv[1]);
    assert(!mkdir(block,0700));
    weread_progress_t old;assert(weread_progress_load("window_book",&old)==ESP_OK);
    if(rescue_mode){
        if(!strcmp(mode,"rescue-internal")){
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
        assert(!rmdir(block));assert(weread_progress_outbox_report("window_book",90000,1700000300,&saved)==ESP_OK&&!saved.dirty);
        assert(internal_erases>0);weread_runtime_stop();html_text_free(&text);
        printf("PASS actual runtime %s retains the verified raw position through TF failure and restores before cloud ACK\n",mode);return 0;
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
