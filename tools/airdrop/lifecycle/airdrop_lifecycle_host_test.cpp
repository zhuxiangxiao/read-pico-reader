/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：R-06/R-08/R-09/R-10/R-18 接收生命周期本地主机回归；真实 read_pico_airdrop.cpp + 平台桩。
 * English: Local host regression for R-06/R-08/R-09/R-10/R-18 receive lifecycle; real read_pico_airdrop.cpp with platform stubs.
 */
/* Host test for the AirDrop receive lifecycle P1s: R-06, R-08, R-09, R-10, R-18.
 *
 * 中文：生命周期主机回归。把真实的 components/read_pico_airdrop/read_pico_airdrop.cpp
 * 连同桩化的 AWDL/BLE/port/esp_vfs_fat 一起编进本 TU，用真实文件系统（build/airdrop-tests 下的
 * 临时目录）验证：临时文件+rename、启动清理、失败清理、stop 前归档排空、busy 计入归档、
 * resume 失败留 ERROR、resume 不再自拷贝目录。
 *
 * English: Lifecycle host regression. The REAL read_pico_airdrop.cpp is included in this TU
 * with stubbed AWDL/BLE/port/esp_vfs_fat surfaces, and a real filesystem (a temp dir under
 * build/airdrop-tests) exercises: temp-file + rename, start-time sweep, start-failure cleanup,
 * archive drain before stop, busy() counting a pending archive, resume failure leaving
 * ERROR, and resume no longer copying directories onto themselves.
 *
 * Run from the repository root (see the build command in the R-05 test header; add
 * -Icomponents/read_pico_airdrop/include -Icomponents/read_pico_airdrop
 * -Icomponents/ble_page_turner/include).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "read_pico_airdrop.cpp"

/* ---- stubs for the surfaces read_pico_airdrop.cpp calls ---- */
static int g_ble_acquire_calls = 0;
static int g_ble_release_calls = 0;
static esp_err_t g_ble_acquire_rc = ESP_OK;
static int g_awdl_begin_calls = 0;
static int g_awdl_end_calls = 0;
static esp_err_t g_awdl_begin_rc = ESP_OK;
static esp_err_t g_awdl_end_rc = ESP_OK;
/* New awdl contract: the first N end() calls time out, the first N begin() calls report an
 * incomplete previous teardown. 新契约：前 N 次 end 超时、前 N 次 begin 报上次未拆完。 */
static int g_awdl_end_timeouts_remaining = 0;
static int g_awdl_begin_invalid_state_remaining = 0;
static int g_ad_begin_calls = 0;
static int g_ad_end_calls = 0;
static int g_ad_poll_calls = 0;
static int g_ad_begin_rc = 0;
static bool g_arch_pending = false;
static bool g_ad_stats_active = false;
static uint32_t g_ad_stats_file_err = 0;
static std::vector<std::string> g_call_log;
static struct AdFileOps g_registered_ops;
static int g_heap_outstanding = 0;

extern "C" {
int esp_read_mac(uint8_t *mac, int) {
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)(0x10 + i);
    return 0;
}
esp_err_t esp_vfs_fat_info(const char *, uint64_t *total, uint64_t *freeb) {
    *total = 1024ull * 1024ull * 1024ull;
    *freeb = 512ull * 1024ull * 1024ull;
    return ESP_OK;
}
void *heap_caps_malloc(size_t n, uint32_t) { void *p = malloc(n); if (p) ++g_heap_outstanding; return p; }
void *heap_caps_calloc(size_t n, size_t sz, uint32_t) { void *p = calloc(n, sz); if (p) ++g_heap_outstanding; return p; }
void *heap_caps_realloc(void *p, size_t n, uint32_t) { return realloc(p, n); }
void heap_caps_free(void *p) { if (p) --g_heap_outstanding; free(p); }
void vTaskDelay(TickType_t) {}
}  /* extern "C" */

/* The project headers declare these with C++ linkage (they are compiled as C++ in the
 * firmware), so their stubs stay outside the extern "C" block above. */
esp_err_t ble_pt_network_acquire(void) { ++g_ble_acquire_calls; return g_ble_acquire_rc; }
void ble_pt_network_release(void) { ++g_ble_release_calls; }
esp_err_t awdl_begin(void) {
    ++g_awdl_begin_calls;
    if (g_awdl_begin_invalid_state_remaining > 0) {
        --g_awdl_begin_invalid_state_remaining;
        return ESP_ERR_INVALID_STATE;   /* 上次 teardown 未完成 / previous teardown incomplete */
    }
    return g_awdl_begin_rc;
}
esp_err_t awdl_end(void) {
    ++g_awdl_end_calls;
    if (g_awdl_end_timeouts_remaining > 0) {
        --g_awdl_end_timeouts_remaining;
        return ESP_ERR_TIMEOUT;        /* 半拆、可重试 / half-torn, retryable */
    }
    return g_awdl_end_rc;
}
void ad_on_file(struct AdFileOps ops) { g_registered_ops = ops; }
int ad_begin(const char *, uint32_t) { ++g_ad_begin_calls; return g_ad_begin_rc; }
void ad_end(void) { ++g_ad_end_calls; g_call_log.push_back("end"); }
void ad_poll(void) { ++g_ad_poll_calls; g_call_log.push_back("poll"); }
bool ad_arch_pending(void) { return g_arch_pending; }
void ad_stats_read(struct AdStatus *out) {
    memset(out, 0, sizeof(*out));
    out->rx_active = g_ad_stats_active ? 1 : 0;
    out->file_err = g_ad_stats_file_err;
    out->rx_bytes = 0;
    out->rx_max = 0;
}

/* ---- helpers ---- */
static int g_failures = 0;
static std::string g_root;

static void check(bool ok, const char *what) {
    if (!ok) { printf("FAIL: %s\n", what); ++g_failures; }
}

static std::string join(const std::string &a, const std::string &b) { return a + "/" + b; }

static bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

static std::string read_file(const std::string &p) {
    FILE *f = fopen(p.c_str(), "rb");
    if (!f) return "<missing>";
    char buf[256] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    return std::string(buf, n);
}

static void write_file(const std::string &p, const char *data) {
    FILE *f = fopen(p.c_str(), "wb");
    if (f) { fwrite(data, 1, strlen(data), f); fclose(f); }
}

/* Every temp file (our prefix + .part) currently in the directory. */
static std::vector<std::string> temp_files(const std::string &dir) {
    std::vector<std::string> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    struct dirent *e;
    while ((e = readdir(d)) != nullptr) {
        if (is_temp_name(e->d_name)) out.push_back(e->d_name);
    }
    closedir(d);
    return out;
}

static void make_dirs(void) {
    g_root = "build/airdrop-tests/airdrop_lifecycle_tmp";
    std::string cmd = "rm -rf '" + g_root + "'";
    if (system(cmd.c_str()) != 0) { /* first run: nothing to remove */ }
    mkdir("build/airdrop-tests", 0775);
    mkdir(g_root.c_str(), 0775);
    mkdir(join(g_root, "books").c_str(), 0775);
    mkdir(join(g_root, "fonts").c_str(), 0775);
    mkdir(join(g_root, "pictures").c_str(), 0775);
}

static read_pico_airdrop_cfg_t make_cfg(void) {
    static std::string books, fonts, pics;
    books = join(g_root, "books");
    fonts = join(g_root, "fonts");
    pics = join(g_root, "pictures");
    return read_pico_airdrop_cfg_t{books.c_str(), fonts.c_str(), pics.c_str(), g_root.c_str()};
}

static esp_err_t start_ok(read_pico_airdrop_cfg_t *cfg) {
    g_ble_acquire_rc = ESP_OK;
    g_awdl_begin_rc = ESP_OK;
    g_awdl_end_rc = ESP_OK;
    g_awdl_end_timeouts_remaining = 0;
    g_awdl_begin_invalid_state_remaining = 0;
    g_ad_begin_rc = 0;
    return read_pico_airdrop_start(cfg);
}

static struct AdFile make_file(const char *name, uint32_t size) {
    struct AdFile f;
    memset(&f, 0, sizeof(f));
    f.name = name;
    f.path = name;
    f.size = size;
    return f;
}

int main(void) {
    make_dirs();
    read_pico_airdrop_cfg_t cfg = make_cfg();

    /* ---- R-10: whole file is published only under its final name ---- */
    check(start_ok(&cfg) == ESP_OK, "start ok");
    check(g_registered_ops.on_begin && g_registered_ops.on_data && g_registered_ops.on_end,
          "sink callbacks registered");

    struct AdFile f = make_file("book.epub", 5);
    check(g_registered_ops.on_begin(nullptr, &f) == AD_FILE_TAKE, "begin takes the file");
    check(!exists(join(join(g_root, "books"), "book.epub")), "final name absent while receiving");
    check(temp_files(join(g_root, "books")).size() == 1, "exactly one temp file open");
    check(g_registered_ops.on_data(nullptr, (const uint8_t *)"hello", 5), "data accepted");
    g_registered_ops.on_end(nullptr, true);
    check(read_file(join(join(g_root, "books"), "book.epub")) == "hello", "final file complete");
    check(temp_files(join(g_root, "books")).empty(), "no temp file left after success");

    /* ---- R-10: an incomplete file never appears under the final name ---- */
    struct AdFile cut = make_file("cut.txt", 10);
    check(g_registered_ops.on_begin(nullptr, &cut) == AD_FILE_TAKE, "begin cut file");
    check(g_registered_ops.on_data(nullptr, (const uint8_t *)"half", 4), "partial data");
    g_registered_ops.on_end(nullptr, false);
    check(!exists(join(join(g_root, "books"), "cut.txt")), "cut file did not appear under final name");
    check(temp_files(join(g_root, "books")).empty(), "cut temp file removed");

    /* ---- R-10: collision suffix behaviour is unchanged ---- */
    write_file(join(join(g_root, "books"), "dup.txt"), "orig");
    struct AdFile dup = make_file("dup.txt", 3);
    check(g_registered_ops.on_begin(nullptr, &dup) == AD_FILE_TAKE, "begin colliding file");
    check(g_registered_ops.on_data(nullptr, (const uint8_t *)"new", 3), "colliding data");
    g_registered_ops.on_end(nullptr, true);
    check(read_file(join(join(g_root, "books"), "dup.txt")) == "orig", "existing file untouched");
    check(read_file(join(join(g_root, "books"), "dup_1.txt")) == "new", "suffix _1 used");

    /* ---- R-10: start sweeps stale temp files, and only ours ---- */
    write_file(join(join(g_root, "books"), ".airdrop-00000000.part"), "stale");
    write_file(join(join(g_root, "fonts"), ".airdrop-00000001.part"), "stale");
    write_file(join(join(g_root, "books"), ".airdrop-keep"), "not a temp");
    write_file(join(join(g_root, "books"), "user.txt"), "user");
    check(start_ok(&cfg) == ESP_OK, "restart ok");
    check(temp_files(join(g_root, "books")).empty(), "books stale temp swept");
    check(temp_files(join(g_root, "fonts")).empty(), "fonts stale temp swept");
    check(exists(join(join(g_root, "books"), ".airdrop-keep")), "non-.part lookalike kept");
    check(read_file(join(join(g_root, "books"), "user.txt")) == "user", "user file kept");

    /* ---- R-10: stop removes an in-flight temp file ---- */
    struct AdFile open_file = make_file("inflight.txt", 100);
    check(g_registered_ops.on_begin(nullptr, &open_file) == AD_FILE_TAKE, "begin in-flight file");
    check(g_registered_ops.on_data(nullptr, (const uint8_t *)"abc", 3), "in-flight data");
    read_pico_airdrop_stop();
    check(temp_files(join(g_root, "books")).empty(), "stop removed the in-flight temp file");
    check(!exists(join(join(g_root, "books"), "inflight.txt")), "in-flight final name absent");

    /* ---- R-09: ad_poll runs before ad_end on stop (drain the acknowledged archive) ---- */
    check(start_ok(&cfg) == ESP_OK, "start for drain order");
    g_call_log.clear();
    read_pico_airdrop_stop();
    check(g_call_log.size() >= 2 && g_call_log[0] == "poll" && g_call_log[1] == "end",
          "stop drains with ad_poll() before ad_end()");

    /* ---- R-09: a pending archive counts as busy and refuses the sleep pause ---- */
    check(start_ok(&cfg) == ESP_OK, "start for busy test");
    g_arch_pending = true;
    check(read_pico_airdrop_busy(), "pending archive makes busy() true");
    check(!read_pico_airdrop_pause_for_sleep(), "sleep pause refused while an archive is pending");
    g_arch_pending = false;
    check(read_pico_airdrop_pause_for_sleep(), "sleep pause allowed once the archive is delivered");
    check(!read_pico_airdrop_active(), "paused service is not active");
    read_pico_airdrop_stop();

    /* ---- R-08: awdl_begin failure releases everything already taken ---- */
    g_call_log.clear();
    int ad_end_before = g_ad_end_calls, awdl_end_before = g_awdl_end_calls;
    int ble_release_before = g_ble_release_calls;
    g_awdl_begin_rc = ESP_FAIL;
    check(read_pico_airdrop_start(&cfg) != ESP_OK, "start fails when awdl_begin fails");
    read_pico_airdrop_status_t st;
    read_pico_airdrop_get_status(&st);
    check(st.state == READ_PICO_AIRDROP_ERROR, "awdl failure leaves status ERROR");
    check(st.last_error[0] != '\0', "awdl failure leaves a message");
    check(!read_pico_airdrop_active(), "awdl failure leaves the service inactive");
    check(g_awdl_end_calls == awdl_end_before + 1, "awdl_end called on awdl_begin failure");
    check(g_ble_release_calls == ble_release_before + 1, "BLE lease released on awdl_begin failure");
    check(g_heap_outstanding == 0, "stdio buffer freed on awdl_begin failure");

    /* ---- R-08: ad_begin failure releases ad_end, awdl_end, BLE and the stdio buffer ---- */
    ad_end_before = g_ad_end_calls;
    awdl_end_before = g_awdl_end_calls;
    ble_release_before = g_ble_release_calls;
    g_awdl_begin_rc = ESP_OK;
    g_ad_begin_rc = -1;
    check(read_pico_airdrop_start(&cfg) != ESP_OK, "start fails when ad_begin fails");
    read_pico_airdrop_get_status(&st);
    check(st.state == READ_PICO_AIRDROP_ERROR, "ad failure leaves status ERROR");
    check(g_ad_end_calls == ad_end_before + 1, "ad_end called on ad_begin failure");
    check(g_awdl_end_calls == awdl_end_before + 1, "awdl_end called on ad_begin failure");
    check(g_ble_release_calls == ble_release_before + 1, "BLE lease released on ad_begin failure");
    check(g_heap_outstanding == 0, "stdio buffer freed on ad_begin failure");

    /* ---- R-08: stop is idempotent and does not double-release the BLE lease ---- */
    g_ad_begin_rc = 0;
    check(start_ok(&cfg) == ESP_OK, "start for idempotent stop");
    ble_release_before = g_ble_release_calls;
    read_pico_airdrop_stop();
    read_pico_airdrop_stop();
    check(g_ble_release_calls == ble_release_before + 1, "double stop releases the BLE lease once");

    /* ---- R-06 + R-18: resume failure leaves ERROR and the saved directories intact ---- */
    check(start_ok(&cfg) == ESP_OK, "start before sleep");
    check(read_pico_airdrop_pause_for_sleep(), "pause before sleep");
    std::string books_before = s_books_dir;
    std::string fonts_before = s_fonts_dir;
    g_ad_begin_rc = -1;      /* the wake-time start fails (memory pressure) */
    read_pico_airdrop_resume_after_sleep();
    read_pico_airdrop_get_status(&st);
    check(!read_pico_airdrop_active(), "failed resume leaves the service inactive");
    check(st.state == READ_PICO_AIRDROP_ERROR, "failed resume leaves status ERROR for the UI");
    check(st.last_error[0] != '\0', "failed resume leaves last_error for the UI");
    check(s_books_dir == books_before && s_fonts_dir == fonts_before,
          "resume kept the session directories (no self-copy)");
    check(g_heap_outstanding == 0, "failed resume released the stdio buffer");

    /* ---- R-06: a successful resume restores the session counters ---- */
    g_ad_begin_rc = 0;
    check(start_ok(&cfg) == ESP_OK, "start before successful resume");
    s_status.completed_files = 3;
    s_status.books_received = 2;
    check(read_pico_airdrop_pause_for_sleep(), "pause before successful resume");
    read_pico_airdrop_resume_after_sleep();
    read_pico_airdrop_get_status(&st);
    check(read_pico_airdrop_active(), "successful resume is active again");
    check(st.completed_files == 3 && st.books_received == 2, "resume restored session counters");
    check(st.state == READ_PICO_AIRDROP_DONE, "resume reports DONE after earlier files");
    read_pico_airdrop_stop();

    /* ---- awdl retry contract: one timed-out end is retried, then a clean end clears the flag ---- */
    check(start_ok(&cfg) == ESP_OK, "start for awdl retry");
    g_awdl_end_timeouts_remaining = 1;   /* first end times out, the retry succeeds */
    awdl_end_before = g_awdl_end_calls;
    read_pico_airdrop_stop();
    check(g_awdl_end_calls == awdl_end_before + 2, "stop retried awdl_end after ESP_ERR_TIMEOUT");
    check(!s_awdl_begun, "a clean teardown cleared s_awdl_begun");
    check(g_heap_outstanding == 0, "stop released the stdio buffer despite the first timeout");

    /* ---- awdl retry contract: a persistent timeout keeps s_awdl_begun so a later stop retries ---- */
    check(start_ok(&cfg) == ESP_OK, "start for persistent awdl timeout");
    g_awdl_end_rc = ESP_ERR_TIMEOUT;
    awdl_end_before = g_awdl_end_calls;
    read_pico_airdrop_stop();
    check(g_awdl_end_calls == awdl_end_before + 3, "one stop tries awdl_end the bounded 3 times");
    check(s_awdl_begun, "s_awdl_begun kept on timeout, so a later stop can retry");
    awdl_end_before = g_awdl_end_calls;
    read_pico_airdrop_stop();
    check(g_awdl_end_calls == awdl_end_before + 3, "a later stop retries the timed-out teardown");
    check(s_awdl_begun, "still flagged while the teardown keeps timing out");
    g_awdl_end_rc = ESP_OK;
    read_pico_airdrop_stop();
    check(!s_awdl_begun, "teardown finally clean and the flag cleared");

    /* ---- awdl contract: begin refused with INVALID_STATE -> finish teardown, retry begin once ---- */
    check(start_ok(&cfg) == ESP_OK, "start before invalid-state begin test");
    read_pico_airdrop_stop();
    g_awdl_begin_invalid_state_remaining = 1;   /* first begin: previous teardown incomplete */
    g_awdl_end_rc = ESP_ERR_INVALID_STATE;      /* recovery finds nothing to tear down */
    int awdl_begin_before = g_awdl_begin_calls;
    awdl_end_before = g_awdl_end_calls;
    check(read_pico_airdrop_start(&cfg) == ESP_OK, "start retried awdl_begin after INVALID_STATE");
    check(g_awdl_begin_calls == awdl_begin_before + 2, "begin was retried exactly once");
    check(g_awdl_end_calls == awdl_end_before + 1,
          "recovery called awdl_end once (INVALID_STATE counts as clean)");
    check(read_pico_airdrop_active(), "service is up after the retried begin");
    read_pico_airdrop_stop();

    /* ---- awdl contract: pause never begins over a half-torn link ---- */
    check(start_ok(&cfg) == ESP_OK, "start for pause timeout test");
    int ad_begin_before = g_ad_begin_calls;
    g_awdl_end_rc = ESP_ERR_TIMEOUT;
    check(!read_pico_airdrop_pause_for_sleep(), "pause refused while awdl teardown times out");
    check(g_ad_begin_calls == ad_begin_before, "no ad_begin over a half-torn link");
    check(read_pico_airdrop_active(), "service stays active after the refused pause");
    check(s_awdl_begun, "s_awdl_begun kept for the next pause retry");
    g_awdl_end_rc = ESP_OK;
    check(read_pico_airdrop_pause_for_sleep(), "pause succeeds once awdl_end is clean");
    check(!read_pico_airdrop_active(), "paused after the clean teardown");
    read_pico_airdrop_stop();

    if (g_failures) {
        printf("airdrop_lifecycle_host_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    printf("airdrop_lifecycle_host_test: PASS (R-06/R-08/R-09/R-10/R-18 + awdl retry contract)\n");
    return 0;
}
