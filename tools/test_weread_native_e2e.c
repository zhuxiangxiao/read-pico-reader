/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：运行生产 C 网络分支和服务，SDK 套接字替身仅连接本地 Mock。
 * English: Run production C networking/services; the SDK socket shim connects only to the local mock.
 */
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_progress.h"
#include "book_source_weread.h"
#include "weread_runtime.h"
#ifdef WEREAD_TEST_MEMORY
#include "weread_memory_test.h"
#endif
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned offline_downloads;
static esp_err_t offline(const char *url, const char *headers, FILE *f, size_t limit,
    weread_cancel_fn fn, void *ctx, weread_download_info_t *info, void *user) {
    (void)url; (void)headers; (void)f; (void)limit; (void)fn; (void)ctx; (void)info; (void)user;
    ++offline_downloads; return ESP_ERR_TIMEOUT;
}
static uint64_t milliseconds(void) {struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+(unsigned long)t.tv_nsec/1000000;}
static weread_result_t completion(void) {
    uint64_t start=milliseconds();
    while(weread_runtime_busy()){assert(milliseconds()-start<15000);usleep(1000);}
    weread_result_t r;assert(weread_runtime_poll(&r));return r;
}
static void control(const char *body) {
    char url[256];snprintf(url,sizeof(url),"%s/mock/control/set_state",weread_client_get_base_url());
    char *resp=NULL;size_t len;int status;
    assert(weread_client_request(url,"POST","Content-Type: application/json\r\n",body,&resp,&len,&status)==ESP_OK&&status==200);free(resp);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT",argv[1],1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    char uid[64], url[256], code[64]; weread_qr_state_t state;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(strstr(url, "https://weread.qq.com/web/confirm?pf=2&uid="));
    assert(weread_auth_poll_qr_login(uid, &state, code, sizeof(code)) == ESP_OK && state == WEREAD_QR_STATE_CONFIRMED);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK && weread_auth_is_authenticated());
    // 自导入封面按原生地址跳到外域时剥离凭据，拒绝降级、循环和超长Location。
    // Native imported covers redirect without leaking credentials; reject downgrade, loops and oversized locations.
    FILE *cover_file=tmpfile();assert(cover_file);weread_download_info_t cover_info={0};
    assert(weread_client_download("https://res.weread.qq.com/mock/cover-native",cover_file,64,NULL,NULL,&cover_info)==ESP_OK&&cover_info.status==200&&cover_info.bytes==8);
    char cover_bytes[8];rewind(cover_file);assert(fread(cover_bytes,1,8,cover_file)==8&&!memcmp(cover_bytes,"\xff\xd8\xff" "cover",8));fclose(cover_file);
    const char *bad_covers[]={"https://res.weread.qq.com/mock/cover-loop","https://res.weread.qq.com/mock/cover-insecure","https://res.weread.qq.com/mock/cover-long"};
    for(size_t i=0;i<sizeof(bad_covers)/sizeof(*bad_covers);++i){cover_file=tmpfile();assert(cover_file);assert(weread_client_download(bad_covers[i],cover_file,64,NULL,NULL,&cover_info)!=ESP_OK);fclose(cover_file);}
    assert(weread_shelf_sync(true) == ESP_OK && weread_shelf_count() == 2);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(book_weread_open("weread://test_book_101") == ESP_OK && book_weread_count() == 2);
    html_text_t text = {0};
    assert(book_weread_load(0,NULL,NULL,&text)==ESP_ERR_NOT_FOUND);
    assert(weread_chapter_download("test_book_101",1)==ESP_OK);
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK);
    assert(text.len && text.count && strstr(text.utf8, "微风吹过荒原") && strstr(text.utf8, "探索者踏上了新的征程"));
    html_text_free(&text);
    weread_progress_t local, cloud; bool conflict;
    assert(weread_progress_sync("test_book_101", &local, &cloud, &conflict) == ESP_OK && !conflict);
    assert(weread_progress_note("test_book_101", 1, 1, 42, 16, 10, 1, "离线前的位置", 1700000100) == ESP_OK);
    assert(weread_progress_report("test_book_101", true, 61000) == ESP_OK);
    assert(weread_progress_load("test_book_101", &local) == ESP_OK && !local.dirty);
    weread_client_set_download_transport(offline, NULL);
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK && !offline_downloads);
    html_text_free(&text); book_weread_close(); weread_shelf_clear();
    weread_client_set_download_transport(NULL,NULL);
    assert(weread_shelf_load() == ESP_OK && weread_shelf_count() == 2);
    // 页面真实任务链：快照、冲突、开书、读秒、强制存盘与关闭射频。/ Exercise real page jobs, choices, time accounting, persistence and shutdown.
    weread_runtime_init();assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));
    weread_result_t result=completion();assert(result.error==ESP_OK&&result.authenticated&&result.count==2);weread_runtime_result_free(&result);
    // 首次未读与显式选择空云端进度都从真实首章开始。/ Unread and explicitly chosen empty cloud positions use the first chapter.
    control("{\"progress_data\":{\"txt_book_202\":{}}}");
    assert(weread_runtime_start(WR_JOB_OPEN,"txt_book_202",NULL));result=completion();
    assert(result.error==ESP_OK&&result.open_ready);weread_runtime_result_free(&result);
    assert(book_weread_open("weread://txt_book_202")==ESP_OK);
    size_t unread_chapter=SIZE_MAX;uint32_t unread_offset=UINT32_MAX;
    assert(weread_reader_resume("weread://txt_book_202",&unread_chapter,&unread_offset)==ESP_OK&&unread_chapter==0&&unread_offset==0);
    book_weread_close();
    assert(weread_progress_note("txt_book_202",1,1,3,25,0,1,"",1700000100)==ESP_OK);
    assert(weread_runtime_start(WR_JOB_OPEN,"txt_book_202",NULL));result=completion();
    assert(result.error==ESP_OK&&result.conflict);cloud=result.cloud;weread_runtime_result_free(&result);
    assert(weread_runtime_choose("txt_book_202",&cloud,true));result=completion();
    assert(result.error==ESP_OK&&result.open_ready);weread_runtime_result_free(&result);
    assert(book_weread_open("weread://txt_book_202")==ESP_OK);
    assert(weread_reader_resume("weread://txt_book_202",&unread_chapter,&unread_offset)==ESP_OK&&unread_chapter==0&&unread_offset==0);
    book_weread_close();
    control("{\"progress_data\":{\"test_book_101\":{\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":3,\"progress\":50,\"updateTime\":2000000000}}}");
    assert(weread_runtime_start(WR_JOB_OPEN,"test_book_101",NULL));result=completion();
    assert(result.error==ESP_OK&&result.conflict&&!result.open_ready);
    if(result.conflict){
        // 合法云端进度未带bookVersion，准备时绑定目录版本。/ Bind a valid cloud reply that omits bookVersion.
        weread_progress_t choice=result.cloud;assert(!choice.book_version);weread_runtime_result_free(&result);
        assert(weread_runtime_choose("test_book_101",&choice,true));result=completion();
    }
    assert(result.open_ready);weread_runtime_result_free(&result);
    assert(book_weread_prepared("test_book_101"));
    char toc_file[512],toc_hold[520];snprintf(toc_file,sizeof(toc_file),"%s/cache/test_book_101/chapters.json",argv[1]);snprintf(toc_hold,sizeof(toc_hold),"%s.hold",toc_file);
    assert(!rename(toc_file,toc_hold));
    assert(book_weread_open("weread://test_book_101")==ESP_OK);
    assert(!book_weread_prepared("test_book_101")&&!rename(toc_hold,toc_file));
    size_t chapter=0;uint32_t offset=0;
    assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_OK&&chapter==0);
    weread_reader_begin("weread://test_book_101",1000);
    assert(book_weread_load(0,NULL,NULL,&text)==ESP_OK);
    offset=(uint32_t)(strstr(text.utf8,"微风")-text.utf8);
    weread_reader_navigated("weread://test_book_101",0,offset,17,text.utf8+offset);
    control("{\"endpoint_errors\":{\"/book/read\":{\"status\":503}}}");
    for(unsigned i=0;i<=5;++i){
        weread_reader_tick("weread://test_book_101",0,offset,17,text.utf8+offset,1000+i*200,true);
        if(weread_runtime_busy()){result=completion();weread_runtime_result_free(&result);}
    }
    weread_reader_tick("weread://test_book_101",0,offset,17,text.utf8+offset,2200,false);
    control("{\"endpoint_errors\":{\"/book/read\":{\"status\":503,\"delay\":2.5}}}");
    uint64_t handoff_started=milliseconds();
    weread_reader_flush(2200,true);
    assert(milliseconds()-handoff_started<2600);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK&&local.dirty&&local.chapter_offset==207&&local.reading_time>=1);
    control("{\"endpoint_errors\":{}}");
    weread_reader_flush(2400,true);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK&&!local.dirty);
    weread_reader_tick("weread://test_book_101",0,offset,17,text.utf8+offset,3500,true);
    result=completion();assert(result.job==WR_JOB_PREFETCH&&result.error==ESP_OK);weread_runtime_result_free(&result);
    weread_client_set_download_transport(offline,NULL);
    html_text_t next={0};unsigned old_downloads=offline_downloads;
    assert(weread_reader_prepare("weread://test_book_101",1)==ESP_OK);
    assert(book_weread_load(1,NULL,NULL,&next)==ESP_OK&&strstr(next.utf8,"预读成功")&&offline_downloads==old_downloads);
    html_text_free(&next);weread_client_set_download_transport(NULL,NULL);
    // 阅读中-2063自愈换版本，NVS仍是旧身份；冷恢复必须使用TF的UID/UTF16位置。
    // A mid-read version retry changes the NVS identity; cold resume must use the TF UID/UTF16 record.
    struct stat old_identity,new_identity;assert(book_weread_stat("weread://test_book_101",&old_identity));
    char chapter_manifest[512];snprintf(chapter_manifest,sizeof(chapter_manifest),"%s/cache/test_book_101/chapter-2.json",argv[1]);
    assert(!unlink(chapter_manifest));
    control("{\"book_version\":101,\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":101},{\"bookId\":\"txt_book_202\",\"bookVersion\":100}]}");
    assert(weread_reader_prepare("weread://test_book_101",1)==ESP_OK);
    assert(book_weread_load(1,NULL,NULL,&next)==ESP_OK);
    uint32_t next_offset=(uint32_t)(strstr(next.utf8,"预读成功")-next.utf8);
    weread_reader_navigated("weread://test_book_101",1,next_offset,86,next.utf8+next_offset);
    weread_reader_tick("weread://test_book_101",1,next_offset,86,next.utf8+next_offset,3800,false);
    weread_reader_flush(3800,true);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK&&local.chapter_uid==2&&local.book_version==101);
    assert(book_weread_stat("weread://test_book_101",&new_identity)&&old_identity.st_size!=new_identity.st_size);
    html_text_free(&next);weread_reader_end(3800);book_weread_close();
    assert(weread_reader_prepare_open("weread://test_book_101")==ESP_OK);
    assert(book_weread_open("weread://test_book_101")==ESP_OK);
    chapter=0;offset=0;
    assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_OK&&chapter==1&&offset==next_offset);
    weread_progress_t valid_position=local;
    local.book_version=100;assert(weread_progress_select("test_book_101",&local,true)==ESP_OK);
    chapter=42;offset=99;assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_ERR_INVALID_RESPONSE&&chapter==42&&offset==99);
    local=valid_position;local.chapter_uid=999;assert(weread_progress_select("test_book_101",&local,true)==ESP_OK);
    assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_ERR_INVALID_RESPONSE&&chapter==42&&offset==99);
    assert(weread_progress_select("test_book_101",&valid_position,true)==ESP_OK);
    weread_auth_session_t account=*weread_auth_get_current_session(),other=account;strcpy(other.vid,"999");
    assert(weread_auth_save_session(&other)==ESP_OK&&weread_auth_load_session(&other)==ESP_OK);
    assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_ERR_INVALID_RESPONSE);
    assert(weread_auth_save_session(&account)==ESP_OK&&weread_auth_load_session(&account)==ESP_OK);
    assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_OK&&chapter==1&&offset==next_offset);
    // 版本0仍是有效unknown身份；只拒绝与当前目录不同的版本。
    // Zero remains a valid unknown version when it matches the current TOC.
    book_weread_close();
    control("{\"book_version\":0,\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":0},{\"bookId\":\"txt_book_202\",\"bookVersion\":1}],\"progress_data\":{\"test_book_101\":{\"chapterUid\":2,\"chapterIdx\":2,\"chapterOffset\":3,\"progress\":50}}}");
    assert(weread_shelf_refresh_book("test_book_101")==ESP_OK);
    assert(weread_runtime_start(WR_JOB_OPEN,"test_book_101",NULL));result=completion();
    assert(result.error==ESP_OK&&result.conflict);cloud=result.cloud;weread_runtime_result_free(&result);
    assert(weread_runtime_choose("test_book_101",&cloud,true));result=completion();
    assert(result.error==ESP_OK&&result.open_ready);weread_runtime_result_free(&result);
    assert(book_weread_open("weread://test_book_101")==ESP_OK);
    assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_OK&&chapter==1);
    assert(weread_reader_resume("weread://test_book_101",&chapter,&offset)==ESP_OK&&chapter==1);
    // shutdown中断阻塞响应；取消后可重新创建任务。/ Shutdown interrupts blocked responses and permits worker restart.
    control("{\"latency\":1.5}");
    assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));usleep(150000);
    uint64_t cancelled_at=milliseconds();weread_runtime_cancel_join();
    assert(milliseconds()-cancelled_at<1000);
    result=completion();assert(result.error!=ESP_OK);weread_runtime_result_free(&result);
    control("{\"latency\":0}");
    html_text_free(&text);weread_reader_end(4000);book_weread_close();
    weread_runtime_media_lost();assert(!weread_shelf_count());
    assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));result=completion();
    assert(result.authenticated&&result.count==2);weread_runtime_result_free(&result);
    char other_card[]="/tmp/weread-other-card-XXXXXX";assert(mkdtemp(other_card));
    assert(!setenv("WEREAD_STORAGE_ROOT",other_card,1));
    assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));result=completion();assert(!result.authenticated&&!result.count);weread_runtime_result_free(&result);
    assert(!setenv("WEREAD_STORAGE_ROOT",argv[1],1));
    assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));result=completion();assert(result.authenticated&&result.count==2);weread_runtime_result_free(&result);
    assert(weread_runtime_start(WR_JOB_CLEAR,"txt_book_202",NULL));result=completion();assert(result.error==ESP_OK);weread_runtime_result_free(&result);
    assert(weread_runtime_start(WR_JOB_CLEAR,"test_book_101",NULL));result=completion();assert(result.error==ESP_OK);weread_runtime_result_free(&result);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK);
    assert(weread_runtime_start(WR_JOB_LOGOUT,NULL,NULL));result=completion();assert(result.error==ESP_OK&&!result.authenticated&&!result.count);weread_runtime_result_free(&result);
    // 65秒扫码等待也必须被退页/锁屏立即打断。/ Exit/lock must also interrupt the65s QR wait immediately.
    assert(weread_runtime_start(WR_JOB_QR,NULL,NULL));result=completion();assert(result.error==ESP_OK);
    strcpy(uid,result.uid);weread_runtime_result_free(&result);
    control("{\"latency\":1.5}");
    assert(weread_runtime_start(WR_JOB_POLL,NULL,uid));usleep(150000);
    cancelled_at=milliseconds();weread_runtime_cancel_join();assert(milliseconds()-cancelled_at<1000);
    result=completion();assert(result.job==WR_JOB_POLL&&result.error!=ESP_OK);weread_runtime_result_free(&result);
    control("{\"latency\":0}");
    weread_runtime_stop();
    assert(weread_auth_clear_session() == ESP_OK && weread_shelf_count() == 0);
    weread_shelf_clear();
#ifdef WEREAD_TEST_MEMORY
    weread_test_memory_report();
#endif
    puts("Production C E2E passed: HTTP QR login -> shelf -> TOC -> encrypted download -> HTML -> progress -> offline reopen -> logout.");
    return 0;
}
