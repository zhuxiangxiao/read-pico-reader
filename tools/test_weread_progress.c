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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mode, requests, renewals, web_reads;
static esp_err_t transport(const char *url, const char *method, const char *headers, const char *body,
                          char **out, size_t *len, int *status, void *ctx) {
    (void)headers; (void)ctx; ++requests; *status = 200;
    const char *response = NULL;
    if (strstr(url, "/weblogin")) { ++renewals; response = "{\"accessToken\":\"new-token\"}"; }
    else if (mode == 1) return ESP_ERR_TIMEOUT;
    else if (strstr(url, "/web/book/read")) {
        assert(!strcmp(method, "POST")); ++web_reads;
        cJSON *request = cJSON_Parse(body);
        assert(cJSON_IsString(cJSON_GetObjectItem(request, "s")));
        assert(cJSON_IsString(cJSON_GetObjectItem(request, "appId")));
        assert(cJSON_IsString(cJSON_GetObjectItem(request, "b")));
        cJSON_Delete(request);
        response = "{\"succ\":1}";
    } else if (strstr(url, "/book/read")) {
        assert(!strcmp(method, "POST"));
        cJSON *request = cJSON_Parse(body);
        assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(request, "bookId")), "test_book_101"));
        assert(!cJSON_HasObjectItem(request, "dirty") && !cJSON_HasObjectItem(request, "lastReportMs"));
        assert(cJSON_GetObjectItem(request, "readingTime")->valueint < 100);
        cJSON_Delete(request);
        response = mode == 2 ? "{}" : mode == 3 ? "{\"errcode\":-2012}" : "{\"succ\":1}";
    } else if (strstr(url, "/book/getProgress")) {
        response = "{\"bookId\":\"test_book_101\",\"book\":{\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":60,\"progress\":25,\"readingTime\":3600,\"updateTime\":400}}";
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
    assert(weread_progress_sync("test_book_101", &local, &cloud, &conflict) == ESP_OK);
    assert(!conflict && local.chapter_offset == 60 && local.reading_time == 0 && !local.dirty);
    weread_progress_t clean=local;clean.chapter_uid=2;clean.chapter_index=2;clean.updated_at=900;
    assert(weread_progress_select("test_book_101",&clean,true)==ESP_OK);
    assert(weread_progress_sync("test_book_101",&local,&cloud,&conflict)==ESP_OK&&conflict&&!local.dirty&&cloud.updated_at<local.updated_at);
    assert(weread_progress_note("test_book_101", 2, 2, 80, 30, 15, 1, "本机位置", 300) == ESP_OK);
    assert(weread_progress_sync("test_book_101", &local, &cloud, &conflict) == ESP_OK);
    assert(conflict && local.chapter_uid == 2 && cloud.chapter_uid == 1);
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
    strcpy(session.vid, "999"); assert(weread_auth_save_session(&session) == ESP_OK && weread_auth_load_session(&session) == ESP_OK);
    assert(weread_progress_load("test_book_101", &local) != ESP_OK);
    weread_auth_clear_session();
    puts("WeRead progress: conflict preservation, explicit choice, dirty retries, 60s/10-page throttle and signed web fallback passed.");
    return 0;
}
