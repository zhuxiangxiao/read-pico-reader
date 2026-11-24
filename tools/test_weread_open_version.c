/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：验证普通开书及下载自愈后的最终版本绑定，冷恢复继续校验持久化版本。
 * English: Bind normal/recovered opens to the final TOC while retaining cold-resume version checks.
 */
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_progress.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_position.h"
#include "cJSON.h"
#include "book_source_weread.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static unsigned version = 100, downloads;
static unsigned toc_requests, http_requests, refresh_requests;
static unsigned reports, reported_offset, reported_percent, reported_seconds;
static bool selfheal, moved, corrupt, corrupt_fail, fail_download, bad_offset, bad_index;
static bool open_toc_heal, double_2063, expired_offline, expired_relogin_drain, mid_open_2013, mid_open_401, mid_open_inject;
static unsigned syncbook_requests, weblogin_requests, getprogress_requests;
static esp_err_t request(const char *url, const char *method, const char *headers, const char *body,
    char **out, size_t *len, int *status, void *ctx) {
    (void)method; (void)headers; (void)body; (void)ctx;
    ++http_requests;
    char text[1024]; *status = 200;
    if (expired_relogin_drain && strstr(url, "/weblogin")) {
        ++weblogin_requests;
        strcpy(text, "{\"vid\":\"123\",\"accessToken\":\"relogin-token\",\"refreshToken\":\"relogin-refresh\"}");
        *out = strdup(text); assert(*out); *len = strlen(text); return ESP_OK;
    }
    if (mid_open_inject && strstr(url, "/weblogin")) {
        ++weblogin_requests;
        *status = 401;
        strcpy(text, "{\"errcode\":-2013,\"errmsg\":\"revoked\"}");
        *out = strdup(text); assert(*out); *len = strlen(text); return ESP_OK;
    }
    if (strstr(url, "/shelf/syncbook")) {
        ++refresh_requests;
        if (mid_open_inject) {
            ++syncbook_requests;
            *status = mid_open_401 ? 401 : 200;
            strcpy(text, "{\"errcode\":-2013,\"errmsg\":\"expired\"}");
            *out = strdup(text); assert(*out); *len = strlen(text); return ESP_OK;
        }
        if (selfheal && downloads) version = 101;
        if (open_toc_heal && refresh_requests >= 2) version = 101;
    }
    if(strstr(url,"/book/read")){
        cJSON *o=cJSON_Parse(body);assert(o);++reports;
        reported_offset=(unsigned)cJSON_GetObjectItem(o,"chapterOffset")->valuedouble;
        reported_percent=(unsigned)cJSON_GetObjectItem(o,"progress")->valuedouble;
        reported_seconds=(unsigned)cJSON_GetObjectItem(o,"readingTime")->valuedouble;
        cJSON_Delete(o);strcpy(text,"{\"succ\":1}");
    }else if (strstr(url, "/book/chapterInfos")){
        ++toc_requests;
        if ((mid_open_2013 || mid_open_401) && body && strstr(body, "uncached_book_102")) {
            snprintf(text, sizeof(text), "{\"data\":[{\"bookId\":\"uncached_book_102\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":100,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"uncached-first\"}]}]}");
        } else if (expired_offline || expired_relogin_drain || mid_open_2013 || mid_open_401) {
            snprintf(text, sizeof(text), "{\"data\":[{\"bookId\":\"test_book_101\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":%u,\"updated\":[{\"chapterUid\":2,\"chapterIdx\":1,\"title\":\"first\"},{\"chapterUid\":1,\"chapterIdx\":2,\"title\":\"second\"}]}]}", version);
        } else {
            snprintf(text, sizeof(text), "{\"data\":[{\"bookId\":\"test_book_101\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":%u,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"first\"},{\"chapterUid\":2,\"chapterIdx\":%u,\"title\":\"second\"}]}]}", version,moved&&version==101?67u:2u);
        }
    }
    else if (strstr(url, "/book/getProgress")) {
        if (mid_open_inject) ++getprogress_requests;
        if (expired_relogin_drain) {
            snprintf(text,sizeof(text),"{\"bookId\":\"test_book_101\",\"book\":{\"chapterUid\":2,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":0,\"bookVersion\":%u}}",version);
        } else {
            snprintf(text,sizeof(text),"{\"bookId\":\"test_book_101\",\"book\":{\"chapterUid\":2,\"chapterIdx\":%u,\"chapterOffset\":%u,\"progress\":86,\"bookVersion\":%u}}",bad_index?67u:2u,bad_offset?UINT32_MAX:20u,open_toc_heal&&refresh_requests<2?100u:version);
        }
    }
    else if (expired_relogin_drain && strstr(url, "/shelf/syncbook")) {
        snprintf(text, sizeof(text), "{\"synckey\":1,\"books\":[{\"bookId\":\"test_book_101\",\"title\":\"fixture\",\"format\":\"epub\",\"bookVersion\":%u}],\"bookProgress\":[{\"bookId\":\"test_book_101\",\"chapterUid\":2,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":0}]}", version);
    }
    else if (mid_open_2013 || mid_open_401) {
        snprintf(text, sizeof(text), "{\"synckey\":1,\"books\":[{\"bookId\":\"test_book_101\",\"title\":\"fixture\",\"format\":\"epub\",\"bookVersion\":100},{\"bookId\":\"uncached_book_102\",\"title\":\"uncached\",\"format\":\"epub\",\"bookVersion\":100}]}");
    }
    else snprintf(text, sizeof(text), "{\"synckey\":1,\"books\":[{\"bookId\":\"test_book_101\",\"title\":\"fixture\",\"format\":\"epub\",\"bookVersion\":%u}]}", (open_toc_heal&&refresh_requests==1)?101u:version);
    *out = strdup(text); assert(*out); *len = strlen(text); return ESP_OK;
}
static esp_err_t download(const char *url, const char *headers, FILE *out, size_t limit,
    weread_cancel_fn cancel, void *arg, weread_download_info_t *info, void *ctx) {
    (void)url; (void)headers; (void)limit; (void)cancel; (void)arg; (void)ctx;
    memset(info, 0, sizeof(*info)); info->status = 200;
    unsigned attempt=downloads++;
    if(fail_download)return ESP_ERR_TIMEOUT;
    if (double_2063 || (!attempt && selfheal)) {
        const char *json = "{\"errcode\":-2063}"; info->bytes = strlen(json);
        assert(fwrite(json, 1, info->bytes, out) == info->bytes); return ESP_OK;
    }
    FILE *file = fopen("build/weread-tests/no-version.zip", "rb"); assert(file);
    strcpy(info->encrypt_key, "pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
    char buf[1024]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), file))) { assert(fwrite(buf, 1, n, out) == n); info->bytes += n; }
    assert(!ferror(file)); fclose(file); return ESP_OK;
}
static void adopt_body(size_t chapter) {
    html_text_t text={0};uint32_t actual_version=0;size_t actual=chapter;
    assert(book_weread_load(chapter,NULL,NULL,&text)==ESP_OK);
    assert(book_weread_commit_loaded(&actual,&actual_version)==ESP_OK&&actual==chapter&&actual_version==101);
    html_text_free(&text);
}
static void complete_map(uint32_t uid) {
    char path[1100];assert(weread_chapter_directory("test_book_101",uid,path,1024)==ESP_OK);
    strcat(path,"/position.map");FILE *file=fopen(path,"rb");assert(file);
    struct stat st;weread_position_header_t h={0};
    assert(!fstat(fileno(file),&st)&&fread(&h,sizeof(h),1,file)==1&&h.magic==WEREAD_POSITION_MAGIC&&h.count&&h.count<=16384&&h.bytes);
    assert((uint64_t)st.st_size==sizeof(h)+(uint64_t)h.count*sizeof(weread_position_point_t));
    weread_position_point_t previous={0},point={0};
    for(uint32_t i=0;i<h.count;++i){
        assert(fread(&point,sizeof(point),1,file)==1&&point.byte<=h.bytes&&point.unit<=h.units);
        assert(!i?(!point.byte&&!point.unit):(point.byte>=previous.byte&&point.unit>=previous.unit));previous=point;
    }
    assert(point.byte==h.bytes&&point.unit==h.units&&fgetc(file)==EOF&&!ferror(file)&&!fclose(file));
}
int main(int argc, char **argv) {
    assert(argc == 3); selfheal = !strcmp(argv[2], "selfheal");moved=!strcmp(argv[2],"moved");
    corrupt_fail=!strcmp(argv[2],"corrupt-fail");corrupt=!strcmp(argv[2],"corrupt")||corrupt_fail;bad_offset=!strcmp(argv[2],"bad-offset");bad_index=!strcmp(argv[2],"bad-index");
    open_toc_heal=!strcmp(argv[2],"open-toc-heal");double_2063=!strcmp(argv[2],"double-2063");expired_offline=!strcmp(argv[2],"expired-offline");
    expired_relogin_drain=!strcmp(argv[2],"expired-relogin-drain");
    mid_open_2013=!strcmp(argv[2],"mid-open-2013");mid_open_401=!strcmp(argv[2],"mid-open-401");
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK);
    weread_auth_session_t session = {0};
    strcpy(session.vid, "123"); strcpy(session.access_token, "test-token"); strcpy(session.refresh_token, "fixture-refresh");
    assert(weread_auth_save_session(&session) == ESP_OK && weread_auth_load_session(&session) == ESP_OK);
    weread_client_set_transport(request, NULL); weread_client_set_download_transport(download, NULL);
    assert(weread_shelf_sync(true) == ESP_OK && weread_toc_sync("test_book_101") == ESP_OK);
    if (mid_open_2013 || mid_open_401) {
        assert(weread_toc_sync("uncached_book_102") == ESP_OK);
        assert(weread_chapter_download("test_book_101", 2) == ESP_OK);
        syncbook_requests = weblogin_requests = getprogress_requests = downloads = 0;
        mid_open_inject = true;
        weread_runtime_init();
        weread_result_t r = {0};
        time_t st = time(NULL);
        assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
        while (weread_runtime_busy()) { assert(time(NULL) - st < 10); usleep(1000); }
        assert(weread_runtime_poll(&r) && r.error == ESP_OK && r.open_ready && !r.authenticated && r.auth_expired);
        weread_runtime_result_free(&r);
        assert(syncbook_requests == 1 && weblogin_requests == 1 && getprogress_requests == 0 && downloads == 0);
        const char *uri = "weread://test_book_101";
        assert(book_weread_open(uri) == ESP_OK);
        size_t ch = 99; uint32_t off = 99;
        assert(weread_reader_resume(uri, &ch, &off) == ESP_OK && ch == 0);
        book_weread_close();
        assert(weread_auth_save_session(&session) == ESP_OK && weread_auth_load_session(&session) == ESP_OK);
        syncbook_requests = weblogin_requests = getprogress_requests = downloads = 0;
        st = time(NULL);
        assert(weread_runtime_start(WR_JOB_OPEN, "uncached_book_102", NULL));
        while (weread_runtime_busy()) { assert(time(NULL) - st < 10); usleep(1000); }
        assert(weread_runtime_poll(&r) && r.error == ESP_ERR_INVALID_STATE && !r.open_ready && !r.authenticated && r.auth_expired);
        weread_runtime_result_free(&r);
        assert(syncbook_requests == 1 && weblogin_requests == 1 && getprogress_requests == 0 && downloads == 0);
        weread_runtime_stop(); weread_shelf_clear();
        printf("Mid-open %s fallback passed.\n", argv[2]);
        return 0;
    }
    if (expired_relogin_drain) {
        assert(weread_chapter_download("test_book_101", 2) == ESP_OK);
        assert(weread_auth_mark_expired() == ESP_OK);
        http_requests = 0; downloads = 0; reports = 0; weblogin_requests = 0;
        weread_runtime_init();
        weread_result_t r = {0};
        time_t st = time(NULL);
        assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
        while (weread_runtime_busy()) { assert(time(NULL) - st < 10); usleep(1000); }
        assert(weread_runtime_poll(&r) && r.error == ESP_OK && r.open_ready && !r.authenticated && r.auth_expired);
        weread_runtime_result_free(&r);
        const char *uri = "weread://test_book_101";
        assert(book_weread_open(uri) == ESP_OK);
        size_t ch = 99; uint32_t off = 99;
        assert(weread_reader_resume(uri, &ch, &off) == ESP_OK && ch == 0);
        html_text_t text = {0}; uint32_t ver = 0;
        assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK);
        assert(book_weread_commit_loaded(&ch, &ver) == ESP_OK);
        weread_reader_begin(uri, 1000);
        weread_reader_navigated_version(uri, 0, 4, 20, "offline-relogin", ver);
        assert(weread_reader_save(2000) == ESP_OK);
        weread_reader_end(2500);
        html_text_free(&text);
        book_weread_close();
        assert(reports == 0 && http_requests == 0);
        weread_progress_t saved;
        assert(weread_progress_load("test_book_101", &saved) == ESP_OK && saved.dirty && saved.position_pending && saved.progress == 20);
        weread_auth_session_t relogin = {0};
        assert(weread_auth_exchange_code("RELOGIN_CODE", &relogin) == ESP_OK);
        assert(weread_auth_is_authenticated() && !weread_auth_is_expired() && weblogin_requests == 1);
        assert(weread_runtime_idle(3000));
        st = time(NULL);
        while (weread_runtime_busy()) { assert(time(NULL) - st < 10); usleep(1000); }
        assert(weread_runtime_poll(&r) && r.job == WR_JOB_REPORT && r.error == ESP_OK);
        weread_runtime_result_free(&r);
        assert(reports == 1 && reported_percent == 20);
        assert(weread_progress_load("test_book_101", &saved) == ESP_OK && !saved.dirty && !saved.position_pending);
        weread_runtime_stop(); weread_shelf_clear();
        puts("Expired offline-write -> exchange_code re-login -> outbox drain passed.");
        return 0;
    }
    if (expired_offline) {
        assert(weread_chapter_download("test_book_101", 2) == ESP_OK);
        assert(weread_auth_mark_expired() == ESP_OK);
        http_requests = 0; downloads = 0;
        weread_runtime_init();
        weread_result_t r = {0};
        time_t st = time(NULL);
        assert(weread_runtime_start(WR_JOB_INIT, NULL, NULL));
        while (weread_runtime_busy()) { assert(time(NULL) - st < 10); usleep(1000); }
        assert(weread_runtime_poll(&r) && !r.authenticated && r.auth_expired && r.count == 1 && r.books != NULL);
        weread_runtime_result_free(&r);
        assert(weread_runtime_start(WR_JOB_SYNC, NULL, NULL));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_ERR_INVALID_STATE && r.auth_expired && r.count == 1);
        weread_runtime_result_free(&r);
        assert(weread_runtime_start(WR_JOB_SYNC, NULL, "continue"));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_ERR_INVALID_STATE && r.auth_expired && r.count == 1);
        weread_runtime_result_free(&r);
        assert(weread_runtime_start(WR_JOB_REPORT, NULL, NULL));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_ERR_INVALID_STATE && r.auth_expired);
        weread_runtime_result_free(&r);
        assert(weread_runtime_start(WR_JOB_NOTE, "test_book_101", NULL));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_ERR_INVALID_STATE && r.auth_expired);
        weread_runtime_result_free(&r);
        assert(weread_runtime_start(WR_JOB_CHAPTER, "test_book_101", "1"));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_ERR_INVALID_STATE && r.auth_expired);
        weread_runtime_result_free(&r);
        assert(weread_runtime_start(WR_JOB_COVER, "test_book_101", "remote"));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error != ESP_OK && r.auth_expired);
        weread_runtime_result_free(&r);
        // Unread cached book opens offline without progress.json
        assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_OK && r.open_ready && !r.authenticated && r.auth_expired);
        weread_runtime_result_free(&r);
        const char *uri = "weread://test_book_101";
        assert(book_weread_open(uri) == ESP_OK);
        size_t ch = 99; uint32_t off = 99;
        assert(weread_reader_resume(uri, &ch, &off) == ESP_OK && ch == 0);
        html_text_t text = {0}; uint32_t ver = 0;
        assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK);
        assert(book_weread_commit_loaded(&ch, &ver) == ESP_OK);
        weread_reader_begin(uri, 1000);
        weread_reader_navigated_version(uri, 0, 4, 20, "offline", ver);
        assert(weread_reader_save(2000) == ESP_OK);
        weread_reader_tick(uri, 0, 4, 20, "offline", 12000, true);
        assert(!weread_runtime_busy());
        assert(!weread_runtime_idle(13000));
        weread_reader_end(13000);
        html_text_free(&text);
        book_weread_close();
        // Read cached book (local-fast) also opens offline
        assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_OK && r.open_ready && r.auth_expired);
        weread_runtime_result_free(&r);
        // Uncached chapter fails with ESP_ERR_INVALID_STATE and zero HTTP calls
        assert(weread_progress_note("test_book_101", 1, 2, 0, 50, 0, 1, "uncached", 1700000200) == ESP_OK);
        assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_ERR_INVALID_STATE && !r.open_ready && r.auth_expired);
        weread_runtime_result_free(&r);
        assert(http_requests == 0 && downloads == 0);
        // Explicit logout wipes auth_expired & shelf
        assert(weread_runtime_start(WR_JOB_LOGOUT, NULL, NULL));
        while (weread_runtime_busy()) usleep(1000);
        assert(weread_runtime_poll(&r) && r.error == ESP_OK && !r.authenticated && !r.auth_expired && !r.count);
        weread_runtime_result_free(&r);
        weread_runtime_stop(); weread_shelf_clear();
        puts("Expired offline read-only open, local save, zero-HTTP gating and logout passed.");
        return 0;
    }
    bool invalid_choice=corrupt||bad_offset||bad_index||double_2063;
    assert(weread_progress_note("test_book_101", invalid_choice?1:2,invalid_choice?1:2,invalid_choice?0:20,invalid_choice?0:86,invalid_choice?7:0,1,"fixture",1700000100)==ESP_OK);
    if(corrupt){
        assert(weread_chapter_download("test_book_101",2)==ESP_OK);
        char path[1100];assert(weread_chapter_directory("test_book_101",2,path,1024)==ESP_OK);
        strcat(path,"/position.map");assert(!truncate(path,sizeof(weread_position_header_t)));
        assert(downloads==1&&!weread_chapter_cached("test_book_101",2));fail_download=corrupt_fail;
    }
    if (!selfheal&&!invalid_choice&&!open_toc_heal) { version = 101; assert(weread_shelf_refresh_book("test_book_101") == ESP_OK); }
    weread_runtime_init(); assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
    time_t started = time(NULL);
    while (weread_runtime_busy()) { assert(time(NULL) - started < 10); usleep(1000); }
    weread_result_t result; assert(weread_runtime_poll(&result));
    if (double_2063) {
        assert(result.error == WEREAD_ERR_VERSION && !result.open_ready && downloads == 2);
        weread_progress_t saved; assert(weread_progress_load("test_book_101", &saved) == ESP_OK && saved.chapter_uid == 1 && saved.dirty && saved.reading_time == 7);
        weread_runtime_result_free(&result); weread_runtime_stop(); weread_shelf_clear();
        puts("Double -2063 stopped after 1 heal and preserved dirty local progress.");
        return 0;
    }
    if(invalid_choice){
        assert(!result.conflict);
        weread_progress_t choice=result.cloud;
        if(corrupt&&!corrupt_fail){
            assert(result.error==ESP_OK&&result.open_ready&&downloads==2&&!reports);
            assert(weread_chapter_cached("test_book_101",2));complete_map(2);
            uint32_t proven=0;assert(book_weread_validate_prepared("test_book_101",choice.chapter_uid,choice.chapter_index,choice.chapter_offset,&proven)==ESP_OK&&proven==100);
            weread_progress_t saved;assert(weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.chapter_uid==choice.chapter_uid&&
                saved.chapter_index==choice.chapter_index&&saved.chapter_offset==choice.chapter_offset&&saved.progress==choice.progress&&saved.book_version==100&&
                !saved.dirty&&!saved.position_pending&&!saved.reading_time&&!saved.pending_pages);
            weread_runtime_result_free(&result);weread_runtime_stop();weread_shelf_clear();
            puts("Corrupt cached map re-downloaded once; complete map and auto-adopted cloud coordinates validated before saving.");return 0;
        }
        assert(result.error==(corrupt_fail?ESP_ERR_TIMEOUT:WEREAD_ERR_POSITION)&&!result.open_ready&&!reports);
        // 原位损坏缓存先被自愈下载一次；开书超时后静默重试再下载一次，共3次尝试。
        // The corrupt cache is healed once, then the open job replays its timeout once:3 download attempts.
        if(corrupt_fail)assert(downloads==3&&!weread_chapter_cached("test_book_101",2));
        weread_progress_t saved;assert(weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.chapter_uid==1&&saved.dirty&&saved.reading_time==7);
        weread_runtime_result_free(&result);weread_runtime_stop();weread_shelf_clear();
        puts("Invalid cloud position rejected without replacing dirty local progress.");return 0;
    }
    if(moved){
        assert(result.error==WEREAD_ERR_POSITION&&!result.open_ready);
        weread_progress_t saved;assert(weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.book_version==100&&saved.chapter_index==2);
        weread_runtime_result_free(&result);weread_runtime_stop();weread_shelf_clear();
        puts("Conflicting chapter ordinal rejected without replacing local progress.");return 0;
    }
    assert(result.error == ESP_OK && result.open_ready && !result.conflict); weread_runtime_result_free(&result);
    const char *uri = "weread://test_book_101";
    assert(book_weread_open(uri) == ESP_OK);
    uint32_t cached = 0, offset = 99; size_t chapter = 42;
    assert(book_weread_cached_version("test_book_101", &cached) && cached == 101);
    assert(weread_reader_resume(uri, &chapter, &offset) == ESP_OK && chapter == 1 && offset == 15);
    if(!strcmp(argv[2],"protection")){
        weread_progress_t saved;assert(weread_progress_load("test_book_101",&saved)==ESP_OK);
        assert(weread_progress_select("test_book_101",&saved,true)==ESP_OK);
        assert(weread_reader_resume(uri,&chapter,&offset)==ESP_OK);
        adopt_body(chapter);
        weread_reader_begin(uri,1000);
        weread_reader_tick(uri,chapter,0,0,"display page",1000,false);
        weread_reader_flush(1000,true);
        assert(!reports&&weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.chapter_offset==20&&saved.progress==86&&!saved.dirty);
        // 只读秒和重排继续保留原协议坐标；不上传估计页首。
        weread_reader_tick(uri,chapter,0,0,"reflowed page",1800,true);
        weread_reader_tick(uri,chapter,3,0,"reflowed again",2200,true);
        weread_reader_flush(2200,true);
        assert(!reports&&weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.chapter_offset==20&&saved.progress==86&&saved.reading_time==1&&saved.dirty&&!saved.position_pending);
        weread_reader_end(2200);book_weread_close();
        assert(weread_reader_prepare_open(uri)==ESP_OK&&book_weread_open(uri)==ESP_OK);
        assert(weread_reader_resume(uri,&chapter,&offset)==ESP_OK);adopt_body(chapter);weread_reader_begin(uri,2200);
        weread_reader_tick(uri,chapter,0,0,"cold display",2200,false);weread_reader_flush(2200,true);
        assert(!reports&&weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.reading_time==1&&!saved.position_pending);
        // 真导航在调用时冻结位置，后续重排不能移动尚未提交的目标。
        uint32_t expected=0;assert(book_weread_map(chapter,true,6,&expected)==ESP_OK);
        weread_reader_navigated(uri,chapter,6,44,"navigation");
        weread_reader_tick(uri,chapter,0,0,"reflow",2300,false);
        weread_reader_flush(2300,true);
        assert(!reports&&weread_progress_load("test_book_101",&saved)==ESP_OK&&saved.dirty&&saved.position_pending&&
            saved.chapter_offset==expected&&saved.progress==44&&saved.reading_time==1&&!strcmp(saved.summary,"navigation"));
        weread_reader_end(2300);book_weread_close();
        assert(weread_runtime_idle(2500));started=time(NULL);
        while(weread_runtime_busy()){assert(time(NULL)-started<10);usleep(1000);}
        assert(weread_runtime_poll(&result)&&result.job==WR_JOB_REPORT&&result.error==ESP_OK);weread_runtime_result_free(&result);
        assert(reports==1&&reported_offset==expected&&reported_percent==44&&reported_seconds==1);
        assert(weread_progress_load("test_book_101",&saved)==ESP_OK&&!saved.dirty&&!saved.position_pending&&!strcmp(saved.summary,"navigation"));
        weread_runtime_stop();weread_shelf_clear();
        puts("Position protection: untouched open, read time, reflow and committed navigation passed.");return 0;
    }
    // 校验后的原始坐标持久化，冷恢复不需要先生成页首检查点。
    // Validated protocol coordinates survive cold resume without a page-start checkpoint.
    assert(weread_reader_resume(uri, &chapter, &offset) == ESP_OK && offset == 15);
    assert(weread_progress_note("test_book_101", 2, 2, 20, 86, 0, 1, "fixture", 1700000200) == ESP_OK);
    chapter = 42; offset = 99;
    assert(weread_reader_resume(uri, &chapter, &offset) == ESP_OK && chapter == 1 && offset == 15);
    assert(downloads == (selfheal ? 2u : 1u));
    book_weread_close();weread_runtime_stop();
    unsigned before_toc=toc_requests;
    assert(weread_runtime_start(WR_JOB_OPEN,"test_book_101",NULL));started=time(NULL);
    while(weread_runtime_busy()){assert(time(NULL)-started<10);usleep(1000);}
    assert(weread_runtime_poll(&result)&&result.open_ready&&result.error==ESP_OK&&!result.conflict);
    assert(toc_requests==before_toc&&weread_reader_open_ready("weread://test_book_101")&&downloads==(selfheal?2u:1u));
    weread_runtime_result_free(&result);weread_runtime_stop();weread_shelf_clear();
    puts(selfheal ? "Open-time version recovery passed." : "Updated-book open passed."); return 0;
}
