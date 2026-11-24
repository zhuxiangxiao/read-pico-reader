#!/usr/bin/env python3
"""Tier 2 Boundary & Corner Cases: WeRead P1 UX Optimizations (WR-UX-06 .. WR-UX-09).

20 boundary/error tests (5 per feature).
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
from weread_client_sim import WereadClient, WereadShelfModel, WereadStorage, book_file_path_supported

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
    client.login_device_exchange("12345678", "mock_skey_123456", "mock_qr_code_xyz123", "eink-ux-b1")
    return storage, client


# ==============================================================================
# WR-UX-06 Boundaries (5 tests)
# ==============================================================================

def test_ux06_b1_8s_pause_never_wakes_offline_or_sleeping_wifi() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        # Case 1: Wi-Fi powered down (wifi_enabled=False)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 20, 60, dirty=1)
        client.note_page_move(1000)
        hist_before = len(client.mock_get_history())
        assert client.tick_reader_pause("test_book_101", 20000) is False
        assert client.owns_network is False
        hist_after = [h for h in client.mock_get_history()[hist_before:] if not h["path"].startswith("/mock/control")]
        assert hist_after == []

        # Case 2: Wi-Fi radio idle-disconnected (wifi_enabled=True, owns_network=False after 600000ms)
        client.connect_network(now_ms=1000)
        assert client.check_rf_idle_disconnect(601000) is True
        assert client.wifi_enabled is True and client.owns_network is False
        client.note_page_move(602000)
        hist_before2 = len(client.mock_get_history())
        assert client.tick_reader_pause("test_book_101", 612000) is False
        hist_after2 = [h for h in client.mock_get_history()[hist_before2:] if not h["path"].startswith("/mock/control")]
        assert hist_after2 == []


def test_ux06_b2_pause_under_8s_or_interval_under_15s_suppressed() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 10, 30, dirty=1)
        client.connect_network(now_ms=1000)
        client.note_page_move(1000)
        assert client.tick_reader_pause("test_book_101", 8999) is False
        assert client.tick_reader_pause("test_book_101", 9000) is True
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 15, 60, dirty=1)
        client.connect_network(now_ms=10000)
        client.note_page_move(10000)
        # 18s >= 8s since move, but 9s < 15s since report at 9000ms
        assert client.tick_reader_pause("test_book_101", 23999) is False
        assert client.tick_reader_pause("test_book_101", 24000) is True


def test_ux06_b3_exit_note_skipped_when_worker_busy_or_clean() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 10, 30, dirty=1)
        client.connect_network(now_ms=1000)
        # Worker busy -> skipped cleanly without blocking
        assert client.end_reader_session("test_book_101", now_ms=2000, busy=True) is False
        # Clean progress (dirty == 0) -> skipped
        client.storage.save_local_progress("test_book_101", {"bookId": "test_book_101", "progress": 10, "dirty": 0})
        assert client.end_reader_session("test_book_101", now_ms=3000, busy=False) is False


def test_ux06_b4_http_activity_at_599s_prevents_600s_disconnect() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.connect_network(now_ms=1000)
        client.now_ms = 600000  # 599s after 1000ms
        client.sync_shelf_full()
        assert client.last_network_activity_ms == 600000
        assert client.check_rf_idle_disconnect(1199999) is False
        assert client.owns_network is True
        assert client.check_rf_idle_disconnect(1200000) is True


def test_ux06_b5_home_card_non_weread_book_never_shows_pending() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        for local_book in ("/sdcard/Books/local.epub", "/sdcard/Books/local.txt", "/sdcard/Books/extra.fb2"):
            card = client.render_home_continue_card(local_book)
            assert card["isWeread"] is False
            assert card["pending"] is False
            assert "待同步" not in card["statusDisplay"]
        assert book_file_path_supported("/sdcard/Books/local.epub") is True
        assert book_file_path_supported("/sdcard/Books/local.txt") is True


# ==============================================================================
# WR-UX-07 Boundaries (5 tests)
# ==============================================================================

def test_ux07_b1_legacy_shelf_json_missing_counts_defaults_zero() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        storage.save_shelf({"books": [{"bookId": "legacy_1", "title": "旧版书架"}]})
        loaded = storage.load_shelf()
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(loaded["books"], authenticated=True)
        assert shelf.classify_badge(loaded["books"][0]) == "NONE"
        assert shelf.visible_rows_needing_probe() == ["legacy_1"]


def test_ux07_b2_whole_book_cache_stops_below_104mib_floor() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        floor_bytes = 104 * 1024 * 1024
        books = [{"bookId": "b_floor", "title": "空间阈值测试", "format": "epub", "bookVersion": 1, "cachedChapters": 0, "totalChapters": 3}]
        storage.save_shelf({"books": books})
        client.mock_set_state({
            "books": books,
            "chapters": {"b_floor": [{"chapterUid": i, "chapterIdx": i, "title": f"第{i}章"} for i in range(1, 4)]},
        })
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=True)
        # First check returns exact 104 MiB (allows ch1), second returns 104 MiB - 4096 (aborts before ch2)
        free_seq = iter([floor_bytes, floor_bytes - 4096])
        client._raw_request("POST", "/mock/control/clear_history")
        res = shelf.cache_whole_book(client, "b_floor", [1, 2, 3], free_bytes_provider=lambda: next(free_seq, floor_bytes - 4096))
        assert res["storage_blocked"] is True
        assert res["cached"] == 1 and res["stopped_at"] == 2
        assert shelf.storage_blocked is True
        assert storage.load_chapter_html("b_floor", 1) is not None
        assert storage.load_chapter_html("b_floor", 2) is None
        dl_reqs = [h for h in client.mock_get_history() if h["path"] == "/book/chapterdownload"]
        assert len(dl_reqs) == 1
        stats_low = storage.compute_cache_stats(free_bytes=103 * 1024 * 1024)
        assert stats_low["free_mb"] < stats_low["reserve_mb"]


def test_ux07_b3_whole_book_cache_skips_already_cached_and_handles_402() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        books = [{"bookId": "b_pay", "title": "付费书", "format": "epub", "bookVersion": 1}]
        client.mock_set_state({
            "books": books,
            "chapters": {"b_pay": [{"chapterUid": i, "chapterIdx": i, "title": f"第{i}章"} for i in range(1, 4)]},
            "chapter_errors": {"b_pay:3": {"status": 402}},
        })
        assert client.download_chapter("b_pay", 1)["status"] == 200
        client._raw_request("POST", "/mock/control/clear_history")
        # Whole-book loop skips already-cached ch1, fetches ch2, stops cleanly on ch3 402
        cached_count = 1
        for uid in (1, 2, 3):
            if storage.load_chapter_html("b_pay", uid) is not None:
                continue
            res = client.download_chapter("b_pay", uid)
            if res["status"] != 200:
                break
            cached_count += 1
        assert cached_count == 2
        dl_reqs = [h for h in client.mock_get_history() if h["path"] == "/book/chapterdownload"]
        # ch1 was skipped! Only ch2 and ch3 were requested
        assert len(dl_reqs) == 2


def test_ux07_b4_struct_size_under_512_bytes() -> None:
    # Verify C header struct weread_shelf_item_t stays bounded (<512B)
    shelf_h = os.path.join(os.path.dirname(TOOLS_DIR), "main", "weread", "weread_shelf.h")
    with open(shelf_h, "r", encoding="utf-8") as f:
        src = f.read()
    assert "weread_shelf_item_t" in src
    # Check array sizes: book_id[64] + title[256] + author[128] = 448 + scalars (<512)
    assert "book_id[64]" in src and "title[256]" in src and "author[128]" in src


def test_ux07_b5_action_card_hides_invalid_actions() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([
            {"bookId": "full", "title": "全离线", "cachedChapters": 5, "totalChapters": 5},
            {"bookId": "none", "title": "未缓存", "cachedChapters": 0, "totalChapters": 5},
        ], authenticated=True)
        assert shelf.long_press_book("full") == ["取消", "清理缓存"]
        assert shelf.long_press_book("none") == ["取消", "缓存整本"]


# ==============================================================================
# WR-UX-08 Boundaries (5 tests)
# ==============================================================================

def test_ux08_b1_filter_empty_category_shows_clean_empty_state() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([{"bookId": "b1", "title": "在读", "progress": 10, "finished": 0}], authenticated=True)
        shelf.set_filter("FINISHED")
        assert shelf.visible_books() == []
        assert shelf.page_count() == 1


def test_ux08_b2_dropdown_hit_boundary_exact_coordinates() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        # Rect: (24, 79, 260, 74) -> x in [24, 284), y in [79, 153)
        assert shelf.hit_dropdown(24, 79) is True
        assert shelf.hit_dropdown(283, 152) is True
        assert shelf.hit_dropdown(23, 79) is False
        assert shelf.hit_dropdown(24, 78) is False
        assert shelf.hit_dropdown(284, 79) is False
        assert shelf.hit_dropdown(24, 153) is False


def test_ux08_b3_slider_boundary_page_count_3_vs_4() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        shelf.load_snapshot([{"bookId": f"b_{i}", "title": f"Book {i}"} for i in range(27)], authenticated=True)
        assert shelf.page_count() == 3 and shelf.tap_footer_page_indicator() is False
        shelf.load_snapshot([{"bookId": f"b_{i}", "title": f"Book {i}"} for i in range(28)], authenticated=True)
        assert shelf.page_count() == 4 and shelf.tap_footer_page_indicator() is True
        assert shelf.slider_jump_to(99) == 3
        assert shelf.slider_jump_to(-5) == 0


def test_ux08_b4_filter_resets_page_to_zero_avoids_oob() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        books = [{"bookId": f"b_{i}", "title": f"Book {i}", "progress": 50 if i < 2 else 0} for i in range(50)]
        shelf.load_snapshot(books, authenticated=True)
        shelf.s_page = 5
        shelf.set_filter("READING")
        assert shelf.s_page == 0 and len(shelf.visible_books()) == 2


def test_ux08_b5_status_filter_classification_mutual_boundaries() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        books = [
            {"bookId": "u0", "title": "U", "progress": 0, "finished": 0, "cachedChapters": 0, "totalChapters": 5},
            {"bookId": "r50", "title": "R", "progress": 50, "finished": 0, "cachedChapters": 2, "totalChapters": 5},
            {"bookId": "f100", "title": "F", "progress": 100, "finished": 0, "cachedChapters": 5, "totalChapters": 5},
        ]
        shelf.load_snapshot(books, authenticated=True)
        counts = shelf.filter_counts()
        assert counts == {"ALL": 3, "READING": 1, "OFFLINE": 1, "UNREAD": 1, "FINISHED": 1}


# ==============================================================================
# WR-UX-09 Boundaries (5 tests)
# ==============================================================================

def test_ux09_b1_clean_finished_when_zero_finished_books_noop() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.sync_shelf_full()
        client.download_chapter("test_book_101", 1, book_type="epub")
        assert storage.clean_finished_books() == []
        assert storage.load_chapter_html("test_book_101", 1) is not None


def test_ux09_b2_clean_preserves_legacy_and_cover_gray_files() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        shelf_data = client.sync_shelf_full()
        shelf_data["books"][0]["finished"] = 1
        storage.save_shelf(shelf_data)
        storage.save_cover_gray("test_book_101", b"GRAY_KEEP")
        storage.save_cover_img("test_book_101", b"IMG_DROP")
        storage.save_chapter_json("test_book_101", 1, {"u": 1})
        storage.save_resources_manifest("test_book_101", {"r": 1})
        storage.save_local_progress("test_book_101", {"progress": 100})
        storage.save_progress_backup("test_book_101", {"progress": 100})
        storage.clean_finished_books()
        bdir = storage.book_cache_dir("test_book_101")
        assert os.path.isfile(os.path.join(bdir, "cover-test_book_101.gray"))
        assert os.path.isfile(os.path.join(bdir, "progress.json"))
        assert os.path.isfile(os.path.join(bdir, "progress.json.backup"))
        assert not os.path.exists(os.path.join(bdir, "cover-test_book_101.img"))
        assert not os.path.exists(os.path.join(bdir, "chapter-1.json"))
        assert not os.path.exists(os.path.join(bdir, "resources.json"))


def test_ux09_b3_clean_while_zero_cache_reports_0mb() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        stats = storage.compute_cache_stats(free_bytes=128 * 1024 * 1024)
        assert stats["cached_books"] == 0
        assert stats["total_bytes"] == 0
        assert stats["total_mb"] == 0.0
        assert stats["free_mb"] == 128.0
        assert stats["reserve_mb"] == 104


def test_ux09_b4_clean_refreshes_shelf_counts_to_zero() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        shelf_data = client.sync_shelf_full()
        shelf_data["books"][0]["finished"] = 1
        shelf_data["books"][0]["cachedChapters"] = 5
        shelf_data["books"][0]["totalChapters"] = 5
        storage.save_shelf(shelf_data)
        storage.clean_finished_books()
        updated = storage.load_shelf()
        b0 = next(b for b in updated["books"] if b["bookId"] == shelf_data["books"][0]["bookId"])
        assert b0["cachedChapters"] == 0
        assert WereadShelfModel.classify_badge(b0) == "NONE"


def test_ux09_b5_non_goals_absent_in_source_and_menus() -> None:
    import glob
    import re
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        menu = shelf.more_menu_items()
        for forbidden in ("划线", "想法", "听书", "买书", "WR-UX-10", "TTS"):
            assert all(forbidden not in item for item in menu)
    repo_root = os.path.dirname(TOOLS_DIR)
    source_files = (
        glob.glob(os.path.join(repo_root, "main", "weread", "*.[ch]"))
        + [os.path.join(repo_root, "main", "apps", "app_weread.c")]
        + glob.glob(os.path.join(repo_root, "main", "apps", "book_weread_*.inc"))
        + [os.path.join(repo_root, "main", "book", "book_source_weread.c")]
    )
    forbidden_re = re.compile(r"\b(WR_UX_10|WR_JOB_HIGHLIGHT|lru_evict|evict_lru)\b|/book/bookmark|/review/")
    for path in source_files:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            txt = f.read()
        m = forbidden_re.search(txt)
        assert m is None, f"Forbidden Non-Goal symbol {m.group(0)} found in {path}"


ALL_TESTS = [
    test_ux06_b1_8s_pause_never_wakes_offline_or_sleeping_wifi,
    test_ux06_b2_pause_under_8s_or_interval_under_15s_suppressed,
    test_ux06_b3_exit_note_skipped_when_worker_busy_or_clean,
    test_ux06_b4_http_activity_at_599s_prevents_600s_disconnect,
    test_ux06_b5_home_card_non_weread_book_never_shows_pending,
    test_ux07_b1_legacy_shelf_json_missing_counts_defaults_zero,
    test_ux07_b2_whole_book_cache_stops_below_104mib_floor,
    test_ux07_b3_whole_book_cache_skips_already_cached_and_handles_402,
    test_ux07_b4_struct_size_under_512_bytes,
    test_ux07_b5_action_card_hides_invalid_actions,
    test_ux08_b1_filter_empty_category_shows_clean_empty_state,
    test_ux08_b2_dropdown_hit_boundary_exact_coordinates,
    test_ux08_b3_slider_boundary_page_count_3_vs_4,
    test_ux08_b4_filter_resets_page_to_zero_avoids_oob,
    test_ux08_b5_status_filter_classification_mutual_boundaries,
    test_ux09_b1_clean_finished_when_zero_finished_books_noop,
    test_ux09_b2_clean_preserves_legacy_and_cover_gray_files,
    test_ux09_b3_clean_while_zero_cache_reports_0mb,
    test_ux09_b4_clean_refreshes_shelf_counts_to_zero,
    test_ux09_b5_non_goals_absent_in_source_and_menus,
]


def run_all_tests() -> None:
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 2 WR-UX P1 Boundaries Suite: {len(ALL_TESTS)} tests ===")
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
