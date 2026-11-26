#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""M4 Host UX Verification: WR-UX-02, WR-UX-04b, WR-UX-05b, WR-UX-06c across metalio47 and pico."""
from pathlib import Path
import re
import subprocess
import tempfile
from home_cover_cache_host_test import function

ROOT = Path(__file__).resolve().parents[1]

COMMON_HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
typedef int esp_err_t;
enum {
    ESP_OK = 0,
    ESP_FAIL = -1,
    ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_NOT_FOUND = 0x105,
    ESP_ERR_TIMEOUT = 0x107,
    WEREAD_ERR_PURCHASE = 0x7901,
    WEREAD_ERR_SOLD_OUT = 0x7902,
    WEREAD_ERR_VERSION = 0x7903
};
enum {
    SHELF = 0, TOC, READING, MANAGE, SEARCH, EDIT, IMPORT, COVER_LOOKUP, BULK,
    READER_PANEL_NONE = 0, READER_PANEL_TOOLS, READER_PANEL_FONT_SETTINGS,
    READER_PANEL_WEIGHT_SETTINGS, READER_PANEL_LAYOUT_SETTINGS, READER_PANEL_RULE_SETTINGS,
    READER_PANEL_BACKGROUND, READER_PANEL_BACKGROUND_PREVIEW, READER_PANEL_FONT_PICKER,
    READER_PANEL_STATS, READER_PANEL_CHAPTER_JUMP, READER_PANEL_BOOKMARKS,
    READER_PANEL_STATS_RECENT, READER_PANEL_REFRESH_SETTINGS, READER_PANEL_TURN_SETTINGS,
    READER_PANEL_KEY_SETTINGS, READER_PANEL_KEY_PICKER, READER_PANEL_TOUCH_SETTINGS,
    READER_PANEL_TOUCH_MORE, READER_PANEL_FONT_RECOVERY,
    WR_JOB_NOTE = 10, WR_JOB_CHAPTER = 11, WR_JOB_PREFETCH = 12,
    MODE_GL16 = 1, MODE_DU = 2,
    EPD_DRAW_ALIGN_LEFT = 0, EPD_DRAW_ALIGN_CENTER = 1, EPD_DRAW_ALIGN_RIGHT = 2,
    UI_GRAY_WHITE = 255, UI_GRAY_BLACK = 0, UI_NAV_BACKGROUND_GRAY = 0xe0,
    UI_LOCK_WIDTH = 684, UI_LOCK_HEIGHT = 1216, UI_MARGIN = 36, UI_PX_BODY = 24,
    BOOK_TOOL_COUNT = 5, READER_IMAGE_ZOOM_MIN = 1000,
    UI_READER_TOOLS_TOP = 1096
};
typedef enum { APP_REDRAW_NONE, APP_REDRAW_PAGE, APP_REDRAW_AREA, APP_REDRAW_FULL } app_redraw_t;
typedef enum { WR_PROGRESS_LOCAL, WR_PROGRESS_PENDING, WR_PROGRESS_SYNCED, WR_PROGRESS_CONFLICT } weread_progress_state_t;
typedef enum { WEREAD_STAGE_NONE = 0, WEREAD_STAGE_PROGRESS, WEREAD_STAGE_TOC, WEREAD_STAGE_CHAPTER, WEREAD_STAGE_PARSE, WEREAD_STAGE_RETRY } weread_stage_t;
typedef enum { UI_GESTURE_PRESS, UI_GESTURE_MOVE, UI_GESTURE_CANCEL, UI_GESTURE_TAP, UI_GESTURE_SWIPE_L, UI_GESTURE_SWIPE_R, UI_GESTURE_SWIPE_U, UI_GESTURE_SWIPE_D, UI_GESTURE_LONG_PRESS } ui_gesture_type_t;
typedef enum { UI_KEY_1 = 0, UI_KEY_2 = 1, UI_KEY_3 = 2 } ui_key_t;
typedef enum { APP_READER_KEY_PREV, APP_READER_KEY_NEXT, APP_READER_KEY_HOME, APP_READER_KEY_FULLSCREEN, APP_READER_KEY_REFRESH, APP_READER_KEY_TOOLS } app_reader_key_action_t;
typedef enum { UI_KEYBOARD_NONE, UI_KEYBOARD_CHANGED, UI_KEYBOARD_DONE } ui_keyboard_result_t;
typedef struct { int x, y, width, height; } EpdRect;
typedef struct { bool touched; int x, y, count; } touch_t;
typedef struct { int64_t now_ms; bool consumed, request_app, request_menu, request_return, user_activity; touch_t *touch; uint8_t *fb; int leaf; } app_ctx_t;
typedef struct { ui_gesture_type_t type; uint16_t x0, y0, x, y; } ui_gesture_event_t;
typedef struct { int job, error; char id[64]; uint32_t chapter_uid; } weread_result_t;
'''


def compile_run(code: str, name: str, extra_flags=None):
    flags = list(extra_flags or [])
    with tempfile.TemporaryDirectory(prefix=f"m4-ux-{name}-") as td:
        p = Path(td)
        (p / "test.c").write_text(code, encoding="utf-8")
        subprocess.run(
            ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-unused-parameter", "-Wno-unknown-attributes", "-fsanitize=address,undefined",
             *flags, str(p / "test.c"), "-o", str(p / "test")],
            check=True,
        )
        subprocess.run([str(p / "test")], check=True)


def run_reader_panel_and_cloud_undo_tests(board: str, book_path: Path, is_metalio: int):
    book_src = book_path.read_text(encoding="utf-8")
    bridge_src = re.sub(r'#include\s+"weread/weread_store\.h"', "", (ROOT / "main/apps/book_weread_bridge.inc").read_text(encoding="utf-8"))
    chapter_src = (ROOT / "main/apps/book_weread_chapter.inc").read_text(encoding="utf-8")
    undo_decl = book_src[book_src.index("static struct {\n    bool valid;\n    uint8_t mode;"):book_src.index("// 微调长按只更新预览")]
    load_fn = function("load_chapter_at", book_path)
    load_wr_start = load_fn.index("if (book_path_is_weread(s_path)) {")
    load_wr_end = load_fn.index("bool clear_toc =")
    load_weread_preamble = load_fn[load_wr_start:load_wr_end]
    idx_ch = load_fn.index("s_chapter = chapter;")
    idx_clr = load_fn.index("s_chapter_jump_undo.valid = false;") + len("s_chapter_jump_undo.valid = false;")
    load_undo_block = load_fn[idx_ch:idx_clr]
    free_fn = function("free_book", book_path)
    tick_fn = function("on_tick", book_path)
    tick_wr_start = tick_fn.index("    if (s_wr_undo_top_erase) {")
    tick_wr_end = tick_fn.index("    if (s_save_failed &&")
    tick_expiry_fn = (
        "static app_redraw_t test_tick_expiry(app_ctx_t *ctx) {\n"
        + tick_fn[tick_wr_start:tick_wr_end]
        + "    return APP_REDRAW_NONE;\n}\n"
    )

    code = COMMON_HEADER + undo_decl + r'''
enum { E0470_TURN_LTR = 0, E0470_TURN_RTL = 1 };
#define ESP_LOGI(...) ((void)0)
#define TT_SPAN(name, expr) do { expr; } while (0)
#define TT_STAMP(name) ((void)0)
typedef struct { int x; } html_note_t;
typedef struct { int x; } blk_t;
typedef struct { int x; } html_run_t;
typedef struct { int x; } reader_dims_ctx_t;
typedef struct { bool dirty; } pending_progress_t;
static int s_view = READING, s_reader_panel = READER_PANEL_NONE, s_mode, s_reader_slider;
static bool s_clear_confirm, s_toolbar, s_reader_footer_pending, s_reader_text_frame = true, s_reader_fullscreen;
static bool s_water_turn_pending, s_reader_turn_pending, s_reader_image_refresh_pending, s_reader_cleanup;
static bool s_reader_dc_recover __attribute__((unused));
static uint8_t s_dc_recover_pages __attribute__((unused));
static unsigned s_back_turns __attribute__((unused));
static bool s_save_due __attribute__((unused)), s_chapter_display_pending __attribute__((unused));
static uint8_t s_save_due_turns __attribute__((unused));
static bool s_reader_image_frame __attribute__((unused)), s_reader_image_exit_pending __attribute__((unused));
static bool s_rc_auto_turning, s_part_more, s_save_failed, s_toc_jump_open;
static bool s_reader_rule_pending, s_txt_opening, s_ticket_cover_started __attribute__((unused)), s_ticket_cover_done __attribute__((unused));
static bool s_reader_favorite, s_chapter_warm_done __attribute__((unused));
static int s_turn_direction __attribute__((unused)), s_water_turn_dir, s_turns, s_unsaved, s_footnote_note = -1, s_chapter_warm_retry __attribute__((unused));
static size_t s_part_base, s_part_begin, s_part_end, s_chapter_lead_skip, s_block_count, s_run_count, s_note_count, s_image_count, s_chapter_warm_target __attribute__((unused));
static unsigned s_chapter_lead_height, s_chapter_lead_width, s_cover_generation __attribute__((unused));
static struct { bool pending; } s_reader_tap;
static uint64_t s_reader_footer_due_ms;
static int64_t s_reader_notice_until, s_stats_activity_ms, s_last_turn_ms;
static EpdRect s_area;
static char s_path[64] = "weread://book_A", s_message[128], s_reader_notice[64], s_title[64], s_book_title[64], s_chapter_heading_title[64], s_chapter_heading_label[32], s_chapter_jump_digits[24];
static char *s_text_buf = NULL;
static const char *s_text_ro = "body text";
#define s_text s_text_buf
static blk_t *s_blocks;
static html_run_t *s_runs;
static html_note_t *s_notes;
static char **s_images;
static reader_dims_ctx_t s_reader_dims_ctx;
static size_t s_text_len = 9, s_chapter = 2, s_page = 1, s_jump_page, s_jump_offset = SIZE_MAX, s_selected_toc = SIZE_MAX, s_selected_toc_offset;
static weread_progress_state_t mock_state = WR_PROGRESS_SYNCED;
static unsigned mock_cached = 1, mock_total = 4, mock_offline = 0;
static size_t mock_page_count = 4;
static bool mock_paused = false, mock_blocked = false, mock_stalled = false, mock_caching = false;
static bool mock_save_failed = false, mock_pending_save = false, mock_rescued = false, mock_has_turned = false, mock_flip_blocked = false;
static bool mock_busy = false, mock_done = false, mock_cloud_ready = false, mock_load_fail = false, mock_cached_ch = true;
static int mock_ch_cached_override[20]; /* 0 = use mock_cached_ch, 1 = false, 2 = true */
static size_t mock_cloud_ch = 5; static uint32_t mock_cloud_off = 120;
static weread_result_t mock_result;
static int cloud_commit_calls, cloud_commit_last, rebase_calls, cancel_calls, load_calls, move_calls_rt, save_calls, end_calls;
static char drawn_texts[32][128]; static int drawn_count;
static void record_text(const char *s) { if (s && drawn_count < 32) snprintf(drawn_texts[drawn_count++], 128, "%s", s); }
static bool has_drawn(const char *needle) { for (int i = 0; i < drawn_count; ++i) if (strstr(drawn_texts[i], needle)) return true; return false; }

static bool ui_rect_hit(EpdRect r, int x, int y) { return r.width > 0 && r.height > 0 && x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height; }
static bool book_path_is_weread(const char *p) { return p && strncmp(p, "weread://", 9) == 0; }
static size_t book_layout_page_count(void) { return mock_page_count; }
static size_t book_layout_page_image_count(size_t p) { (void)p; return 0; }
static size_t book_layout_page_for_offset(size_t off) { return off / 100; }
static size_t book_layout_page_start_offset(size_t p) { return p * 100; }
static int reader_chapter_jump_hit(uint16_t x, uint16_t y) { (void)x; (void)y; return -1; }
static app_redraw_t reader_chapter_jump_action(app_ctx_t *c, uint16_t x, uint16_t y) { (void)c; (void)x; (void)y; return APP_REDRAW_NONE; }
static size_t reader_page_offset(size_t p) { return p * 100; }
static unsigned percent(size_t p) { return (unsigned)(p * 25); }
static void wr_copy(char *o, size_t c, const char *i) { snprintf(o, c, "%s", i ? i : ""); }
static void copy_text(char *o, size_t c, const char *i) { snprintf(o, c, "%s", i ? i : ""); }
static void *wr_alloc(size_t n) { return calloc(1, n); }
static void lock_draw(void) {}
static void unlock_draw(void) {}
static void invalidate_prep(void) {}
static void reader_page_cache_clear(void) {}
static void release_page_images(void) {}
static void prepare_inline_image(void) {}
static void save_progress(void) { ++save_calls; }
static void render(app_ctx_t *c, uint8_t *f) { (void)c; (void)f; }
static int64_t esp_timer_get_time(void) { return 1000000; }
static void weread_reader_end(uint64_t now) { (void)now; ++end_calls; }
static void pico_boot_book_end(bool ok) { (void)ok; }
static void reader_font_reload_cancel(void) {}
static void reader_font_recovery_reset(void) {}
static void book_cover_jobs_cancel(unsigned g) { (void)g; }
static void flush_ticket_stats(void) {}
static void e0470_page_turn_release(void) {}
static pending_progress_t *pending_find(const char *p) { (void)p; return NULL; }
static void pending_discard(const char *p) { (void)p; }
static void book_layout_set_image_dims(void *fn, void *ctx) { (void)fn; (void)ctx; }
static void book_layout_free(void) {}
static void book_layout_set_chapter_lead(size_t a, unsigned b) { (void)a; (void)b; }
static void book_layout_set_chapter_lead_width(unsigned w) { (void)w; }
static void book_layout_set_inline_images(void *a, size_t b) { (void)a; (void)b; }
static void html_notes_free(html_note_t *n, size_t c) { (void)c; free(n); }
static void book_layout_set_runs(html_run_t *r, size_t c) { (void)r; (void)c; }
static void book_close(void) {}
static void app_font_activate_system(void) {}
static size_t book_navigation_count(void) { return 8; }
static esp_err_t book_navigation_title(size_t i, char *o, size_t c) { (void)i; snprintf(o, c, "Ch"); return ESP_OK; }
static size_t current_toc_position(void) { return 0; }
static void book_toc_set_cached_probe(bool (*fn)(size_t, void *), void *u) { (void)fn; (void)u; }
static void book_toc_prepare_page(size_t n, size_t cur, int pg) { (void)n; (void)cur; (void)pg; }
static uint32_t book_weread_uid(size_t ch) { return (uint32_t)(ch + 10); }
static uint32_t book_weread_chapter_index(size_t ch) { return (uint32_t)(ch + 1); }
static void weread_reader_navigated_version(const char *p, size_t c, uint32_t o, unsigned pct, const char *s, uint32_t v) {
    (void)p; (void)c; (void)o; (void)pct; (void)s; (void)v; ++move_calls_rt;
}
static void weread_reader_tick(const char *p, size_t c, uint32_t o, unsigned pct, const char *s, uint64_t n, bool a) {
    (void)p; (void)c; (void)o; (void)pct; (void)s; (void)n; (void)a;
    if (mock_flip_blocked) { mock_blocked = true; mock_flip_blocked = false; }
}
static weread_progress_state_t weread_reader_state(void) { return mock_state; }
static bool weread_reader_save_failed(void) { return mock_save_failed; }
static bool weread_reader_pending_save(void) { return mock_pending_save; }
static bool weread_reader_rescued(void) { return mock_rescued; }
static unsigned weread_reader_offline(void) { return mock_offline; }
static void weread_reader_cached_progress(unsigned *c, unsigned *t) { *c = mock_cached; *t = mock_total; }
static bool weread_reader_storage_blocked(void) { return mock_blocked; }
static bool weread_reader_caching(void) { return mock_caching; }
static bool weread_reader_cache_fill_enabled(void) { return !mock_paused; }
static bool weread_reader_cache_forward_stalled(void) { return mock_stalled; }
static void weread_reader_cache_forward_set(bool on) { mock_paused = !on; }
static bool weread_reader_chapter_cached(const char *u, size_t ch) {
    (void)u;
    if (ch < 20 && mock_ch_cached_override[ch]) return mock_ch_cached_override[ch] == 2;
    return mock_cached_ch;
}
static bool weread_reader_cached(const char *u, size_t ch) { return weread_reader_chapter_cached(u, ch); }
static esp_err_t weread_reader_save(uint64_t n) { (void)n; return ESP_OK; }
static bool weread_runtime_busy(void) { return mock_busy; }
static bool weread_runtime_start(int j, const char *i, const char *v) { (void)j; (void)i; (void)v; mock_busy = true; return true; }
static bool weread_runtime_poll(weread_result_t *r) { if (mock_busy || !mock_done) return false; mock_done = false; *r = mock_result; return true; }
static void weread_runtime_result_free(weread_result_t *r) { memset(r, 0, sizeof(*r)); }
static void weread_runtime_cancel(void) { ++cancel_calls; mock_busy = false; }
static bool weread_runtime_prefetch_active(const char *i, uint32_t u) { (void)i; (void)u; return false; }
static void weread_runtime_prefetch_adopt(uint32_t u) { (void)u; }
static bool weread_reader_poll_cloud_jump(const char *u, size_t *c, uint32_t *o) { (void)u; if (!mock_cloud_ready) return false; *c = mock_cloud_ch; *o = mock_cloud_off; return true; }
static void weread_reader_commit_cloud_jump(const char *u, bool ok) { (void)u; mock_cloud_ready = false; ++cloud_commit_calls; cloud_commit_last = ok ? 1 : 0; }
static bool weread_reader_stage(weread_stage_t *s, uint64_t *b) { (void)s; (void)b; return false; }
static bool weread_reader_stage_text(char *o, size_t c) { (void)o; (void)c; return false; }
static bool weread_runtime_reader_has_turned(const char *u) { (void)u; return mock_has_turned; }
static esp_err_t weread_runtime_cloud_jump_rebase(const char *u) { (void)u; ++rebase_calls; mock_cloud_ready = false; return ESP_OK; }
static bool rc_draw_current(uint8_t *fb) { (void)fb; return false; }
static void ui_text_set_system_scale(bool on) { (void)on; }
static void ui_fill_rect_fast(EpdRect r, uint8_t g, uint8_t *fb) { (void)r; (void)g; (void)fb; }
static void ui_fill_round_rect(uint8_t *fb, EpdRect r, int rad, uint8_t g) { (void)fb; (void)r; (void)rad; (void)g; }
static void ui_hairline(uint8_t *fb, int y, int x, int w, uint8_t g) { (void)fb; (void)y; (void)x; (void)w; (void)g; }
static void ui_text_box(uint8_t *fb, EpdRect r, int px, const char *s, int al, bool inv) { (void)fb; (void)r; (void)px; (void)al; (void)inv; record_text(s); }
static bool bookmark_is_current(void) { return false; }
static void draw_reader_tool_icon(uint8_t *fb, int cx, int y, int i, bool m) { (void)fb; (void)cx; (void)y; (void)i; (void)m; }
static void draw_font_settings(uint8_t *f) { (void)f; }
static void draw_weight_settings(uint8_t *f) { (void)f; }
static void draw_layout_settings(uint8_t *f) { (void)f; }
static void draw_rule_settings(uint8_t *f) { (void)f; }
static void draw_background_directory(uint8_t *f) { (void)f; }
static void draw_background_picker(uint8_t *f) { (void)f; }
static void draw_font_picker(uint8_t *f) { (void)f; }
static void draw_reader_stats(uint8_t *f) { (void)f; }
static void draw_reader_chapter_jump(uint8_t *f) { (void)f; }
static void draw_reader_bookmarks(uint8_t *f) { (void)f; }
static void draw_reader_stats_recent(uint8_t *f) { (void)f; }
static void draw_reading_settings(uint8_t *f) { (void)f; }
static void draw_turn_settings(uint8_t *f) { (void)f; }
static void draw_key_settings(uint8_t *f) { (void)f; }
static void draw_key_picker(uint8_t *f) { (void)f; }
static EpdRect reader_fast_panel_rect(void) { return (EpdRect){0}; }
static void display_reader_panel_gray4_prepare(uint8_t *f, EpdRect r) { (void)f; (void)r; }
static void rc_pause_auto(void) {}
static void weread_reader_input(bool text_page) { (void)text_page; }
static void reader_zoom_reset(void) {}
static void set_reader_view(int v) { s_view = v; }
static int book_toc_open(size_t p) { (void)p; return 0; }
static void skip_hidden_image_pages(app_ctx_t *c, int d) { (void)c; (void)d; }
static size_t book_chapter_count(void) { return 20; }
static void ticket_count_turn(void) {}
static uint8_t app_settings_reader_full_pages(void) { return 0; }
static int app_settings_reader_turn_effect(void) { return 0; }
static bool __attribute__((unused)) app_settings_low_ghost(void) { return false; }
static const char *app_settings_book_background_path(void) { return ""; }
static app_redraw_t paint_reading(app_ctx_t *c, int m) { (void)c; s_mode = m; s_area = (EpdRect){0, 176, 684, 872}; return APP_REDRAW_AREA; }

static bool load_chapter_at(app_ctx_t *ctx, size_t ch, size_t off, bool last, const char *a, size_t src);
static bool next_chapter_can_adopt(size_t ch) { (void)ch; return false; }
static bool commit_next_chapter(app_ctx_t *c, size_t ch) { (void)c; (void)ch; return false; }
'''
    code += bridge_src + "\n"
    for fn_name in ("book_tool_count", "tool_rect", "reader_tools_visible", "reader_chapter_undo_rect", "reader_chapter_undo_visible", "draw_reader_panel"):
        code += function(fn_name, book_path) + "\n"
    code += chapter_src + "\n"
    code += r'''
static bool load_chapter_at(app_ctx_t *ctx, size_t chapter, size_t offset, bool last_page, const char *anchor, size_t source_offset) {
    (void)last_page;
''' + load_weread_preamble + r'''
    if (mock_load_fail) return false;
    ++load_calls;
    s_page = offset / 100;
''' + load_undo_block + r'''
    return true;
}
'''
    code += free_fn + "\n"
    code += tick_expiry_fn + "\n"
    for fn_name in ("load_chapter", "reader_turn_chrome", "turn_page", "reader_chapter_jump_commit", "reader_chapter_jump_gesture", "wr_cloud_undo_commit", "wr_cloud_undo_gesture"):
        code += function(fn_name, book_path) + "\n"

    code += r'''
int main(void) {
    app_ctx_t ctx = {.now_ms = 1000, .fb = (uint8_t *)1};
    uint8_t fb = 0;
    s_text = strdup(s_text_ro); assert(s_text);

    // 1. WR-UX-02 footer status + offline caption + no cache_delta wakeup + tool6 label
    mock_state = WR_PROGRESS_SYNCED; assert(!strcmp(wr_reader_status(), ""));
    mock_state = WR_PROGRESS_LOCAL; mock_has_turned = false; assert(!strcmp(wr_reader_status(), ""));
    mock_has_turned = true; assert(!strcmp(wr_reader_status(), "本地保存 · "));
    mock_state = WR_PROGRESS_PENDING; assert(!strcmp(wr_reader_status(), "待同步 · "));
    mock_state = WR_PROGRESS_CONFLICT; assert(!strcmp(wr_reader_status(), "进度冲突 · "));
    mock_state = WR_PROGRESS_SYNCED; mock_save_failed = true; assert(!strcmp(wr_reader_status(), "未保存 · ")); mock_save_failed = false;
    mock_pending_save = true; assert(!strcmp(wr_reader_status(), "待保存 · ")); mock_pending_save = false;
    mock_rescued = true; assert(!strcmp(wr_reader_status(), "暂存机内 · ")); mock_rescued = false;

    char cap[64];
    mock_blocked = false; mock_cached = 1; mock_total = 4; mock_paused = false;
    wr_reader_offline_caption(cap, sizeof(cap)); assert(!strcmp(cap, ""));
    mock_cached = 4; wr_reader_offline_caption(cap, sizeof(cap)); assert(!strcmp(cap, ""));
    mock_blocked = true; wr_reader_offline_caption(cap, sizeof(cap)); assert(!strcmp(cap, "空间不足 · "));
    mock_blocked = false;

    // Background cache progress / offline / caching flips never wake footer; state/blocked flips do
    mock_state = WR_PROGRESS_SYNCED; s_reader_footer_pending = false;
    wr_reader_sample(2000, true);
    s_reader_footer_pending = false;
    mock_cached = 2; mock_offline = 3; mock_caching = true;
    wr_reader_sample(3000, true);
    assert(!s_reader_footer_pending);
    mock_flip_blocked = true;
    wr_reader_sample(4000, true);
    assert(s_reader_footer_pending);
    mock_blocked = false; s_reader_footer_pending = false;

    // Toolbar button 6 dynamic labels in draw_reader_panel()
    s_reader_fullscreen = false; s_reader_panel = READER_PANEL_TOOLS;
    mock_cached = 3; mock_total = 10; mock_paused = false; mock_stalled = false;
    drawn_count = 0; draw_reader_panel(&fb); assert(has_drawn("缓存 3/10"));
    mock_cached = 10; drawn_count = 0; draw_reader_panel(&fb); assert(has_drawn("已全存"));
    mock_cached = 3; mock_paused = true; drawn_count = 0; draw_reader_panel(&fb); assert(has_drawn("已暂停"));
    mock_paused = false; mock_stalled = true; drawn_count = 0; draw_reader_panel(&fb); assert(has_drawn("重试缓存"));
    mock_stalled = false; mock_total = 0; drawn_count = 0; draw_reader_panel(&fb); assert(has_drawn("缓存中"));

    // 2. WR-UX-04b: pages==0 auto-jump -> [返回原处] -> tap restores cross-chapter position
    s_reader_panel = READER_PANEL_NONE; s_chapter = 2; s_page = 1; mock_has_turned = false;
    mock_cached_ch = true; mock_cloud_ready = true; mock_cloud_ch = 7; mock_cloud_off = 200;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_PAGE);
    assert(s_chapter == 7 && s_page == 2 && s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_BACK);
    assert(s_chapter_jump_undo.target_chapter == 2 && s_chapter_jump_undo.target_offset == 100);
    assert(s_chapter_jump_undo.until_ms == ctx.now_ms + 6000);
    drawn_count = 0; draw_reader_panel(&fb); assert(has_drawn("已接力至手机最新进度") && has_drawn("返回原处"));
    EpdRect btn = reader_chapter_undo_rect();
    ui_gesture_event_t tap_btn = {.type = UI_GESTURE_TAP, .x0 = btn.x + 10, .y0 = btn.y + 10, .x = btn.x + 10, .y = btn.y + 10};
    bool handled = false;
    int moves_before = move_calls_rt;
    assert(wr_cloud_undo_gesture(&ctx, &tap_btn, &handled) == APP_REDRAW_PAGE && handled);
    assert(s_chapter == 2 && s_page == 1 && !s_chapter_jump_undo.valid && move_calls_rt == moves_before + 1);

    // 3. WR-UX-04b: pages>0 rebase-only -> [点击跳转] -> cold chapter completion commits true without wr_reader_move
    mock_has_turned = true; mock_cloud_ready = true; mock_cloud_ch = 8; mock_cloud_off = 300;
    int loads_before = load_calls;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_AREA);
    assert(load_calls == loads_before && rebase_calls == 1 && s_chapter == 2);
    assert(s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_JUMP);
    drawn_count = 0; draw_reader_panel(&fb); assert(has_drawn("发现更新的手机进度（第 9 章）") && has_drawn("点击跳转"));
    mock_cached_ch = false;
    handled = false;
    assert(wr_cloud_undo_gesture(&ctx, &tap_btn, &handled) == APP_REDRAW_PAGE && handled);
    assert(s_wr_jump.pending && s_wr_jump.cloud_commit);
    mock_busy = false; mock_done = true; mock_cached_ch = true;
    mock_result = (weread_result_t){.job = WR_JOB_CHAPTER, .error = ESP_OK, .chapter_uid = book_weread_uid(8)};
    strcpy(mock_result.id, "book_A");
    int cc_before = cloud_commit_calls; moves_before = move_calls_rt;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_PAGE);
    assert(s_chapter == 8 && s_page == 3 && cloud_commit_calls == cc_before + 1 && cloud_commit_last == 1 && move_calls_rt == moves_before);

    // 4. WR-UX-04b: tapping outside button on cloud card dismisses & commits false; wr_reader_move dismisses & sets top_erase
    mock_cloud_ready = true; mock_cloud_ch = 9; mock_cloud_off = 0;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_AREA);
    ui_gesture_event_t tap_card = {.type = UI_GESTURE_TAP, .x0 = 120, .y0 = 180, .x = 120, .y = 180};
    handled = false; cc_before = cloud_commit_calls;
    assert(wr_cloud_undo_gesture(&ctx, &tap_card, &handled) == APP_REDRAW_AREA && handled);
    assert(!s_chapter_jump_undo.valid && cloud_commit_calls == cc_before + 1 && cloud_commit_last == 0);

    mock_cloud_ready = true; mock_cloud_ch = 10; mock_cloud_off = 0;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_AREA);
    s_wr_undo_top_erase = false; cc_before = cloud_commit_calls;
    wr_reader_move();
    assert(!s_chapter_jump_undo.valid && s_wr_undo_top_erase && cloud_commit_calls == cc_before + 1 && cloud_commit_last == 0);

    // 5. Local WR_UNDO_LOCAL remains unaffected
    s_page = 0;
    assert(reader_chapter_jump_commit(&ctx, 2, false) == APP_REDRAW_PAGE);
    assert(s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_LOCAL && s_page == 2);
    handled = false;
    assert(wr_cloud_undo_gesture(&ctx, &tap_btn, &handled) == APP_REDRAW_NONE && !handled);
    assert(reader_chapter_jump_gesture(&ctx, &tap_btn, &handled) == APP_REDRAW_PAGE && handled && s_page == 0);

    // 6. Challenge 1: cached cross-chapter turn_page() while WR_UNDO_CLOUD_JUMP / WR_UNDO_CLOUD_BACK is active
    //    must set s_wr_undo_top_erase and commit false (for WR_UNDO_CLOUD_JUMP).
    s_reader_fullscreen = false; s_reader_panel = READER_PANEL_NONE;
    s_chapter = 2; s_page = 3; mock_cached_ch = true; mock_has_turned = true;
    mock_cloud_ready = true; mock_cloud_ch = 11; mock_cloud_off = 0;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_AREA);
    assert(s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_JUMP);
    s_wr_undo_top_erase = false; cc_before = cloud_commit_calls;
    assert(turn_page(&ctx, 1) == APP_REDRAW_AREA);
    assert(s_chapter == 3 && s_page == 0 && !s_chapter_jump_undo.valid);
    assert(s_wr_undo_top_erase && cloud_commit_calls == cc_before + 1 && cloud_commit_last == 0);

    // Also verify WR_UNDO_CLOUD_BACK cached cross-chapter turn sets s_wr_undo_top_erase without spurious cloud commit.
    s_chapter = 3; s_page = 3; mock_has_turned = false;
    mock_cloud_ready = true; mock_cloud_ch = 5; mock_cloud_off = 100;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_PAGE);
    assert(s_chapter == 5 && s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_BACK);
    s_page = 3; s_wr_undo_top_erase = false; cc_before = cloud_commit_calls;
    assert(turn_page(&ctx, 1) == APP_REDRAW_AREA);
    assert(s_chapter == 6 && !s_chapter_jump_undo.valid && s_wr_undo_top_erase && cloud_commit_calls == cc_before);

    // 7. Challenge 2: wr_chapter_tick() defers cloud jumps while s_reader_panel != READER_PANEL_NONE or s_clear_confirm,
    //    and surfaces immediately once the panel/modal closes.
    s_chapter = 6; s_page = 1; mock_has_turned = true;
    mock_cloud_ready = true; mock_cloud_ch = 12; mock_cloud_off = 200;
    s_reader_panel = READER_PANEL_FONT_SETTINGS;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_NONE && mock_cloud_ready && !s_chapter_jump_undo.valid);
    s_reader_panel = READER_PANEL_NONE; s_clear_confirm = true;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_NONE && mock_cloud_ready && !s_chapter_jump_undo.valid);
    s_clear_confirm = false;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_AREA && !mock_cloud_ready && s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_JUMP);

    // Also verify pages==0 does not forcibly close an open panel.
    s_chapter_jump_undo.valid = false; mock_has_turned = false;
    mock_cloud_ready = true; mock_cloud_ch = 13; mock_cloud_off = 0;
    s_reader_panel = READER_PANEL_FONT_SETTINGS;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_NONE && mock_cloud_ready && s_reader_panel == READER_PANEL_FONT_SETTINGS && s_chapter == 6);
    s_reader_panel = READER_PANEL_NONE;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_PAGE && !mock_cloud_ready && s_chapter == 13 && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_BACK);

    // 8. Challenge 3: reader_chapter_jump_commit() while WR_UNDO_CLOUD_JUMP is active commits false (both changed and !changed).
    mock_has_turned = true; mock_cloud_ready = true; mock_cloud_ch = 14; mock_cloud_off = 0;
    s_page = 1;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_AREA && s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_JUMP);
    cc_before = cloud_commit_calls;
    assert(reader_chapter_jump_commit(&ctx, 2, false) == APP_REDRAW_PAGE);
    assert(cloud_commit_calls == cc_before + 1 && cloud_commit_last == 0);
    assert(s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_LOCAL && s_page == 2);

    mock_cloud_ready = true; mock_cloud_ch = 15; mock_cloud_off = 0;
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_AREA && s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_CLOUD_JUMP);
    cc_before = cloud_commit_calls;
    assert(reader_chapter_jump_commit(&ctx, 2, false) == APP_REDRAW_PAGE); // !changed
    assert(cloud_commit_calls == cc_before + 1 && cloud_commit_last == 0 && !s_chapter_jump_undo.valid);

    // 9. Tier 5 Bug 1 regression: active WR_UNDO_LOCAL is cleared by cold wr_chapter_queue() so it never occludes capsule/402
    s_page = 0;
    assert(reader_chapter_jump_commit(&ctx, 2, false) == APP_REDRAW_PAGE);
    assert(s_chapter_jump_undo.valid && s_chapter_jump_undo.mode == WR_UNDO_LOCAL);
    mock_cached_ch = false;
    assert(wr_chapter_queue(&ctx, 16, 0, false, NULL, SIZE_MAX));
    assert(s_wr_jump.pending && !s_chapter_jump_undo.valid);
    wr_chapter_cancel();

    // 10. Tier 5 Bug 2 regression: 1-page chapter forward-cold + backward-cached turn_page() cancels s_wr_jump and prevents hijack
    mock_page_count = 1; s_chapter = 2; s_page = 0;
    mock_ch_cached_override[3] = 1; /* uncached forward */
    mock_ch_cached_override[1] = 2; /* cached backward */
    assert(turn_page(&ctx, 1) == APP_REDRAW_PAGE);
    assert(s_wr_jump.pending && s_wr_jump.chapter == 3 && !strcmp(s_reader_notice, "正在准备章节…"));
    assert(turn_page(&ctx, -1) == APP_REDRAW_AREA);
    assert(!s_wr_jump.pending && !s_reader_notice[0] && s_chapter == 1);
    mock_busy = false; mock_done = true;
    mock_result = (weread_result_t){.job = WR_JOB_CHAPTER, .error = ESP_OK, .chapter_uid = book_weread_uid(3)};
    strcpy(mock_result.id, "book_A");
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_NONE && s_chapter == 1);
    memset(mock_ch_cached_override, 0, sizeof(mock_ch_cached_override));
    mock_page_count = 4; mock_cached_ch = true;

    // 11. Tier 5 Gap 4a: same-chapter wr_cloud_undo_commit() (both WR_UNDO_CLOUD_JUMP and WR_UNDO_CLOUD_BACK)
    s_chapter = 4; s_page = 1;
    s_chapter_jump_undo.valid = true; s_chapter_jump_undo.mode = WR_UNDO_CLOUD_JUMP;
    s_chapter_jump_undo.chapter = 4; s_chapter_jump_undo.target_chapter = 4; s_chapter_jump_undo.target_offset = 300;
    s_chapter_jump_undo.until_ms = ctx.now_ms + 6000;
    cc_before = cloud_commit_calls;
    assert(wr_cloud_undo_commit(&ctx) == APP_REDRAW_PAGE);
    assert(s_chapter == 4 && s_page == 3 && !s_chapter_jump_undo.valid && cloud_commit_calls == cc_before + 1 && cloud_commit_last == 1);

    s_chapter_jump_undo.valid = true; s_chapter_jump_undo.mode = WR_UNDO_CLOUD_BACK;
    s_chapter_jump_undo.chapter = 4; s_chapter_jump_undo.target_chapter = 4; s_chapter_jump_undo.target_offset = 100;
    s_chapter_jump_undo.until_ms = ctx.now_ms + 6000;
    moves_before = move_calls_rt; cc_before = cloud_commit_calls;
    assert(wr_cloud_undo_commit(&ctx) == APP_REDRAW_PAGE);
    assert(s_chapter == 4 && s_page == 1 && !s_chapter_jump_undo.valid && move_calls_rt == moves_before + 1 && cloud_commit_calls == cc_before);

    // 12. Tier 5 Gap 4b: 6000ms WR_UNDO_CLOUD_JUMP expiry in on_tick() erases {96, 152, 492, 142} and commits false
    s_view = READING; s_reader_panel = READER_PANEL_NONE;
    s_chapter_jump_undo.valid = true; s_chapter_jump_undo.mode = WR_UNDO_CLOUD_JUMP;
    s_chapter_jump_undo.chapter = 4; s_chapter_jump_undo.target_chapter = 9; s_chapter_jump_undo.target_offset = 0;
    s_chapter_jump_undo.until_ms = ctx.now_ms + 6000;
    ctx.now_ms += 6000; cc_before = cloud_commit_calls;
    assert(test_tick_expiry(&ctx) == APP_REDRAW_AREA);
    assert(!s_chapter_jump_undo.valid && cloud_commit_calls == cc_before + 1 && cloud_commit_last == 0);
    assert(s_area.x == 96 && s_area.y == 152 && s_area.width == 492 && s_area.height == 142);

    // 13. Tier 5 Gap 4c: 5000ms TOC banner expiry in on_tick() erases {34, 214, 616, 36}
    s_view = TOC; s_toc_jump_open = false;
    copy_text(s_reader_notice, sizeof(s_reader_notice), "手机端购买本章后，再翻页即可加载");
    s_reader_notice_until = ctx.now_ms + 5000;
    ctx.now_ms += 5000;
    assert(test_tick_expiry(&ctx) == APP_REDRAW_AREA);
    assert(!s_reader_notice[0] && s_area.x == 34 && s_area.y == 214 && s_area.width == 616 && s_area.height == 36);

    // 14. Tier 5 Bug 3 regression: free_book() resets s_wr_undo_top_erase = false so next local EPUB tick does not fire {96, 152, 492, 24}
    s_view = READING; s_reader_fullscreen = false; s_chapter_jump_undo.valid = false;
    s_wr_undo_top_erase = true;
    free_book();
    assert(!s_wr_undo_top_erase);
    snprintf(s_path, sizeof(s_path), "/sdcard/Books/local.epub");
    s_text = strdup("local epub body"); assert(s_text);
    s_area = (EpdRect){0};
    assert(test_tick_expiry(&ctx) == APP_REDRAW_NONE);
    assert(s_area.width == 0 && s_area.height == 0);
    free(s_text); s_text = NULL;

    return 0;
}
'''
    compile_run(code, f"panel_undo_{board}", [f"-DKIIKO_BOARD_METALIO47={is_metalio}"])


def run_metalio_cache_and_panel_undo_tests(book_path: Path):
    book_src = book_path.read_text(encoding="utf-8")
    bridge_src = re.sub(r'#include\s+"weread/weread_store\.h"', "", (ROOT / "main/apps/book_weread_bridge.inc").read_text(encoding="utf-8"))
    undo_decl = book_src[book_src.index("static struct {\n    bool valid;\n    uint8_t mode;"):book_src.index("// 微调长按只更新预览")]
    panel_frame_decl = book_src[book_src.index("static struct {\n    bool valid,fullscreen;"):book_src.index("static void reader_panel_record(")]
    layout_top_def = next(l for l in book_src.splitlines() if l.startswith("#define READER_LAYOUT_TOP "))

    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "reader_page_cache.h"
typedef int esp_err_t;
enum {
    ESP_OK = 0,
    ESP_FAIL = -1,
    SHELF = 0, TOC, READING,
    READER_PANEL_NONE = 0, READER_PANEL_TOOLS, READER_PANEL_FONT_SETTINGS,
    READER_PANEL_WEIGHT_SETTINGS, READER_PANEL_LAYOUT_SETTINGS, READER_PANEL_RULE_SETTINGS,
    READER_PANEL_BACKGROUND, READER_PANEL_BACKGROUND_PREVIEW, READER_PANEL_FONT_PICKER,
    READER_PANEL_STATS, READER_PANEL_CHAPTER_JUMP, READER_PANEL_BOOKMARKS,
    READER_PANEL_STATS_RECENT, READER_PANEL_REFRESH_SETTINGS, READER_PANEL_TURN_SETTINGS,
    READER_PANEL_KEY_SETTINGS, READER_PANEL_KEY_PICKER, READER_PANEL_TOUCH_SETTINGS,
    READER_PANEL_TOUCH_MORE, READER_PANEL_FONT_RECOVERY,
    EPD_DRAW_ALIGN_LEFT = 0, EPD_DRAW_ALIGN_CENTER = 1, EPD_DRAW_ALIGN_RIGHT = 2,
    UI_GRAY_WHITE = 0xff, UI_GRAY_BLACK = 0x00, UI_NAV_BACKGROUND_GRAY = 0xe0,
    UI_LOCK_WIDTH = 684, UI_LOCK_HEIGHT = 1216, UI_MARGIN = 36,
    BOOK_TOOL_COUNT = 5, UI_READER_TOOLS_TOP = 1096, READER_STATS_TOP = 500
};
enum EpdDrawMode { MODE_GL16 = 1, MODE_DU = 2 };
enum EpdDrawError { EPD_DRAW_SUCCESS = 0, EPD_DRAW_ERROR = -1 };
typedef enum { APP_REDRAW_NONE, APP_REDRAW_PAGE, APP_REDRAW_AREA, APP_REDRAW_FULL } app_redraw_t;
typedef enum { WR_PROGRESS_LOCAL, WR_PROGRESS_PENDING, WR_PROGRESS_SYNCED, WR_PROGRESS_CONFLICT } weread_progress_state_t;
typedef enum {
    DISPLAY_READER_GRAY_PAGE = 0,
    DISPLAY_READER_GRAY_BACKGROUND,
    DISPLAY_READER_GRAY_NORMALIZE,
    DISPLAY_READER_GRAY_NORMALIZE_GRAY,
    DISPLAY_READER_GRAY_IMAGE
} display_reader_gray_profile_t;
typedef int reader_panel_t;
typedef struct { int x, y, width, height; } EpdRect;
typedef struct { uint8_t *front_fb, *back_fb; } epd_hl_t;
typedef struct { int64_t now_ms; bool request_app; epd_hl_t *hl; uint8_t *fb; } app_ctx_t;
typedef struct { bool started; uint32_t generation; } prep_cursor_t;
#define ESP_LOGI(...) ((void)0)
''' + layout_top_def + "\n" + undo_decl + r'''
static int s_view = READING, s_presented_view = READING, s_reader_panel = READER_PANEL_NONE, s_mode = MODE_GL16, s_reader_slider = -1;
static bool s_clear_confirm, s_toolbar, s_reader_footer_pending, s_reader_text_frame = true, s_reader_fullscreen, s_toc_jump_open;
static bool s_water_turn_pending, s_reader_turn_pending, s_reader_cleanup, s_fonts_reload_pending, s_reader_image_refresh_pending, s_reader_image_exit_pending;
static bool s_zoom_active, s_page_images, s_reader_favorite;
static int s_next_page = -1, s_prep_page = -1, s_du_count, s_px = 24;
static uint32_t s_next_generation, s_layout_gen = 1;
static uint8_t *s_next_fb;
static prep_cursor_t s_prep_cursor;
static EpdRect s_area;
static char s_path[64] = "weread://book_A", s_reader_notice[64];
static char *s_text;
static size_t s_text_len = 20, s_page, s_chapter, s_selected_toc = SIZE_MAX, s_selected_toc_offset;
static uint64_t s_reader_footer_due_ms;
static int64_t s_reader_notice_until, s_stats_activity_ms;
static unsigned mock_cached = 1, mock_total = 4, mock_offline = 0;
static bool mock_paused, mock_blocked, mock_stalled, mock_caching, mock_save_failed, mock_pending_save, mock_rescued, mock_has_turned;
static weread_progress_state_t mock_state = WR_PROGRESS_SYNCED;
static int fast_panel_updates;
static EpdRect last_fast_rect;

static size_t fb_bytes(void) { return (size_t)UI_LOCK_WIDTH * UI_LOCK_HEIGHT; }
static uint32_t book_layout_generation(void) { return s_layout_gen; }
static size_t book_layout_page_count(void) { return 4; }
static size_t book_layout_page_image_count(size_t p) { (void)p; return 0; }
static size_t reader_page_offset(size_t p) { return p * 100; }
static unsigned percent(size_t p) { return (unsigned)(p * 25); }
static const char *app_settings_book_background_path(void) { return ""; }
static bool book_path_is_weread(const char *p) { return p && strncmp(p, "weread://", 9) == 0; }
static void wr_copy(char *o, size_t c, const char *i) { snprintf(o, c, "%s", i ? i : ""); }
static void copy_text(char *o, size_t c, const char *i) { snprintf(o, c, "%s", i ? i : ""); }
static size_t book_navigation_count(void) { return 8; }
static size_t current_toc_position(void) { return 0; }
static void book_toc_set_cached_probe(bool (*fn)(size_t, void *), void *u) { (void)fn; (void)u; }
static void book_toc_prepare_page(size_t n, size_t cur, int pg) { (void)n; (void)cur; (void)pg; }
static bool weread_reader_chapter_cached(const char *u, size_t ch) { (void)u; (void)ch; return true; }
static void weread_runtime_prefetch_adopt(uint32_t u) { (void)u; }
static void weread_runtime_cancel(void) {}
static EpdRect body_rect(void) { return s_reader_fullscreen ? (EpdRect){36, 96, 612, 1020} : (EpdRect){36, 176, 612, 872}; }
static EpdRect reader_area(void) { return body_rect(); }
static void lock_draw(void) {}
static void unlock_draw(void) {}
static int64_t esp_timer_get_time(void) { return 1000000; }
static int epd_get_rotation(void) { return 0; }
static bool display_reader_fast_panels_enabled(void) { return false; }
static void display_reader_panel_gray4_prepare(uint8_t *f, EpdRect r) { (void)f; (void)r; }
static bool display_page_prepare_begin(void) { return true; }
static void display_page_prepare_end(bool p) { (void)p; }
static void guard_draw_result(epd_hl_t *hl, enum EpdDrawError err) { (void)hl; (void)err; }
static bool rc_draw_current(uint8_t *fb) { (void)fb; return false; }
static void ui_text_set_system_scale(bool on) { (void)on; }
static void ui_clear_page(uint8_t *fb) { memset(fb, UI_GRAY_WHITE, fb_bytes()); }
static void paint_rect_bytes(uint8_t *fb, EpdRect r, uint8_t g) {
    if (!fb) return;
    int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    int x1 = r.x + r.width > UI_LOCK_WIDTH ? UI_LOCK_WIDTH : r.x + r.width;
    int y1 = r.y + r.height > UI_LOCK_HEIGHT ? UI_LOCK_HEIGHT : r.y + r.height;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            fb[(size_t)y * UI_LOCK_WIDTH + (size_t)x] = g;
}
static void ui_fill_rect_fast(EpdRect r, uint8_t g, uint8_t *fb) { paint_rect_bytes(fb, r, g); }
static void ui_fill_round_rect(uint8_t *fb, EpdRect r, int rad, uint8_t g) { (void)rad; paint_rect_bytes(fb, r, g); }
static void ui_draw_round_rect(uint8_t *fb, EpdRect r, int rad, uint8_t g) { (void)fb; (void)r; (void)rad; (void)g; }
static void ui_draw_control_frame(uint8_t *fb, EpdRect r, int rad, uint8_t g) { (void)fb; (void)r; (void)rad; (void)g; }
static void ui_hairline(uint8_t *fb, int y, int x, int w, uint8_t g) { (void)fb; (void)y; (void)x; (void)w; (void)g; }
static void ui_text_box(uint8_t *fb, EpdRect r, int px, const char *s, int al, bool inv) { (void)fb; (void)r; (void)px; (void)s; (void)al; (void)inv; }
static void book_layout_draw_page(uint8_t *fb, size_t page, EpdRect body, int px) { (void)fb; (void)page; (void)body; (void)px; }
static bool reader_zoom_image(void) { return false; }
static bool reader_prefetch_resume(size_t page) { (void)page; return false; }
static void prepare_inline_image(void) {}
static bool app_settings_reader_hide_images(void) { return false; }
static void ui_reader_background_prepare(const char *p) { (void)p; }
static bool bookmark_is_current(void) { return false; }
static void draw_reader_tool_icon(uint8_t *fb, int cx, int y, int i, bool m) { (void)fb; (void)cx; (void)y; (void)i; (void)m; }
static void draw_font_settings(uint8_t *f) { (void)f; }
static void draw_weight_settings(uint8_t *f) { (void)f; }
static void draw_layout_settings(uint8_t *f) { (void)f; }
static void draw_rule_settings(uint8_t *f) { (void)f; }
static void draw_background_directory(uint8_t *f) { (void)f; }
static void draw_background_picker(uint8_t *f) { (void)f; }
static void draw_font_picker(uint8_t *f) { (void)f; }
static void draw_reader_stats(uint8_t *f) { (void)f; }
static void draw_reader_chapter_jump(uint8_t *f) { (void)f; }
static void draw_reader_bookmarks(uint8_t *f) { (void)f; }
static void draw_reader_stats_recent(uint8_t *f) { (void)f; }
static void draw_reading_settings(uint8_t *f) { (void)f; }
static void draw_turn_settings(uint8_t *f) { (void)f; }
static void draw_key_settings(uint8_t *f) { (void)f; }
static void draw_key_picker(uint8_t *f) { (void)f; }
static int reader_font_top(void) { return 700; }
static int reading_settings_top(void) { return 700; }
static int reader_font_picker_top(void) { return 500; }
static EpdRect reader_slider_refresh_rect(int s) { (void)s; return (EpdRect){0, 800, 684, 60}; }
static enum EpdDrawError update_display_reader_gray(epd_hl_t *hl, EpdRect area, display_reader_gray_profile_t prof, bool norm) {
    (void)hl; (void)prof; (void)norm;
    ++fast_panel_updates;
    last_fast_rect = area;
    return EPD_DRAW_SUCCESS;
}
static void weread_reader_navigated_version(const char *p, size_t c, uint32_t o, unsigned pct, const char *s, uint32_t v) { (void)p; (void)c; (void)o; (void)pct; (void)s; (void)v; }
static void weread_reader_tick(const char *p, size_t c, uint32_t o, unsigned pct, const char *s, uint64_t n, bool a) { (void)p; (void)c; (void)o; (void)pct; (void)s; (void)n; (void)a; }
static weread_progress_state_t weread_reader_state(void) { return mock_state; }
static bool weread_reader_save_failed(void) { return mock_save_failed; }
static bool weread_reader_pending_save(void) { return mock_pending_save; }
static bool weread_reader_rescued(void) { return mock_rescued; }
static unsigned weread_reader_offline(void) { return mock_offline; }
static void weread_reader_cached_progress(unsigned *c, unsigned *t) { *c = mock_cached; *t = mock_total; }
static bool weread_reader_storage_blocked(void) { return mock_blocked; }
static bool weread_reader_caching(void) { return mock_caching; }
static bool weread_reader_cache_fill_enabled(void) { return !mock_paused; }
static bool weread_reader_cache_forward_stalled(void) { return mock_stalled; }
static void weread_reader_cache_forward_set(bool on) { mock_paused = !on; }
static bool weread_runtime_reader_has_turned(const char *u) { (void)u; return mock_has_turned; }
static void weread_reader_commit_cloud_jump(const char *u, bool ok) { (void)u; (void)ok; }
static uint32_t book_weread_chapter_index(size_t ch) { return (uint32_t)(ch + 1); }
static void invalidate_prep(void);
''' + panel_frame_decl + "\n"
    code += bridge_src + "\n"
    for fn_name in ("book_tool_count", "tool_rect", "reader_tools_visible", "reader_chapter_undo_rect", "reader_chapter_undo_visible"):
        code += function(fn_name, book_path) + "\n"
    code += function("reader_fast_panel_rect", book_path) + "\n"
    code += function("draw_reader_panel", book_path) + "\n"
    code += r'''
static void draw_reader_outer(uint8_t *fb, size_t page) {
    (void)page;
    if (reader_chapter_undo_visible()) {
        EpdRect notice = {104, 160, 476, 126};
        ui_fill_round_rect(fb, notice, 20, 0xee);
    }
    draw_reader_panel(fb);
}
'''
    for fn_name in (
        "invalidate_prep", "pause_reader_prep", "draw_reader", "draw_reader_cached",
        "reader_panel_record", "reader_sheet_profile", "reader_panel_short_ready", "reader_toolbar_panel",
        "reader_toolbar_transition", "reader_closing_panel_to_text", "reader_panel_fast_present",
        "reader_slider_overlay_ready", "reader_slider_feedback_present", "paint_reading",
    ):
        code += function(fn_name, book_path) + "\n"

    code += r'''
int main(void) {
    uint8_t *front_fb = malloc(fb_bytes());
    uint8_t *back_fb = malloc(fb_bytes());
    assert(front_fb && back_fb);
    s_text = strdup("metalio47 reader body");
    assert(s_text);
    epd_hl_t hl = {.front_fb = front_fb, .back_fb = back_fb};
    app_ctx_t ctx = {.now_ms = 1000, .hl = &hl, .fb = front_fb};

    // 1. Fullscreen WR_UNDO_CLOUD_BACK on page 0 -> draw_reader_cached & paint_reading must NOT bake undo banner
    //    into reader_page_cache; turn 0->1 (wr_reader_move) -> turn 1->0 yields clean paper (0xff).
    reader_page_cache_clear();
    s_reader_fullscreen = true; s_view = READING; s_presented_view = READING;
    s_reader_panel = READER_PANEL_NONE; s_clear_confirm = false; s_reader_notice[0] = 0;
    s_chapter = 5; s_page = 0;
    s_chapter_jump_undo.valid = true; s_chapter_jump_undo.mode = WR_UNDO_CLOUD_BACK;
    s_chapter_jump_undo.chapter = 5; s_chapter_jump_undo.target_chapter = 2; s_chapter_jump_undo.target_offset = 100;
    s_chapter_jump_undo.until_ms = 7000;

    draw_reader_cached(front_fb, 0);
    assert(front_fb[200 * UI_LOCK_WIDTH + 120] == 0xee);
    assert(reader_page_cache_find(0, book_layout_generation(), fb_bytes()) == NULL);

    assert(paint_reading(&ctx, MODE_GL16) == APP_REDRAW_AREA);
    assert(front_fb[200 * UI_LOCK_WIDTH + 120] == 0xee);
    assert(reader_page_cache_find(0, book_layout_generation(), fb_bytes()) == NULL);

    wr_reader_move();
    assert(!s_chapter_jump_undo.valid && s_wr_undo_top_erase);
    s_page = 1;
    assert(paint_reading(&ctx, MODE_GL16) == APP_REDRAW_AREA);
    assert(reader_page_cache_find(1, book_layout_generation(), fb_bytes()) != NULL);

    s_page = 0;
    draw_reader_cached(front_fb, 0);
    assert(front_fb[200 * UI_LOCK_WIDTH + 120] == UI_GRAY_WHITE);

    // Also verify WR_UNDO_CLOUD_JUMP and WR_UNDO_LOCAL are never baked into reader_page_cache
    reader_page_cache_clear();
    s_chapter_jump_undo.valid = true; s_chapter_jump_undo.mode = WR_UNDO_CLOUD_JUMP;
    draw_reader_cached(front_fb, 0);
    assert(reader_page_cache_find(0, book_layout_generation(), fb_bytes()) == NULL);
    assert(paint_reading(&ctx, MODE_GL16) == APP_REDRAW_AREA);
    assert(reader_page_cache_find(0, book_layout_generation(), fb_bytes()) == NULL);

    s_chapter_jump_undo.valid = true; s_chapter_jump_undo.mode = WR_UNDO_LOCAL;
    draw_reader_cached(front_fb, 0);
    assert(reader_page_cache_find(0, book_layout_generation(), fb_bytes()) == NULL);
    assert(paint_reading(&ctx, MODE_GL16) == APP_REDRAW_AREA);
    assert(reader_page_cache_find(0, book_layout_generation(), fb_bytes()) == NULL);

    // 2. Active WR_UNDO_CLOUD_JUMP / WR_UNDO_LOCAL -> reader_panel_record(EPD_DRAW_SUCCESS) ->
    //    pause_reader_prep(); s_reader_panel = READER_PANEL_TOOLS; must make reader_panel_fast_present() return false.
    uint8_t undo_modes[2] = {WR_UNDO_CLOUD_JUMP, WR_UNDO_LOCAL};
    for (int i = 0; i < 2; ++i) {
        s_reader_fullscreen = false; s_view = READING; s_presented_view = READING;
        s_reader_panel = READER_PANEL_NONE; s_clear_confirm = false; s_reader_notice[0] = 0;
        s_page = 0; s_chapter = 5;
        s_chapter_jump_undo.valid = true; s_chapter_jump_undo.mode = undo_modes[i];
        s_chapter_jump_undo.chapter = 5; s_chapter_jump_undo.target_chapter = 8; s_chapter_jump_undo.until_ms = 7000;
        draw_reader_cached(front_fb, 0);
        assert(front_fb[200 * UI_LOCK_WIDTH + 120] == 0xee);
        memcpy(back_fb, front_fb, fb_bytes());
        reader_panel_record(EPD_DRAW_SUCCESS);
        assert(!s_reader_panel_frame.valid);
        pause_reader_prep();
        s_reader_panel = READER_PANEL_TOOLS;
        fast_panel_updates = 0;
        assert(!reader_panel_fast_present(&ctx, APP_REDRAW_PAGE));
        assert(fast_panel_updates == 0);
        // Even if s_reader_panel_frame.valid were true, short_ready / closing / slider_overlay must still reject active undo.
        s_reader_panel_frame.valid = true;
        s_reader_panel_frame.text = s_text;
        s_reader_panel_frame.rotation = 0;
        s_reader_panel_frame.panel = READER_PANEL_FONT_SETTINGS;
        s_reader_panel_frame.page = s_page;
        s_reader_panel_frame.chapter = s_chapter;
        s_reader_panel_frame.generation = book_layout_generation();
        s_reader_panel_frame.fullscreen = s_reader_fullscreen;
        s_reader_panel_frame.images = 0;
        s_reader_panel = READER_PANEL_FONT_SETTINGS;
        assert(!reader_panel_short_ready(APP_REDRAW_PAGE));
        assert(!reader_slider_overlay_ready(&ctx));
        s_reader_panel = READER_PANEL_NONE;
        assert(!reader_closing_panel_to_text());
    }

    reader_page_cache_clear();
    free(s_text);
    free(front_fb);
    free(back_fb);
    return 0;
}
'''
    compile_run(
        code,
        "metalio_cache_panel_undo",
        [
            "-Wno-unused-variable",
            "-DKIIKO_BOARD_METALIO47=1",
            f"-I{ROOT / 'main'}",
            f"-I{ROOT / 'main/book'}",
            f"-I{ROOT / 'tools/weread_stubs'}",
            str(ROOT / "main/book/reader_page_cache.c"),
        ],
    )


def run_input_and_toc_retry_tests(board: str, book_path: Path, is_metalio: int):
    book_src = book_path.read_text(encoding="utf-8")
    bridge_src = re.sub(r'#include\s+"weread/weread_store\.h"', "", (ROOT / "main/apps/book_weread_bridge.inc").read_text(encoding="utf-8"))
    chapter_src = (ROOT / "main/apps/book_weread_chapter.inc").read_text(encoding="utf-8")
    undo_decl = book_src[book_src.index("static struct {\n    bool valid;\n    uint8_t mode;"):book_src.index("// 微调长按只更新预览")]

    layout_h = (ROOT / "main/ui/ui_reader_layout.h").read_text(encoding="utf-8")
    layout_h = layout_h.replace("#pragma once", "").replace('#include "epdiy.h"', "").replace(", UI_READER_TOOLS_TOP=1096", "")
    code = COMMON_HEADER + layout_h + "\n" + undo_decl + r'''
#define BOOK_TOC_ROWS 7
#define BOOK_TOC_BACK (-2)
#define BOOK_TOC_PREV (-3)
#define BOOK_TOC_NEXT (-4)
#define BOOK_TOC_SKIP_PREV (-5)
#define BOOK_TOC_SKIP_NEXT (-6)
#define BOOK_TOC_JUMP (-7)
#define BOOK_TOC_JUMP_CANCEL (-8)
#define BOOK_TOC_JUMP_CONFIRM (-9)
#define BOOK_TOC_JUMP_TRACK (-10)
#define BOOK_TOC_SELECT_VOLUME (-11)
''' + r'''
static int s_view = READING, s_reader_panel = READER_PANEL_NONE, s_mode, s_reader_slider, s_footnote_note = -1;
static bool s_clear_confirm, s_toolbar, s_reader_footer_pending, s_reader_text_frame = true, s_reader_fullscreen;
static bool s_scan_pending, s_toc_jump_open, s_toc_jump_drag, s_sort_menu, s_batch_confirm;
static bool s_library_setup_pending, s_rescan_prompt, s_editor_from_bulk, s_import_candidates, s_bulk_selecting;
static bool s_category_menu __attribute__((unused)), s_shelf_open_queued __attribute__((unused)), s_fonts_reload_pending __attribute__((unused)), s_reader_rule_pending __attribute__((unused)), s_save_failed __attribute__((unused));
static uint8_t *s_editor_cover;
static char s_batch_message[32], s_search_draft[64] __attribute__((unused)), s_book_title[64] __attribute__((unused));
static struct { int x; } s_search_input __attribute__((unused));
static struct { bool pending; } s_reader_tap;
static uint64_t s_reader_footer_due_ms;
static int64_t s_reader_notice_until, s_stats_activity_ms, s_save_retry_ms __attribute__((unused));
static EpdRect s_area, s_footnote_marker;
static char s_path[64] = "weread://book_A", s_message[128], s_reader_notice[64], s_title[64];
static const char *s_text = "body text";
static size_t s_text_len = 9, s_chapter = 2, s_page = 1, s_selected_toc = SIZE_MAX, s_selected_toc_offset;
static bool mock_busy, mock_done, mock_cached_ch, mock_load_fail, mock_sync_fail;
static int mock_toc_hit = 3, mock_key3_action = APP_READER_KEY_NEXT, s_toc_jump_percent;
static weread_result_t mock_result;
static int cancel_calls, key_action_calls, turn_calls, toc_prep_calls, set_view_calls;
static int book_toc_jump_hit(uint16_t x, uint16_t y) { (void)x; (void)y; return BOOK_TOC_JUMP_CANCEL; }
static int book_toc_jump_percent(uint16_t x) { (void)x; return 0; }
static int book_toc_pages(size_t chapters) { (void)chapters; return 4; }
static void toc_cached_markers_prepare(int page);

static bool ui_rect_hit(EpdRect r, int x, int y) { return r.width > 0 && r.height > 0 && x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height; }
static bool book_path_is_weread(const char *p) { return p && strncmp(p, "weread://", 9) == 0; }
static size_t book_layout_page_count(void) { return 4; }
static size_t book_layout_page_image_count(size_t p) { (void)p; return 0; }
static size_t reader_page_offset(size_t p) { return p * 100; }
static unsigned percent(size_t p) { return (unsigned)(p * 25); }
static void wr_copy(char *o, size_t c, const char *i) { snprintf(o, c, "%s", i ? i : ""); }
static void copy_text(char *o, size_t c, const char *i) { snprintf(o, c, "%s", i ? i : ""); }
static void *wr_alloc(size_t n) { return calloc(1, n); }
static void invalidate_prep(void) {}
static void pause_reader_prep(void) {}
static void save_progress(void) {}
static void render(app_ctx_t *c, uint8_t *f) { (void)c; (void)f; }
static int leaves(void) { return 5; }
static int book_toc_open(size_t pos) { (void)pos; return 0; }
static int book_toc_hit(uint16_t x, uint16_t y, int page, size_t ch) { (void)x; (void)y; (void)page; (void)ch; return mock_toc_hit; }
static bool book_toc_picker_close(int *p) { (void)p; return false; }
static bool book_toc_picker_toggle(int *p) { (void)p; return false; }
static bool book_toc_toggle(int t, int *p) { (void)t; (void)p; return false; }
static int book_toc_page_from_percent(int pct, int pgs) { (void)pct; (void)pgs; return 0; }
static size_t book_navigation_count(void) { return 20; }
static size_t book_navigation_chapter(size_t i) { return i; }
static size_t book_chapter_count(void) { return 20; }
static const char *book_navigation_anchor(size_t i) { (void)i; return NULL; }
static size_t book_navigation_source_offset(size_t i) { (void)i; return SIZE_MAX; }
static esp_err_t book_navigation_title(size_t i, char *o, size_t c) { (void)i; snprintf(o, c, "Ch"); return ESP_OK; }
static size_t current_toc_position(void) { return 0; }
static void book_toc_set_cached_probe(bool (*fn)(size_t, void *), void *u) { (void)fn; (void)u; }
static void book_toc_prepare_page(size_t n, size_t cur, int pg) { (void)n; (void)cur; (void)pg; ++toc_prep_calls; }
static uint32_t book_weread_uid(size_t ch) { return (uint32_t)(ch + 10); }
static void weread_reader_navigated_version(const char *p, size_t c, uint32_t o, unsigned pct, const char *s, uint32_t v) { (void)p; (void)c; (void)o; (void)pct; (void)s; (void)v; }
static void weread_reader_tick(const char *p, size_t c, uint32_t o, unsigned pct, const char *s, uint64_t n, bool a) { (void)p; (void)c; (void)o; (void)pct; (void)s; (void)n; (void)a; }
static weread_progress_state_t weread_reader_state(void) { return WR_PROGRESS_SYNCED; }
static bool weread_reader_save_failed(void) { return false; }
static bool weread_reader_pending_save(void) { return false; }
static bool weread_reader_rescued(void) { return false; }
static unsigned weread_reader_offline(void) { return 0; }
static void weread_reader_cached_progress(unsigned *c, unsigned *t) { *c = 1; *t = 4; }
static bool weread_reader_storage_blocked(void) { return false; }
static bool weread_reader_caching(void) { return false; }
static bool weread_reader_cache_fill_enabled(void) { return true; }
static bool weread_reader_cache_forward_stalled(void) { return false; }
static void weread_reader_cache_forward_set(bool on) { (void)on; }
static bool weread_reader_chapter_cached(const char *u, size_t ch) { (void)u; (void)ch; return mock_cached_ch; }
static bool weread_reader_cached(const char *u, size_t ch) { (void)u; (void)ch; return mock_cached_ch; }
static esp_err_t weread_reader_save(uint64_t n) { (void)n; return ESP_OK; }
static bool weread_runtime_busy(void) { return mock_busy; }
static bool weread_runtime_start(int j, const char *i, const char *v) { (void)j; (void)i; (void)v; mock_busy = true; return true; }
static bool weread_runtime_poll(weread_result_t *r) { if (mock_busy || !mock_done) return false; mock_done = false; *r = mock_result; return true; }
static void weread_runtime_result_free(weread_result_t *r) { memset(r, 0, sizeof(*r)); }
static void weread_runtime_cancel(void) { ++cancel_calls; mock_busy = false; }
static bool weread_runtime_prefetch_active(const char *i, uint32_t u) { (void)i; (void)u; return false; }
static void weread_runtime_prefetch_adopt(uint32_t u) { (void)u; }
static bool weread_reader_poll_cloud_jump(const char *u, size_t *c, uint32_t *o) { (void)u; (void)c; (void)o; return false; }
static void weread_reader_commit_cloud_jump(const char *u, bool ok) { (void)u; (void)ok; }
static bool weread_reader_stage(weread_stage_t *s, uint64_t *b) { (void)s; (void)b; return false; }
static bool weread_reader_stage_text(char *o, size_t c) { (void)o; (void)c; return false; }
static bool weread_runtime_reader_has_turned(const char *u) { (void)u; return false; }
static esp_err_t weread_runtime_cloud_jump_rebase(const char *u) { (void)u; return ESP_OK; }
static void set_reader_view(int v) { s_view = v; ++set_view_calls; }
static EpdRect body_rect(void) { return ui_reader_body(s_reader_fullscreen, false, 24, false); }
static EpdRect reader_area(void) { return ui_reader_area(s_reader_fullscreen, false); }
static app_redraw_t reader_header_action(app_ctx_t *c, uint16_t x, uint16_t y, bool *h) { (void)c; (void)x; (void)y; *h = false; return APP_REDRAW_NONE; }
static int reader_footnote_hit(uint16_t x, uint16_t y, EpdRect *m) { (void)x; (void)y; (void)m; return -1; }
static bool reader_page_image_hit(uint16_t x, uint16_t y) { (void)x; (void)y; return false; }
static app_redraw_t rc_body_tap(app_ctx_t *c, uint16_t x, uint16_t y, bool im, bool *h) { (void)c; (void)x; (void)y; (void)im; *h = false; return APP_REDRAW_NONE; }
static bool book_reader_center(EpdRect r, uint16_t x, uint16_t y) { (void)r; (void)x; (void)y; return false; }
static app_redraw_t toggle_reader_fullscreen(app_ctx_t *c) { (void)c; return APP_REDRAW_FULL; }
static app_redraw_t reader_panel_action(app_ctx_t *c, uint16_t x, uint16_t y) { (void)c; (void)x; (void)y; return APP_REDRAW_NONE; }
static int reader_image_slots(size_t p) { (void)p; return 0; }
static EpdRect book_layout_image_rect(void) { return (EpdRect){0}; }
static bool reader_zoom_mode(void) { return false; }
static int book_reader_tap_feed_any(void *t, EpdRect r, uint16_t x, uint16_t y, bool vt, bool bv, int64_t now) { (void)t; (void)r; (void)x; (void)y; (void)vt; (void)bv; (void)now; return 0; }
static int book_reader_tap_feed(void *t, EpdRect r, uint16_t x, uint16_t y, bool vt, bool bv, int64_t now) { (void)t; (void)r; (void)x; (void)y; (void)vt; (void)bv; (void)now; return 0; }
static bool app_settings_reader_vertical_turn(void) { return false; }
static bool app_settings_book_vertical(void) { return false; }
static bool s_zoom_active __attribute__((unused)); static size_t s_image_zoom_page __attribute__((unused)); static int s_image_zoom __attribute__((unused));
static app_redraw_t turn_page(app_ctx_t *c, int d) { (void)c; (void)d; ++turn_calls; return APP_REDRAW_PAGE; }
static app_redraw_t toc_content_redraw(void) { return APP_REDRAW_PAGE; }
static app_redraw_t manage_action(app_ctx_t *c, uint16_t x, uint16_t y) { (void)c; (void)x; (void)y; return APP_REDRAW_NONE; }
static void search_finish(app_ctx_t *c, bool k) { (void)c; (void)k; }
static bool ui_text_input_tap(void *i, EpdRect r, int px, bool b, void *u, uint16_t x, uint16_t y) { (void)i; (void)r; (void)px; (void)b; (void)u; (void)x; (void)y; return false; }
static app_redraw_t editor_paint(app_ctx_t *c, bool f) { (void)c; (void)f; return APP_REDRAW_NONE; }
static void ui_text_edit_init(void *i, char *b, size_t c) { (void)i; (void)b; (void)c; }
static void ui_keyboard_begin(void *i, bool b) { (void)i; (void)b; }
static ui_keyboard_result_t ui_keyboard_tap(uint16_t x, uint16_t y, int top, int64_t now) { (void)x; (void)y; (void)top; (void)now; return UI_KEYBOARD_NONE; }
static bool ui_keyboard_pending(void) { return false; }
static app_redraw_t shelf_action_at(app_ctx_t *c, uint16_t x, uint16_t y) { (void)c; (void)x; (void)y; return APP_REDRAW_NONE; }
static void rc_pause_auto(void) {}
static void rc_cancel_side(void) {}
static bool s_rc_double_pending;
static void reader_indent_hold_cancel(void) {}
static app_redraw_t reader_key_action(app_ctx_t *c, int a) { (void)c; (void)a; ++key_action_calls; return APP_REDRAW_PAGE; }
static int app_settings_reader_key_action(int idx) { if (idx == 2) return mock_key3_action; return idx == 0 ? APP_READER_KEY_PREV : APP_READER_KEY_TOOLS; }
static int app_settings_reader_hold_action(void) { return APP_READER_KEY_HOME; }
static void ui_nav_request(app_ctx_t *c, int t) { (void)c; (void)t; }
static void ui_keyboard_end(void) {}
static app_redraw_t import_finish(app_ctx_t *c) { (void)c; return APP_REDRAW_NONE; }
static app_redraw_t library_cancel_candidates(app_ctx_t *c) { (void)c; return APP_REDRAW_NONE; }
static int selected_count(void) { return 0; }
static void clear_selection(void) {}
static app_redraw_t bulk_finish(app_ctx_t *c) { (void)c; return APP_REDRAW_NONE; }
static app_redraw_t bulk_action(app_ctx_t *c, uint16_t x, uint16_t y) { (void)c; (void)x; (void)y; return APP_REDRAW_NONE; }
static bool load_chapter_at(app_ctx_t *ctx, size_t ch, size_t off, bool last, const char *a, size_t src);
static bool next_chapter_can_adopt(size_t ch) { (void)ch; return false; }
static bool commit_next_chapter(app_ctx_t *c, size_t ch) { (void)c; (void)ch; return false; }
'''
    code += bridge_src + "\n" + chapter_src + "\n"
    code += r'''
static bool load_chapter_at(app_ctx_t *ctx, size_t ch, size_t off, bool last, const char *a, size_t src) {
    if (mock_sync_fail) {
        copy_text(s_message, sizeof(s_message), "本地位置未保存，请检查存储卡后重试");
        return false;
    }
    if (!s_wr_jump.committing && !weread_reader_cached(s_path, ch)) {
        wr_chapter_queue(ctx, ch, off, last, a, src);
        return false;
    }
    if (mock_load_fail) return false;
    s_chapter = ch;
    s_page = 0;
    return true;
}
'''
    action_fn = function("action_at", book_path)
    # Trim the tail after READING tap_feed if needed? Let's compile action_at with minimal stubs for shelf tail
    code += r'''
static bool s_reading_only_guard __attribute__((unused));
static int s_pressed_control;
static bool s_font_runtime_fallback_seen __attribute__((unused));
static void weread_reader_input(bool text_page) { (void)text_page; }
static app_redraw_t bulk_return_shelf(app_ctx_t *c) { (void)c; return APP_REDRAW_NONE; }
static app_redraw_t library_setup_action(app_ctx_t *c, uint16_t x, uint16_t y) { (void)c; (void)x; (void)y; return APP_REDRAW_NONE; }
static app_redraw_t cover_lookup_back(void) { return APP_REDRAW_NONE; }
static app_redraw_t cover_lookup_page(int d) { (void)d; return APP_REDRAW_NONE; }
static void reader_font_recovery_reset(void) {}
static bool ttf_font_ready(void) { return false; }
static bool ttf_font_is_builtin(void) { return false; }
static void shelf_category_remember(app_ctx_t *c) { (void)c; }
static int shelf_rows(void) { return 1; }
static int book_count_for_category(void) { return 0; }
static app_redraw_t open_requested_book(app_ctx_t *c) { (void)c; return APP_REDRAW_NONE; }
static EpdRect row_rect(int r) { (void)r; return (EpdRect){0}; }
static bool s_shelf_page_pending __attribute__((unused));
static void prepare_covers(app_ctx_t *c) { (void)c; }
static bool book_reader_action(int t) { (void)t; return false; }
static app_redraw_t reader_footnote_action(app_ctx_t *c, uint16_t x, uint16_t y) { (void)c; (void)x; (void)y; return APP_REDRAW_NONE; }
static bool ui_keyboard_key(int k) { (void)k; return false; }
static app_redraw_t shelf_key_tick(app_ctx_t *c) { (void)c; return APP_REDRAW_NONE; }
'''
    # Extract TOC + READING action_at, on_key, on_key_long, plus check on_tick TOC sync
    # Note: action_at has SHELF/IMPORT/COVER_LOOKUP/BULK branches at the top/bottom that reference many shelf helpers.
    # Slice only the TOC and READING sections from action_at into test wrappers to keep stubs tight while testing exact production statements!
    toc_start = action_fn.index("    if (s_view == TOC) {")
    toc_end = action_fn.index("    if (s_view == MANAGE)")
    reading_start = action_fn.index("    if (s_view == READING) {")
    reading_end = action_fn.index("        // 翻页按用户选择的")
    toc_reading_fn = (
        "static app_redraw_t test_action_at(app_ctx_t *ctx, uint16_t x, uint16_t y) {\n"
        + action_fn[toc_start:toc_end]
        + action_fn[reading_start:reading_end]
        + "    }\n    return APP_REDRAW_NONE;\n}\n"
    )
    code += toc_reading_fn + "\n"
    code += function("on_key", book_path) + "\n"
    code += function("on_key_long", book_path) + "\n"

    code += r'''
int main(void) {
    app_ctx_t ctx = {.now_ms = 1000, .fb = (uint8_t *)1, .leaf = 2};

    // 1. WR-UX-05b: Capsule tap cancel in non-fullscreen (y=165 above reader_area) and fullscreen
    mock_cached_ch = false;
    assert(wr_chapter_queue(&ctx, 5, 0, false, NULL, SIZE_MAX));
    assert(s_wr_jump.pending && s_view == READING && s_chapter == 2 && s_page == 1);
    s_reader_fullscreen = false;
    int cc = cancel_calls;
    assert(test_action_at(&ctx, 300, 165) == APP_REDRAW_AREA);
    assert(!s_wr_jump.pending && cancel_calls == cc + 1 && s_chapter == 2 && s_page == 1);
    assert(s_area.x == 208 && s_area.y == 150 && s_area.width == 268 && s_area.height == 68 && s_mode == MODE_GL16);

    // 2. WR-UX-05b: UI_KEY_1 / UI_KEY_2 cancel in on_key(); UI_KEY_3 NEXT does NOT cancel; UI_KEY_3 HOME cancels; on_key_long UI_KEY_2 cancels
    assert(wr_chapter_queue(&ctx, 5, 0, false, NULL, SIZE_MAX));
    mock_key3_action = APP_READER_KEY_NEXT;
    int ka = key_action_calls;
    assert(on_key(&ctx, UI_KEY_3) == APP_REDRAW_PAGE && key_action_calls == ka + 1 && s_wr_jump.pending);
    assert(on_key(&ctx, UI_KEY_1) == APP_REDRAW_AREA && !s_wr_jump.pending && key_action_calls == ka + 1);

    assert(wr_chapter_queue(&ctx, 5, 0, false, NULL, SIZE_MAX));
    assert(on_key(&ctx, UI_KEY_2) == APP_REDRAW_AREA && !s_wr_jump.pending && key_action_calls == ka + 1);

    assert(wr_chapter_queue(&ctx, 5, 0, false, NULL, SIZE_MAX));
    mock_key3_action = APP_READER_KEY_HOME;
    assert(on_key(&ctx, UI_KEY_3) == APP_REDRAW_AREA && !s_wr_jump.pending && key_action_calls == ka + 1);

    assert(wr_chapter_queue(&ctx, 5, 0, false, NULL, SIZE_MAX));
    assert(on_key_long(&ctx, UI_KEY_2) == APP_REDRAW_AREA && !s_wr_jump.pending && key_action_calls == ka + 1);

    // 3. WR-UX-05b: TOC cold jump failure auto-returns to original leaf with non-blocking banner, allowing immediate same-row retry
    s_view = TOC; ctx.leaf = 2; mock_toc_hit = 6; mock_cached_ch = false; s_message[0] = 0; s_reader_notice[0] = 0;
    assert(test_action_at(&ctx, 200, 400) == APP_REDRAW_PAGE);
    assert(s_wr_jump.pending && s_wr_jump.toc == 6 && s_wr_jump.toc_page == 2 && s_view == READING);
    mock_busy = false; mock_done = true;
    mock_result = (weread_result_t){.job = WR_JOB_CHAPTER, .error = WEREAD_ERR_PURCHASE, .chapter_uid = book_weread_uid(6)};
    strcpy(mock_result.id, "book_A");
    assert(wr_chapter_tick(&ctx) == APP_REDRAW_PAGE);
    assert(!s_wr_jump.pending && s_view == TOC && ctx.leaf == 2 && !s_message[0]);
    assert(!strcmp(s_reader_notice, "手机端购买本章后，再翻页即可加载") && s_reader_notice_until == ctx.now_ms + 5000);

    // Immediate same-row retry succeeds when chapter is now cached!
    mock_cached_ch = true;
    assert(test_action_at(&ctx, 200, 400) == APP_REDRAW_PAGE);
    assert(s_view == READING && s_chapter == 6 && !s_reader_notice[0]);

    // 4. WR-UX-05b: Synchronous TOC failure moves s_message to non-blocking s_reader_notice and preserves ctx.leaf
    s_view = TOC; ctx.leaf = 4; mock_sync_fail = true;
    assert(test_action_at(&ctx, 200, 400) == APP_REDRAW_PAGE);
    assert(s_view == TOC && ctx.leaf == 4 && !s_message[0] && strstr(s_reader_notice, "存储卡") && s_reader_notice_until == ctx.now_ms + 5000);
    mock_sync_fail = false;

    return 0;
}
'''
    compile_run(code, f"input_toc_{board}", [f"-DKIIKO_BOARD_METALIO47={is_metalio}", f"-I{ROOT / 'main/ui'}"])


def run_dashboard_tests(board: str, dash_path: Path, is_metalio: int):
    dash_src = dash_path.read_text(encoding="utf-8")
    # Reuse the exact extractor logic as test_weread_ux_e2e.py for WR-UX-06c plus extra pico stat-fail check
    m = re.search(r"typedef\s+struct\s*\{[^}]*\}\s*home_book_t\s*;", dash_src, re.S)
    assert m, f"home_book_t not found in {dash_path}"
    home_book_typedef = m.group(0)
    parts = [
        home_book_typedef,
        function("is_book", dash_path),
        function("dashboard_weread_pending", dash_path),
        function("title_for", dash_path),
        function("consider_book", dash_path),
        function("scan_root", dash_path),
        function("recent_visit", dash_path),
        function("scan_books", dash_path),
        function("render_root", dash_path),
    ]
    has_refresh = not is_metalio
    if has_refresh:
        parts.append(function("refresh_progress", dash_path))
    parts.append(function("on_enter", dash_path))

    code = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "weread_progress.h"
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
#ifndef ESP_ERR_NOT_FINISHED
#define ESP_ERR_NOT_FINISHED 0x109
#endif
typedef enum { EPD_DRAW_ALIGN_LEFT = 0, EPD_DRAW_ALIGN_CENTER = 1, EPD_DRAW_ALIGN_RIGHT = 2 } EpdFontFlags;
typedef struct { int x, y, width, height; } EpdRect;
typedef struct { uint32_t file_size; uint16_t chapter; uint32_t byte_off; uint8_t px, pct; uint32_t last_open_s; } book_progress_t;
typedef struct { int64_t now_ms; } app_ctx_t;
#define BOOK_STORE_PATH_MAX 288
''' + home_book_typedef + r'''
typedef struct { bool present, mounted; } read_pico_sd_info_t;
typedef enum { APP_HOME_FOLDER = 99 } app_home_component_t;
#define UI_ROOT_GRAY 0xf0
#define UI_GRAY_WHITE 0xf0
#define BOOK_EXCERPT_CAP 128
#define HOME_ROOT 0
static home_book_t s_current, s_recent[6];
static char s_excerpt[BOOK_EXCERPT_CAP], s_cached_last_path[BOOK_STORE_PATH_MAX] = "weread://book_A";
static bool s_avatar_ready, s_reading_days_valid, s_books_cache_valid, s_scan_pending, s_featured_cover_dirty __attribute__((unused));
static bool s_clock_base_attempted, s_clock_pending, s_time_pending, s_widget_pending, s_component_pending, s_press_pending, s_save_error, s_home_force_full_once;
static bool s_progress_retry_pending __attribute__((unused)), s_folder_attempted __attribute__((unused));
static int s_view, s_time_stamp, s_widget_stamp, s_motion_ms, s_cover_next;
static int64_t s_home_scan_after __attribute__((unused)), s_shelf_warm_next __attribute__((unused)), s_shelf_warm_after __attribute__((unused)), s_progress_retry_after __attribute__((unused));
static unsigned s_weread_cover_revision, s_cover_generation;
static uint64_t s_cover_font_key;
static uint32_t s_cached_progress_revision = 1, s_folder_key __attribute__((unused));
static EpdRect s_component_dirty;
static struct { bool valid; } s_widget_trail, s_time_trail;
static struct tm s_date;
static uint8_t s_avatar[60 * 60], *s_folder;
static bool s_folder_fast;
static unsigned s_widget_phase;
static char drawn[4096];
static char mock_last[BOOK_STORE_PATH_MAX] = "weread://book_A";
static bool mock_pending = true, mock_stat_fail = false;
static int load_local_calls;
static home_book_t *book_at(int index) { return index ? &s_recent[index - 1] : &s_current; }
static home_book_t *featured(void) { return s_current.path[0] ? &s_current : &s_recent[0]; }
static bool book_path_is_weread(const char *p) { return p && strncmp(p, "weread://", 9) == 0; }
static bool book_path_is_supported_target(const char *p) { return p && p[0]; }
static bool book_file_path_supported(const char *p) { return p && p[0]; }
static int book_target_stat(const char *p, struct stat *st) {
    if (mock_stat_fail || !p || !st || strcmp(p, mock_last) != 0) { errno = EIO; return -1; }
    memset(st, 0, sizeof(*st)); st->st_mode = S_IFREG | 0644; st->st_size = 1024; return 0;
}
static bool app_setup_library_pending(void) { return false; }
static void weread_runtime_load_local(void) { ++load_local_calls; }
static unsigned book_weread_cover_revision(void) { return s_weread_cover_revision; }
static void app_home_cover_mode_changed(void) {}
static void app_book_home_shelf_warm_stop(void) {}
static uint64_t book_auto_cover_font_key(void) { return 0; }
static void clear_cover_cache(void) {}
static unsigned book_cover_jobs_begin(void) { return 1; }
static uint32_t book_progress_revision(void) { return s_cached_progress_revision; }
static void clock_base_prepare(void) {}
static void ui_home_clock_preview_invalidate(void) {}
static bool app_settings_home_full_refresh(void) { return false; }
static void refresh_days(void) {}
static void avatar_prepare_state(void) {}
static bool scan_state_matches(const read_pico_sd_info_t *sd, esp_err_t e) { (void)sd; (void)e; return true; }
static void read_pico_sd_start_probe(void) {}
static void remember_scan_state(const read_pico_sd_info_t *sd) { (void)sd; }
static void restore_cover_cache(void) {}
static void rp_boot_mark(const char *s) { (void)s; }
static uint32_t folder_key(void) { return 0; }
static void refresh_excerpt(void) {}
static bool book_progress_last_path(char *o, size_t c) { snprintf(o, c, "%s", mock_last); return *mock_last != 0; }
static bool book_progress_load(const char *p, uint32_t sz, book_progress_t *out) {
    if (!p || !out || strcmp(p, mock_last) != 0) return false;
    memset(out, 0, sizeof(*out)); out->file_size = sz; out->pct = 42; return true;
}
static esp_err_t book_progress_load_checked(const char *p, uint32_t sz, book_progress_t *out) {
    return book_progress_load(p, sz, out) ? ESP_OK : ESP_ERR_NVS_NOT_FOUND;
}
static esp_err_t book_progress_list(bool (*v)(const char *, const book_progress_t *, void *), void *c) { (void)v; (void)c; return ESP_OK; }
static bool recent_visit(const char *p, const book_progress_t *pr, void *c);
static void copy_title(char *d, size_t c, const char *s) { snprintf(d, c, "%s", s ? s : ""); }
static bool book_weread_metadata(const char *p, char *t, size_t tc, char *a, size_t ac) {
    if (!book_path_is_weread(p)) return false;
    snprintf(t, tc, "WeRead Title"); snprintf(a, ac, "Author"); return true;
}
static bool book_weread_cover_metadata(const char *p, char *t, size_t tc, char *a, size_t ac) { (void)p; (void)t; (void)tc; (void)a; (void)ac; return false; }
static bool book_name_from_path(const char *p, char *t, size_t tc, char *a, size_t ac) { (void)p; snprintf(t, tc, "Local"); a[0] = 0; return true; }
static esp_err_t read_pico_sd_get_info(read_pico_sd_info_t *sd) { if (sd) memset(sd, 0, sizeof(*sd)); return ESP_OK; }
static const char *app_settings_books_dir(void) { return "/sdcard/Books"; }
static bool book_store_flash_ready(void) { return false; }
static esp_err_t book_epub_metadata_cached(const char *p, char *t, size_t tc, char *a, size_t ac) { (void)p; (void)t; (void)tc; (void)a; (void)ac; return ESP_FAIL; }
static esp_err_t book_epub_metadata(const char *p, char *t, size_t tc, char *a, size_t ac) { (void)p; (void)t; (void)tc; (void)a; (void)ac; return ESP_FAIL; }
static bool book_author_clean_display(char *s, size_t c) { (void)s; (void)c; return false; }
static bool pico_boot_asset_allowed(const char *p) { (void)p; return true; }
esp_err_t weread_progress_load(const char *id, weread_progress_t *out) {
    if (!id || !out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    if (strcmp(id, "book_A") != 0) return ESP_ERR_NOT_FOUND;
    if (mock_pending) { out->dirty = true; out->position_pending = true; }
    else out->last_report_ms = 1000;
    return ESP_OK;
}
weread_progress_state_t weread_progress_state(const weread_progress_t *p) {
    if (!p) return WR_PROGRESS_LOCAL;
    if (p->sync_conflict) return WR_PROGRESS_CONFLICT;
    if (p->dirty && p->position_pending) return WR_PROGRESS_PENDING;
    return p->last_report_ms ? WR_PROGRESS_SYNCED : WR_PROGRESS_LOCAL;
}
static void record_d(const char *s) { if (s) { size_t n = strlen(drawn); snprintf(drawn + n, sizeof(drawn) - n, "%s\n", s); } }
static void ui_nav_status(uint8_t *f) { (void)f; }
static void ui_nav_draw(uint8_t *f, int a) { (void)f; (void)a; }
static unsigned today_seconds(void) { return 600; }
static void fit(char *t, int px, int w) { (void)t; (void)px; (void)w; }
static void label(uint8_t *f, int x, int y, int px, const char *s, EpdFontFlags al, uint8_t g) { (void)f; (void)x; (void)y; (void)px; (void)al; (void)g; record_d(s); }
static void ui_text(uint8_t *f, int x, int y, int px, const char *s, EpdFontFlags al, bool inv) { (void)f; (void)x; (void)y; (void)px; (void)al; (void)inv; record_d(s); }
static void label_on(uint8_t *f, int x, int y, int px, const char *s, EpdFontFlags al, uint8_t g, uint8_t bg) { (void)f; (void)x; (void)y; (void)px; (void)al; (void)g; (void)bg; record_d(s); }
static void ring(uint8_t *f, int cx, int cy, int r, int th, uint32_t s, unsigned g) { (void)f; (void)cx; (void)cy; (void)r; (void)th; (void)s; (void)g; }
static unsigned app_settings_home_goal_minutes(void) { return 30; }
static app_home_component_t app_settings_home_component(void) { return APP_HOME_FOLDER; }
static bool app_settings_main_fast_refresh(void) { return false; }
static void ui_wallpaper_draw_avatar(uint8_t *f, uint8_t *a, EpdRect r, int rad) { (void)f; (void)a; (void)r; (void)rad; }
static void epd_fill_circle(int x, int y, int r, uint8_t g, uint8_t *f) { (void)x; (void)y; (void)r; (void)g; (void)f; }
static void ui_root_card(uint8_t *f, EpdRect r, int rad) { (void)f; (void)r; (void)rad; }
static void ui_fill_round_rect(uint8_t *f, EpdRect r, int rad, uint8_t g) { (void)f; (void)r; (void)rad; (void)g; }
static void home_frame(uint8_t *f, EpdRect r, int rad) { (void)f; (void)r; (void)rad; }
static void circle_frame(uint8_t *f, int x, int y, int r) { (void)f; (void)x; (void)y; (void)r; }
static void ui_fill_rect_fast(EpdRect r, uint8_t g, uint8_t *f) { (void)r; (void)g; (void)f; }
static int ui_text_fixed_width_px(int px, const char *s) { return s ? (int)strlen(s) * px / 2 : 0; }
static void ui_draw_separator(uint8_t *f, int y, int x, int w, uint8_t g) { (void)f; (void)y; (void)x; (void)w; (void)g; }
static void draw_cover(uint8_t *f, const home_book_t *b, EpdRect r) { (void)f; (void)b; (void)r; }
static bool continue_card_pressed(void) { return false; }
static int title_lines(uint8_t *f, const char *t, uint8_t bg) { (void)f; (void)bg; record_d(t); return 1; }
static void ui_home_folder_draw(uint8_t *f, uint8_t *fd, bool fast, EpdRect r) { (void)f; (void)fd; (void)fast; (void)r; }
static void clock_draw_region(uint8_t *f, EpdRect b, const struct tm *d, bool bin, int st, EpdRect c, unsigned p) { (void)f; (void)b; (void)d; (void)bin; (void)st; (void)c; (void)p; }
static int clock_style(app_home_component_t c) { (void)c; return 0; }
static const char *component_name(app_home_component_t c) { (void)c; return "最近阅读书夹"; }
static unsigned month_length(const struct tm *d) { (void)d; return 30; }
static void calendar(uint8_t *f, int x, int y, int sx, int sy, int px, unsigned g) { (void)f; (void)x; (void)y; (void)sx; (void)sy; (void)px; (void)g; }
''' + "\n".join(parts[1:]) + r'''
int main(void) {
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    strcpy(mock_last, "weread://book_A");
    mock_pending = true;
    scan_books();
    drawn[0] = 0; render_root(NULL);
    assert(strstr(drawn, "已读 42% · 待同步") != NULL);

    mock_pending = false;
    memset(&s_current, 0, sizeof(s_current));
    scan_books();
    drawn[0] = 0; render_root(NULL);
    assert(strstr(drawn, "已读 42%") != NULL && strstr(drawn, "待同步") == NULL);

#if !KIIKO_BOARD_METALIO47
    mock_pending = true; mock_stat_fail = true;
    refresh_progress();
    assert(s_current.weread_pending);
    drawn[0] = 0; render_root(NULL);
    assert(strstr(drawn, "已读 42% · 待同步") != NULL);
    mock_stat_fail = false;
#endif

    memset(&s_current, 0, sizeof(s_current));
    strcpy(mock_last, "/sdcard/Books/local.epub");
    mock_pending = true;
    scan_books();
    drawn[0] = 0; render_root(NULL);
    assert(strstr(drawn, "已读 42%") != NULL && strstr(drawn, "待同步") == NULL);

    /* Tier 5 Gap 4d: on_enter() dual-board weread_pending transitions */
    app_ctx_t ctx = {.now_ms = 5000};
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    strcpy(mock_last, "weread://book_A");
    strcpy(s_cached_last_path, "weread://book_A");
    mock_pending = false;
    scan_books();
    assert(!s_current.weread_pending);
    s_books_cache_valid = true;
    mock_pending = true;
    int ll_before = load_local_calls;
    on_enter(&ctx);
    assert(load_local_calls == ll_before + 1 && !s_books_cache_valid && s_current.weread_pending);
    drawn[0] = 0; render_root(NULL);
    assert(strstr(drawn, "已读 42% · 待同步") != NULL);
    s_books_cache_valid = true;
    mock_pending = false;
    on_enter(&ctx);
    assert(!s_books_cache_valid && !s_current.weread_pending);
    drawn[0] = 0; render_root(NULL);
    assert(strstr(drawn, "已读 42%") != NULL && strstr(drawn, "待同步") == NULL);
    return 0;
}
'''
    compile_run(code, f"dash_{board}", ["-Wno-sign-compare", f"-DKIIKO_BOARD_METALIO47={is_metalio}", f"-I{ROOT / 'main/weread'}", f"-I{ROOT / 'tools/weread_stubs'}"])


def main():
    boards = [
        ("metalio47", ROOT / "main/apps/app_book.c", ROOT / "main/apps/app_dashboard.c", 1),
        ("pico", ROOT / "boards/pico/src/main/apps/app_book.c", ROOT / "boards/pico/src/main/apps/app_dashboard.c", 0),
    ]
    for board, book_path, dash_path, is_metalio in boards:
        run_reader_panel_and_cloud_undo_tests(board, book_path, is_metalio)
        if is_metalio:
            run_metalio_cache_and_panel_undo_tests(book_path)
        run_input_and_toc_retry_tests(board, book_path, is_metalio)
        run_dashboard_tests(board, dash_path, is_metalio)
        print(f"PASS: {board} WR-UX-02, WR-UX-04b, WR-UX-05b, WR-UX-06c")
    print("PASS: all M4 dual-board UX host tests")


if __name__ == "__main__":
    main()
