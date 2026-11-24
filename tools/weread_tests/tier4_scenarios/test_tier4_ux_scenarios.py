#!/usr/bin/env python3
"""Tier 4: Real-World Application Scenarios for WeRead WR-UX-01..09.

6 realistic end-to-end workflows:
1. Token Expiry on Subway Commute (WR-UX-01 + WR-UX-06 + WR-UX-07)
2. Seamless Phone-to-E-Ink Handoff & No-Interrupt Guard (WR-UX-04 + WR-UX-02 + WR-UX-03 + WR-UX-06)
3. Paywall, Offline Boundary & Version Self-Heal Recovery (WR-UX-05 + WR-UX-02)
4. Large Library Triage, Pinyin Search, Whole-Book Cache & Preemption (WR-UX-07 + WR-UX-08 + WR-UX-03)
5. Storage Pressure & Safe Cache Reclamation (WR-UX-09 + WR-UX-07 + WR-UX-02)
6. Explicit Logout vs Passive Expiry & Zero-Regression Local Check (WR-UX-01 + WR-UX-06 + Invariants)
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
from weread_client_sim import WereadClient, WereadReaderModel, WereadShelfModel, WereadStorage, book_file_path_supported

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


def _setup_client(tmp_dir: str) -> tuple[WereadStorage, WereadClient]:
    storage = WereadStorage(tmp_dir)
    client = WereadClient(PORT, storage)
    client.mock_reset()
    client.login_device_exchange("12345678", "mock_skey_123456", "mock_qr_code_xyz123", "eink-t4-ux")
    return storage, client


def test_scenario1_subway_token_expiry_offline_read_and_relogin() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _setup_client(tmp)
        client.mark_expired_on_2013 = True
        assert client.download_chapter("test_book_101", 1, book_type="epub")["status"] == 200
        assert client.download_chapter("test_book_101", 2, book_type="epub")["status"] == 200
        # Passive -2013 on renewal
        client.mock_set_state({
            "endpoint_errors": {"/shelf/sync": {"status": 401, "body": {"errcode": -2013}}},
            "renew_error": {"status": 401, "body": {"errcode": -2013}},
        })
        status, _, _ = client.request("GET", "/shelf/sync?synckey=0")
        assert status == 401 and storage.is_auth_expired() and storage.owner_matches("12345678")

        books = [
            {"bookId": "test_book_101", "title": "全离线书", "cachedChapters": 2, "totalChapters": 2},
            {"bookId": "part_book", "title": "部分缓存", "cachedChapters": 1, "totalChapters": 4},
            {"bookId": "txt_book_202", "title": "未缓存书", "cachedChapters": 0, "totalChapters": 1},
        ]
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=False, expired=True)
        ui = shelf.render()
        assert ui["screen"] == "SHELF" and ui["badges"]["test_book_101"] == "OFFLINE_PILL"
        assert shelf.preempt_for_open("test_book_101") == "WR_JOB_OPEN"

        # Read offline and write local progress -> Home shows '· 待同步'
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 2, 2, 70, 420, dirty=1)
        assert client.render_home_continue_card("weread://test_book_101")["pendingBadge"] == "· 待同步"

        # Tap uncached book -> QR with back-to-shelf -> re-scan QR -> clears expired & syncs progress
        assert shelf.preempt_for_open("txt_book_202") == "QR_LOGIN_PROMPT"
        assert shelf.render()["backToShelf"] is True
        client.mock_set_state({"endpoint_errors": {}})
        client.set_wifi_enabled(True)
        uid, _ = client.login_qr_get_uid()
        poll = client.login_qr_poll(uid)
        client.login_device_exchange(str(poll["vid"]), poll["skey"], poll["code"])
        assert storage.is_auth_expired() is False
        assert client.sync_progress_to_cloud("test_book_101") is True
        assert client.render_home_continue_card("weread://test_book_101")["pendingBadge"] == ""
        assert not client.mock_get_violations()


def test_scenario2_phone_to_eink_handoff_and_no_interrupt_guard() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _setup_client(tmp)
        shelf = WereadShelfModel(storage)
        shelf.requested_book_id = "test_book_101"
        shelf.load_snapshot([{"bookId": "test_book_101", "title": "荒原起源", "cachedChapters": 2, "totalChapters": 6}], authenticated=True)
        assert shelf.render()["screen"] == "DIRECT_OPEN"

        reader = WereadReaderModel(current_chapter=2, current_offset=100)
        # pages == 0 -> auto-jumps to phone ch 5 with 6s undo banner
        assert reader.on_cloud_jump_arrival(target_chapter=5, target_offset=300, now_ms=1000) == "AUTO_JUMPED"
        assert (reader.current_chapter, reader.current_offset) == (5, 300)
        assert reader.tap_banner() == "RESTORED_PREV"
        assert (reader.current_chapter, reader.current_offset) == (2, 100)

        # User turns page (pages > 0) -> late phone ch 6 update MUST NOT force-jump
        reader.turn_page(new_offset=220, now_ms=2000)
        assert reader.on_cloud_jump_arrival(target_chapter=6, target_offset=50, now_ms=3000) == "REBASED_OPT_IN"
        assert (reader.current_chapter, reader.current_offset) == (2, 220)
        assert "第 6 章" in reader.undo_banner_text

        # 8s pause on hot Wi-Fi silently reports and clears footer '待同步 · '
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 2, 2, 42, 180, dirty=1)
        assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_PENDING") == "待同步 · "
        client.connect_network(now_ms=3500)
        client.note_page_move(3500)
        assert client.tick_reader_pause("test_book_101", 11500) is True
        assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_SYNCED") == ""


def test_scenario3_paywall_offline_boundary_and_version_self_heal() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _setup_client(tmp)
        client.mock_set_state({
            "enforce_book_version": True,
            "book_versions": {"test_book_101": 103},
            "chapters": {"test_book_101": [{"chapterUid": i, "chapterIdx": i, "title": f"第{i}章"} for i in range(1, 11)]},
            "chapter_errors": {
                "test_book_101:8": {"status": 402},
                "test_book_101:3": {"status": 402},
            },
        })
        reader = WereadReaderModel(current_chapter=2, current_offset=0)
        res = reader.prepare_open(client, "test_book_101", chapter_uid=1, book_type="epub", book_version=100)
        assert res["status"] == 200 and reader.heal_attempts == 1

        # TOC cold jump to Ch 8 hits 402 -> returns to TOC page 2 -> forward turn Ch 3 hits 402 -> clears -> loads
        assert reader.complete_toc_cold_jump(client, "test_book_101", target_uid=8, origin_toc_page=2, now_ms=1000) is False
        assert reader.view == "TOC" and reader.toc_page == 2
        reader.view = "READING"
        reader.current_chapter = 2
        res_pay = reader.forward_turn_cold(client, "test_book_101", next_uid=3, now_ms=2000)
        assert res_pay["status"] == 402 and "手机端购买" in reader.notice_text
        client.mock_set_state({"chapter_errors": {}})
        res_ok = reader.forward_turn_cold(client, "test_book_101", next_uid=3, now_ms=3000)
        assert res_ok["status"] == 200 and reader.current_chapter == 3

        # Cold jump to Ch 10 cancelled cleanly via Back/capsule
        reader.start_cold_jump(target_chapter=10)
        assert reader.wr_chapter_cancel() is True
        assert reader.current_chapter == 3


def test_scenario4_large_library_triage_search_cache_and_preemption() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _setup_client(tmp)
        books = []
        for i in range(35):
            title = "三体" if i == 5 else f"藏书 {i:02d}"
            books.append({
                "bookId": f"book_{i:02d}",
                "title": title,
                "progress": 40 if i < 30 else 0,
                "finished": 0,
                "cachedChapters": 0,
                "totalChapters": 4,
            })
        client.mock_set_state({
            "books": books,
            "chapters": {"book_01": [{"chapterUid": i, "chapterIdx": i, "title": f"第{i}章"} for i in range(1, 5)]},
        })
        shelf = WereadShelfModel(storage, style=6)
        shelf.load_snapshot(books, authenticated=True)
        assert shelf.page_count() == 4
        assert shelf.swipe("L") is True and shelf.s_page == 1
        assert shelf.hit_dropdown(100, 100) is True
        shelf.set_filter("READING")
        assert shelf.page_count() == 4
        assert shelf.tap_footer_page_indicator() is True
        assert shelf.slider_jump_to(2) == 2

        # Pinyin search "st" -> 三体 -> clear
        shelf.set_search_draft("st")
        shelf.commit_search()
        assert [b["bookId"] for b in shelf.visible_books()] == ["book_05"]
        shelf.clear_search()

        # Whole-book cache + 3s throttle + preemption
        shelf.long_press_book("book_01")
        shelf.start_whole_book_cache("book_01", now_ms=1000)
        for ch in range(1, 5):
            assert client.download_chapter("book_01", ch, book_type="epub")["status"] == 200
            shelf.tick_whole_book_cache_progress(ch, 4, now_ms=1000 + ch * 1500)
        assert shelf.render()["badges"]["book_01"] == "OFFLINE_PILL"

        shelf.start_whole_book_cache("book_02", now_ms=10000)
        assert shelf.preempt_for_open("book_01") == "WR_JOB_OPEN"
        assert shelf.cache_job_book_id is None


def test_scenario5_storage_pressure_and_safe_cache_reclamation() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _setup_client(tmp)
        books = [
            {"bookId": "test_book_101", "title": "在读小说", "progress": 45, "finished": 0, "cachedChapters": 1, "totalChapters": 4},
            {"bookId": "txt_book_202", "title": "已读完文本", "progress": 100, "finished": 1, "cachedChapters": 1, "totalChapters": 1},
        ]
        storage.save_shelf({"books": books})
        client.download_chapter("test_book_101", 1, book_type="epub")
        client.download_chapter("txt_book_202", 1, book_type="txt")
        storage.save_cover_gray("test_book_101", b"GRAY_101")
        storage.save_cover_gray("txt_book_202", b"GRAY_202")
        storage.save_cover_img("txt_book_202", b"RAW_IMG_202")
        storage.save_local_progress("test_book_101", {"progress": 45, "dirty": 0})
        storage.save_local_progress("txt_book_202", {"progress": 100, "dirty": 0})

        assert WereadReaderModel.footer_cache_prefix(1, 4, storage_blocked=True) == "空间不足 · "
        assert WereadReaderModel.toolbar_button6_label(1, 4, storage_blocked=True) == "重试缓存"

        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=True)
        stats = shelf.open_cache_management(free_bytes=90 * 1024 * 1024)
        assert stats["cached_books"] == 2 and stats["reserve_mb"] == 104

        client._raw_request("POST", "/mock/control/clear_history")
        cleaned = storage.clean_finished_books()
        assert cleaned == ["txt_book_202"]
        assert storage.load_cover_gray("txt_book_202") == b"GRAY_202"
        assert storage.load_local_progress("txt_book_202")["progress"] == 100
        assert not os.path.exists(os.path.join(storage.book_cache_dir("txt_book_202"), "cover-txt_book_202.img"))
        shelf.load_snapshot(storage.load_shelf()["books"], authenticated=True)
        shelf.render()
        assert [h for h in client.mock_get_history() if h["path"].startswith("/covers/")] == []


def test_scenario6_explicit_logout_vs_passive_expiry_and_local_invariants() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _setup_client(tmp)
        client.connect_network(now_ms=1000)
        client.now_ms = 590000
        client.sync_shelf_full()
        assert client.check_rf_idle_disconnect(700000) is False
        client.set_wifi_enabled(False)
        client.record_reading_progress("test_book_101", 1, 1, 60, 240, dirty=1)
        client.connect_network(now_ms=705000)
        assert client.end_reader_session("test_book_101", now_ms=706000, busy=False) is True

        # Explicit WR_JOB_LOGOUT wipes auth.json completely (distinct from passive -2013)
        client.logout()
        assert not os.path.exists(storage.auth_file)
        assert storage.is_auth_expired() is False
        assert storage.owner_matches("12345678") is False

        # Subsequent /web/getuid must have zero auth/cookie leaks and zero security violations
        client._raw_request("POST", "/mock/control/clear_history")
        client.login_qr_get_uid()
        hist = [h for h in client.mock_get_history() if h["path"] == "/web/getuid"]
        assert hist and "accesstoken" not in hist[0]["headers"] and "cookie" not in hist[0]["headers"]
        assert not client.mock_get_violations()
        assert book_file_path_supported("/sdcard/Books/classic.epub") is True
        assert book_file_path_supported("/sdcard/Books/notes.txt") is True


ALL_TESTS = [
    test_scenario1_subway_token_expiry_offline_read_and_relogin,
    test_scenario2_phone_to_eink_handoff_and_no_interrupt_guard,
    test_scenario3_paywall_offline_boundary_and_version_self_heal,
    test_scenario4_large_library_triage_search_cache_and_preemption,
    test_scenario5_storage_pressure_and_safe_cache_reclamation,
    test_scenario6_explicit_logout_vs_passive_expiry_and_local_invariants,
]


def run_all_tests() -> None:
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 4 WR-UX Scenarios Suite: {len(ALL_TESTS)} scenarios ===")
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
