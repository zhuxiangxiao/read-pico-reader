#!/usr/bin/env python3
"""Tier 3: Cross-Feature Combination Test Suite for WeRead WR-UX-01..09.

Covers 12 pairwise feature interaction combinations:
1. WR-UX-01 × WR-UX-07: Expired -2013 session × Scheme A offline badges & open
2. WR-UX-01 × WR-UX-09: Expired -2013 session × Cache Management cleanup
3. WR-UX-02 × WR-UX-05: Cold-jump cancel + 402 retry × Footer/Toolbar 6 dynamic state
4. WR-UX-02 × WR-UX-07: Whole-book cache completion × Reader/Shelf count sync
5. WR-UX-03 × WR-UX-08: Shelf SWIPE_L/R paging × Status filter + Pinyin search + slider
6. WR-UX-04 × WR-UX-05: Late cloud-jump opt-in × Cold chapter download + cancel
7. WR-UX-04 × WR-UX-06: Auto cloud jump + undo × 8s pause silent WR_JOB_NOTE
8. WR-UX-05 × WR-UX-07: -2063 version self-heal × cached_chapters/total_chapters update
9. WR-UX-06 × WR-UX-02: 8s pause WR_JOB_NOTE completing × Footer & Home pending badge clear
10. WR-UX-07 × WR-UX-09: Whole-book cache preemption × Cache Management cleanup
11. WR-UX-08 × WR-UX-09: Shelf filtered to 已读完 × [清理已读完书籍] -> return to shelf
12. WR-UX-03 × WR-UX-01: Home s_requested[0] direct open × expired: true session
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
from weread_client_sim import WereadClient, WereadReaderModel, WereadShelfModel, WereadStorage

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
    client.login_device_exchange("12345678", "mock_skey_123456", "mock_qr_code_xyz123", "eink-t3-ux")
    return storage, client


def test_comb1_expired_session_x_scheme_a_badges() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        assert client.download_chapter("test_book_101", 1, book_type="epub")["status"] == 200
        client.handle_terminal_2013()
        client.set_wifi_enabled(False)
        books = [
            {"bookId": "test_book_101", "title": "全离线书", "cachedChapters": 2, "totalChapters": 2},
            {"bookId": "txt_book_202", "title": "未缓存书", "cachedChapters": 0, "totalChapters": 1},
        ]
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=False, expired=True)
        ui = shelf.render()
        assert ui["badges"]["test_book_101"] == "OFFLINE_PILL"
        assert ui["badges"]["txt_book_202"] == "NONE"
        assert shelf.preempt_for_open("test_book_101") == "WR_JOB_OPEN"
        assert shelf.preempt_for_open("txt_book_202") == "QR_LOGIN_PROMPT"


def test_comb2_expired_session_x_cache_cleanup() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        shelf_data = client.sync_shelf_full()
        for b in shelf_data["books"]:
            if b["bookId"] == "txt_book_202":
                b["finished"] = 1
        storage.save_shelf(shelf_data)
        client.download_chapter("test_book_101", 1, book_type="epub")
        client.download_chapter("txt_book_202", 1, book_type="txt")
        storage.save_cover_gray("txt_book_202", b"GRAY_KEEP")
        storage.save_local_progress("test_book_101", {"progress": 50, "dirty": 1})
        client.handle_terminal_2013()
        cleaned = storage.clean_finished_books()
        assert cleaned == ["txt_book_202"]
        assert storage.is_auth_expired() is True and storage.owner_matches("12345678")
        assert storage.load_cover_gray("txt_book_202") == b"GRAY_KEEP"
        assert storage.load_local_progress("test_book_101")["progress"] == 50


def test_comb3_cold_jump_cancel_and_402_x_footer_toolbar6() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.mock_set_state({"chapter_errors": {"test_book_101:2": {"status": 402}}})
        reader = WereadReaderModel(current_chapter=1, current_offset=0)
        reader.start_cold_jump(target_chapter=5)
        assert reader.wr_chapter_cancel() is True
        res = reader.forward_turn_cold(client, "test_book_101", next_uid=2, now_ms=1000)
        assert res["status"] == 402
        assert WereadReaderModel.footer_should_wake("WR_PROGRESS_SYNCED", "WR_PROGRESS_SYNCED", cache_delta=True) is False
        assert WereadReaderModel.toolbar_button6_label(1, 5, stalled=True) == "重试缓存"


def test_comb4_whole_book_cache_completion_x_reader_shelf_sync() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        books = [{"bookId": "test_book_101", "title": "荒原起源", "cachedChapters": 0, "totalChapters": 2, "format": "epub", "bookVersion": 100}]
        storage.save_shelf({"books": books})
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=True)
        shelf.start_whole_book_cache("test_book_101", now_ms=1000)
        assert client.download_chapter("test_book_101", 1, book_type="epub")["status"] == 200
        assert client.download_chapter("test_book_101", 2, book_type="epub")["status"] == 200
        shelf.tick_whole_book_cache_progress(2, 2, now_ms=4500)
        assert shelf.render()["badges"]["test_book_101"] == "OFFLINE_PILL"
        assert WereadReaderModel.toolbar_button6_label(2, 2) == "已全存"
        assert WereadReaderModel.footer_cache_prefix(2, 2) == ""


def test_comb5_swipe_paging_x_filter_search_slider() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        books = [
            {"bookId": f"r_{i}", "title": f"三体 {i}" if i < 30 else f"普通书 {i}", "progress": 20 if i < 32 else 0, "finished": 0}
            for i in range(40)
        ]
        shelf.load_snapshot(books, authenticated=True)
        assert shelf.page_count() == 5
        assert shelf.can_open_page_slider() is True
        shelf.set_filter("READING")
        assert shelf.page_count() == 4
        assert shelf.swipe("L") is True and shelf.s_page == 1
        shelf.set_search_draft("st")
        shelf.commit_search()
        # Now 30 matching books -> 4 pages -> slider stays enabled, or filter narrower
        assert shelf.page_count() == 4
        assert shelf.tap_footer_page_indicator() is True


def test_comb6_opt_in_cloud_jump_x_cold_download_cancel() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        reader = WereadReaderModel(current_chapter=1, current_offset=0)
        reader.turn_page(new_offset=100, now_ms=1000)
        assert reader.on_cloud_jump_arrival(target_chapter=5, target_offset=0, now_ms=1200) == "REBASED_OPT_IN"
        reader.start_cold_jump(target_chapter=5)
        assert reader.wr_chapter_cancel() is True
        assert (reader.current_chapter, reader.current_offset) == (1, 100)
        assert reader.undo_banner_active is True


def test_comb7_auto_cloud_jump_undo_x_8s_pause_note() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        reader = WereadReaderModel(current_chapter=1, current_offset=80)
        assert reader.on_cloud_jump_arrival(target_chapter=2, target_offset=0, now_ms=1000) == "AUTO_JUMPED"
        assert reader.tap_banner() == "RESTORED_PREV"
        assert (reader.current_chapter, reader.current_offset) == (1, 80)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 12, 60, dirty=1)
        client.connect_network(now_ms=2000)
        client.note_page_move(2000)
        assert client.tick_reader_pause("test_book_101", 10000) is True
        assert storage.load_local_progress("test_book_101")["dirty"] == 0


def test_comb8_version_self_heal_x_chapter_counts_update() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.sync_shelf_full()
        storage.update_shelf_chapter_counts("test_book_101", cached=2, total=2)
        client.mock_set_state({
            "enforce_book_version": True,
            "book_versions": {"test_book_101": 102},
            "chapters": {"test_book_101": [{"chapterUid": i, "chapterIdx": i, "title": f"Ch{i}"} for i in range(1, 5)]},
        })
        reader = WereadReaderModel()
        res = reader.prepare_open(client, "test_book_101", chapter_uid=1, book_type="epub", book_version=100)
        assert res["status"] == 200 and reader.heal_attempts == 1
        toc = storage.load_toc("test_book_101")
        storage.update_shelf_chapter_counts("test_book_101", cached=1, total=len(toc["updated"]))
        b = next(x for x in storage.load_shelf()["books"] if x["bookId"] == "test_book_101")
        assert b["cachedChapters"] == 1 and b["totalChapters"] == 4


def test_comb9_8s_pause_sync_x_footer_and_home_badge_clear() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 55, 200, dirty=1)
        assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_PENDING") == "待同步 · "
        assert client.render_home_continue_card("weread://test_book_101")["pendingBadge"] == "· 待同步"
        client.connect_network(now_ms=1000)
        client.note_page_move(1000)
        assert client.tick_reader_pause("test_book_101", 9000) is True
        assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_SYNCED") == ""
        assert client.render_home_continue_card("weread://test_book_101")["pendingBadge"] == ""


def test_comb10_whole_book_cache_preempt_x_cleanup() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        books = [
            {"bookId": "test_book_101", "title": "A", "cachedChapters": 1, "totalChapters": 3, "finished": 0},
            {"bookId": "txt_book_202", "title": "B", "cachedChapters": 1, "totalChapters": 1, "finished": 1},
        ]
        storage.save_shelf({"books": books})
        client.download_chapter("txt_book_202", 1, book_type="txt")
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=True)
        shelf.start_whole_book_cache("test_book_101", now_ms=1000)
        assert shelf.preempt_for_open("test_book_101") == "WR_JOB_OPEN"
        assert shelf.active_job == "WR_JOB_OPEN"
        cleaned = storage.clean_finished_books()
        assert cleaned == ["txt_book_202"]


def test_comb11_filtered_finished_x_clean_finished_return_to_shelf() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        books = [
            {"bookId": "fin_1", "title": "已读完1", "progress": 100, "finished": 1, "cachedChapters": 2, "totalChapters": 2},
            {"bookId": "rd_1", "title": "在读1", "progress": 30, "finished": 0, "cachedChapters": 1, "totalChapters": 2},
        ]
        storage.save_shelf({"books": books})
        storage.save_chapter_html("fin_1", 1, "<p>fin</p>")
        storage.save_cover_gray("fin_1", b"GRAY_FIN")
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=True)
        shelf.set_filter("FINISHED")
        assert shelf.filter_counts()["OFFLINE"] == 1
        storage.clean_finished_books()
        # Reload snapshot after cleanup while keeping filter
        shelf.books = storage.load_shelf()["books"]
        shelf.filter_page(reset_page=False)
        assert [b["bookId"] for b in shelf.visible_books()] == ["fin_1"]
        assert shelf.visible_books()[0]["cachedChapters"] == 0
        assert shelf.filter_counts()["OFFLINE"] == 0
        assert storage.has_cover_gray("fin_1") is True


def test_comb12_direct_open_x_expired_session() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.handle_terminal_2013()
        books = [
            {"bookId": "cached_book", "title": "Cached", "cachedChapters": 1, "totalChapters": 1},
            {"bookId": "uncached_book", "title": "Uncached", "cachedChapters": 0, "totalChapters": 1},
        ]
        shelf_cached = WereadShelfModel(storage)
        shelf_cached.requested_book_id = "cached_book"
        shelf_cached.load_snapshot(books, authenticated=False, expired=True)
        ui1 = shelf_cached.render()
        assert ui1["screen"] == "DIRECT_OPEN" and ui1["gridFlashed"] is False

        shelf_uncached = WereadShelfModel(storage)
        shelf_uncached.requested_book_id = "uncached_book"
        shelf_uncached.load_snapshot(books, authenticated=False, expired=True)
        ui2 = shelf_uncached.render()
        assert ui2["screen"] == "QR_LOGIN_PROMPT" and ui2["backToShelf"] is True


ALL_TESTS = [
    test_comb1_expired_session_x_scheme_a_badges,
    test_comb2_expired_session_x_cache_cleanup,
    test_comb3_cold_jump_cancel_and_402_x_footer_toolbar6,
    test_comb4_whole_book_cache_completion_x_reader_shelf_sync,
    test_comb5_swipe_paging_x_filter_search_slider,
    test_comb6_opt_in_cloud_jump_x_cold_download_cancel,
    test_comb7_auto_cloud_jump_undo_x_8s_pause_note,
    test_comb8_version_self_heal_x_chapter_counts_update,
    test_comb9_8s_pause_sync_x_footer_and_home_badge_clear,
    test_comb10_whole_book_cache_preempt_x_cleanup,
    test_comb11_filtered_finished_x_clean_finished_return_to_shelf,
    test_comb12_direct_open_x_expired_session,
]


def run_all_tests() -> None:
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 3 WR-UX Combinations Suite: {len(ALL_TESTS)} tests ===")
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
