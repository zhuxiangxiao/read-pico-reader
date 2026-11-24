/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：真实加密归档的生产章节下载、流式解包与离线缓存验证。
 * English: Test production chapter downloads, stream extraction and offline caches with real encrypted archives.
 */
#include "weread_chapter.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_auth.h"
#include "book_weread_crypto.h"
#include "book_source_weread.h"
#include "weread_position.h"
#include "weread_store.h"
#include <assert.h>
#include <dirent.h>
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
    const char *r = strstr(url, "/book/chapterInfos") ? mode==9?
        "{\"data\":[{\"bookId\":\"test_book_101\",\"synckey\":3,\"clearAll\":1,\"bookVersion\":100,\"updated\":[{\"chapterUid\":4,\"chapterIdx\":1,\"title\":\"混淆章\"}]}]}":
        mode==8?
        "{\"data\":[{\"bookId\":\"test_book_101\",\"synckey\":2,\"clearAll\":1,\"bookVersion\":100,\"updated\":[{\"chapterUid\":3,\"chapterIdx\":1,\"title\":\"脚注章\"}]}]}":
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
    if (!txt) assert(strstr(url, "chapters=0,") || strstr(url, "chapters=2") || strstr(url,"chapters=3") || strstr(url,"chapters=4"));
    const char *fixture = txt ? "tools/fixtures/weread/sample_txt_chapter1.tar" :
        mode == 9 ? "build/weread-tests/obfuscated.zip" :
        mode == 8 ? "build/weread-tests/notes.zip" : mode == 7 ? "build/weread-tests/no-version.zip" :
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
static size_t source_file(const char *root,const char *leaf,char *out,size_t cap) {
    char path[600];snprintf(path,sizeof(path),"%s/%s",root,leaf);
    FILE *f=fopen(path,"rb");assert(f);size_t n=fread(out,1,cap-1,f);assert(feof(f));fclose(f);out[n]=0;return n;
}
static void merged_notes(void) {
    mode=8;char *body=NULL;size_t len=0;html_text_t parsed={0};
    assert(weread_chapter_load("test_book_101",3,&body,&len)==ESP_OK);
    // 同一文档纯片段和合并文档间相对链接映射到隔离别名，保留原id与未合并链接。
    // Local fragments and links between merged documents share isolated aliases; original IDs and unmerged links remain.
    assert(!strstr(body,"Text#note")&&strstr(body,"id=\"note\"")&&strstr(body,"Other/missing.xhtml#note"));
    free(body);
    assert(weread_toc_sync("test_book_101")==ESP_OK&&book_weread_open("weread://test_book_101")==ESP_OK);
    assert(book_weread_load(0,NULL,NULL,&parsed)==ESP_OK&&parsed.note_count==4&&parsed.image_count==1);
    assert(!strcmp(parsed.images[0],"Images/picture.png"));
    const char *notes[]={"第一注","第二注","第二注","第三注"};
    for(size_t i=0;i<4;++i)assert(parsed.notes[i].text_len>=strlen(notes[i])&&
        !memcmp(parsed.utf8+parsed.notes[i].text_start,notes[i],strlen(notes[i])));
    for(size_t i=0;i<parsed.count;++i) {
        if(!strncmp(parsed.utf8+parsed.blocks[i].offset,"第一注",strlen("第一注")))assert(parsed.blocks[i].align==2);
        if(!strncmp(parsed.utf8+parsed.blocks[i].offset,"第三注",strlen("第三注")))assert(parsed.blocks[i].align==1);
    }
    char dir[512],original[2048];assert(weread_chapter_directory("test_book_101",3,dir,sizeof(dir))==ESP_OK);
    const char *files[]={"Text/one.xhtml","Text/two.xhtml","Text/three.xhtml"};
    const char *words[]={"乙","丙","第三注"};uint32_t units=0;
    for(size_t i=0;i<3;++i) {
        size_t n=source_file(dir,files[i],original,sizeof(original));
        const char *at=strstr(original,words[i]);assert(at);
        uint32_t expected=units+weread_utf16_length(original,(size_t)(at-original));
        uint32_t local=(uint32_t)(strstr(parsed.utf8,words[i])-parsed.utf8),cloud=0,back=0;
        assert(book_weread_map(0,true,local,&cloud)==ESP_OK&&cloud==expected);
        assert(book_weread_map(0,false,cloud,&back)==ESP_OK&&back==local);
        if(!i) {
            // 无WeRead别名的本地HTML/EPUB脚注仍走原id路径。/ Local HTML/EPUB notes without WeRead aliases still use their original IDs.
            html_text_t native={0};assert(html_to_blocks(original,n,&native)==ESP_OK&&native.note_count==1);html_text_free(&native);
        }
        units+=weread_utf16_length(original,n);
    }
    html_text_free(&parsed);book_weread_close();mode=0;
}
static void scan_fat_tree(const char *dir, bool in_wr, size_t *wr_css_count) {
    DIR *d = opendir(dir);
    assert(d);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        for (const char *p = e->d_name; *p; ++p) assert(!strchr("*:<>|\"?\\", *p));
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);
        struct stat st;
        assert(!stat(child, &st));
        if (S_ISDIR(st.st_mode)) {
            scan_fat_tree(child, in_wr || !strcmp(e->d_name, "_wr"), wr_css_count);
        } else if (in_wr) {
            size_t len = strlen(e->d_name);
            if (len >= 4 && !strcmp(e->d_name + len - 4, ".css")) ++(*wr_css_count);
        }
    }
    closedir(d);
}
static void obfuscated_names(void) {
    mode = 9;
    char *body = NULL; size_t len = 0; html_text_t parsed = {0};
    assert(weread_chapter_load("test_book_101", 4, &body, &len) == ESP_OK);
    assert(body && strstr(body, "混淆正文"));
    free(body);
    char dir[512];
    assert(weread_chapter_directory("test_book_101", 4, dir, sizeof(dir)) == ESP_OK);
    size_t wr_css_count = 0;
    scan_fat_tree(dir, false, &wr_css_count);
    assert(wr_css_count >= 1);
    assert(weread_toc_sync("test_book_101") == ESP_OK && book_weread_open("weread://test_book_101") == ESP_OK);
    assert(book_weread_load(0, NULL, NULL, &parsed) == ESP_OK);
    assert(parsed.count >= 1 && parsed.note_count == 2 && parsed.image_count == 2);
    assert(!strncmp(parsed.utf8 + parsed.blocks[0].offset, "混淆正文", strlen("混淆正文")) &&
           parsed.blocks[0].align == 1);
    const char *notes[] = {"混淆脚注", "编码脚注"};
    for (size_t i = 0; i < 2; ++i) {
        assert(parsed.notes[i].text_len >= strlen(notes[i]) &&
               !memcmp(parsed.utf8 + parsed.notes[i].text_start, notes[i], strlen(notes[i])));
    }
    const char *tags[] = {"OBF1", "OBF2"};
    for (size_t i = 0; i < 2; ++i) {
        uint8_t *img = NULL; size_t ilen = 0; bool png = false;
        assert(book_weread_image(0, parsed.images[i], &img, &ilen, &png) == ESP_OK);
        assert(png && ilen == 8 + 4 && !memcmp(img + 8, tags[i], 4));
        free(img);
    }
    assert(weread_resource_path(NULL, "Styles/:****::****.css", dir, sizeof(dir)) && !strcmp(dir, "Styles/:****::****.css"));
    assert(weread_resource_path("Text/ch1.xhtml", "../Images/:****.png", dir, sizeof(dir)) && !strcmp(dir, "Images/:****.png"));
    assert(weread_resource_path("Text/:****1.xhtml", ":****2.xhtml", dir, sizeof(dir)) && !strcmp(dir, "Text/:****2.xhtml"));
    assert(!weread_resource_path(NULL, "https://qq.com/x", dir, sizeof(dir)));
    assert(!weread_resource_path(NULL, "mailto:a@b", dir, sizeof(dir)));
    assert(!weread_resource_path(NULL, "data:image/png;base64,AA", dir, sizeof(dir)));
    assert(!weread_resource_path(NULL, "C:escape", dir, sizeof(dir)));
    assert(!weread_resource_path(NULL, "../escape", dir, sizeof(dir)));
    assert(!weread_resource_path(NULL, "/abs", dir, sizeof(dir)));
    html_text_free(&parsed); book_weread_close(); mode = 0;
}

static void assert_file_bytes(const char *path, const char *expected) {
    FILE *f = fopen(path, "rb");
    assert(f != NULL);
    char buf[256];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    assert(feof(f));
    fclose(f);
    buf[n] = 0;
    assert(strcmp(buf, expected) == 0);
}

static void test_expired_and_cache_cleanup(const char *root) {
    mode = 0;
    assert(weread_toc_sync("test_book_101") == ESP_OK);

    const char *keep_names[] = {
        "progress.json", "progress.json.tmp", "progress.json.previous",
        "progress.json.123", "chapters.json.previous",
        "cover-0123456789abcdef.gray", "cover.gray"
    };
    const char *keep_vals[] = {
        "PROGRESS_KEEP", "TMP_KEEP", "PREV_KEEP",
        "VID_KEEP", "TOC_PREV_KEEP",
        "GRAY_KEEP", "LEGACY_GRAY_KEEP"
    };
    for (size_t i = 0; i < sizeof(keep_names) / sizeof(*keep_names); ++i) {
        char p[512];
        assert(wr_cache_path("test_book_101", keep_names[i], p, sizeof(p)) == ESP_OK);
        assert(wr_write_bytes(p, keep_vals[i], strlen(keep_vals[i])) == ESP_OK);
    }
    const char *del_names[] = {
        "cover-0123456789abcdef.img", "cover-0123456789abcdef.img.part",
        "cover-0123456789abcdef.failed", "cover.img", "cover.failed",
        "archive.tar", "chapter_9.html", "chapter-9.html.part", "chapter-9.json.tmp"
    };
    for (size_t i = 0; i < sizeof(del_names) / sizeof(*del_names); ++i) {
        char p[512];
        assert(wr_cache_path("test_book_101", del_names[i], p, sizeof(p)) == ESP_OK);
        assert(wr_write_bytes(p, "TEMP_DELETE_ME", 14) == ESP_OK);
    }
    char txt_keep[512], txt_del[512];
    assert(wr_cache_path("txt_book_202", "cover.gray", txt_keep, sizeof(txt_keep)) == ESP_OK);
    assert(wr_write_bytes(txt_keep, "TXT_GRAY", 8) == ESP_OK);
    assert(wr_cache_path("txt_book_202", "archive.zip", txt_del, sizeof(txt_del)) == ESP_OK);
    assert(wr_write_bytes(txt_del, "TXT_ARCHIVE", 11) == ESP_OK);

    /* WR-UX-01a expired read-only open/read/map without network */
    assert(weread_auth_mark_expired() == ESP_OK);
    assert(weread_auth_is_expired() && !weread_auth_is_authenticated());
    int dl_before = downloads, req_before = requests;
    assert(weread_chapter_cached("test_book_101", 1));
    char *text = NULL; size_t len = 0;
    assert(weread_chapter_load("test_book_101", 1, &text, &len) == ESP_OK && len > 0);
    free(text); text = NULL;
    assert(weread_chapter_load("test_book_101", 999, &text, &len) == ESP_ERR_INVALID_STATE && !text);
    assert(book_weread_prepare_open("test_book_101") == ESP_OK);
    assert(book_weread_open("weread://test_book_101") == ESP_OK);
    html_text_t parsed = {0};
    assert(book_weread_load(0, NULL, NULL, &parsed) == ESP_OK);
    uint32_t mapped = 0;
    assert(book_weread_map(0, true, 0, &mapped) == ESP_OK);
    assert(book_weread_map_detached("test_book_101", 1, 100, false, 0, &mapped) == ESP_OK);
    html_text_free(&parsed);
    assert(downloads == dl_before && requests == req_before);

    /* WR-UX-09a storage info, inspection, and cached chapter count */
    uint64_t free_b = 0, total_b = 0;
    assert(weread_chapter_storage_info(&free_b, &total_b) == ESP_OK && free_b > 0 && total_b > 0);
    uint16_t cached_c = 0, total_c = 0;
    assert(weread_chapter_count_cached("test_book_101", &cached_c, &total_c, NULL, NULL) == ESP_OK);
    assert(cached_c == 1 && total_c == 1);

    weread_chapter_book_cache_info_t binfo = {0};
    assert(weread_chapter_inspect_book_cache("test_book_101", &binfo, NULL, NULL) == ESP_OK);
    assert(binfo.chapter_bytes > 0 && binfo.cover_bytes > 0 && binfo.meta_bytes > 0 && binfo.reclaimable_bytes > 0);
    assert(binfo.total_bytes == binfo.chapter_bytes + binfo.cover_bytes + binfo.meta_bytes);
    assert(binfo.chapter_manifest_count >= 1);

    weread_chapter_cache_stats_t stats = {0};
    assert(weread_chapter_inspect_cache_stats(&stats, NULL, NULL) == ESP_OK);
    assert(stats.book_dir_count >= 2 && stats.books_with_chapters >= 2);
    assert(stats.reclaimable_bytes >= binfo.reclaimable_bytes && stats.free_bytes > 0);

    /* Active book cannot be cleaned; clean_all_caches skips open book and cleans txt_book_202 */
    assert(weread_chapter_clean_book_cache("test_book_101", NULL, NULL, NULL) == ESP_ERR_INVALID_STATE);
    uint64_t freed_all = 0; uint32_t cleaned_books = 0;
    assert(weread_chapter_clean_all_caches(&freed_all, &cleaned_books, NULL, NULL) == ESP_OK);
    assert(cleaned_books == 1 && freed_all > 0);
    assert(access(txt_del, F_OK) != 0);
    assert_file_bytes(txt_keep, "TXT_GRAY");
    assert(weread_chapter_cached("test_book_101", 1));

    book_weread_close();

    /* Mid-clean cancellation */
    stop = true;
    assert(weread_chapter_clean_book_cache("test_book_101", NULL, cancel, NULL) == ESP_ERR_INVALID_STATE);
    stop = false;

    /* Selective cleanup of test_book_101 deletes chapters/, res/, manifests, temp covers; keeps progress/TOC/gray */
    uint64_t freed = 0;
    assert(weread_chapter_clean_book_cache("test_book_101", &freed, cancel, NULL) == ESP_OK && freed > 0);
    for (size_t i = 0; i < sizeof(del_names) / sizeof(*del_names); ++i) {
        char p[512];
        assert(wr_cache_path("test_book_101", del_names[i], p, sizeof(p)) == ESP_OK);
        assert(access(p, F_OK) != 0);
    }
    char chapters_dir[512], res_dir[512], ch1_json[512], res_json[512];
    assert(wr_cache_path("test_book_101", "chapters", chapters_dir, sizeof(chapters_dir)) == ESP_OK);
    assert(wr_cache_path("test_book_101", "res", res_dir, sizeof(res_dir)) == ESP_OK);
    assert(wr_cache_path("test_book_101", "chapter-1.json", ch1_json, sizeof(ch1_json)) == ESP_OK);
    assert(wr_cache_path("test_book_101", "resources.json", res_json, sizeof(res_json)) == ESP_OK);
    assert(access(chapters_dir, F_OK) != 0 && access(res_dir, F_OK) != 0);
    assert(access(ch1_json, F_OK) != 0 && access(res_json, F_OK) != 0);

    for (size_t i = 0; i < sizeof(keep_names) / sizeof(*keep_names); ++i) {
        char p[512];
        assert(wr_cache_path("test_book_101", keep_names[i], p, sizeof(p)) == ESP_OK);
        assert_file_bytes(p, keep_vals[i]);
    }
    char toc_json[512], auth_json[512], shelf_json[512], dev_id[512];
    assert(wr_cache_path("test_book_101", "chapters.json", toc_json, sizeof(toc_json)) == ESP_OK);
    snprintf(auth_json, sizeof(auth_json), "%s/auth.json", root);
    snprintf(shelf_json, sizeof(shelf_json), "%s/shelf.json", root);
    snprintf(dev_id, sizeof(dev_id), "%s/device_id", root);
    assert(access(toc_json, F_OK) == 0 && access(auth_json, F_OK) == 0 &&
           access(shelf_json, F_OK) == 0 && access(dev_id, F_OK) == 0);

    assert(weread_chapter_count_cached("test_book_101", &cached_c, &total_c, NULL, NULL) == ESP_OK);
    assert(cached_c == 0 && total_c == 1);
    assert(weread_chapter_inspect_book_cache("test_book_101", &binfo, NULL, NULL) == ESP_OK);
    assert(binfo.reclaimable_bytes == 0 && binfo.chapter_bytes == 0 && binfo.cover_bytes > 0 && binfo.meta_bytes > 0);
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
    back=123;assert(book_weread_map(0,false,UINT32_MAX,&back)==WEREAD_ERR_POSITION&&back==123);
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
    merged_notes();
    obfuscated_names();
    assert(weread_chapter_directory("../escape", 1, dir, sizeof(dir)) == ESP_ERR_INVALID_ARG);
    assert(weread_resource_path("Text/ch1.xhtml", "../Images/cover.jpg", dir, sizeof(dir)) && !strcmp(dir, "Images/cover.jpg"));
    assert(!weread_resource_path(NULL, "../escape", dir, sizeof(dir)));
    assert(!weread_resource_path(NULL, "https://qq.com/x", dir, sizeof(dir)));
    FILE *large = fopen("build/weread-tests/large.zip", "rb"); assert(large);
    assert(book_weread_unpack_zip_file(large, NULL, 0, large_sink, NULL) == ESP_OK); fclose(large);
    assert(expanded == 2u * 1024u * 1024u && chunks >= 512);
    large=fopen("build/weread-tests/multichunk.zip","rb");assert(large);
    assert(book_weread_unpack_zip_file(large,NULL,0,random_sink,NULL)==ESP_OK&&random_expanded==300000);fclose(large);
    test_expired_and_cache_cleanup(root);
    weread_shelf_clear(); weread_auth_clear_session();
    puts("WeRead chapters: encrypted ZIP, TAR, ordered assembly, offline, cancellation, 402/499/-2063 and bounded inflation passed.");
    return 0;
}
