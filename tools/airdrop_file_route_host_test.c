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
#include <string.h>

#include "../components/read_pico_airdrop/airdrop_file_route.h"
#include "../components/read_pico_airdrop/airdrop/core/ad_cpio_stream.h"

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

typedef struct {
    int begins;
    int ends;
    bool last_complete;
    char last_name[128];
    uint8_t payload[64];
    uint32_t payload_len;
} sink_recorder_t;

static int sink_begin(void *ctx, const struct AdCpioStreamFile *f) {
    sink_recorder_t *rec = (sink_recorder_t *)ctx;
    const char *base = NULL;
    airdrop_route_kind_t route = airdrop_classify_filename(f->name, &base);
    if (route == AIRDROP_ROUTE_SKIP) {
        return AD_CPIO_FILE_SKIP;
    }
    if (route == AIRDROP_ROUTE_INVALID) {
        return AD_CPIO_FILE_FAIL;
    }
    ++rec->begins;
    snprintf(rec->last_name, sizeof(rec->last_name), "%s", base);
    rec->payload_len = 0;
    return AD_CPIO_FILE_TAKE;
}

static bool sink_data(void *ctx, const uint8_t *data, uint32_t len) {
    sink_recorder_t *rec = (sink_recorder_t *)ctx;
    if (rec->payload_len + len <= sizeof(rec->payload)) {
        memcpy(rec->payload + rec->payload_len, data, len);
    }
    rec->payload_len += len;
    return true;
}

static void sink_end(void *ctx, bool complete) {
    sink_recorder_t *rec = (sink_recorder_t *)ctx;
    ++rec->ends;
    rec->last_complete = complete;
}

int main(void) {
    const char *base = NULL;
    assert(airdrop_classify_filename("./books/._novel.epub", &base) == AIRDROP_ROUTE_SKIP);
    assert(airdrop_classify_filename(".DS_Store", &base) == AIRDROP_ROUTE_SKIP);
    assert(airdrop_classify_filename("folder/novel.EPUB", &base) == AIRDROP_ROUTE_BOOK && strcmp(base, "novel.EPUB") == 0);
    assert(airdrop_classify_filename("notes.txt", &base) == AIRDROP_ROUTE_BOOK && strcmp(base, "notes.txt") == 0);
    assert(airdrop_classify_filename("font.TTF", &base) == AIRDROP_ROUTE_FONT && strcmp(base, "font.TTF") == 0);
    assert(airdrop_classify_filename("font.otf", &base) == AIRDROP_ROUTE_FONT && strcmp(base, "font.otf") == 0);
    assert(airdrop_classify_filename("cover.JPEG", &base) == AIRDROP_ROUTE_PICTURE && strcmp(base, "cover.JPEG") == 0);
    assert(airdrop_classify_filename("cover.png", &base) == AIRDROP_ROUTE_PICTURE && strcmp(base, "cover.png") == 0);
    assert(airdrop_classify_filename("archive.pdf", &base) == AIRDROP_ROUTE_OTHER && strcmp(base, "archive.pdf") == 0);
    assert(airdrop_classify_filename("..", &base) == AIRDROP_ROUTE_INVALID);

    fake_fs_t fs = {
        .existing = {
            "/sdcard/books/novel.epub",
            "/sdcard/books/novel_1.epub",
        },
        .count = 2,
    };
    char path[320] = {0};
    char name[128] = {0};
    assert(airdrop_resolve_unique_path("/sdcard/books", "novel.epub", fake_exists, &fs,
                                       path, sizeof(path), name, sizeof(name)));
    assert(strcmp(path, "/sdcard/books/novel_2.epub") == 0);
    assert(strcmp(name, "novel_2.epub") == 0);

    // 验证 ODC CPIO 流式解包跳过 "._" 元数据并完整提取正常文件。
    // Verify ODC CPIO streaming skips "._" AppleDouble sidecar and extracts the real file.
    sink_recorder_t rec = {0};
    struct AdCpioStream st;
    struct AdCpioStreamOps ops = {&rec, sink_begin, sink_data, sink_end};
    ad_cpio_stream_init(&st, ops);

    // Entry 1: AppleDouble sidecar "._a.txt" (size 4, mode 0100644)
    // Entry 2: Regular file "a.txt" (size 5, mode 0100644, content "hello")
    // Entry 3: "TRAILER!!!"
    const char odc_stream[] =
        "07070700000100000110064400000000000000000100000000000000000100000000004._a.txt\0junk"
        "070707000001000001100644000000000000000001000000000000000000600000000005a.txt\0hello"
        "070707000000000000000000000000000000000001000000000000000001300000000000TRAILER!!!\0";
    uint32_t consumed = 0;
    int rc = ad_cpio_stream_feed(&st, (const uint8_t *)odc_stream, sizeof(odc_stream) - 1, &consumed);
    assert(rc == AD_CPIO_STREAM_DONE);
    assert(ad_cpio_stream_finish(&st) == AD_CPIO_STREAM_DONE);
    assert(rec.begins == 1 && rec.ends == 1 && rec.last_complete);
    assert(strcmp(rec.last_name, "a.txt") == 0);
    assert(rec.payload_len == 5 && memcmp(rec.payload, "hello", 5) == 0);

    puts("airdrop_file_route_host_test: PASS");
    return 0;
}
