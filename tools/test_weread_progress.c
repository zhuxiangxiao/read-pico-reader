/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：生产进度模块的冲突、节流、断网脏记录与签名回退验证。
 * English: Test production progress conflicts, throttling, offline dirty state and signed fallback.
 */
#include "weread_progress.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "cJSON.h"
#include "weread_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mode, requests, renewals, web_reads, fail_web_once;
static const char *progress_response, *native_response;
static esp_err_t transport(const char *url, const char *method, const char *headers, const char *body,
                          char **out, size_t *len, int *status, void *ctx) {
    (void)headers; (void)ctx; ++requests; *status = 200;
    const char *response = NULL;
    if (strstr(url, "/weblogin")) { ++renewals; response = "{\"accessToken\":\"new-token\"}"; }
    else if (mode == 1) return ESP_ERR_TIMEOUT;
    else if (strstr(url, "/web/book/read")) {
        assert(!strcmp(method, "POST")); ++web_reads;
        if (fail_web_once) {
            fail_web_once = 0;
            response = "{}";
        } else {
            cJSON *request = cJSON_Parse(body);
            assert(cJSON_IsString(cJSON_GetObjectItem(request, "s")));
            assert(cJSON_IsString(cJSON_GetObjectItem(request, "appId")));
            assert(cJSON_IsString(cJSON_GetObjectItem(request, "b")));
            cJSON_Delete(request);
            response = "{\"succ\":1}";
        }
    } else if (strstr(url, "/book/read")) {
        assert(!strcmp(method, "POST"));
        cJSON *request = cJSON_Parse(body);
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "bookId")), "test_book_101"));
        assert(!cJSON_HasObjectItem(request, "dirty") && !cJSON_HasObjectItem(request, "lastReportMs") && !cJSON_HasObjectItem(request,"positionPending"));
        assert(cJSON_GetObjectItem(request, "readingTime")->valueint < 100);
        cJSON_Delete(request);
        if (mode == 4) {
            *status = 401;
            response = "{\"errCode\":-2012}";
        } else {
            response = mode == 2 ? "{}" : mode == 3 ? "{\"errcode\":-2012}" : "{\"succ\":1}";
        }
    } else if (strstr(url, "/shelf/syncbook")) {
        assert(!strcmp(method,"POST")&&strstr(body,"test_book_101"));
        response=native_response?native_response:"{}";
    } else if (strstr(url, "/book/getProgress")) {
        response = progress_response ? progress_response : "{\"bookId\":\"test_book_101\",\"book\":{\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":60,\"progress\":25,\"readingTime\":3600,\"updateTime\":400}}";
    } else assert(false);
    *out = strdup(response); assert(*out); *len = strlen(*out); return ESP_OK;
}
int main(void) {
    char root[] = "/tmp/kiiko-weread-progress-XXXXXX"; assert(mkdtemp(root));
    assert(weread_auth_init(root) == ESP_OK);
    weread_auth_session_t session = {0}; strcpy(session.vid, "123");
    strcpy(session.access_token, "test-token"); strcpy(session.refresh_token, "test-refresh");
    assert(weread_auth_save_session(&session) == ESP_OK && weread_auth_load_session(&session) == ESP_OK);
    weread_client_set_transport(transport, NULL);
    weread_progress_t local, cloud; bool conflict;
    const char *unread[] = {
        "{\"bookId\":\"test_book_101\",\"book\":{}}",
        "{\"bookId\":\"test_book_101\",\"book\":{\"chapterUid\":0,\"chapterIdx\":0,\"chapterOffset\":0,\"progress\":0}}",
    };
    for (size_t i = 0; i < sizeof(unread)/sizeof(*unread); ++i) {
        progress_response = unread[i];
        assert(weread_progress_sync("test_book_101", &local, &cloud, &conflict) == ESP_OK);
        assert(!conflict && !local.chapter_uid && !cloud.chapter_uid && !local.progress);
        // 未读候选不应留下无法恢复的零UID进度文件。/ Never persist an unusable zero-UID record.
        assert(weread_progress_load("test_book_101", &local) == ESP_ERR_NOT_FOUND);
    }
    const char *invalid[] = {
        "{\"book\":null}", "{\"book\":[]}", "{\"book\":{\"chapterUid\":\"0\"}}",
        "{\"book\":{\"chapterUid\":0,\"progress\":86}}", "{\"book\":{\"chapterOffset\":1}}",
        "{\"book\":{\"chapterIdx\":1}}", "{\"book\":{\"chapterUid\":1}}",
        "{\"book\":{\"errcode\":-1}}", "{\"errcode\":-1}",
        "{\"bookId\":\"other_book\",\"book\":{}}",
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(*invalid); ++i) {
        progress_response = invalid[i];
        assert(weread_progress_sync("test_book_101", &local, &cloud, &conflict) == ESP_ERR_INVALID_RESPONSE);
    }
    progress_response = NULL;
    assert(weread_progress_sync("test_book_101", &local, &cloud, &conflict) == ESP_OK);
    assert(!conflict && local.chapter_offset == 60 && local.reading_time == 0 && !local.dirty);
    assert(weread_progress_note_time("test_book_101",5,300)==ESP_OK);
    int time_requests=requests;
    assert(weread_progress_report("test_book_101",true,90000)==ESP_OK&&requests==time_requests);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK&&local.reading_time==5&&local.dirty&&!local.position_pending&&local.chapter_offset==60);
    assert(weread_progress_checkpoint("test_book_101",&local)==ESP_OK&&weread_progress_load("test_book_101",&local)==ESP_OK&&!local.position_pending);
    assert(weread_progress_report("test_book_101",true,90001)==ESP_OK&&requests==time_requests);
    weread_progress_t clean=local;clean.chapter_uid=2;clean.chapter_index=2;clean.updated_at=900;
    assert(weread_progress_select("test_book_101",&clean,true)==ESP_OK);
    assert(weread_progress_sync("test_book_101",&local,&cloud,&conflict)==ESP_OK&&!conflict&&!local.dirty&&local.chapter_uid==2&&cloud.updated_at<local.updated_at);
    assert(weread_progress_note("test_book_101", 2, 2, 80, 30, 15, 1, "本机位置", 300) == ESP_OK);
    assert(weread_progress_sync("test_book_101", &local, &cloud, &conflict) == ESP_OK);
    assert(!conflict && local.chapter_uid == 2 && cloud.chapter_uid == 1);
    assert(weread_progress_load("test_book_101", &local) == ESP_OK && local.chapter_uid == 2 && local.dirty);
    assert(weread_progress_select("test_book_101", &cloud, true) == ESP_OK);
    assert(weread_progress_load("test_book_101", &local) == ESP_OK && !local.dirty && !local.reading_time);
    assert(weread_progress_note("test_book_101", 1, 1, 90, 28, 10, 1, "新位置", 500) == ESP_OK);
    int before = requests;
    assert(weread_progress_report("test_book_101", false, 1000) == ESP_OK && requests == before);
    mode = 1; assert(weread_progress_report("test_book_101", true, 2000) == ESP_ERR_TIMEOUT);
    assert(weread_progress_load("test_book_101", &local) == ESP_OK && local.dirty && local.reading_time == 10);
    before = requests;
    assert(weread_progress_report("test_book_101", false, 10000) == ESP_ERR_TIMEOUT && requests == before);
    mode = 2; assert(weread_progress_report("test_book_101", true, 2000) != ESP_OK);
    assert(weread_progress_load("test_book_101", &local) == ESP_OK && local.dirty);
    mode = 3; assert(weread_progress_report("test_book_101", true, 2000) == ESP_OK);
    assert(web_reads == 1 && renewals == 1 && weread_auth_is_authenticated());
    assert(weread_progress_load("test_book_101", &local) == ESP_OK && !local.dirty && !local.pending_pages);
    assert(weread_progress_note("test_book_101", 1, 1, 110, 29, 3, 10, "十页", 600) == ESP_OK);
    mode = 0; before = requests;
    assert(weread_progress_report("test_book_101", false, 3000) == ESP_OK && requests == before + 1);
    assert(weread_progress_note("test_book_101", 1, 1, 130, 30, 2, 1, "计时", 700) == ESP_OK);
    before = requests; assert(weread_progress_report("test_book_101", false, 63000) == ESP_OK && requests == before + 1);
    assert(weread_progress_note("test_book_101", 1, 1, 130, 101, 2, 1, "", 700) == ESP_ERR_INVALID_ARG);
    assert(weread_progress_note("test_book_101",2,2,20,0,10,1,"native test",800)==ESP_OK);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK);
    native_response="{\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":2,\"chapterIdx\":45,\"chapterOffset\":20,\"progress\":86,\"readingTime\":3600}]}";
    before=requests;
    assert(weread_progress_sync("test_book_101",&local,&cloud,&conflict)==ESP_OK&&requests==before+1);
    assert(!conflict&&local.progress==86&&local.chapter_index==45&&!local.dirty);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK&&!local.dirty&&!local.reading_time&&!local.pending_pages);
    assert(weread_progress_ahead(&cloud,&(weread_progress_t){.chapter_uid=2,.chapter_index=2,.chapter_offset=20}));
    assert(!weread_progress_ahead(&(weread_progress_t){.chapter_uid=2,.chapter_index=2,.chapter_offset=20},&cloud));
    native_response="{\"bookProgress\":[]}";before=requests;
    assert(weread_progress_sync("test_book_101",&local,&cloud,&conflict)==ESP_OK&&requests==before+2&&cloud.chapter_uid==1);
    const char *partial_native[]={"{\"bookProgress\":[{\"bookId\":\"test_book_101\"}]}","{\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":2,\"chapterIdx\":45,\"progress\":86}]}"};
    for(size_t i=0;i<sizeof(partial_native)/sizeof(*partial_native);++i){
        native_response=partial_native[i];before=requests;
        assert(weread_progress_sync("test_book_101",&local,&cloud,&conflict)==ESP_OK&&requests==before+2&&cloud.chapter_uid==1&&cloud.chapter_offset==60);
    }
    native_response="{\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":2,\"chapterIdx\":45,\"progress\":86},{\"bookId\":\"test_book_101\",\"chapterUid\":1,\"chapterIdx\":1,\"progress\":0}]}";
    assert(weread_progress_sync("test_book_101",&local,&cloud,&conflict)==ESP_ERR_INVALID_RESPONSE);
    {
        weread_progress_t unread_cloud; uint32_t fresh_ver = 0, last_ch = 0;
        const char *unread_syncbooks[] = {
            "{\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":100}],\"bookProgress\":[]}",
            "{\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":100}],\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":0}]}"
        };
        for (size_t i = 0; i < sizeof(unread_syncbooks) / sizeof(*unread_syncbooks); ++i) {
            native_response = unread_syncbooks[i]; before = requests; fresh_ver = 0; last_ch = 0;
            assert(weread_progress_fetch_version("test_book_101", &unread_cloud, &fresh_ver, &last_ch) == ESP_OK);
            assert(requests == before + 1 && !unread_cloud.chapter_uid && fresh_ver == 100);
        }
        native_response = "{\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":100}]}";
        progress_response = "{\"bookId\":\"test_book_101\",\"book\":{}}";
        before = requests;
        assert(weread_progress_fetch_version("test_book_101", &unread_cloud, &fresh_ver, &last_ch) == ESP_OK);
        assert(requests == before + 2 && !unread_cloud.chapter_uid && fresh_ver == 100);
        progress_response = "{}";
        assert(weread_progress_fetch_version("test_book_101", &unread_cloud, &fresh_ver, &last_ch) == ESP_OK && !unread_cloud.chapter_uid && fresh_ver == 100);
        native_response = "{}";
        assert(weread_progress_fetch_version("test_book_101", &unread_cloud, &fresh_ver, &last_ch) == ESP_ERR_INVALID_RESPONSE);
        progress_response = NULL;
    }
    native_response=NULL;
    assert(weread_progress_note("test_book_101",1,1,60,25,1,1,"legacy",900)==ESP_OK);
    char path[512];cJSON *legacy=NULL;assert(wr_cache_path("test_book_101","progress.json",path,sizeof(path))==ESP_OK&&wr_read_json(path,&legacy)==ESP_OK);
    cJSON_DeleteItemFromObject(legacy,"positionPending");assert(wr_write_json(path,legacy)==ESP_OK);cJSON_Delete(legacy);
    assert(weread_progress_load("test_book_101",&local)==ESP_OK&&local.dirty&&local.position_pending);
    assert(weread_progress_report("test_book_101",true,90002)==ESP_OK);
    {
        assert(weread_progress_note("test_book_101",1,1,150,35,2,1,"outbox-skip-renew",950)==ESP_OK);
        native_response="{\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":0}],\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":60,\"progress\":25}]}";
        int renew_before=renewals, web_before=web_reads;
        mode=4;
        weread_progress_t reported={0};
        assert(weread_progress_outbox_report("test_book_101",90100,960,&reported)==ESP_OK);
        assert(renewals==renew_before && web_reads==web_before+1 && !reported.dirty && !reported.position_pending);
        int req_before = requests; web_before = web_reads;
        assert(weread_progress_note("test_book_101",1,1,160,36,2,1,"learned-web",970)==ESP_OK);
        native_response="{\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":0}],\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":150,\"progress\":35}]}";
        assert(weread_progress_outbox_report("test_book_101",90200,980,&reported)==ESP_OK);
        assert(requests==req_before+2 && web_reads==web_before+1 && !reported.dirty);
        fail_web_once = 1; mode = 0; req_before = requests; web_before = web_reads;
        assert(weread_progress_note("test_book_101",1,1,170,37,2,1,"fallback-after-web-fail",990)==ESP_OK);
        native_response="{\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":0}],\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":160,\"progress\":36}]}";
        assert(weread_progress_outbox_report("test_book_101",90300,1000,&reported)==ESP_OK);
        assert(requests==req_before+3 && web_reads==web_before+1 && !reported.dirty);
        mode=4; weread_progress_outbox_reset();
        assert(weread_progress_note("test_book_101",1,1,180,38,2,1,"reset-learned-web",1010)==ESP_OK);
        native_response="{\"books\":[{\"bookId\":\"test_book_101\",\"bookVersion\":0}],\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":170,\"progress\":37}]}";
        req_before = requests; web_before = web_reads;
        assert(weread_progress_outbox_report("test_book_101",90400,1020,&reported)==ESP_OK);
        assert(requests==req_before+3 && web_reads==web_before+1 && !reported.dirty);
        mode=0; native_response=NULL;
    }
    strcpy(session.vid, "999"); assert(weread_auth_save_session(&session) == ESP_OK && weread_auth_load_session(&session) == ESP_OK);
    assert(weread_progress_load("test_book_101", &local) != ESP_OK);
    weread_auth_clear_session();
    puts("WeRead progress: conflict preservation, explicit choice, dirty retries, 60s/10-page throttle and signed web fallback passed.");
    return 0;
}
