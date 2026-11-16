/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：真实加密归档的生产章节下载、流式解包与离线缓存验证。
 * English: Test production chapter downloads, stream extraction and offline caches with real encrypted archives.
 */
#include "weread_chapter.h"
#include "weread_shelf.h"
#include "weread_auth.h"
#include "book_weread_crypto.h"
#include "book_source_weread.h"
#include "weread_position.h"
#include "weread_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int mode, downloads, requests;
static bool stop;
static bool cancel(void *ctx) { (void)ctx; return stop; }
static esp_err_t request(const char *url, const char *method, const char *headers, const char *body,
                        char **out, size_t *len, int *status, void *ctx) {
    (void)method; (void)headers; (void)body; (void)ctx; ++requests; *status = 200;
    const char *r = strstr(url, "/book/chapterInfos") ?
        "{\"data\":[{\"bookId\":\"test_book_101\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":100,\"updated\":[{\"chapterUid\":1,\"chapterIdx\":1,\"title\":\"第一章\"}]}]}" :
        "{\"synckey\":1,\"books\":[{\"bookId\":\"test_book_101\",\"title\":\"测试书\",\"format\":\"epub\",\"bookVersion\":100},{\"bookId\":\"txt_book_202\",\"title\":\"测试 TXT\",\"format\":\"txt\",\"bookVersion\":1}]}";
    *out = strdup(r); assert(*out); *len = strlen(*out); return ESP_OK;
}
static esp_err_t download(const char *url, const char *headers, FILE *out, size_t limit,
    weread_cancel_fn cancelled, void *cancel_ctx, weread_download_info_t *info, void *ctx) {
    (void)ctx; ++downloads;
    assert(strstr(headers, "accessToken: test-token"));
    if (mode == 1) { info->status = 402; return ESP_OK; }
    if (mode == 2) { info->status = 499; return ESP_OK; }
    if (mode == 3) {
        const char *r = "{\"errcode\":-2063}"; info->status = 499;
        info->bytes = strlen(r); assert(fwrite(r, 1, info->bytes, out) == info->bytes);
        mode = 0; return ESP_OK;
    }
    bool txt = strstr(url, "bookType=txt") != NULL;
    if (!txt) assert(strstr(url, "chapters=0,") || strstr(url, "chapters=2"));
    const char *fixture = txt ? "tools/fixtures/weread/sample_txt_chapter1.tar" :
        mode == 7 ? "build/weread-tests/no-version.zip" :
        mode == 4 ? "tools/fixtures/weread/corrupted_crc.zip" :
        mode == 5 ? "tools/fixtures/weread/path_traversal_attack.zip" : "tools/fixtures/weread/sample_epub_chapter0_and_1.zip";
    FILE *f = fopen(fixture, "rb"); assert(f);
    info->status = 200;
    strcpy(info->encrypt_key, "pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
    char buf[97]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f))) {
        if (mode == 6) stop = true;
        if (cancelled && cancelled(cancel_ctx)) { fclose(f); return ESP_ERR_INVALID_STATE; }
        assert(n <= limit - info->bytes); assert(fwrite(buf, 1, n, out) == n); info->bytes += n;
    }
    fclose(f); return ESP_OK;
}
static size_t expanded, chunks;
static size_t random_expanded;
static bool random_sink(void *ctx,const char *name,const uint8_t *data,size_t n,size_t off,size_t total){
    (void)ctx;(void)data;assert(!strcmp(name,"random.bin")&&n<=4096&&off==random_expanded&&total==300000);random_expanded+=n;return true;
}
static bool large_sink(void *ctx, const char *name, const uint8_t *data, size_t n, size_t off, size_t total) {
    (void)ctx; assert(!strcmp(name, "large.txt")); assert(n <= 4096 && off == expanded && total == 2u * 1024u * 1024u);
    for (size_t i = 0; i < n; ++i) assert(data[i] == 'x');
    expanded += n; ++chunks; return true;
}
int main(void) {
    char root[] = "/tmp/kiiko-weread-chapter-XXXXXX"; assert(mkdtemp(root));
    assert(weread_auth_init(root) == ESP_OK);
    weread_auth_session_t session = {0}; strcpy(session.vid, "123");
    strcpy(session.access_token, "test-token"); strcpy(session.refresh_token, "test-refresh");
    assert(weread_auth_save_session(&session) == ESP_OK && weread_auth_load_session(&session) == ESP_OK);
    weread_client_set_transport(request, NULL); weread_client_set_download_transport(download, NULL);
    weread_chapter_set_cancel(cancel, NULL);
    assert(weread_shelf_sync(true) == ESP_OK);
    char *text = NULL; size_t len = 0;
    mode = 1; assert(weread_chapter_load("test_book_101", 1, &text, &len) == WEREAD_ERR_PURCHASE && !text);
    mode = 2; assert(weread_chapter_load("test_book_101", 1, &text, &len) == WEREAD_ERR_SOLD_OUT && !text);
    mode = 4; assert(weread_chapter_load("test_book_101", 1, &text, &len) == ESP_ERR_INVALID_CRC && !text);
    mode = 5; assert(weread_chapter_load("test_book_101", 1, &text, &len) != ESP_OK && !text);
    mode = 6; assert(weread_chapter_load("test_book_101", 1, &text, &len) != ESP_OK && !text); stop = false;
    mode = 3; int before = downloads;
    assert(weread_chapter_load("test_book_101", 1, &text, &len) == ESP_OK);
    const char *first = strstr(text, "微风吹过荒原"), *second = strstr(text, "探索者踏上了新的征程");
    assert(downloads == before + 2 && len && first && second && first < second); free(text);
    char dir[512]; assert(weread_chapter_resource_directory("test_book_101", dir, sizeof(dir)) == ESP_OK);
    before = downloads; int req_before = requests;
    assert(weread_chapter_load("test_book_101", 1, &text, &len) == ESP_OK); free(text);
    assert(downloads == before && requests == req_before);
    mode=7;assert(weread_chapter_load("test_book_101",2,&text,&len)==ESP_OK&&strstr(text,"真实清单兼容"));free(text);mode=0;
    assert(book_weread_open("weread://test_book_101") == ESP_OK && book_weread_count() == 1);
    struct stat identity;assert(book_weread_stat("weread://test_book_101",&identity)&&identity.st_size>0&&identity.st_size<=INT32_MAX);
    html_text_t parsed = {0};
    assert(book_weread_load(0, NULL, NULL, &parsed) == ESP_OK);
    assert(parsed.count >= 3 && strstr(parsed.utf8, "微风吹过荒原") && strstr(parsed.utf8, "探索者踏上了新的征程"));
    assert(!strstr(parsed.utf8, "<html"));
    uint32_t co=0,back=0;
    uint32_t local=(uint32_t)(strstr(parsed.utf8,"微风吹过荒原")-parsed.utf8);
    assert(book_weread_map(0,true,local,&co)==ESP_OK && co==207);
    assert(book_weread_map(0,false,co,&back)==ESP_OK && back==local);
    local=(uint32_t)(strstr(parsed.utf8,"探索者踏上了新的征程")-parsed.utf8);
    assert(book_weread_map(0,true,local,&co)==ESP_OK && co==439);
    assert(book_weread_map(0,false,co,&back)==ESP_OK && back==local);
    html_text_free(&parsed);
    // 单独覆盖代理对、实体、标签和反向映射。/ Cover surrogate pairs, entities, markup and inverse mapping.
    assert(weread_utf16_length("甲😀乙",strlen("甲😀乙"))==4);
    assert(weread_chapter_directory("test_book_101",1,dir,sizeof(dir))==ESP_OK);
    const char *html="<p>甲😀&amp;乙</p>";char map_path[600],body_path[600];
    snprintf(body_path,sizeof(body_path),"%s/body.html",dir);
    assert(wr_write_bytes(body_path,html,strlen(html))==ESP_OK);
    struct {weread_position_header_t h;weread_position_point_t p[2];} map={
        {WEREAD_POSITION_MAGIC,2,(uint32_t)strlen(html),16},{{0,0},{(uint32_t)strlen(html),16}}};
    snprintf(map_path,sizeof(map_path),"%s/position.map",dir);
    assert(wr_write_bytes(map_path,&map,sizeof(map))==ESP_OK);
    assert(book_weread_load(0,NULL,NULL,&parsed)==ESP_OK);
    local=(uint32_t)(strstr(parsed.utf8,"乙")-parsed.utf8);
    assert(book_weread_map(0,true,local,&co)==ESP_OK && co==11);
    assert(book_weread_map(0,false,co,&back)==ESP_OK && back==local);
    local=(uint32_t)(strstr(parsed.utf8,"&")-parsed.utf8);
    assert(book_weread_map(0,true,local,&co)==ESP_OK && co==6);
    html_text_free(&parsed);book_weread_close();
    mode = 0; assert(weread_chapter_load("txt_book_202", 1, &text, &len) == ESP_OK);
    assert(len && strstr(text, "序章")); free(text);
    assert(weread_chapter_directory("../escape", 1, dir, sizeof(dir)) == ESP_ERR_INVALID_ARG);
    assert(weread_resource_path("Text/ch1.xhtml", "../Images/cover.jpg", dir, sizeof(dir)) && !strcmp(dir, "Images/cover.jpg"));
    assert(!weread_resource_path(NULL, "../escape", dir, sizeof(dir)));
    assert(!weread_resource_path(NULL, "https://qq.com/x", dir, sizeof(dir)));
    FILE *large = fopen("build/weread-tests/large.zip", "rb"); assert(large);
    assert(book_weread_unpack_zip_file(large, NULL, 0, large_sink, NULL) == ESP_OK); fclose(large);
    assert(expanded == 2u * 1024u * 1024u && chunks >= 512);
    large=fopen("build/weread-tests/multichunk.zip","rb");assert(large);
    assert(book_weread_unpack_zip_file(large,NULL,0,random_sink,NULL)==ESP_OK&&random_expanded==300000);fclose(large);
    weread_shelf_clear(); weread_auth_clear_session();
    puts("WeRead chapters: encrypted ZIP, TAR, ordered assembly, offline, cancellation, 402/499/-2063 and bounded inflation passed.");
    return 0;
}
