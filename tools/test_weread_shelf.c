/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：直接验证生产 C 书架与目录模块的事务、离线与账号边界。
 * English: Exercise production C shelf/TOC transactions, offline reads and account boundaries.
 */
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

static int mode, batches, calls, toc_mode,last_batch;
static char root[] = "/tmp/kiiko-weread-shelf-XXXXXX";

#ifdef WEREAD_TEST_FAT_ROOT
// FatFS不能mkdir挂载根目录，返回EINVAL而非POSIX的EEXIST。
// FatFS rejects mkdir of a mount root with EINVAL rather than POSIX EEXIST.
int weread_test_mkdir(const char *path, mode_t permissions) {
    if(!strcmp(path,root)){errno=EINVAL;return -1;}
    return mkdirat(AT_FDCWD,path,permissions);
}
#endif

static char *reply(const char *text) { char *p = strdup(text); assert(p); return p; }
static esp_err_t transport(const char *url, const char *method, const char *headers,
                          const char *body, char **out, size_t *len, int *status, void *ctx) {
    (void)ctx; (void)headers; ++calls; *status = 200;
    assert(strstr(url, "https://i.weread.qq.com/"));
    if (strstr(url, "/shelf/syncbook")) {
        assert(!strcmp(method, "POST")); ++batches;
        if (mode == 3 && batches == 2) return ESP_ERR_TIMEOUT;
        cJSON *req = cJSON_Parse(body), *ids = cJSON_GetObjectItem(req, "bookIds");
        int n = cJSON_GetArraySize(ids);
        last_batch=n;
        assert(n > 0 && n <= 20 && cJSON_IsArray(cJSON_GetObjectItem(req, "albumIds")));
        cJSON *r = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(r, "books"), *v;
        cJSON_ArrayForEach(v, ids) {
            if((mode==7||mode==8)&&!strcmp(v->valuestring,"book_02"))continue;
            cJSON *b = cJSON_CreateObject();
            cJSON_AddStringToObject(b, "bookId", v->valuestring);
            cJSON_AddStringToObject(b, "title", "一份真实 C 缓存");
            cJSON_AddStringToObject(b, "format", mode==9&&!strcmp(v->valuestring,"book_02")?"audio":"epub");
            cJSON_AddNumberToObject(b, "bookVersion", mode>=8?13:12);
            cJSON_AddItemToArray(a, b);
        }
        *out = cJSON_PrintUnformatted(r); cJSON_Delete(r); cJSON_Delete(req);
    } else if (strstr(url, "/shelf/sync?")) {
        assert(!strcmp(method, "GET"));
        if (mode == 1) {
            assert(strstr(url, "synckey=100&onlyBookid=1"));
            *out = reply("{\"synckey\":101,\"bookIds\":[\"C\"],\"bookProgress\":[{\"bookId\":\"C\",\"progress\":46,\"updateTime\":999}],\"removed\":[{\"bookId\":\"B\"}]}");
        } else if (mode == 8 || mode == 9 || mode == 11) {
            assert(strstr(url, mode==9||mode==11?"synckey=201&":"synckey=200&"));
            *out=reply(mode==9?"{\"synckey\":202,\"bookIds\":[\"book_02\"]}":
                "{\"synckey\":201,\"bookIds\":[\"book_02\",\"book_03\"]}");
        } else if (mode == 2 || mode == 3 || mode==7 || mode==10) {
            cJSON *r = cJSON_CreateObject(), *ids = cJSON_AddArrayToObject(r, "bookIds");
            if(mode==10)assert(strstr(url,"synckey=200&"));
            for (int i = 0; i < 45; ++i) {
                if(mode==10&&i==2)continue;
                char id[32]; snprintf(id, sizeof(id), "book_%02d", i);
                cJSON_AddItemToArray(ids, cJSON_CreateString(id));
            }
            if(mode==10){cJSON *removed=cJSON_AddArrayToObject(r,"removed");cJSON_AddItemToArray(removed,cJSON_CreateString("book_02"));}
            cJSON_AddNumberToObject(r, "synckey", mode==10?202:200);
            *out = cJSON_PrintUnformatted(r); cJSON_Delete(r);
        } else if (mode == 4) *out = reply("{\"synckey\":300,\"bookIds\":[\"../../escape\"]}");
        else if (mode == 5) *out = reply("{\"synckey\":300,\"books\":[]}");
        else if (mode == 6) *out = reply("{\"synckey\":301,\"books\":{}}");
        else {
        assert(strstr(url,"synckey=0")&&strstr(url,"lectureSynckey=0")&&strstr(url,"onlyBookid=1"));
            *out = reply("{\"synckey\":100,\"books\":[{\"bookId\":\"A\",\"title\":\"甲\",\"format\":\"epub\",\"readUpdateTime\":2,\"bookVersion\":12},{\"bookId\":\"B\",\"title\":\"乙\",\"format\":\"txt\",\"readUpdateTime\":3},{\"bookId\":\"C\",\"title\":\"丙\",\"format\":\"epub\",\"readUpdateTime\":1},{\"bookId\":\"audio\",\"format\":\"audio\"},{\"bookId\":\"lecture\",\"title\":\"讲书\"}],\"lectureBooks\":[{\"bookId\":\"lecture\"}]}");
        }
    } else if (strstr(url, "/book/chapterInfos")) {
        cJSON *req = cJSON_Parse(body);
        assert(!strcmp(cJSON_GetArrayItem(cJSON_GetObjectItem(req, "bookIds"), 0)->valuestring, "A"));
        double key = cJSON_GetArrayItem(cJSON_GetObjectItem(req, "synckeys"), 0)->valuedouble;
        double expected_key = toc_mode == 0 ? 0 : toc_mode == 1 ? 10 : toc_mode <= 3 ? 11 : toc_mode == 4 ? 12 : toc_mode == 5 ? 13 : 14;
        assert(key == expected_key);
        cJSON_Delete(req);
        if (toc_mode == 0) *out = reply("{\"data\":[{\"bookId\":\"A\",\"synckey\":10,\"clearAll\":1,\"bookVersion\":12,\"updated\":[{\"chapterUid\":90,\"chapterIdx\":2,\"title\":\"第二章\",\"updateTime\":9},{\"chapterUid\":80,\"chapterIdx\":1,\"title\":\"第一章\"}],\"removed\":[]}]}");
        else if (toc_mode == 1) *out = reply("{\"data\":[{\"bookId\":\"A\",\"synckey\":11,\"clearAll\":0,\"updated\":[{\"chapterUid\":70,\"chapterIdx\":3,\"title\":\"第三章\"}],\"removed\":[80]}]}");
        else if (toc_mode == 2) *out = reply("{\"data\":[{\"bookId\":\"A\",\"synckey\":12,\"clearAll\":1,\"bookVersion\":13,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"新版本\"}],\"removed\":[]}]}");
        else if (toc_mode == 4) *out = reply("{\"data\":[{\"bookId\":\"A\",\"synckey\":13,\"clearAll\":1,\"bookVersion\":13,\"updated\":[{\"chapterUid\":2,\"chapterIdx\":1,\"title\":\"\\\"quoted\\\" \\\\ slash \\n\\t\\u0001\",\"anchors\":[{\"title\":\"x\"}],\"tar\":\"deadbeef\",\"paid\":1,\"files\":[\"a.xhtml\"]}],\"removed\":[]}]}");
        else if (toc_mode == 5) *out = reply("{\"data\":[{\"bookId\":\"A\",\"synckey\":14,\"clearAll\":1,\"bookVersion\":13,\"updated\":[{\"chapterUid\":50,\"chapterIdx\":1,\"title\":\"old\"},{\"chapterUid\":50,\"chapterIdx\":2,\"title\":\"new\"}],\"removed\":[]}]}");
        else if (toc_mode == 6) *out = reply("{\"data\":[{\"bookId\":\"A\",\"synckey\":99,\"clearAll\":1,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"bad\"}]}],\"errcode\":-1}");
        else if (toc_mode == 7) *out = reply("{\"data\":[]}garbage");
        else if (toc_mode == 8) *out = reply("{\"data\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[0]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]}");
        else *out = reply("{\"data\":[{\"bookId\":\"A\",\"synckey\":12,\"clearAll\":1,\"updated\":[{\"chapterUid\":0,\"chapterIdx\":1,\"title\":\"损坏\"}]}]}");
    } else { assert(false); }
    *len = strlen(*out); return ESP_OK;
}
static const weread_shelf_item_t *find_item(const weread_shelf_item_t *items, size_t n, const char *id) {
    for (size_t i = 0; i < n; ++i) if (!strcmp(items[i].book_id, id)) return &items[i];
    return NULL;
}
static void test_chapter_counts_and_expired_shelf(void) {
    weread_shelf_item_t *items = NULL; size_t n = 0;
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK && n == 3);
    const weread_shelf_item_t *a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 0 && a->total_chapters == 0);
    free(items);

    assert(weread_shelf_set_chapter_counts("A", 3, 10) == ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 3 && a->total_chapters == 10);
    free(items);

    weread_shelf_clear();
    assert(weread_shelf_load() == ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 3 && a->total_chapters == 10);
    free(items);

    /* Clamping cached > total */
    assert(weread_shelf_set_chapter_counts("A", 25, 10) == ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 10 && a->total_chapters == 10);
    free(items);

    /* Invalid / missing IDs */
    assert(weread_shelf_set_chapter_counts("../bad", 1, 2) == ESP_ERR_INVALID_ARG);
    assert(weread_shelf_set_chapter_counts("missing_book", 1, 2) == ESP_ERR_NOT_FOUND);

    /* No-op short-circuit & write-failure rollback */
    char shelf_tmp[512];
    snprintf(shelf_tmp, sizeof(shelf_tmp), "%s/shelf.json.tmp", root);
    assert(weread_test_mkdir(shelf_tmp, 0700) == 0);
    assert(weread_shelf_set_chapter_counts("A", 10, 10) == ESP_OK);
    assert(weread_shelf_set_chapter_counts("A", 2, 10) != ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 10 && a->total_chapters == 10);
    free(items);
    assert(rmdir(shelf_tmp) == 0);

    /* Preserved across single-book refresh and full sync when bookVersion unchanged */
    mode = 0;
    assert(weread_shelf_refresh_book("A") == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 10 && a->total_chapters == 10);
    free(items);

    /* Cleared to 0/0 when bookVersion bumps via merge_book (shelf_refresh_book) or set_version, while OOM/save failure preserves old counts */
    mode = 8;
    assert(weread_shelf_refresh_book("A") == ESP_OK);
    assert(weread_shelf_book_version("A") == 13);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 0 && a->total_chapters == 0);
    free(items);
    assert(weread_shelf_set_chapter_counts("A", 5, 10) == ESP_OK);
    assert(weread_shelf_set_version("A", 13) == ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 5 && a->total_chapters == 10);
    free(items);
    assert(weread_test_mkdir(shelf_tmp, 0700) == 0);
    assert(weread_shelf_set_version("A", 14) != ESP_OK);
    assert(weread_shelf_book_version("A") == 13);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 5 && a->total_chapters == 10);
    free(items);
    assert(rmdir(shelf_tmp) == 0);
    assert(weread_shelf_set_version("A", 14) == ESP_OK);
    assert(weread_shelf_book_version("A") == 14);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 0 && a->total_chapters == 0);
    free(items);
    weread_shelf_clear();
    assert(weread_shelf_load() == ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 0 && a->total_chapters == 0);
    free(items);
    assert(weread_shelf_set_version("A", 12) == ESP_OK);
    mode = 0;

    /* Reset cached count to 0 while keeping total */
    assert(weread_shelf_set_chapter_counts("A", 0, 10) == ESP_OK);
    assert(weread_shelf_snapshot(&items, &n) == ESP_OK);
    a = find_item(items, n, "A");
    assert(a && a->cached_chapters == 0 && a->total_chapters == 10);
    free(items);

    /* WR-UX-01a expired mode: local shelf/TOC/counts work; network sync/refresh blocked */
    assert(weread_auth_mark_expired() == ESP_OK);
    assert(weread_auth_is_expired() && !weread_auth_is_authenticated());
    int calls_before = calls;
    weread_shelf_clear();
    assert(weread_shelf_load() == ESP_OK && weread_shelf_count() == 3);
    assert(weread_shelf_set_chapter_counts("A", 4, 10) == ESP_OK);
    weread_toc_t toc = {0};
    assert(weread_toc_load("A", &toc) == ESP_OK && toc.count == 1);
    weread_toc_free(&toc);
    assert(weread_shelf_sync(false) == ESP_ERR_INVALID_STATE);
    assert(weread_shelf_refresh_book("A") == ESP_ERR_INVALID_STATE);
    assert(calls == calls_before);
}
static void account(const char *vid) {
    weread_auth_session_t session = {0};
    strcpy(session.vid, vid); strcpy(session.access_token, "test-only-token");
    strcpy(session.refresh_token, "test-only-refresh");
    assert(weread_auth_save_session(&session) == ESP_OK);
    assert(weread_auth_load_session(&session) == ESP_OK);
}
int main(void) {
    assert(mkdtemp(root)); assert(weread_auth_init(root) == ESP_OK); account("123");
    weread_client_set_transport(transport, NULL);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_shelf_count() == 3 && weread_shelf_synckey() == 100);
    uint64_t now=(uint64_t)time(NULL);
    assert(weread_shelf_cache_fresh(now)&&!weread_shelf_cache_fresh(now+301)&&!weread_shelf_cache_fresh(now-2));
    uint64_t cover_key=weread_cover_key("A","https://cdn.qq.com/one");
    assert(cover_key!=weread_cover_key("A","https://cdn.qq.com/two")&&cover_key!=weread_cover_key("B","https://cdn.qq.com/one"));
    char cover_path[512];assert(weread_cover_cache_path("A","gray",cover_path,sizeof(cover_path))==ESP_OK&&strstr(cover_path,"/cover-"));
    assert(weread_cover_cache_path("A","../escape",cover_path,sizeof(cover_path))!=ESP_OK);
    weread_book_t book;
    assert(weread_shelf_get(0, &book) && !strcmp(book.book_id, "B"));
    assert(!weread_shelf_find("audio", &book) && !weread_shelf_find("lecture", &book));
    mode = 1; assert(weread_shelf_sync(false) == ESP_OK);
    assert(weread_shelf_count() == 2 && !weread_shelf_find("B", &book));
    assert(weread_shelf_get(0, &book) && !strcmp(book.book_id, "C") && book.progress == 46);
    weread_shelf_clear(); int before = calls;
    assert(weread_shelf_load() == ESP_OK && calls == before && weread_shelf_synckey() == 101);
    mode = 4; assert(weread_shelf_sync(false) != ESP_OK && weread_shelf_synckey() == 101);
    mode = 6; assert(weread_shelf_sync(false) != ESP_OK && weread_shelf_count() == 2);
    mode = 3; batches = 0; assert(weread_shelf_sync(true) == ESP_ERR_TIMEOUT);
    assert(batches == 2 && weread_shelf_count() == 2 && weread_shelf_synckey() == 101);
    mode = 2; batches = 0; assert(weread_shelf_sync(true) == ESP_OK);
    assert(batches == 3 && weread_shelf_count() == 45);
    weread_shelf_clear();batches=0;
    assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_count()==6&&last_batch==6&&weread_shelf_sync_pending());
    assert(weread_shelf_synckey()==200);
    assert(!weread_shelf_cache_fresh((uint64_t)time(NULL)));
    mode=3;assert(weread_shelf_sync_step(false)==ESP_ERR_TIMEOUT&&weread_shelf_count()==6);
    assert(!weread_shelf_sync_pending()&&weread_shelf_load()==ESP_OK&&weread_shelf_count()==6);
    // 中断后从已持久化原生游标取成员删除增量，保留成功详情。/ A restart applies native removal deltas and retains successful metadata.
    mode=10;assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_count()>=6);
    assert(!weread_shelf_find("book_02",&book));
    while(weread_shelf_sync_pending())assert(weread_shelf_sync_step(false)==ESP_OK);
    assert(weread_shelf_count()==44&&weread_shelf_synckey()==202);
    weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&!weread_shelf_find("book_02",&book));
    weread_shelf_clear();
    mode=2;assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_count()==6);
    assert(weread_shelf_sync_step(false)==ESP_OK&&weread_shelf_count()==26&&last_batch==20);
    assert(weread_shelf_sync_step(false)==ESP_OK&&weread_shelf_count()==45&&!weread_shelf_sync_pending()&&weread_shelf_synckey()==200);
    // 已有书详情遗漏也须重试其版本变更；原子接口回滚，渐进接口持久待补队列和原生游标。
    // Atomic sync rolls back; staged sync commits the cursor together with the unresolved detail queue.
    mode=8;assert(weread_shelf_sync(false)==ESP_ERR_INVALID_RESPONSE&&weread_shelf_synckey()==200);
    assert(weread_shelf_find("book_03",&book)&&book.book_version==12);
    assert(weread_shelf_sync_step(true)==ESP_OK&&!weread_shelf_sync_pending()&&weread_shelf_synckey()==201);
    size_t done,total,skipped;weread_shelf_sync_progress(&done,&total,&skipped);
    assert(done==2&&total==2&&skipped==1&&weread_shelf_find("book_02",&book)&&book.book_version==12);
    assert(weread_shelf_find("book_03",&book)&&book.book_version==13);
    weread_shelf_clear();assert(weread_shelf_load()==ESP_OK&&weread_shelf_synckey()==201);
    mode=11;assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_synckey()==201);
    assert(weread_shelf_find("book_02",&book)&&book.book_version==13);
    // 明确返回audio不是遗漏，可移除旧书并提交游标。/ An explicit audio result is not omitted: remove the old book and commit the cursor.
    mode=9;assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_synckey()==202);
    weread_shelf_sync_progress(&done,&total,&skipped);
    assert(skipped==0&&weread_shelf_count()==44&&!weread_shelf_find("book_02",&book));
    mode=2;assert(weread_shelf_sync(true)==ESP_OK&&weread_shelf_count()==45);
    mode=7;assert(weread_shelf_sync(true)==ESP_ERR_INVALID_RESPONSE&&weread_shelf_count()==45);
    weread_shelf_clear();assert(weread_shelf_sync_step(true)==ESP_OK&&weread_shelf_count()==5);
    while(weread_shelf_sync_pending())assert(weread_shelf_sync_step(false)==ESP_OK);
    weread_shelf_sync_progress(&done,&total,&skipped);
    assert(weread_shelf_count()==44&&done==45&&total==45&&skipped==1&&weread_shelf_synckey()==200);
    assert(weread_shelf_cache_fresh((uint64_t)time(NULL)));
    assert(!weread_shelf_cache_fresh((uint64_t)time(NULL)+301));
    mode=2;assert(weread_shelf_sync(true)==ESP_OK&&weread_shelf_count()==45);
    char tmp[512]; snprintf(tmp, sizeof(tmp), "%s/shelf.json.tmp", root); assert(!mkdir(tmp, 0700));
    mode = 5; assert(weread_shelf_sync(true) != ESP_OK && weread_shelf_count() == 45);
    assert(!rmdir(tmp)); assert(weread_shelf_load() == ESP_OK && weread_shelf_count() == 45);
    mode = 0; assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("A") == ESP_OK);
    weread_toc_t toc = {0}; assert(weread_toc_load("A", &toc) == ESP_OK);
    assert(toc.count == 2 && toc.chapters[0].uid == 80 && toc.book_version == 12); weread_toc_free(&toc);
    toc_mode = 1; assert(weread_toc_sync("A") == ESP_OK); assert(weread_toc_load("A", &toc) == ESP_OK);
    assert(toc.count == 2 && toc.chapters[0].uid == 90 && toc.chapters[1].uid == 70); weread_toc_free(&toc);
    toc_mode = 3; assert(weread_toc_sync("A") != ESP_OK); assert(weread_toc_load("A", &toc) == ESP_OK);
    assert(toc.synckey == 11 && toc.count == 2); weread_toc_free(&toc);
    toc_mode = 2; assert(weread_toc_sync("A") == ESP_OK); assert(weread_toc_load("A", &toc) == ESP_OK);
    assert(toc.count == 1 && toc.chapters[0].uid == 1 && toc.book_version == 13); weread_toc_free(&toc);
    toc_mode = 4; assert(weread_toc_sync("A") == ESP_OK); assert(weread_toc_load("A", &toc) == ESP_OK);
    assert(toc.count == 1 && toc.chapters[0].uid == 2 && !strcmp(toc.chapters[0].title, "\"quoted\" \\ slash \n\t\x01"));
    weread_toc_free(&toc);
    {
        char toc_path[512], raw_disk[2048];
        assert(wr_cache_path("A", "chapters.json", toc_path, sizeof(toc_path)) == ESP_OK);
        FILE *tf = fopen(toc_path, "rb"); assert(tf);
        size_t rn = fread(raw_disk, 1, sizeof(raw_disk) - 1, tf); fclose(tf); raw_disk[rn] = 0;
        assert(!strstr(raw_disk, "anchors") && !strstr(raw_disk, "deadbeef") && !strstr(raw_disk, "\"paid\"") && !strstr(raw_disk, "\"files\""));
    }
    toc_mode = 5; assert(weread_toc_sync("A") == ESP_OK); assert(weread_toc_load("A", &toc) == ESP_OK);
    assert(toc.count == 1 && toc.chapters[0].uid == 50 && toc.chapters[0].index == 2 && !strcmp(toc.chapters[0].title, "new"));
    weread_toc_free(&toc);
    toc_mode = 6; assert(weread_toc_sync("A") != ESP_OK);
    toc_mode = 7; assert(weread_toc_sync("A") != ESP_OK);
    toc_mode = 8; assert(weread_toc_sync("A") != ESP_OK);
    assert(weread_toc_load("A", &toc) == ESP_OK && toc.count == 1 && toc.chapters[0].uid == 50);
    weread_toc_free(&toc);
    assert(weread_toc_sync("../escape") == ESP_ERR_INVALID_ARG);
    account("456"); assert(weread_shelf_count() == 0 && weread_shelf_load() != ESP_OK);
    assert(!weread_shelf_cache_fresh((uint64_t)time(NULL))&&cover_key!=weread_cover_key("A","https://cdn.qq.com/one"));
    assert(weread_toc_load("A", &toc) != ESP_OK); account("123");
    assert(weread_shelf_load() == ESP_OK);
    char url[128]; assert(weread_cover_get_url("http://cdn.qq.com/s_cover?x=/s_query", url, sizeof(url)) == ESP_OK);
    assert(!strcmp(url, "https://cdn.qq.com/t6_cover?x=/s_query"));
    assert(weread_cover_get_url("https://cdn.qq.com/file?x=/s_query", url, sizeof(url)) == ESP_OK);
    assert(!strcmp(url, "https://cdn.qq.com/file?x=/s_query"));
    assert(weread_cover_get_url("https://cdn.qq.com/s_cover", url, 8) == ESP_ERR_INVALID_SIZE);
    assert(weread_cover_get_url("file:///etc/passwd", url, sizeof(url)) != ESP_OK);
    weread_cover_retry_t retry = {0};
    weread_cover_retry_result(&retry, 500, 100); assert(retry.not_before_ms == 5100);
    assert(weread_cover_retry_url("https://cdn.qq.com/s_x", &retry, 101, url, sizeof(url)) == ESP_ERR_TIMEOUT);
    weread_cover_retry_result(&retry, 500, 5100); assert(retry.not_before_ms == 20100);
    weread_cover_retry_result(&retry, 500, 20100); assert(retry.not_before_ms == 65100);
    weread_cover_retry_result(&retry, 404, 65100);
    assert(weread_cover_retry_url("http://cdn.qq.com/s_x", &retry, 65100, url, sizeof(url)) == ESP_OK);
    assert(!strcmp(url, "https://cdn.qq.com/s_x"));
    weread_cover_retry_result(&retry, 200, 65101); assert(!retry.failures && !retry.original);
    test_chapter_counts_and_expired_shelf();
    weread_shelf_clear(); weread_auth_clear_session(); weread_client_set_transport(NULL, NULL);
    puts("WeRead shelf/TOC: transaction, batching, rollback, offline, isolation and cover tests passed.");
    return 0;
}
