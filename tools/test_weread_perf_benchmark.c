/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
/* Comprehensive deterministic WeRead performance & I/O/allocation benchmark driver. */
#ifndef WR_PERF_NO_SPY_MACROS
#define WR_PERF_NO_SPY_MACROS
#endif
#include "weread_perf_io_spy.h"

#include "weread_auth.h"
#include "weread_chapter.h"
#include "weread_client.h"
#include "weread_position.h"
#include "weread_progress.h"
#include "weread_runtime.h"
#include "weread_shelf.h"
#include "weread_store.h"
#include "weread_toc.h"
#include "book_cover.h"
#include "book_source_weread.h"
#include "book_weread_crypto.h"
#include "weread_memory_test.h"
#include "esp_heap_caps.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

wr_perf_io_stats_t g_wr_perf_io;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static void print_stats(const char *tag, uint64_t elapsed_us, double extra_val, const char *extra_key) {
    printf("METRIC name=%s us=%llu fopen=%llu fseek=%llu fread=%llu fread_bytes=%llu "
           "fwrite=%llu fwrite_bytes=%llu stat=%llu lstat=%llu rename=%llu fsync=%llu "
           "opendir=%llu shelf_writes=%llu shelf_bytes=%llu",
           tag,
           (unsigned long long)elapsed_us,
           (unsigned long long)atomic_load(&g_wr_perf_io.fopen_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.fseek_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.fread_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.fread_bytes),
           (unsigned long long)atomic_load(&g_wr_perf_io.fwrite_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.fwrite_bytes),
           (unsigned long long)atomic_load(&g_wr_perf_io.stat_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.lstat_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.rename_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.fsync_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.opendir_calls),
           (unsigned long long)atomic_load(&g_wr_perf_io.shelf_writes),
           (unsigned long long)atomic_load(&g_wr_perf_io.shelf_bytes));
    if (extra_key) printf(" %s=%.3f", extra_key, extra_val);
    printf("\n");
    fflush(stdout);
}

/* ---- NVS spy backend ---- */
typedef struct {
    bool valid[2];
    uint8_t slot[2][512];
    size_t len[2];
    uint8_t media[WEREAD_MEDIA_ID_BYTES];
    uint64_t reads;
    uint64_t writes;
    uint64_t erases;
} bench_rescue_t;

static bench_rescue_t g_rescue;

static esp_err_t bench_rescue_read(unsigned slot, void *out, size_t cap, void *ctx) {
    bench_rescue_t *r = (bench_rescue_t *)ctx;
    r->reads++;
    if (slot >= 2 || !r->valid[slot]) return ESP_ERR_NOT_FOUND;
    if (r->len[slot] != cap) return ESP_ERR_INVALID_SIZE;
    memcpy(out, r->slot[slot], cap);
    return ESP_OK;
}
static esp_err_t bench_rescue_write(unsigned slot, const void *data, size_t len, void *ctx) {
    bench_rescue_t *r = (bench_rescue_t *)ctx;
    r->writes++;
    if (slot >= 2 || len > sizeof(r->slot[slot])) return ESP_ERR_NO_MEM;
    memcpy(r->slot[slot], data, len);
    r->len[slot] = len;
    r->valid[slot] = true;
    return ESP_OK;
}
static esp_err_t bench_rescue_erase(unsigned slot, void *ctx) {
    bench_rescue_t *r = (bench_rescue_t *)ctx;
    r->erases++;
    if (slot < 2) { r->valid[slot] = false; r->len[slot] = 0; }
    return ESP_OK;
}
static bool bench_rescue_media(uint8_t out[WEREAD_MEDIA_ID_BYTES], void *ctx) {
    bench_rescue_t *r = (bench_rescue_t *)ctx;
    memcpy(out, r->media, WEREAD_MEDIA_ID_BYTES);
    return true;
}

static void init_auth_session(const char *root) {
    wr_memory_init();
    assert(!setenv("WEREAD_STORAGE_ROOT", root, 1));
    assert(weread_auth_init(root) == ESP_OK);
    weread_auth_session_t s = {0};
    strcpy(s.vid, "123");
    strcpy(s.access_token, "bench-access-token");
    strcpy(s.refresh_token, "bench-refresh-token");
    strcpy(s.device_id, "bench-device-1");
    assert(weread_auth_save_session(&s) == ESP_OK && weread_auth_load_session(&s) == ESP_OK);
    memset(&g_rescue, 0, sizeof(g_rescue));
    memset(g_rescue.media, 0xA5, sizeof(g_rescue.media));
    weread_progress_rescue_backend_t rb = {
        .read = bench_rescue_read,
        .write = bench_rescue_write,
        .erase = bench_rescue_erase,
        .media = bench_rescue_media,
        .ctx = &g_rescue,
    };
    weread_progress_rescue_backend(&rb);
}

/* ---- Workload 1: Crypto + Streaming Unpack ---- */
static bool zip_noop_sink(void *ctx, const char *name, const uint8_t *bytes, size_t len, size_t offset, size_t total) {
    (void)name; (void)offset; (void)total;
    uint64_t *sum = (uint64_t *)ctx;
    if (bytes && len) *sum += len;
    return true;
}

static int bench_crypto(const char *fixtures_dir) {
    char zip_path[512], tar_path[512];
    snprintf(zip_path, sizeof(zip_path), "%s/bench_pack.zip", fixtures_dir);
    snprintf(tar_path, sizeof(tar_path), "%s/bench_txt.tar", fixtures_dir);

    struct stat zst, tst;
    assert(stat(zip_path, &zst) == 0 && stat(tar_path, &tst) == 0);
    const uint8_t pw[] = "test_password_123";
    char *vbuf = malloc(8192);
    assert(vbuf);

    /* Warmup once */
    FILE *fz = fopen(zip_path, "rb"); assert(fz);
    setvbuf(fz, vbuf, _IOFBF, 8192);
    uint64_t bytes_out = 0;
    assert(book_weread_unpack_zip_file(fz, pw, sizeof(pw) - 1, zip_noop_sink, &bytes_out) == ESP_OK);
    fclose(fz);
    uint64_t single_zip_out = bytes_out;

    wr_perf_io_reset();
    bytes_out = 0;
    const int zip_iters = 20;
    uint64_t t0 = now_us();
    for (int i = 0; i < zip_iters; ++i) {
        fz = fopen(zip_path, "rb"); assert(fz);
        setvbuf(fz, vbuf, _IOFBF, 8192);
        assert(book_weread_unpack_zip_file(fz, pw, sizeof(pw) - 1, zip_noop_sink, &bytes_out) == ESP_OK);
        fclose(fz);
    }
    uint64_t dt_zip = now_us() - t0;
    double mbps_zip = ((double)bytes_out / (1024.0 * 1024.0)) / ((double)dt_zip / 1e6);
    assert(bytes_out == single_zip_out * (uint64_t)zip_iters);
    print_stats("crypto_zip_unpack_20x", dt_zip, mbps_zip, "mb_per_s");

    /* GilbertVernam + TAR stream unpack */
    uint8_t gv_key[16] = {'p','D','B','9','W','z','F','l','4','Y','F','m','R','n','I','Z'};
    FILE *ft = fopen(tar_path, "rb"); assert(ft);
    size_t tar_sz = (size_t)tst.st_size;
    uint8_t *raw_tar = malloc(tar_sz);
    assert(raw_tar && fread(raw_tar, 1, tar_sz, ft) == tar_sz);
    fclose(ft);

    uint8_t *work = malloc(tar_sz);
    assert(work);
    wr_perf_io_reset();
    bytes_out = 0;
    const int tar_iters = 30;
    t0 = now_us();
    for (int i = 0; i < tar_iters; ++i) {
        memcpy(work, raw_tar, tar_sz);
        book_weread_gilbert_vernam(work, tar_sz, gv_key, sizeof(gv_key), 17);
        book_weread_gilbert_vernam(work, tar_sz, gv_key, sizeof(gv_key), 17);
        FILE *f = fopen(tar_path, "rb"); assert(f);
        setvbuf(f, vbuf, _IOFBF, 8192);
        assert(book_weread_unpack_tar_file(f, zip_noop_sink, &bytes_out) == ESP_OK);
        fclose(f);
    }
    uint64_t dt_tar = now_us() - t0;
    double mbps_tar = ((double)(tar_sz * (size_t)tar_iters) / (1024.0 * 1024.0)) / ((double)dt_tar / 1e6);
    print_stats("crypto_gv_tar_30x", dt_tar, mbps_tar, "mb_per_s");

    free(work);
    free(raw_tar);
    free(vbuf);
    weread_test_memory_report();
    return 0;
}

/* ---- Shared mock transports for Workloads 2, 3, 4, 5, 6 ---- */
static const char *g_fixtures;
static unsigned g_shelf_books = 800;
static unsigned g_toc_chapters = 3000;
static char *g_toc_payload = NULL;
static size_t g_toc_payload_len = 0;
static int g_shelf_mode = 0; /* 0=full initial, 1=incremental delta */
static char *g_progress_payload = NULL;
static char *g_dl_file_override = NULL;
static bool g_dl_fail = false;

static esp_err_t bench_http_request(const char *url, const char *method, const char *headers, const char *body,
                                    char **out, size_t *len, int *status, void *ctx) {
    (void)method; (void)headers; (void)ctx;
    *status = 200;
    if (strstr(url, "/shelf/syncbook")) {
        char *buf = malloc(1024);
        assert(buf);
        int n = snprintf(buf, 1024,
            "{\"synckey\":200,\"books\":[{\"bookId\":\"bench_book\",\"title\":\"Bench Book\",\"author\":\"Author\",\"format\":\"epub\",\"bookVersion\":100}],"
            "\"bookProgress\":[{\"bookId\":\"bench_book\",\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":120,\"progress\":5}]}");
        *out = buf; *len = (size_t)n;
        return ESP_OK;
    }
    if (strstr(url, "/shelf/sync")) {
        size_t cap = g_shelf_books * 360 + 4096;
        char *buf = malloc(cap);
        assert(buf);
        size_t used = 0;
        if (g_shelf_mode == 0) {
            if (g_shelf_books == 1) {
                used += (size_t)snprintf(buf + used, cap - used,
                    "{\"synckey\":100,\"books\":[{\"bookId\":\"bench_book\",\"title\":\"Bench Book\",\"author\":\"Author\","
                    "\"cover\":\"https://res.weread.qq.com/c_0000.jpg\",\"format\":\"epub\",\"bookVersion\":100}],"
                    "\"bookProgress\":[{\"bookId\":\"bench_book\",\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":100,\"progress\":10,\"updateTime\":1700000000}],"
                    "\"bookIds\":[\"bench_book\"]}");
                *out = buf; *len = used;
                return ESP_OK;
            }
            used += (size_t)snprintf(buf + used, cap - used, "{\"synckey\":100,\"books\":[");
            for (unsigned i = 0; i < g_shelf_books; ++i) {
                used += (size_t)snprintf(buf + used, cap - used,
                    "%s{\"bookId\":\"book_%04u\",\"title\":\"WeRead Title Number %04u\",\"author\":\"Author %03u\","
                    "\"cover\":\"https://res.weread.qq.com/c_%04u.jpg\",\"format\":\"epub\",\"bookVersion\":100}",
                    i ? "," : "", i, i, i % 100, i);
            }
            used += (size_t)snprintf(buf + used, cap - used, "],\"bookProgress\":[");
            for (unsigned i = 0; i < g_shelf_books; ++i) {
                used += (size_t)snprintf(buf + used, cap - used,
                    "%s{\"bookId\":\"book_%04u\",\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":100,\"progress\":%u,\"updateTime\":%u}",
                    i ? "," : "", i, (i % 90) + 1, 1700000000u + i);
            }
            used += (size_t)snprintf(buf + used, cap - used, "],\"bookIds\":[");
            for (unsigned i = 0; i < g_shelf_books; ++i) {
                used += (size_t)snprintf(buf + used, cap - used, "%s\"book_%04u\"", i ? "," : "", i);
            }
            used += (size_t)snprintf(buf + used, cap - used, "]}");
        } else {
            used += (size_t)snprintf(buf + used, cap - used, "{\"synckey\":101,\"books\":[");
            for (unsigned i = 0; i < 20; ++i) {
                used += (size_t)snprintf(buf + used, cap - used,
                    "%s{\"bookId\":\"book_%04u\",\"title\":\"Updated Title %04u\",\"author\":\"Author %u\","
                    "\"cover\":\"https://res.weread.qq.com/c_%04u.jpg\",\"format\":\"epub\",\"bookVersion\":101}",
                    i ? "," : "", i * 10, i * 10, i, i * 10);
            }
            used += (size_t)snprintf(buf + used, cap - used, "],\"bookProgress\":[");
            for (unsigned i = 0; i < 10; ++i) {
                used += (size_t)snprintf(buf + used, cap - used,
                    "%s{\"bookId\":\"book_%04u\",\"chapterUid\":2,\"chapterIdx\":2,\"chapterOffset\":200,\"progress\":25,\"updateTime\":1700009000}",
                    i ? "," : "", i * 5);
            }
            used += (size_t)snprintf(buf + used, cap - used, "]}");
        }
        *out = buf; *len = used;
        return ESP_OK;
    }
    if (strstr(url, "/book/chapterInfos")) {
        if (g_toc_payload) {
            *out = malloc(g_toc_payload_len + 1);
            assert(*out);
            memcpy(*out, g_toc_payload, g_toc_payload_len + 1);
            *len = g_toc_payload_len;
            return ESP_OK;
        }
        size_t cap = g_toc_chapters * 140 + 512;
        char *buf = malloc(cap);
        assert(buf);
        size_t used = (size_t)snprintf(buf, cap,
            "{\"data\":[{\"bookId\":\"bench_book\",\"bookVersion\":100,\"synckey\":1,\"clearAll\":1,\"updated\":[");
        for (unsigned i = 0; i < g_toc_chapters; ++i) {
            used += (size_t)snprintf(buf + used, cap - used,
                "%s{\"chapterUid\":%u,\"chapterIdx\":%u,\"title\":\"Chapter %u\",\"wordCount\":1200}",
                i ? "," : "", i + 1, i + 1, i + 1);
        }
        used += (size_t)snprintf(buf + used, cap - used, "]}]}");
        *out = buf; *len = used;
        return ESP_OK;
    }
    if (strstr(url, "/book/getProgress")) {
        const char *p = g_progress_payload ? g_progress_payload :
            "{\"bookId\":\"bench_book\",\"book\":{\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":1,\"updateTime\":100}}";
        *out = strdup(p); assert(*out); *len = strlen(*out);
        return ESP_OK;
    }
    if (strstr(url, "/book/read")) {
        (void)body;
        *out = strdup("{\"succ\":1}"); assert(*out); *len = strlen(*out);
        return ESP_OK;
    }
    assert(false && "unexpected URL");
    return ESP_FAIL;
}

static esp_err_t bench_http_download(const char *url, const char *headers, FILE *out, size_t limit,
                                     weread_cancel_fn cancel, void *cc, weread_download_info_t *info, void *ctx) {
    (void)headers; (void)ctx; (void)cancel; (void)cc;
    if (g_dl_fail) return ESP_ERR_TIMEOUT;
    char path[512];
    if (g_dl_file_override) {
        snprintf(path, sizeof(path), "%s", g_dl_file_override);
    } else if (strstr(url, "c_")) {
        snprintf(path, sizeof(path), "%s/cover.png", g_fixtures);
    } else {
        snprintf(path, sizeof(path), "%s/long_chapter.zip", g_fixtures);
    }
    FILE *in = fopen(path, "rb");
    assert(in);
    info->status = 200;
    strcpy(info->encrypt_key, "pDB9WzFl/4YFmRnIZe6L7tJOiLv+dZQ+ZqkP6U6I99g=");
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        assert(info->bytes + n <= limit);
        assert(fwrite(buf, 1, n, out) == n);
        info->bytes += n;
    }
    fclose(in);
    return ESP_OK;
}

/* ---- Workload 2: Chapter Assemble + Long-Chapter Map + CSS Switch + Image Probe ---- */
static int bench_chapter(const char *root, const char *fixtures) {
    init_auth_session(root);
    g_fixtures = fixtures;
    g_toc_chapters = 12;
    weread_client_set_transport(bench_http_request, NULL);
    weread_client_set_download_transport(bench_http_download, NULL);
    /* Single-book shelf seed */
    g_shelf_books = 1;
    assert(weread_shelf_sync(true) == ESP_OK);
    char shelf_json[512];
    snprintf(shelf_json, sizeof(shelf_json), "%s/shelf.json", root);
    /* Add bench_book explicitly via syncbook/custom */
    g_dl_file_override = NULL;
    assert(weread_toc_sync("bench_book") == ESP_OK);
    /* Make bench_book entry via shelf refresh */
    char long_zip[512];
    snprintf(long_zip, sizeof(long_zip), "%s/long_chapter.zip", fixtures);
    g_dl_file_override = long_zip;

    /* Put 200 dummy images in shared resources/Images/ and 2 css files in resources/Styles/ */
    char res_img_dir[512], res_css_dir[512];
    assert(wr_cache_path("bench_book", "resources/Images", res_img_dir, sizeof(res_img_dir)) == ESP_OK);
    assert(wr_cache_path("bench_book", "resources/Styles", res_css_dir, sizeof(res_css_dir)) == ESP_OK);
    assert(wr_mkdirs(res_img_dir) == ESP_OK && wr_mkdirs(res_css_dir) == ESP_OK);
    for (int i = 0; i < 200; ++i) {
        char p[512];
        snprintf(p, sizeof(p), "%s/img_%03d.png", res_img_dir, i);
        FILE *f = fopen(p, "wb"); assert(f); fwrite("PNG", 1, 3, f); fclose(f);
    }
    for (int i = 0; i < 2; ++i) {
        char p[512];
        snprintf(p, sizeof(p), "%s/common_%d.css", res_css_dir, i);
        FILE *f = fopen(p, "wb"); assert(f); fprintf(f, "p.c%d { margin: 4px; }\n", i); fclose(f);
    }

    /* 2a. Download + unpack + assemble + position.map write for 10 chapters */
    wr_perf_io_reset();
    uint64_t t0 = now_us();
    for (uint32_t uid = 1; uid <= 10; ++uid) {
        assert(weread_chapter_download("bench_book", uid) == ESP_OK);
    }
    uint64_t dt_asm = now_us() - t0;
    print_stats("chapter_download_assemble_10x", dt_asm, (double)dt_asm / 10000.0, "ms_per_ch");

    /* 2b. Open book + load 30k-char chapter + 40 bidirectional mappings */
    assert(book_weread_open("weread://bench_book") == ESP_OK);
    html_text_t text = {0};
    wr_perf_io_reset();
    t0 = now_us();
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK && text.len > 20000);
    assert(book_weread_commit_loaded(&(size_t){0}, &(uint32_t){0}) == ESP_OK);
    uint64_t dt_load = now_us() - t0;
    print_stats("long_chapter_load", dt_load, (double)book_weread_map_cache_bytes(), "map_cache_bytes");

    wr_perf_io_reset();
    t0 = now_us();
    for (int i = 0; i < 20; ++i) {
        uint32_t off = (uint32_t)((size_t)(i + 1) * (text.len / 22));
        uint32_t cloud = 0, back = 0;
        assert(book_weread_map_version(0, 100, true, off, &cloud) == ESP_OK);
        assert(book_weread_map_version(0, 100, false, cloud, &back) == ESP_OK);
    }
    uint64_t dt_map = now_us() - t0;
    print_stats("long_chapter_map_40x", dt_map, (double)book_weread_map_cache_bytes(), "map_cache_bytes");
    html_text_free(&text);

    /* 2c. 9 consecutive chapter switches (CSS rescan impact) */
    wr_perf_io_reset();
    t0 = now_us();
    for (size_t ch = 1; ch < 10; ++ch) {
        html_text_t t = {0};
        size_t c = ch; uint32_t v = 0;
        assert(book_weread_load(ch, NULL, NULL, &t) == ESP_OK);
        assert(book_weread_commit_loaded(&c, &v) == ESP_OK);
        html_text_free(&t);
    }
    uint64_t dt_sw = now_us() - t0;
    print_stats("chapter_switch_9x_with_200_res", dt_sw, (double)dt_sw / 9000.0, "ms_per_switch");

    /* 2d. Image dimensions probe on 512 KiB PNG */
    wr_perf_io_reset();
    t0 = now_us();
    unsigned w = 0, h = 0;
    {
        static const size_t kProbes[] = {64u, 4096u, 65536u, 0u};
        bool ok = false;
        for (size_t p = 0; p < sizeof(kProbes) / sizeof(kProbes[0]); ++p) {
            uint8_t *img = NULL; size_t ilen = 0, itotal = 0; bool png = false;
            assert(book_weread_image_prefix(0, "Images/big.png", kProbes[p], &img, &ilen, &png, &itotal) == ESP_OK);
            ok = book_image_dimensions(img, ilen, png, &w, &h);
            free(img);
            if (ok || !kProbes[p] || ilen >= itotal) break;
        }
        assert(ok);
    }
    uint64_t dt_img = now_us() - t0;
    assert(w > 0 && h > 0);
    print_stats("image_dim_probe_512k", dt_img, (double)w, "width");

    book_weread_close();
    weread_shelf_clear();
    weread_toc_free(&(weread_toc_t){0});
    assert(weread_auth_clear_session() == ESP_OK);
    weread_test_memory_report();
    return 0;
}

/* ---- Workload 3: 800-Book Shelf Cold Load, Sync, Incremental Sync, Snapshots, Lookups ---- */
static int bench_shelf(const char *root) {
    init_auth_session(root);
    g_shelf_books = 800;
    weread_client_set_transport(bench_http_request, NULL);

    /* 3a. Full sync 800 books (single response merge or batched) */
    g_shelf_mode = 0;
    wr_perf_io_reset();
    uint64_t t0 = now_us();
    assert(weread_shelf_sync(true) == ESP_OK);
    uint64_t dt_full = now_us() - t0;
    print_stats("shelf_800_full_sync", dt_full, 800.0, "books");

    /* 3b. Batched detail sync (initial sync + 10x incremental updates) */
    weread_shelf_clear();
    char sp[512]; snprintf(sp, sizeof(sp), "%s/shelf.json", root);
    unlink(sp);
    g_shelf_mode = 0;
    wr_perf_io_reset();
    t0 = now_us();
    /* Force step sync if API exposes weread_shelf_sync_step; fallback to 10x incremental updates */
    assert(weread_shelf_sync(true) == ESP_OK);
    g_shelf_mode = 1;
    for (int b = 0; b < 10; ++b) {
        assert(weread_shelf_sync(false) == ESP_OK);
    }
    uint64_t dt_inc = now_us() - t0;
    print_stats("shelf_800_sync_plus_10inc", dt_inc, 10.0, "inc_rounds");

    /* 3c. Cold load from disk + 800 lookups + snapshot */
    weread_shelf_clear();
    wr_perf_io_reset();
    t0 = now_us();
    assert(weread_shelf_load() == ESP_OK);
    uint64_t dt_load = now_us() - t0;
    print_stats("shelf_800_cold_load", dt_load, 800.0, "books");

    wr_perf_io_reset();
    t0 = now_us();
    for (unsigned i = 0; i < 800; ++i) {
        char id[32];
        snprintf(id, sizeof(id), "book_%04u", i);
        weread_book_t bk = {0};
        assert(weread_shelf_find(id, &bk));
        uint32_t ver = weread_shelf_book_version(id);
        assert(ver >= 100);
    }
    weread_shelf_item_t *snap = NULL; size_t scount = 0;
    assert(weread_shelf_snapshot(&snap, &scount) == ESP_OK && scount == 800);
    free(snap);
    uint64_t dt_lookup = now_us() - t0;
    print_stats("shelf_800_lookup_and_snap", dt_lookup, 800.0, "books");

    weread_shelf_clear();
    assert(weread_auth_clear_session() == ESP_OK);
    weread_test_memory_report();
    return 0;
}

/* ---- Workload 4: 3,000-Chapter TOC Load, Sync, Merge, Reader Offsets & Index ---- */
static void wait_worker_done(void);
static int bench_toc(const char *root, const char *fixtures) {
    init_auth_session(root);
    g_fixtures = fixtures;
    g_shelf_books = 1;
    g_toc_chapters = 3000;
    weread_client_set_transport(bench_http_request, NULL);
    assert(weread_shelf_sync(true) == ESP_OK);

    /* Rename first bookId or fetch bench_book via syncbook/toc directly */
    /* Write minimal shelf or let toc_sync run */
    wr_perf_io_reset();
    uint64_t t0 = now_us();
    assert(weread_toc_sync("bench_book") == ESP_OK);
    uint64_t dt_sync = now_us() - t0;
    print_stats("toc_3000_full_sync", dt_sync, 3000.0, "chapters");

    /* Incremental merge: 50 updated chapters */
    {
        size_t cap = 50 * 140 + 512;
        g_toc_payload = malloc(cap);
        assert(g_toc_payload);
        size_t used = (size_t)snprintf(g_toc_payload, cap,
            "{\"data\":[{\"bookId\":\"bench_book\",\"bookVersion\":100,\"synckey\":2,\"clearAll\":0,\"updated\":[");
        for (unsigned i = 0; i < 50; ++i) {
            unsigned uid = i * 50 + 1;
            used += (size_t)snprintf(g_toc_payload + used, cap - used,
                "%s{\"chapterUid\":%u,\"chapterIdx\":%u,\"title\":\"Updated Ch %u\",\"wordCount\":1500}",
                i ? "," : "", uid, uid, uid);
        }
        used += (size_t)snprintf(g_toc_payload + used, cap - used, "]}]}");
        g_toc_payload_len = used;
    }
    wr_perf_io_reset();
    t0 = now_us();
    assert(weread_toc_sync("bench_book") == ESP_OK);
    uint64_t dt_merge = now_us() - t0;
    print_stats("toc_3000_inc_merge_50", dt_merge, 50.0, "updated");
    free(g_toc_payload); g_toc_payload = NULL; g_toc_payload_len = 0;

    /* Cold load from disk */
    weread_toc_t toc = {0};
    wr_perf_io_reset();
    t0 = now_us();
    assert(weread_toc_load("bench_book", &toc) == ESP_OK && toc.count == 3000);
    uint64_t dt_load = now_us() - t0;
    print_stats("toc_3000_cold_load", dt_load, 3000.0, "chapters");
    weread_toc_free(&toc);

    /* Reader offsets & UID lookups */
    assert(book_weread_open("weread://bench_book") == ESP_OK);
    wr_perf_io_reset();
    t0 = now_us();
    volatile uint64_t acc = 0;
    for (size_t i = 0; i < 3000; ++i) {
        acc += book_weread_offset(i);
        acc += book_weread_total();
        assert(book_weread_index((uint32_t)(i + 1)) == i);
    }
    uint64_t dt_queries = now_us() - t0;
    (void)acc;
    print_stats("toc_3000_offset_total_index", dt_queries, 3000.0, "queries");
    book_weread_close();

    /* 3,000-chapter cache scan & WR_JOB_CACHE_BOOK initial scan (5 chapters cached on disk) */
    char long_zip[512];
    snprintf(long_zip, sizeof(long_zip), "%s/long_chapter.zip", fixtures);
    g_dl_file_override = long_zip;
    g_dl_fail = false;
    weread_client_set_download_transport(bench_http_download, NULL);
    for (uint32_t u = 1; u <= 5; ++u) assert(weread_chapter_download("bench_book", u) == ESP_OK);

    uint16_t cached_cnt = 0, total_cnt = 0;
    wr_perf_io_reset();
    t0 = now_us();
    assert(weread_chapter_count_cached("bench_book", &cached_cnt, &total_cnt, NULL, NULL) == ESP_OK);
    uint64_t dt_scan = now_us() - t0;
    assert(cached_cnt == 5 && total_cnt == 3000);
    print_stats("toc_3000_count_cached_scan", dt_scan, (double)cached_cnt, "cached");

    weread_runtime_init();
    g_dl_fail = true;
    wr_perf_io_reset();
    t0 = now_us();
    assert(weread_runtime_start(WR_JOB_CACHE_BOOK, "bench_book", NULL));
    wait_worker_done();
    uint64_t dt_cb = now_us() - t0;
    print_stats("cache_book_3000ch_scan_5cached", dt_cb, 3000.0, "chapters");
    g_dl_fail = false;
    g_dl_file_override = NULL;
    weread_runtime_stop();

    weread_shelf_clear();
    assert(weread_auth_clear_session() == ESP_OK);
    weread_test_memory_report();
    return 0;
}

/* ---- Workload 5: Local-Fast Open + 20 Consecutive Page-Turn Saves ---- */
static void wait_worker_done(void) {
    uint64_t deadline = now_us() + 5000000ULL;
    while (weread_runtime_busy()) {
        assert(now_us() < deadline);
        usleep(500);
    }
    weread_result_t r;
    if (weread_runtime_poll(&r)) weread_runtime_result_free(&r);
}

static int bench_open_save(const char *root, const char *fixtures) {
    init_auth_session(root);
    g_fixtures = fixtures;
    g_shelf_books = 1;
    g_toc_chapters = 4;
    weread_client_set_transport(bench_http_request, NULL);
    weread_client_set_download_transport(bench_http_download, NULL);
    g_dl_file_override = NULL;

    /* Seed single-book shelf for bench_book via real weread_shelf_sync */
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("bench_book") == ESP_OK);
    assert(weread_chapter_download("bench_book", 1) == ESP_OK);
    weread_progress_t init_p = {.chapter_uid = 1, .chapter_index = 1, .chapter_offset = 10, .book_version = 100};
    assert(weread_progress_select("bench_book", &init_p, true) == ESP_OK);
    weread_runtime_init();

    /* Measure local-fast open (WR_JOB_OPEN + book_weread_open + book_weread_load) */
    g_rescue.reads = g_rescue.writes = g_rescue.erases = 0;
    wr_perf_io_reset();
    uint64_t t0 = now_us();
    assert(weread_runtime_start(WR_JOB_OPEN, "bench_book", NULL));
    wait_worker_done();
    assert(book_weread_open("weread://bench_book") == ESP_OK);
    size_t ch = 0; uint32_t off = 0;
    assert(weread_reader_resume("weread://bench_book", &ch, &off) == ESP_OK);
    html_text_t text = {0};
    assert(book_weread_load(ch, NULL, NULL, &text) == ESP_OK);
    assert(book_weread_commit_loaded(&ch, &(uint32_t){100}) == ESP_OK);
    uint64_t dt_open = now_us() - t0;
    print_stats("local_fast_open_long", dt_open, (double)g_rescue.reads, "nvs_reads");

    /* 20 consecutive page-turn saves */
    weread_reader_begin("weread://bench_book", 1000);
    g_rescue.reads = g_rescue.writes = g_rescue.erases = 0;
    wr_perf_io_reset();
    t0 = now_us();
    for (unsigned p = 1; p <= 20; ++p) {
        uint32_t target = (uint32_t)((size_t)p * (text.len / 25));
        weread_reader_navigated_version("weread://bench_book", 0, target, (unsigned)(p * 4), "Bench", 100);
        assert(weread_reader_save(1000 + p * 1200) == ESP_OK);
    }
    uint64_t dt_save = now_us() - t0;
    printf("NVS bench_open_save_20x nvs_reads=%llu nvs_writes=%llu nvs_erases=%llu\n",
           (unsigned long long)g_rescue.reads, (unsigned long long)g_rescue.writes, (unsigned long long)g_rescue.erases);
    print_stats("page_turn_save_20x_long", dt_save, (double)dt_save / 20000.0, "ms_per_save");

    html_text_free(&text);
    weread_reader_end(30000);
    book_weread_close();
    weread_runtime_stop();
    weread_shelf_clear();
    assert(weread_auth_clear_session() == ESP_OK);
    weread_test_memory_report();
    return 0;
}

/* ---- Workload 6: 10-Chapter Prefetch + Probe Shelf + Cache Book Scan ---- */
static int bench_prefetch(const char *root, const char *fixtures) {
    init_auth_session(root);
    g_fixtures = fixtures;
    g_shelf_books = 800;
    g_toc_chapters = 20;
    weread_client_set_transport(bench_http_request, NULL);
    weread_client_set_download_transport(bench_http_download, NULL);
    g_shelf_mode = 0;
    assert(weread_shelf_sync(true) == ESP_OK);
    /* Also write toc for book_0000 */
    g_toc_payload = malloc(20 * 140 + 512);
    size_t used = (size_t)snprintf(g_toc_payload, 20 * 140 + 512,
        "{\"data\":[{\"bookId\":\"book_0000\",\"bookVersion\":100,\"synckey\":1,\"clearAll\":1,\"updated\":[");
    for (unsigned i = 0; i < 20; ++i) {
        used += (size_t)snprintf(g_toc_payload + used, 20 * 140 + 512 - used,
            "%s{\"chapterUid\":%u,\"chapterIdx\":%u,\"title\":\"Ch %u\"}", i ? "," : "", i + 1, i + 1, i + 1);
    }
    used += (size_t)snprintf(g_toc_payload + used, 20 * 140 + 512 - used, "]}]}");
    g_toc_payload_len = used;
    assert(weread_toc_sync("book_0000") == ESP_OK);
    free(g_toc_payload); g_toc_payload = NULL; g_toc_payload_len = 0;

    char small_zip[512];
    snprintf(small_zip, sizeof(small_zip), "%s/small_chapter.zip", fixtures);
    g_dl_file_override = small_zip;

    assert(weread_chapter_download("book_0000", 1) == ESP_OK);
    weread_progress_t init_p = {.chapter_uid = 1, .chapter_index = 1, .chapter_offset = 0, .book_version = 100};
    assert(weread_progress_select("book_0000", &init_p, true) == ESP_OK);
    weread_runtime_init();
    assert(weread_runtime_start(WR_JOB_OPEN, "book_0000", NULL));
    wait_worker_done();
    assert(book_weread_open("weread://book_0000") == ESP_OK);
    size_t pch = 0; uint32_t poff = 0;
    assert(weread_reader_resume("weread://book_0000", &pch, &poff) == ESP_OK);
    weread_reader_begin("weread://book_0000", 1000);

    assert(weread_runtime_start(WR_JOB_PREFETCH, "book_0000", "scan"));
    wait_worker_done();
    wr_perf_io_reset();
    uint64_t t0 = now_us();
    /* Prefetch chapters 2..11 via "forward" after scan is marked complete */
    for (size_t idx = 1; idx <= 10; ++idx) {
        assert(weread_runtime_start(WR_JOB_PREFETCH, "book_0000", "forward"));
        wait_worker_done();
    }
    uint64_t dt_pf = now_us() - t0;
    print_stats("prefetch_10ch_with_800_shelf", dt_pf, 10.0, "chapters");

    /* Probe shelf 9 visible books */
    wr_perf_io_reset();
    t0 = now_us();
    assert(weread_runtime_start(WR_JOB_PROBE_SHELF, "", "book_0000,book_0001,book_0002,book_0003,book_0004,book_0005,book_0006,book_0007,book_0008"));
    wait_worker_done();
    uint64_t dt_probe = now_us() - t0;
    print_stats("probe_shelf_9_with_800_shelf", dt_probe, 9.0, "books");

    weread_reader_end(2000);
    book_weread_close();
    weread_runtime_stop();
    weread_shelf_clear();
    assert(weread_auth_clear_session() == ESP_OK);
    weread_test_memory_report();
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <bench> <storage_root> <fixtures_dir>\n", argv[0]);
        return 2;
    }
    const char *bench = argv[1];
    const char *root = argv[2];
    const char *fixtures = argv[3];
    if (!strcmp(bench, "crypto")) return bench_crypto(fixtures);
    if (!strcmp(bench, "chapter")) return bench_chapter(root, fixtures);
    if (!strcmp(bench, "shelf")) return bench_shelf(root);
    if (!strcmp(bench, "toc")) return bench_toc(root, fixtures);
    if (!strcmp(bench, "open_save")) return bench_open_save(root, fixtures);
    if (!strcmp(bench, "prefetch")) return bench_prefetch(root, fixtures);
    fprintf(stderr, "Unknown bench: %s\n", bench);
    return 2;
}
