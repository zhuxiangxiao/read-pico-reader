/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：隔空投送文件归类、去重后缀与流式 CPIO 解包宿主测试。
 *
 * English: Host test for AirDrop file routing, collision suffixing, and streaming CPIO extraction.
 *
 * 冻结：仅供宿主测试。
 * Frozen: Host tests only.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../components/read_pico_airdrop/airdrop_file_route.h"
#include "../components/read_pico_airdrop/airdrop/core/ad_cpio_stream.h"

/* ---- 路由与重名 / Routing and collisions ---- */

typedef struct {
    const char *existing[8];
    size_t count;
} fake_fs_t;

static bool fake_exists(const char *path, void *ctx) {
    const fake_fs_t *fs = (const fake_fs_t *)ctx;
    for (size_t i = 0; i < fs->count; ++i) {
        if (fs->existing[i] && strcmp(fs->existing[i], path) == 0) {
            return true;
        }
    }
    return false;
}

/* ---- CPIO fixture builders ---- */

static void put_oct(uint8_t *p, int n, uint32_t v) {
    for (int i = n - 1; i >= 0; i--) {
        p[i] = (uint8_t)('0' + (v & 7));
        v >>= 3;
    }
}

static void put_hex(uint8_t *p, int n, uint32_t v) {
    for (int i = n - 1; i >= 0; i--) {
        const uint32_t d = v & 0xf;
        p[i] = (uint8_t)(d < 10 ? '0' + d : 'a' + d - 10);
        v >>= 4;
    }
}

/* ODC (070707): 76-byte header, no padding. */
static uint32_t build_odc_entry(uint8_t *p, uint32_t mode, const char *name, const void *data,
                                uint32_t len) {
    const uint32_t namesize = (uint32_t)strlen(name) + 1;
    memcpy(p, "070707", 6);
    put_oct(p + 6, 6, 0);      /* dev */
    put_oct(p + 12, 6, 1);     /* ino */
    put_oct(p + 18, 6, mode);
    put_oct(p + 24, 6, 0);     /* uid */
    put_oct(p + 30, 6, 0);     /* gid */
    put_oct(p + 36, 6, 1);     /* nlink */
    put_oct(p + 42, 6, 0);     /* rdev */
    put_oct(p + 48, 11, 0);    /* mtime */
    put_oct(p + 59, 6, namesize);
    put_oct(p + 65, 11, len);
    uint32_t off = 76;
    memcpy(p + off, name, namesize);
    off += namesize;
    if (len) {
        memcpy(p + off, data, len);
        off += len;
    }
    return off;
}

/* Newc (070701): 110-byte header, name and payload padded to 4 bytes. */
static uint32_t build_newc_entry(uint8_t *p, uint32_t mode, const char *name, const void *data,
                                 uint32_t len) {
    const uint32_t namesize = (uint32_t)strlen(name) + 1;
    memcpy(p, "070701", 6);
    put_hex(p + 6, 8, 1);      /* ino */
    put_hex(p + 14, 8, mode);
    put_hex(p + 22, 8, 0);     /* uid */
    put_hex(p + 30, 8, 0);     /* gid */
    put_hex(p + 38, 8, 1);     /* nlink */
    put_hex(p + 46, 8, 0);     /* mtime */
    put_hex(p + 54, 8, len);
    put_hex(p + 62, 8, 0);     /* devmajor */
    put_hex(p + 70, 8, 0);     /* devminor */
    put_hex(p + 78, 8, 0);     /* rdevmajor */
    put_hex(p + 86, 8, 0);     /* rdevminor */
    put_hex(p + 94, 8, namesize);
    put_hex(p + 102, 8, 0);    /* check */
    uint32_t off = 110;
    memcpy(p + off, name, namesize);
    off += namesize;
    while (off & 3u) p[off++] = 0;
    if (len) {
        memcpy(p + off, data, len);
        off += len;
    }
    while (off & 3u) p[off++] = 0;
    return off;
}

/* ---- Sink recorder ---- */

enum sink_mode { SINK_TAKE = 0, SINK_SKIP = 1, SINK_FAIL = 2 };

typedef struct {
    enum sink_mode mode;
    int fail_data_at; /* -1 = never; else fail on this 0-based on_data call */
    int begins;
    int skips;
    int fails;
    int ends_complete;
    int ends_incomplete;
    int data_calls;
    uint32_t payload_len;
    uint8_t payload[64];
    char name[256];
} sink_recorder_t;

static int sink_begin(void *ctx, const struct AdCpioStreamFile *f) {
    sink_recorder_t *rec = (sink_recorder_t *)ctx;
    const char *base = NULL;
    const airdrop_route_kind_t route = airdrop_classify_filename(f->name, &base);
    if (route == AIRDROP_ROUTE_SKIP || rec->mode == SINK_SKIP) {
        ++rec->skips;
        return AD_CPIO_FILE_SKIP;
    }
    if (route == AIRDROP_ROUTE_INVALID || rec->mode == SINK_FAIL) {
        ++rec->fails;
        return AD_CPIO_FILE_FAIL;
    }
    ++rec->begins;
    snprintf(rec->name, sizeof(rec->name), "%s", base);
    rec->payload_len = 0;
    return AD_CPIO_FILE_TAKE;
}

static bool sink_data(void *ctx, const uint8_t *data, uint32_t len) {
    sink_recorder_t *rec = (sink_recorder_t *)ctx;
    if (rec->fail_data_at >= 0 && rec->data_calls == rec->fail_data_at) {
        ++rec->data_calls;
        return false;
    }
    if (rec->payload_len + len <= sizeof(rec->payload)) {
        memcpy(rec->payload + rec->payload_len, data, len);
    }
    rec->payload_len += len;
    ++rec->data_calls;
    return true;
}

static void sink_end(void *ctx, bool complete) {
    sink_recorder_t *rec = (sink_recorder_t *)ctx;
    if (complete) {
        ++rec->ends_complete;
    } else {
        ++rec->ends_incomplete;
    }
}

/* Feed the whole archive in `chunk`-sized pieces, then finish the stream. */
static int feed_all(const uint8_t *buf, uint32_t len, uint32_t chunk, sink_recorder_t *rec) {
    struct AdCpioStream st;
    struct AdCpioStreamOps ops = {rec, sink_begin, sink_data, sink_end};
    ad_cpio_stream_init(&st, ops);
    uint32_t off = 0;
    int rc = AD_CPIO_STREAM_OK;
    while (off < len && rc == AD_CPIO_STREAM_OK) {
        uint32_t use = len - off < chunk ? len - off : chunk;
        uint32_t used = 0;
        rc = ad_cpio_stream_feed(&st, buf + off, use, &used);
        off += used;
        if (used == 0 && rc == AD_CPIO_STREAM_OK) {
            break; /* no progress: malformed */
        }
    }
    if (rc == AD_CPIO_STREAM_OK || rc == AD_CPIO_STREAM_DONE) {
        rc = ad_cpio_stream_finish(&st);
    }
    return rc;
}

static void check_chunks(const uint8_t *buf, uint32_t len, const char *expect_name,
                         const char *expect_payload) {
    const uint32_t payload_len = (uint32_t)strlen(expect_payload);
    for (uint32_t chunk = 1; chunk <= 128; chunk = chunk * 2 + 1) {
        sink_recorder_t rec = {.fail_data_at = -1};
        assert(feed_all(buf, len, chunk, &rec) == AD_CPIO_STREAM_DONE);
        assert(rec.begins == 1 && rec.ends_complete == 1 && rec.ends_incomplete == 0);
        assert(strcmp(rec.name, expect_name) == 0);
        assert(rec.payload_len == payload_len);
        assert(memcmp(rec.payload, expect_payload, payload_len) == 0);
    }
}

int main(void) {
    /* ---- Routing / collision rules ---- */
    const char *base = NULL;
    assert(airdrop_classify_filename("./books/._novel.epub", &base) == AIRDROP_ROUTE_SKIP);
    assert(airdrop_classify_filename(".DS_Store", &base) == AIRDROP_ROUTE_SKIP);
    assert(airdrop_classify_filename("folder/novel.EPUB", &base) == AIRDROP_ROUTE_BOOK &&
           strcmp(base, "novel.EPUB") == 0);
    assert(airdrop_classify_filename("notes.txt", &base) == AIRDROP_ROUTE_BOOK &&
           strcmp(base, "notes.txt") == 0);
    assert(airdrop_classify_filename("font.TTF", &base) == AIRDROP_ROUTE_FONT);
    assert(airdrop_classify_filename("font.otf", &base) == AIRDROP_ROUTE_FONT);
    assert(airdrop_classify_filename("cover.JPEG", &base) == AIRDROP_ROUTE_PICTURE);
    assert(airdrop_classify_filename("cover.png", &base) == AIRDROP_ROUTE_PICTURE);
    assert(airdrop_classify_filename("archive.pdf", &base) == AIRDROP_ROUTE_OTHER);
    assert(airdrop_classify_filename("..", &base) == AIRDROP_ROUTE_INVALID);

    fake_fs_t fs = {
        .existing = {"/sdcard/books/novel.epub", "/sdcard/books/novel_1.epub"},
        .count = 2,
    };
    char path[320] = {0};
    char name[128] = {0};
    assert(airdrop_resolve_unique_path("/sdcard/books", "novel.epub", fake_exists, &fs, path,
                                       sizeof(path), name, sizeof(name)));
    assert(strcmp(path, "/sdcard/books/novel_2.epub") == 0);
    assert(strcmp(name, "novel_2.epub") == 0);

    /* ---- CPIO: Apple-shaped ODC archive (directory, then ./file) ---- */
    uint8_t odc[1024];
    uint32_t odc_n = 0;
    odc_n += build_odc_entry(odc + odc_n, 0040755u, ".", NULL, 0);
    odc_n += build_odc_entry(odc + odc_n, 0100644u, "./novel.epub", "hello", 5);
    odc_n += build_odc_entry(odc + odc_n, 0, "TRAILER!!!", NULL, 0);
    check_chunks(odc, odc_n, "novel.epub", "hello");

    /* ---- CPIO: Apple-shaped NEWC archive with 4-byte padding ---- */
    uint8_t newc[2048];
    uint32_t newc_n = 0;
    newc_n += build_newc_entry(newc + newc_n, 0040755u, ".", NULL, 0);
    newc_n += build_newc_entry(newc + newc_n, 0100644u, "./novel.epub", "hello", 5);
    newc_n += build_newc_entry(newc + newc_n, 0, "TRAILER!!!", NULL, 0);
    check_chunks(newc, newc_n, "novel.epub", "hello");

    /* ---- Sidecar is skipped with no data/end callbacks; real file still delivered ---- */
    uint8_t side[1024];
    uint32_t side_n = 0;
    side_n += build_odc_entry(side + side_n, 0100644u, "._novel.epub", "junk", 4);
    side_n += build_odc_entry(side + side_n, 0100644u, "./novel.epub", "hello", 5);
    side_n += build_odc_entry(side + side_n, 0, "TRAILER!!!", NULL, 0);
    {
        sink_recorder_t rec = {.fail_data_at = -1};
        assert(feed_all(side, side_n, 3, &rec) == AD_CPIO_STREAM_DONE);
        assert(rec.begins == 1 && rec.skips == 1);
        assert(rec.data_calls == 1 && rec.ends_complete == 1 && rec.ends_incomplete == 0);
        assert(strcmp(rec.name, "novel.epub") == 0);
    }

    /* ---- Zero-length regular file: begin + end(true) with no data ---- */
    uint8_t empty[256];
    uint32_t empty_n = 0;
    empty_n += build_odc_entry(empty + empty_n, 0100644u, "empty.txt", NULL, 0);
    empty_n += build_odc_entry(empty + empty_n, 0, "TRAILER!!!", NULL, 0);
    {
        sink_recorder_t rec = {.fail_data_at = -1};
        assert(feed_all(empty, empty_n, 7, &rec) == AD_CPIO_STREAM_DONE);
        assert(rec.begins == 1 && rec.data_calls == 0);
        assert(rec.ends_complete == 1 && rec.ends_incomplete == 0);
    }

    /* ---- Truncated payload: one incomplete end, BAD verdict ---- */
    {
        uint8_t trunc[512];
        uint32_t trunc_n = 0;
        char big[32];
        memset(big, 'x', sizeof(big));
        trunc_n += build_odc_entry(trunc + trunc_n, 0100644u, "big.bin", big, 20);
        trunc_n += build_odc_entry(trunc + trunc_n, 0, "TRAILER!!!", NULL, 0);
        sink_recorder_t rec = {.fail_data_at = -1};
        const int rc = feed_all(trunc, 76 + 8 + 18, 4, &rec);
        assert(rc == AD_CPIO_STREAM_BAD);
        assert(rec.begins == 1 && rec.ends_incomplete == 1 && rec.ends_complete == 0);
    }

    /* ---- on_data failure: WRITE verdict, on_end(false) exactly once ---- */
    {
        uint8_t data_archive[512];
        uint32_t data_n = 0;
        char big[32];
        memset(big, 'y', sizeof(big));
        data_n += build_odc_entry(data_archive + data_n, 0100644u, "big.bin", big, 20);
        data_n += build_odc_entry(data_archive + data_n, 0, "TRAILER!!!", NULL, 0);
        sink_recorder_t rec = {.fail_data_at = 0};
        const int rc = feed_all(data_archive, data_n, 2, &rec);
        assert(rc == AD_CPIO_STREAM_WRITE);
        assert(rec.begins == 1 && rec.ends_incomplete == 1 && rec.ends_complete == 0);
    }

    /* ---- on_begin failure: WRITE verdict, no on_end ---- */
    {
        uint8_t one[512];
        uint32_t one_n = 0;
        one_n += build_odc_entry(one + one_n, 0100644u, "novel.epub", "hello", 5);
        one_n += build_odc_entry(one + one_n, 0, "TRAILER!!!", NULL, 0);
        sink_recorder_t rec = {.mode = SINK_FAIL, .fail_data_at = -1};
        assert(feed_all(one, one_n, 2, &rec) == AD_CPIO_STREAM_WRITE);
        assert(rec.fails == 1 && rec.ends_incomplete == 0 && rec.ends_complete == 0);
    }

    /* ---- No TRAILER but a clean record boundary: finish() reports DONE ---- */
    {
        uint8_t clean[512];
        uint32_t clean_n = 0;
        clean_n += build_odc_entry(clean + clean_n, 0100644u, "novel.epub", "hello", 5);
        sink_recorder_t rec = {.fail_data_at = -1};
        assert(feed_all(clean, clean_n, 5, &rec) == AD_CPIO_STREAM_DONE);
        assert(rec.begins == 1 && rec.ends_complete == 1);
    }

    /* ---- Over-long name: streaming parser rejects the archive (documented limit) ---- */
    {
        char long_name[258];
        memset(long_name, 'a', 257);
        long_name[257] = 0;
        uint8_t long_archive[1024];
        const uint32_t long_n = build_odc_entry(long_archive, 0100644u, long_name, "x", 1);
        sink_recorder_t rec = {.fail_data_at = -1};
        assert(feed_all(long_archive, long_n, 64, &rec) == AD_CPIO_STREAM_BAD);
    }

    puts("airdrop_file_route_host_test: PASS");
    return 0;
}
