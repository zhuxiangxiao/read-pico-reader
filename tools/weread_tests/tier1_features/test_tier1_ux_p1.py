#!/usr/bin/env python3
"""Tier 1 Feature Coverage: WeRead P1 UX Optimizations (WR-UX-06 .. WR-UX-09).

20 tests (5 per feature):
- WR-UX-06: 600000ms RF Idle, 8s Pause Report, Exit Note, Home '· 待同步'
- WR-UX-07: Deterministic Offline Badges, Long-Press Action Card, Whole-Book Cache
- WR-UX-08: Title Dropdown Status Filter & Footer Page-Jump Slider
- WR-UX-09: '更多 -> 缓存管理' Stats & Selective Chapter Cleanup
"""

from __future__ import annotations

import os
import sys
import tempfile
import threading
import time
import urllib.request

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
COMMON_DIR = os.path.join(os.path.dirname(SCRIPT_DIR), "common")
if COMMON_DIR not in sys.path:
    sys.path.insert(0, COMMON_DIR)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import weread_mock_server
from weread_client_sim import WereadClient, WereadShelfModel, WereadStorage

PORT = int(os.environ.get("WEREAD_MOCK_PORT", "8989"))


def ensure_mock_server() -> None:
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/mock/control/health", timeout=1) as resp:
            if resp.status == 200:
                return
    except Exception:
        pass
    httpd = weread_mock_server.ThreadedHTTPServer(("127.0.0.1", PORT), weread_mock_server.WeReadMockRequestHandler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    time.sleep(0.1)


def _authed_client(tmp_dir: str) -> tuple[WereadStorage, WereadClient]:
    storage = WereadStorage(tmp_dir)
    client = WereadClient(PORT, storage)
    client.mock_reset()
    client.login_device_exchange(
        vid="12345678",
        skey="mock_skey_123456",
        code="mock_qr_code_xyz123",
        device_id="eink-ux-p1",
    )
    return storage, client


# ==============================================================================
# WR-UX-06: 600000ms RF Idle, 8s Pause Report, Exit Note, Home '· 待同步' (5 tests)
# ==============================================================================

def test_ux06_rf_idle_threshold_remains_600000ms() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.connect_network(now_ms=10000)
        assert client.rf_idle_timeout_ms == 600000
        assert client.check_rf_idle_disconnect(10000 + 60000) is False
        assert client.check_rf_idle_disconnect(10000 + 90000) is False
        assert client.check_rf_idle_disconnect(10000 + 300000) is False
        assert client.check_rf_idle_disconnect(10000 + 599999) is False
        assert client.owns_network is True
        assert client.check_rf_idle_disconnect(10000 + 600000) is True
        assert client.owns_network is False


def test_ux06_real_http_activity_refreshes_network_active() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.connect_network(now_ms=10000)
        client.now_ms = 500000
        client.sync_shelf_full()
        assert client.last_network_activity_ms == 500000
        assert client.check_rf_idle_disconnect(610000) is False
        assert client.owns_network is True
        assert client.check_rf_idle_disconnect(1100000) is True


def test_ux06_8s_pause_silent_report_with_15s_spacing() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.connect_network(now_ms=1000)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 20, 60, dirty=1)
        client.connect_network(now_ms=2000)
        client.note_page_move(2000)
        # Before 8s: suppressed
        assert client.tick_reader_pause("test_book_101", 9999) is False
        # At 8s: fires silent WR_JOB_NOTE
        assert client.tick_reader_pause("test_book_101", 10000) is True
        assert storage.load_local_progress("test_book_101")["dirty"] == 0
        # Second move at 11s: even 8s later (19s), < 15s since last report (10s) -> suppressed
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 25, 90, dirty=1)
        client.connect_network(now_ms=11000)
        client.note_page_move(11000)
        assert client.tick_reader_pause("test_book_101", 19000) is False
        # At 25s (>= 15s since 10s report AND >= 8s since 11s move): fires
        assert client.tick_reader_pause("test_book_101", 25000) is True
        assert storage.load_local_progress("test_book_101")["dirty"] == 0


def test_ux06_normal_exit_fires_nonblocking_note() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 2, 2, 45, 300, dirty=1)
        client.connect_network(now_ms=5000)
        assert client.end_reader_session("test_book_101", now_ms=6000, busy=False) is True
        assert storage.load_local_progress("test_book_101")["dirty"] == 0
        hist = [h for h in client.mock_get_history() if h["path"] == "/book/read"]
        assert len(hist) == 1
        cloud = client.get_cloud_progress("test_book_101")
        assert cloud["progress"] == 45 and cloud["chapterUid"] == 2


def test_ux06_home_continue_card_shows_pending_badge() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 38, 120, dirty=1)
        card_pending = client.render_home_continue_card("weread://test_book_101")
        assert card_pending["pending"] is True
        assert card_pending["pendingBadge"] == "· 待同步"
        assert "38%" in card_pending["statusDisplay"] and "· 待同步" in card_pending["statusDisplay"]
        client.set_wifi_enabled(True)
        assert client.sync_progress_to_cloud("test_book_101") is True
        card_synced = client.render_home_continue_card("weread://test_book_101")
        assert card_synced["pending"] is False
        assert card_synced["pendingBadge"] == ""
        assert "· 待同步" not in card_synced["statusDisplay"]


# ==============================================================================
# WR-UX-07: Deterministic Offline Badges, Long-Press Action Card, Whole-Book Cache (5 tests)
# ==============================================================================

def test_ux07_shelf_json_persists_and_preserves_chapter_counts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.sync_shelf_full()
        storage.update_shelf_chapter_counts("test_book_101", cached=2, total=5)
        # Re-sync from server (which doesn't send cachedChapters/totalChapters) must preserve local counts!
        merged = client.sync_shelf_full()
        book = next(b for b in merged["books"] if b["bookId"] == "test_book_101")
        assert book["cachedChapters"] == 2 and book["totalChapters"] == 5


def test_ux07_scheme_a_grid_badges_classification() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        books = [
            {"bookId": "b_none", "title": "未缓存", "cachedChapters": 0, "totalChapters": 10},
            {"bookId": "b_dot", "title": "部分缓存", "cachedChapters": 3, "totalChapters": 10},
            {"bookId": "b_full", "title": "整本离线", "cachedChapters": 10, "totalChapters": 10},
        ]
        shelf.load_snapshot(books, authenticated=True)
        ui = shelf.render()
        assert ui["badges"]["b_none"] == "NONE"
        assert ui["badges"]["b_dot"] == "DOT"
        assert ui["badges"]["b_full"] == "OFFLINE_PILL"
        assert ui["listCaptions"]["b_none"] == ""
        assert ui["listCaptions"]["b_dot"] == "已存 3/10 章"
        assert ui["listCaptions"]["b_full"] == "整本离线 · 10 章"


def test_ux07_visible_row_lazy_probe_bounded_and_zero_io_render() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        books = [{"bookId": f"b_{i}", "title": f"Book {i}", "cachedChapters": 0, "totalChapters": 0} for i in range(25)]
        shelf.load_snapshot(books, authenticated=True)
        probe_ids = shelf.visible_rows_needing_probe()
        assert len(probe_ids) == 9
        assert probe_ids == [f"b_{i}" for i in range(9)]
        io_before = storage.io_read_count
        ui = shelf.render()
        assert ui["screen"] == "SHELF"
        assert storage.io_read_count == io_before
        assert shelf.io_calls_during_render == 0


def test_ux07_long_press_action_card_three_actions() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        storage.save_shelf({"books": [{"bookId": "b_partial", "title": "部分书", "cachedChapters": 2, "totalChapters": 5, "progress": 40}]})
        storage.save_chapter_html("b_partial", 1, "<p>ch1</p>")
        storage.save_chapter_html("b_partial", 2, "<p>ch2</p>")
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(storage.load_shelf()["books"], authenticated=True)
        actions = shelf.long_press_book("b_partial")
        assert shelf.screen == "ACTION_CARD"
        assert actions == ["取消", "清理缓存", "缓存整本"]
        ui = shelf.render()
        assert ui["screen"] == "ACTION_CARD"
        card = ui["actionCard"]
        assert card["title"] == "部分书"
        assert card["cachedChapters"] == 2 and card["totalChapters"] == 5 and card["progress"] == 40
        assert shelf.tap_action_card("清理缓存") == "CLEANED"
        assert shelf.screen == "SHELF"
        assert storage.load_chapter_html("b_partial", 1) is None
        assert shelf.books[0]["cachedChapters"] == 0
        actions2 = shelf.long_press_book("b_partial")
        assert actions2 == ["取消", "缓存整本"]
        assert shelf.tap_action_card("取消") == "CANCELLED"
        assert shelf.screen == "SHELF"


def test_ux07_whole_book_cache_throttles_3s_and_preempted_by_open() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        books = [
            {"bookId": "b_multi", "title": "长篇小说", "cachedChapters": 0, "totalChapters": 4, "format": "epub", "bookVersion": 1},
            {"bookId": "b_other", "title": "另一本书", "cachedChapters": 1, "totalChapters": 1},
        ]
        client.mock_set_state({
            "books": books,
            "chapters": {"b_multi": [{"chapterUid": i, "chapterIdx": i, "title": f"第{i}章"} for i in range(1, 5)]},
        })
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=True)
        shelf.start_whole_book_cache("b_multi", now_ms=1000)
        assert shelf.active_job == "WR_JOB_CACHE_BOOK"
        # Download ch1 at 1500ms (< 3s): UI message throttled
        assert client.download_chapter("b_multi", 1, book_type="epub")["status"] == 200
        assert shelf.tick_whole_book_cache_progress(1, 4, now_ms=1500) is False
        # Download ch2 at 4100ms (>= 3s): UI message refreshes
        assert client.download_chapter("b_multi", 2, book_type="epub")["status"] == 200
        assert shelf.tick_whole_book_cache_progress(2, 4, now_ms=4100) is True
        assert "正在缓存《长篇小说》 2/4 章" in shelf.bottom_message
        # Tapping another book immediately preempts WR_JOB_CACHE_BOOK for WR_JOB_OPEN
        job = shelf.preempt_for_open("b_other")
        assert job == "WR_JOB_OPEN"
        assert shelf.cache_job_book_id is None


# ==============================================================================
# WR-UX-08: Title Dropdown Status Filter & Footer Page-Jump Slider (5 tests)
# ==============================================================================

def test_ux08_title_label_and_dropdown_hit_rect() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([{"bookId": "b1", "title": "书1", "progress": 20}], authenticated=True)
        assert shelf.title_label() == "微信读书 ▾"
        assert shelf.hit_dropdown(100, 100) is True and shelf.dropdown_open is True
        ui_open = shelf.render()
        items = ui_open["dropdownItems"]
        assert len(items) == 5
        assert [it["label"] for it in items] == ["全部书籍", "在读", "已离线", "未读", "已读完"]
        assert {it["mode"]: it["count"] for it in items} == {"ALL": 1, "READING": 1, "OFFLINE": 0, "UNREAD": 0, "FINISHED": 0}
        shelf.set_filter("READING")
        assert shelf.title_label() == "在读 ▾"
        assert shelf.dropdown_open is False
        assert "dropdownItems" not in shelf.render()


def test_ux08_five_status_filters_with_accurate_counts() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        books = [
            {"bookId": "b1", "title": "在读未离线", "progress": 30, "finished": 0, "cachedChapters": 1, "totalChapters": 5},
            {"bookId": "b2", "title": "已读完且离线", "progress": 100, "finished": 1, "cachedChapters": 5, "totalChapters": 5},
            {"bookId": "b3", "title": "未读且离线", "progress": 0, "finished": 0, "cachedChapters": 3, "totalChapters": 3},
            {"bookId": "b4", "title": "未读未缓存", "progress": 0, "finished": 0, "cachedChapters": 0, "totalChapters": 4},
        ]
        shelf.load_snapshot(books, authenticated=True)
        counts = shelf.filter_counts()
        assert counts == {"ALL": 4, "READING": 1, "OFFLINE": 2, "UNREAD": 2, "FINISHED": 1}
        shelf.set_filter("OFFLINE")
        assert [b["bookId"] for b in shelf.visible_books()] == ["b2", "b3"]
        shelf.set_filter("UNREAD")
        assert [b["bookId"] for b in shelf.visible_books()] == ["b3", "b4"]


def test_ux08_filter_combines_with_search_and_sort() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        books = [
            {"bookId": "b1", "title": "三体 I", "progress": 40, "finished": 0},
            {"bookId": "b2", "title": "三体 II", "progress": 100, "finished": 1},
            {"bookId": "b3", "title": "荒原", "progress": 20, "finished": 0},
        ]
        shelf.load_snapshot(books, authenticated=True)
        shelf.set_filter("READING")
        shelf.set_search_draft("st")
        shelf.commit_search()
        vis = shelf.visible_books()
        assert [b["bookId"] for b in vis] == ["b1"]


def test_ux08_footer_page_indicator_opens_slider_when_gt_3() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        books = [{"bookId": f"b_{i}", "title": f"Book {i}"} for i in range(30)]
        shelf.load_snapshot(books, authenticated=True)
        assert shelf.page_count() == 4
        assert shelf.tap_footer_page_indicator() is True
        assert shelf.screen == "SLIDER"
        assert shelf.slider_jump_to(2) == 2 and shelf.s_page == 2 and shelf.screen == "SHELF"


def test_ux08_footer_page_indicator_noop_when_le_3() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        books = [{"bookId": f"b_{i}", "title": f"Book {i}"} for i in range(27)]
        shelf.load_snapshot(books, authenticated=True)
        assert shelf.page_count() == 3
        assert shelf.tap_footer_page_indicator() is False
        assert shelf.screen == "SHELF"


# ==============================================================================
# WR-UX-09: '更多 -> 缓存管理' Stats & Selective Chapter Cleanup (5 tests)
# ==============================================================================

def test_ux09_more_menu_has_cache_management_4th_item() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([{"bookId": "b1", "title": "Book 1"}], authenticated=True)
        items = shelf.more_menu_items()
        assert len(items) == 4 and items[3] == "缓存管理"
        stats = shelf.open_cache_management(free_bytes=256 * 1024 * 1024)
        assert shelf.active_job == "WR_JOB_CACHE_STATS"
        assert shelf.screen == "CACHE_MGMT"
        assert stats["reserve_mb"] == 104


def test_ux09_cache_stats_reports_books_mb_free_and_104mb_note() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.download_chapter("test_book_101", 1, book_type="epub")
        storage.save_chapter_html("txt_book_202", 1, "<p>" + ("x" * 1024 * 1024) + "</p>")
        stats = storage.compute_cache_stats(free_bytes=300 * 1024 * 1024)
        assert stats["cached_books"] == 2
        assert stats["total_mb"] >= 1.0
        assert stats["free_mb"] == 300.0
        assert stats["reserve_mb"] == 104


def test_ux09_clean_finished_books_preserves_covers_and_progress() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        shelf_data = client.sync_shelf_full()
        for b in shelf_data["books"]:
            if b["bookId"] == "txt_book_202":
                b["finished"] = 1
                b["progress"] = 100
                b["cachedChapters"] = 1
                b["totalChapters"] = 1
            else:
                b["cachedChapters"] = 1
                b["totalChapters"] = 2
        storage.save_shelf(shelf_data)
        client.download_chapter("test_book_101", 1, book_type="epub")
        client.download_chapter("txt_book_202", 1, book_type="txt")
        storage.save_cover_gray("txt_book_202", b"GRAY_FINISHED")
        storage.save_cover_img("txt_book_202", b"IMG_RAW")
        storage.save_chapter_json("txt_book_202", 1, {"uid": 1})
        storage.save_resources_manifest("txt_book_202", {"res": []})
        storage.save_local_progress("txt_book_202", {"progress": 100, "dirty": 0})
        storage.save_progress_backup("txt_book_202", {"progress": 100})

        cleaned = storage.clean_finished_books()
        assert cleaned == ["txt_book_202"]
        # Finished book chapters/res/legacy removed
        assert storage.load_chapter_html("txt_book_202", 1) is None
        assert not os.path.exists(os.path.join(storage.book_cache_dir("txt_book_202"), "cover-txt_book_202.img"))
        assert not os.path.exists(os.path.join(storage.book_cache_dir("txt_book_202"), "chapter-1.json"))
        assert not os.path.exists(os.path.join(storage.book_cache_dir("txt_book_202"), "resources.json"))
        # Preserved: auth.json, shelf.json, progress.json*, cover-*.gray, unfinished book cache
        assert os.path.exists(storage.auth_file)
        assert os.path.exists(storage.shelf_file)
        assert storage.load_cover_gray("txt_book_202") == b"GRAY_FINISHED"
        assert storage.load_local_progress("txt_book_202")["progress"] == 100
        assert os.path.exists(os.path.join(storage.book_cache_dir("txt_book_202"), "progress.json.backup"))
        assert storage.load_chapter_html("test_book_101", 1) is not None


def test_ux09_clean_all_offline_chapters_preserves_metadata() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.sync_shelf_full()
        client.download_chapter("test_book_101", 1, book_type="epub")
        client.download_chapter("txt_book_202", 1, book_type="txt")
        storage.save_cover_gray("test_book_101", b"GRAY_101")
        storage.save_cover_gray("txt_book_202", b"GRAY_202")
        storage.save_local_progress("test_book_101", {"progress": 42, "dirty": 1})

        cleaned = storage.clean_all_offline_chapters()
        assert set(cleaned) >= {"test_book_101", "txt_book_202"}
        assert storage.load_chapter_html("test_book_101", 1) is None
        assert storage.load_chapter_html("txt_book_202", 1) is None
        assert os.path.exists(storage.auth_file)
        assert os.path.exists(storage.shelf_file)
        assert storage.load_cover_gray("test_book_101") == b"GRAY_101"
        assert storage.load_cover_gray("txt_book_202") == b"GRAY_202"
        assert storage.load_local_progress("test_book_101")["progress"] == 42


def test_ux09_return_to_shelf_after_clean_zero_cover_redownload() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        shelf_data = client.sync_shelf_full()
        # Simulate fetching covers once before cleanup
        status, _, jpeg = client.request("GET", "/covers/s_test_book_101.jpg")
        assert status == 200 and jpeg.startswith(b"\xff\xd8\xff")
        storage.save_cover_gray("test_book_101", jpeg)
        shelf_data["books"][0]["finished"] = 1
        storage.save_shelf(shelf_data)
        client.download_chapter("test_book_101", 1, book_type="epub")

        # Clear mock history and perform finished cleanup + return to shelf
        client._raw_request("POST", "/mock/control/clear_history")
        storage.clean_finished_books()
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(storage.load_shelf()["books"], authenticated=True)
        # Render checks RAM/disk cover-*.gray without issuing HTTP /covers/* requests
        assert storage.has_cover_gray("test_book_101") is True
        shelf.render()
        cover_reqs = [h for h in client.mock_get_history() if h["path"].startswith("/covers/")]
        assert cover_reqs == []


ALL_TESTS = [
    test_ux06_rf_idle_threshold_remains_600000ms,
    test_ux06_real_http_activity_refreshes_network_active,
    test_ux06_8s_pause_silent_report_with_15s_spacing,
    test_ux06_normal_exit_fires_nonblocking_note,
    test_ux06_home_continue_card_shows_pending_badge,
    test_ux07_shelf_json_persists_and_preserves_chapter_counts,
    test_ux07_scheme_a_grid_badges_classification,
    test_ux07_visible_row_lazy_probe_bounded_and_zero_io_render,
    test_ux07_long_press_action_card_three_actions,
    test_ux07_whole_book_cache_throttles_3s_and_preempted_by_open,
    test_ux08_title_label_and_dropdown_hit_rect,
    test_ux08_five_status_filters_with_accurate_counts,
    test_ux08_filter_combines_with_search_and_sort,
    test_ux08_footer_page_indicator_opens_slider_when_gt_3,
    test_ux08_footer_page_indicator_noop_when_le_3,
    test_ux09_more_menu_has_cache_management_4th_item,
    test_ux09_cache_stats_reports_books_mb_free_and_104mb_note,
    test_ux09_clean_finished_books_preserves_covers_and_progress,
    test_ux09_clean_all_offline_chapters_preserves_metadata,
    test_ux09_return_to_shelf_after_clean_zero_cover_redownload,
]


def run_all_tests() -> None:
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 1 WR-UX P1 (WR-UX-06..09) Suite: {len(ALL_TESTS)} tests ===")
    for fn in ALL_TESTS:
        try:
            fn()
            print(f"  [OK] {fn.__name__}")
            passed += 1
        except Exception as exc:
            print(f"  [FAIL] {fn.__name__}: {exc}", file=sys.stderr)
            failed += 1
    print(f"=== Result: {passed} passed, {failed} failed (total {len(ALL_TESTS)}) ===")
    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    run_all_tests()
