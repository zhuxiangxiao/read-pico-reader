/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_chapter.h"
#include "weread_client.h"
#include "weread_progress.h"
#include "weread_shelf.h"
#include "weread_store.h"
#include "weread_toc.h"
#include "book_source_weread.h"
#include "html_text.h"
#include "cJSON.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

static uint64_t s_free_bytes = 512ULL * 1024ULL * 1024ULL;
int decoupling_statvfs(const char *path, struct statvfs *buf) {
    (void)path;
    memset(buf, 0, sizeof(*buf));
    buf->f_bsize = 4096;
    buf->f_frsize = 4096;
    buf->f_bavail = s_free_bytes / 4096;
    return 0;
}

static uint64_t wall_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static const char *s_fixtures_dir;
static const char *s_encrypt_key;
static atomic_uint s_downloads;
static atomic_uint s_reads_posted;
static atomic_uint s_slow_download_ms;
static atomic_uint s_slow_note_ms;
static atomic_bool s_download_entered;
static atomic_bool s_note_entered;
static atomic_uint s_stop_after_downloads;

static esp_err_t http_transport(const char *url, const char *method, const char *headers,
    const char *body, char **out, size_t *len, int *status, void *ctx) {
    (void)method; (void)headers; (void)body; (void)ctx;
    char text[4096] = {0};
    *status = 200;
    if (strstr(url, "/book/chapterInfos")) {
        char items[2048] = {0};
        size_t pos = 0;
        for (int i = 0; i < 8; ++i) {
            pos += (size_t)snprintf(items + pos, sizeof(items) - pos,
                "%s{\"chapterUid\":%d,\"chapterIdx\":%d,\"title\":\"Chapter %d\",\"level\":1,\"wordCount\":200,\"updateTime\":1700000000}",
                i ? "," : "", 10 + i, i + 1, i);
        }
        snprintf(text, sizeof(text),
            "{\"data\":[{\"bookId\":\"decouple_book\",\"synckey\":1,\"clearAll\":1,\"bookVersion\":100,\"updated\":[%s],\"removed\":[]}]}",
            items);
    } else if (strstr(url, "/shelf/syncbook")) {
        snprintf(text, sizeof(text),
            "{\"books\":[{\"bookId\":\"decouple_book\",\"bookVersion\":100}],\"bookProgress\":[{\"bookId\":\"decouple_book\",\"chapterUid\":10,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":0}]}");
    } else if (strstr(url, "/book/read")) {
        atomic_store(&s_note_entered, true);
        unsigned delay = atomic_load(&s_slow_note_ms);
        for (unsigned waited = 0; waited < delay; waited += 10) usleep(10000);
        atomic_fetch_add(&s_reads_posted, 1);
        snprintf(text, sizeof(text), "{\"succ\":1}");
    } else {
        snprintf(text, sizeof(text), "{}");
    }
    *out = strdup(text);
    assert(*out);
    *len = strlen(text);
    return ESP_OK;
}

static esp_err_t download_transport(const char *url, const char *headers, FILE *file, size_t limit,
    weread_cancel_fn cancel, void *cancel_ctx, weread_download_info_t *info, void *ctx) {
    (void)headers; (void)limit; (void)ctx;
    const char *q = strstr(url, "chapters=");
    assert(q);
    q += 9;
    const char *amp = strchr(q, '&');
    const char *comma = strchr(q, ',');
    if (comma && (!amp || comma < amp)) q = comma + 1;
    unsigned uid = (unsigned)strtoul(q, NULL, 10);
    atomic_store(&s_download_entered, true);
    unsigned delay = atomic_load(&s_slow_download_ms);
    for (unsigned waited = 0; waited < delay; waited += 10) {
        if (cancel && cancel(cancel_ctx)) return ESP_ERR_INVALID_STATE;
        usleep(10000);
    }
    if (cancel && cancel(cancel_ctx)) return ESP_ERR_INVALID_STATE;
    unsigned count = atomic_fetch_add(&s_downloads, 1) + 1;
    unsigned cap = atomic_load(&s_stop_after_downloads);
    if (cap && count >= cap) s_free_bytes = 32ULL * 1024ULL * 1024ULL;
    char zip_path[512];
    snprintf(zip_path, sizeof(zip_path), "%s/%u.zip", s_fixtures_dir, uid);
    FILE *src = fopen(zip_path, "rb");
    assert(src);
    char buf[4096];
    size_t total = 0, n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        assert(fwrite(buf, 1, n, file) == n);
        total += n;
    }
    fclose(src);
    info->status = 200;
    info->bytes = total;
    snprintf(info->encrypt_key, sizeof(info->encrypt_key), "%s", s_encrypt_key);
    snprintf(info->content_type, sizeof(info->content_type), "application/zip");
    return ESP_OK;
}

static void setup_book(const char *root) {
    assert(weread_auth_init(root) == ESP_OK);
    weread_auth_session_t s = {
        .vid = "123", .skey = "skey", .access_token = "tok", .refresh_token = "ref",
        .device_id = "dev"
    };
    assert(weread_auth_save_session(&s) == ESP_OK && weread_auth_load_session(&s) == ESP_OK);
    char shelf_path[512];
    snprintf(shelf_path, sizeof(shelf_path), "%s/shelf.json", root);
    {
        WR_GUARD;
        assert(wr_mkdirs(root) == ESP_OK);
        cJSON *sh = cJSON_CreateObject(), *books = cJSON_AddArrayToObject(sh, "books"), *b = cJSON_CreateObject();
        assert(sh && books && b);
        assert(cJSON_AddStringToObject(b, "bookId", "decouple_book"));
        assert(cJSON_AddStringToObject(b, "title", "Decouple Test"));
        assert(cJSON_AddStringToObject(b, "format", "epub"));
        assert(cJSON_AddNumberToObject(b, "bookVersion", 100));
        assert(cJSON_AddNumberToObject(sh, "synckey", 1));
        assert(wr_set_owner(sh));
        cJSON_AddItemToArray(books, b);
        assert(wr_write_json(shelf_path, sh) == ESP_OK);
        cJSON_Delete(sh);
    }
    assert(weread_shelf_load() == ESP_OK);
    weread_client_set_transport(http_transport, NULL);
    weread_client_set_download_transport(download_transport, NULL);
    assert(weread_toc_sync("decouple_book") == ESP_OK);
    assert(weread_progress_select("decouple_book", &(weread_progress_t){
        .chapter_uid = 10, .chapter_index = 1, .book_version = 100
    }, true) == ESP_OK);
    assert(weread_chapter_download("decouple_book", 10) == ESP_OK);
    assert(weread_reader_prepare_open("weread://decouple_book") == ESP_OK);
    assert(book_weread_open("weread://decouple_book") == ESP_OK);
    size_t ch = 0; uint32_t off = 0;
    assert(weread_reader_resume("weread://decouple_book", &ch, &off) == ESP_OK && ch == 0);
    weread_reader_begin("weread://decouple_book", 1000);
}

static void drain_worker(void) {
    uint64_t deadline = wall_ms() + 10000;
    while (weread_runtime_busy()) {
        assert(wall_ms() < deadline);
        usleep(5000);
    }
}

int main(int argc, char **argv) {
    assert(argc == 5);
    const char *root = argv[1];
    s_fixtures_dir = argv[2];
    s_encrypt_key = argv[3];
    const char *mode = argv[4];

    setup_book(root);
    atomic_store(&s_downloads, 0);
    const char *uri = "weread://decouple_book";
    html_text_t text = {0};
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK);
    size_t c = 0; uint32_t ver = 0;
    assert(book_weread_commit_loaded(&c, &ver) == ESP_OK && ver == 100);
    weread_reader_tick(uri, 0, 0, 1, "start", 1000, true);

    if (!strcmp(mode, "concurrent-io")) {
        atomic_store(&s_slow_download_ms, 1200);
        atomic_store(&s_download_entered, false);
        // Trigger WR_JOB_PREFETCH on chapter 1 at +300ms
        weread_reader_tick(uri, 0, 0, 1, "start", 1300, true);
        uint64_t wait_dl = wall_ms() + 5000;
        while (!atomic_load(&s_download_entered)) {
            assert(wall_ms() < wait_dl);
            usleep(2000);
        }
        assert(weread_runtime_busy());
        uint64_t fg_start = wall_ms();
        weread_reader_input(false);
        html_text_t reload = {0};
        assert(book_weread_load(0, NULL, NULL, &reload) == ESP_OK && reload.len > 0);
        uint8_t *img = NULL; size_t ilen = 0; bool png = false;
        assert(reload.image_count == 1 && reload.images && reload.images[0]);
        assert(book_weread_image(0, reload.images[0], &img, &ilen, &png) == ESP_OK && ilen > 0 && png);
        free(img);
        html_text_free(&reload);
        weread_reader_navigated_version(uri, 0, 5, 10, "mid", 100);
        assert(weread_reader_save(1350) == ESP_OK);
        unsigned cached = 0, total = 0;
        weread_reader_cached_progress(&cached, &total);
        assert(total == 8);
        uint64_t elapsed = wall_ms() - fg_start;
        assert(elapsed < 100);
        printf("PASS: foreground load/image/save/input elapsed=%llums while worker stayed in 1200ms download\n",
               (unsigned long long)elapsed);
        weread_runtime_cancel();
        drain_worker();
    } else if (!strcmp(mode, "nonblocking-exit")) {
        // Pre-warm chapter 1 & 2 quickly so WR_JOB_NOTE is ready to fire
        atomic_store(&s_slow_download_ms, 0);
        weread_reader_tick(uri, 0, 0, 1, "start", 1300, true);
        drain_worker();
        weread_reader_tick(uri, 0, 0, 1, "start", 1350, true);
        weread_reader_navigated_version(uri, 0, 4, 5, "p4", 100);
        assert(weread_reader_save(1400) == ESP_OK);
        // Trigger urgent chapter transition + WR_JOB_NOTE with 800ms /book/read delay
        atomic_store(&s_slow_note_ms, 800);
        atomic_store(&s_note_entered, false);
        html_text_t ch1_text = {0};
        assert(book_weread_load(1, NULL, NULL, &ch1_text) == ESP_OK);
        size_t c1 = 1; uint32_t v1 = 0;
        assert(book_weread_commit_loaded(&c1, &v1) == ESP_OK);
        html_text_free(&ch1_text);
        weread_reader_navigated_version(uri, 1, 0, 15, "ch1", v1);
        weread_reader_tick(uri, 1, 0, 15, "ch1", 2500, true);
        weread_reader_tick(uri, 1, 0, 15, "ch1", 2800, true);
        drain_worker();
        weread_reader_tick(uri, 1, 0, 15, "ch1", 3500, true);
        weread_reader_tick(uri, 1, 0, 15, "ch1", 3501, true);
        uint64_t wait_note = wall_ms() + 5000;
        while (!atomic_load(&s_note_entered)) {
            assert(wall_ms() < wait_note);
            usleep(2000);
        }
        uint64_t exit_start = wall_ms();
        weread_reader_pause_bg();
        assert(weread_reader_save(2600) == ESP_OK);
        weread_reader_end(2600);
        book_weread_close();
        uint64_t exit_ms = wall_ms() - exit_start;
        assert(exit_ms < 100);
        assert(weread_runtime_busy()); // WR_JOB_NOTE finishes asynchronously!
        drain_worker();
        weread_result_t r;
        if (weread_runtime_poll(&r)) weread_runtime_result_free(&r);
        assert(atomic_load(&s_reads_posted) >= 1);

        // Now verify nonblocking exit while WR_JOB_PREFETCH is in slow 1500ms download
        assert(weread_reader_prepare_open(uri) == ESP_OK);
        assert(book_weread_open(uri) == ESP_OK);
        size_t rc = 0; uint32_t ro = 0;
        assert(weread_reader_resume(uri, &rc, &ro) == ESP_OK);
        weread_reader_begin(uri, 4000);
        atomic_store(&s_slow_download_ms, 0);
        weread_reader_tick(uri, rc, 0, 1, "start", 4000, true);
        weread_reader_tick(uri, rc, 0, 1, "start", 4350, true);
        drain_worker();
        weread_reader_tick(uri, rc, 0, 1, "start", 5050, true);
        drain_worker();
        atomic_store(&s_slow_download_ms, 1500);
        atomic_store(&s_download_entered, false);
        weread_reader_tick(uri, rc, 0, 1, "start", 5200, true);
        uint64_t wait_dl = wall_ms() + 5000;
        while (!atomic_load(&s_download_entered)) {
            assert(wall_ms() < wait_dl);
            usleep(2000);
        }
        uint64_t pf_exit_start = wall_ms();
        weread_reader_pause_bg();
        weread_reader_end(5300);
        book_weread_close();
        uint64_t pf_exit_ms = wall_ms() - pf_exit_start;
        assert(pf_exit_ms < 100);
        // Immediate reopen while background is unwinding must not crash or fail.
        unsigned cached = 0, total = 0;
        weread_reader_cached_progress(&cached, &total);
        assert(!cached && !total);
        drain_worker();
        printf("PASS: book exit returned in %llums during active WR_JOB_NOTE (finished async) and %llums during slow WR_JOB_PREFETCH\n",
               (unsigned long long)exit_ms, (unsigned long long)pf_exit_ms);
    } else if (!strcmp(mode, "whole-book-prefetch")) {
        atomic_store(&s_slow_download_ms, 0);
        uint64_t sim = 1000;
        for (int step = 0; step < 30; ++step) {
            sim += (step == 0 ? 300 : 1000);
            weread_reader_tick(uri, 0, 0, 1, "idle", sim, true);
            drain_worker();
        }
        unsigned cached = 0, total = 0;
        weread_reader_cached_progress(&cached, &total);
        assert(cached == 8 && total == 8);
        assert(weread_reader_offline() == 7);
        unsigned downloads_done = atomic_load(&s_downloads);
        assert(downloads_done == 7); // ch0 already cached + ch1..ch7
        // Further idle ticks must NOT re-download anything
        for (int extra = 0; extra < 5; ++extra) {
            sim += 1000;
            weread_reader_tick(uri, 0, 0, 1, "idle", sim, true);
            drain_worker();
        }
        assert(atomic_load(&s_downloads) == downloads_done);
        // Opening directly on last chapter still scans previously cached chapters (8/8).
        weread_reader_end(sim + 100);
        book_weread_close();
        assert(weread_progress_select("decouple_book", &(weread_progress_t){
            .chapter_uid = 17, .chapter_index = 8, .book_version = 100
        }, true) == ESP_OK);
        assert(weread_reader_prepare_open(uri) == ESP_OK);
        assert(book_weread_open(uri) == ESP_OK);
        size_t lc = 0; uint32_t lo = 0, src_unit = 0, mapped_cand = 0;
        assert(weread_reader_resume_chapter(uri, &lc, &src_unit) == ESP_OK && lc == 7 && src_unit == 0);
        html_text_t cand_text = {0};
        assert(book_weread_load(lc, NULL, NULL, &cand_text) == ESP_OK);
        assert(book_weread_map_candidate(lc, false, src_unit, &mapped_cand) == ESP_OK && mapped_cand == 0);
        book_weread_discard_loaded();
        html_text_free(&cand_text);
        assert(weread_reader_resume(uri, &lc, &lo) == ESP_OK && lc == 7 && lo == 0);
        weread_reader_begin(uri, sim + 200);
        weread_reader_tick(uri, 7, 0, 99, "last", sim + 200, true);
        weread_reader_tick(uri, 7, 0, 99, "last", sim + 1200, true);
        drain_worker();
        weread_reader_tick(uri, 7, 0, 99, "last", sim + 1250, true);
        drain_worker();
        weread_reader_tick(uri, 7, 0, 99, "last", sim + 1300, true);
        drain_worker();
        weread_reader_cached_progress(&cached, &total);
        assert(cached == 8 && total == 8);
        assert(atomic_load(&s_downloads) == downloads_done);
        printf("PASS: whole-book idle batch cached %u/%u chapters (ahead=%u) and scanned last-chapter open\n",
               cached, total, weread_reader_offline());
    } else {
        assert(!strcmp(mode, "storage-cap"));
        atomic_store(&s_slow_download_ms, 0);
        atomic_store(&s_stop_after_downloads, 3);
        uint64_t sim = 1000;
        for (int step = 0; step < 20; ++step) {
            sim += (step == 0 ? 300 : 1000);
            weread_reader_tick(uri, 0, 0, 1, "idle", sim, true);
            drain_worker();
        }
        unsigned cached = 0, total = 0;
        weread_reader_cached_progress(&cached, &total);
        assert(weread_reader_storage_blocked());
        assert(cached == 4 && total == 8);
        printf("PASS: whole-book prefetch stopped cleanly on 104 MiB reserve at %u/%u cached\n",
               cached, total);
    }

    html_text_free(&text);
    weread_reader_end(99999);
    book_weread_close();
    weread_runtime_stop();
    weread_shelf_clear();
    return 0;
}
