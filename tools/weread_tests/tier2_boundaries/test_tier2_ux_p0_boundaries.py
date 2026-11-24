#!/usr/bin/env python3
"""Tier 2 Boundary & Corner Cases: WeRead P0 UX Optimizations (WR-UX-01 .. WR-UX-05).

25 boundary/error tests (5 per feature).
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


def _authed_client(tmp_dir: str) -> tuple[WereadStorage, WereadClient]:
    storage = WereadStorage(tmp_dir)
    client = WereadClient(PORT, storage)
    client.mock_reset()
    client.login_device_exchange("12345678", "mock_skey_123456", "mock_qr_code_xyz123", "eink-ux-b0")
    return storage, client


# ==============================================================================
# WR-UX-01 Boundaries (5 tests)
# ==============================================================================

def test_ux01_b1_expired_with_empty_shelf_kicks_to_qr() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.handle_terminal_2013()
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([], authenticated=False, expired=True)
        ui = shelf.render()
        assert ui["screen"] == "QR_LOGIN_PROMPT"
        assert ui["backToShelf"] is False


def test_ux01_b2_expired_tap_uncached_book_prompts_relogin() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.handle_terminal_2013()
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([{"bookId": "uncached_1", "title": "未缓存", "cachedChapters": 0, "totalChapters": 3}], authenticated=False, expired=True)
        assert shelf.render()["screen"] == "SHELF"
        next_state = shelf.preempt_for_open("uncached_1")
        assert next_state == "QR_LOGIN_PROMPT"
        assert shelf.render()["backToShelf"] is True
        assert shelf.qr_back_to_shelf() is True
        assert shelf.render()["screen"] == "SHELF"


def test_ux01_b3_relogin_from_expired_clears_expired_flag() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.handle_terminal_2013()
        assert storage.is_auth_expired() is True
        # Mock deviceType=7 renew error does not block deviceType=4 QR re-login
        client.mock_set_state({"renew_error": {"status": 401, "body": {"errcode": -2013}}})
        uid, _ = client.login_qr_get_uid()
        poll = client.login_qr_poll(uid)
        assert poll.get("code") == "mock_qr_code_xyz123"
        auth = client.login_device_exchange(str(poll["vid"]), poll["skey"], poll["code"])
        assert "expired" not in auth
        assert storage.is_auth_expired() is False
        assert client.is_authenticated is True


def test_ux01_b4_qr_error_404_500_clears_stale_qr_buffer() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.logout()
        shelf = WereadShelfModel(storage)
        shelf.screen = "QR_LOGIN_PROMPT"
        shelf.qr_uid = "old_uid"
        shelf.qr_url = "https://weread.qq.com/web/confirm?pf=2&uid=old_uid"
        client.mock_set_state({"endpoint_errors": {"/web/getlogininfo": {"status": 500, "body": {"errcode": -1}}}})
        status, _, _ = client.request("POST", "/web/getlogininfo", body={"uid": "old_uid"}, auto_renew=False)
        assert status == 500
        shelf.handle_qr_poll_result("LOGIN_FAILED")
        assert shelf.qr_url == "" and shelf.qr_uid == ""
        assert shelf.render()["qrCleared"] is True


def test_ux01_b5_expired_auth_json_missing_vid_rejected() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        storage.save_auth({"expired": True, "deviceId": "eink-dev-only"})
        assert storage.is_auth_expired() is True
        assert storage.owner_matches("12345678") is False
        storage.save_auth({"expired": True, "vid": ""})
        assert storage.owner_matches("12345678") is False


# ==============================================================================
# WR-UX-02 Boundaries (5 tests)
# ==============================================================================

def test_ux02_b1_progress_moved_while_local_vs_stationary() -> None:
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", moved=False) == ""
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", moved=True) == "本地保存 · "
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", save_failed=True, moved=True) == "未保存 · "
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", pending_save=True, moved=True) == "待保存 · "
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", rescued=True, moved=True) == "暂存机内 · "


def test_ux02_b2_storage_blocked_overrides_full_or_paused() -> None:
    assert WereadReaderModel.footer_cache_prefix(cached=5, total=10, paused=True, storage_blocked=True) == "空间不足 · "
    assert WereadReaderModel.toolbar_button6_label(5, 10, paused=False, storage_blocked=True) == "重试缓存"
    assert WereadReaderModel.non_fullscreen_topbar_cache_label(5, 10, paused=False, storage_blocked=True) == "重试缓存"


def test_ux02_b3_total_chapters_zero_does_not_claim_full() -> None:
    assert WereadReaderModel.toolbar_button6_label(0, 0) != "已全存"
    assert WereadReaderModel.toolbar_button6_label(2, 0) != "已全存"
    assert WereadReaderModel.non_fullscreen_topbar_cache_label(0, 0) == ""
    assert WereadReaderModel.footer_cache_prefix(0, 0) == ""


def test_ux02_b4_rapid_10_chapter_bg_prefetch_zero_footer_wakeups() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.mock_set_state({
            "chapters": {
                "test_book_101": [
                    {"chapterUid": i, "chapterIdx": i, "title": f"第{i}章"} for i in range(1, 11)
                ]
            }
        })
        reader = WereadReaderModel(current_chapter=1, current_offset=0)
        assert reader.sample_footer(storage, "test_book_101", sync_state="WR_PROGRESS_SYNCED", total_chapters=10) is False
        for uid in range(1, 11):
            res = client.download_chapter("test_book_101", uid, book_type="epub")
            assert res["status"] == 200
            assert reader.sample_footer(storage, "test_book_101", sync_state="WR_PROGRESS_SYNCED", total_chapters=10) is False
        assert reader.cached_chapters == 10
        assert reader.footer_wake_count == 0
        assert reader.s_reader_footer_pending is False
        assert WereadReaderModel.non_fullscreen_topbar_cache_label(reader.cached_chapters, 10) == "已全存"


def test_ux02_b5_non_weread_local_book_footer_unaffected() -> None:
    for local_path in ("/sdcard/Books/novel.epub", "/sdcard/Books/story.txt"):
        assert book_file_path_supported(local_path) is True
        # Local books never inject WeRead sync/cache prefixes
        assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_SYNCED") == ""
        assert WereadReaderModel.footer_cache_prefix(0, 0) == ""


# ==============================================================================
# WR-UX-03 Boundaries (5 tests)
# ==============================================================================

def test_ux03_b1_swipe_at_first_and_last_page_clamps() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        shelf.load_snapshot([{"bookId": f"b_{i}", "title": f"Book {i}"} for i in range(12)], authenticated=True)
        assert shelf.page_count() == 2
        assert shelf.swipe("R") is False and shelf.s_page == 0
        assert shelf.swipe("L") is True and shelf.s_page == 1
        assert shelf.swipe("L") is False and shelf.s_page == 1


def test_ux03_b2_search_clear_when_empty_and_overlong_query() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([{"bookId": "b1", "title": "Book"}], authenticated=True)
        shelf.clear_search()
        assert shelf.query == "" and shelf.search_draft == ""
        shelf.set_search_draft("a" * 120)
        assert len(shelf.search_draft.encode("utf-8")) <= 64
        shelf.search_active = True
        assert shelf.swipe("L") is True and shelf.search_page == 1
        assert shelf.swipe("L") is True and shelf.search_page == 2
        assert shelf.swipe("L") is False and shelf.search_page == 2
        shelf.commit_search()
        assert shelf.search_page == 0
        shelf.clear_search()
        assert shelf.search_draft == ""


def test_ux03_b3_direct_open_missing_book_id_clears_requested() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.requested_book_id = "non_existent_999"
        shelf.load_snapshot([{"bookId": "test_book_101", "title": "存在书"}], authenticated=True)
        ui = shelf.render()
        assert ui["screen"] == "SHELF"
        assert shelf.requested_book_id == ""


def test_ux03_b4_direct_open_while_unauth_defers_until_login() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.logout()
        shelf = WereadShelfModel(storage)
        shelf.requested_book_id = "test_book_101"
        shelf.load_snapshot([], authenticated=False, expired=False)
        assert shelf.render()["screen"] == "QR_LOGIN_PROMPT"
        # After QR login + sync, requested_book_id opens directly without grid flash
        client.login_device_exchange("12345678", "mock_skey_123456", "mock_qr_code_xyz123")
        synced = client.sync_shelf_full()
        shelf.load_snapshot(synced["books"], authenticated=True, expired=False)
        ui = shelf.render()
        assert ui["screen"] == "DIRECT_OPEN" and ui["gridFlashed"] is False


def test_ux03_b5_search_pinyin_match_empty_shelf_zero_results() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([], authenticated=True)
        shelf.set_search_draft("st")
        shelf.commit_search()
        assert shelf.visible_books() == []
        assert shelf.page_count() == 1


# ==============================================================================
# WR-UX-04 Boundaries (5 tests)
# ==============================================================================

def test_ux04_b1_undo_banner_expires_exactly_at_6000ms() -> None:
    reader = WereadReaderModel(current_chapter=1, current_offset=0)
    reader.on_cloud_jump_arrival(target_chapter=4, target_offset=100, now_ms=5000)
    reader.tick(10999)
    assert reader.undo_banner_active is True
    reader.tick(11000)
    assert reader.undo_banner_active is False


def test_ux04_b2_cloud_same_position_as_local_no_banner() -> None:
    reader = WereadReaderModel(current_chapter=3, current_offset=250)
    res = reader.on_cloud_jump_arrival(target_chapter=3, target_offset=250, now_ms=1000)
    assert res == "NOOP"
    assert reader.undo_banner_active is False


def test_ux04_b3_user_turn_across_save_persists_pages_gt_zero() -> None:
    reader = WereadReaderModel(current_chapter=1, current_offset=0)
    reader.turn_page(new_offset=120, now_ms=1000)
    reader.save_local_only()
    assert reader.pages_turned_since_open == 1
    res = reader.on_cloud_jump_arrival(target_chapter=5, target_offset=300, now_ms=2000)
    assert res == "REBASED_OPT_IN"
    assert (reader.current_chapter, reader.current_offset) == (1, 120)


def test_ux04_b4_undo_when_prev_chapter_evicted_handles_gracefully() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        storage.save_chapter_html("b1", 1, "<p>ch1</p>")
        reader = WereadReaderModel(current_chapter=1, current_offset=50)
        reader.on_cloud_jump_arrival(target_chapter=2, target_offset=10, now_ms=1000)
        storage.purge_cached_chapters("b1")
        action = reader.tap_banner(storage=storage, book_id="b1")
        assert action == "QUEUED_COLD_UNDO"
        assert reader.jump_pending is True and reader.jump_target_chapter == 1


def test_ux04_b5_multiple_cloud_updates_after_turn_coalesce_rebase() -> None:
    reader = WereadReaderModel(current_chapter=1, current_offset=100)
    reader.turn_page(new_offset=200, now_ms=1000)
    reader.on_cloud_jump_arrival(target_chapter=3, target_offset=50, now_ms=1500)
    assert "第 3 章" in reader.undo_banner_text
    reader.on_cloud_jump_arrival(target_chapter=7, target_offset=90, now_ms=2000)
    assert "第 7 章" in reader.undo_banner_text
    assert (reader.current_chapter, reader.current_offset) == (1, 200)


# ==============================================================================
# WR-UX-05 Boundaries (5 tests)
# ==============================================================================

def test_ux05_b1_cancel_when_not_pending_is_noop() -> None:
    reader = WereadReaderModel(current_chapter=2, current_offset=40)
    assert reader.wr_chapter_cancel() is False
    assert (reader.current_chapter, reader.current_offset) == (2, 40)


def test_ux05_b2_402_notice_expires_after_5s_and_allows_back_turn() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.mock_set_state({"chapter_errors": {"test_book_101:2": {"status": 402}}})
        reader = WereadReaderModel(current_chapter=1, current_offset=200)
        reader.forward_turn_cold(client, "test_book_101", next_uid=2, now_ms=1000)
        assert reader.notice_text != ""
        reader.tick(5999)
        assert reader.notice_text != ""
        reader.tick(6000)
        assert reader.notice_text == ""
        reader.turn_page(new_offset=100, now_ms=6500)
        assert reader.current_offset == 100


def test_ux05_b3_toc_cold_jump_from_page_N_restores_exact_page() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.mock_set_state({"chapter_errors": {"test_book_101:2": {"status": 500}}})
        reader = WereadReaderModel()
        assert reader.complete_toc_cold_jump(client, "test_book_101", target_uid=2, origin_toc_page=4, now_ms=1000) is False
        assert reader.view == "TOC" and reader.toc_page == 4


def test_ux05_b4_prepare_open_double_2063_stops_after_one_heal() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.mock_set_state({
            "enforce_book_version": True,
            "book_versions": {"test_book_101": 999},
            # syncbook returns old 100 while chapterdownload expects 999 -> double -2063
            "books": [
                {"bookId": "test_book_101", "title": "Test", "format": "epub", "bookVersion": 100},
            ],
            "endpoint_errors": {},
        })
        # Override /shelf/syncbook to serve stale 100
        client.mock_set_state({
            "endpoint_errors": {"/shelf/syncbook": {"status": 200, "body": {"books": [{"bookId": "test_book_101", "bookVersion": 100}]}}}
        })
        reader = WereadReaderModel()
        res = reader.prepare_open(client, "test_book_101", chapter_uid=1, book_type="epub", book_version=99)
        assert reader.heal_attempts == 1
        assert res.get("errcode") == -2063


def test_ux05_b5_499_sold_out_does_not_loop_version_heal() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        _, client = _authed_client(tmp)
        client.mock_set_state({
            "chapter_errors": {"test_book_101:1": {"status": 499, "body": {"errcode": -2064, "errmsg": "SoldOut"}}}
        })
        reader = WereadReaderModel()
        res = reader.prepare_open(client, "test_book_101", chapter_uid=1, book_type="epub", book_version=100)
        assert res["status"] == 499 and res["errcode"] == -2064
        assert reader.heal_attempts == 0


ALL_TESTS = [
    test_ux01_b1_expired_with_empty_shelf_kicks_to_qr,
    test_ux01_b2_expired_tap_uncached_book_prompts_relogin,
    test_ux01_b3_relogin_from_expired_clears_expired_flag,
    test_ux01_b4_qr_error_404_500_clears_stale_qr_buffer,
    test_ux01_b5_expired_auth_json_missing_vid_rejected,
    test_ux02_b1_progress_moved_while_local_vs_stationary,
    test_ux02_b2_storage_blocked_overrides_full_or_paused,
    test_ux02_b3_total_chapters_zero_does_not_claim_full,
    test_ux02_b4_rapid_10_chapter_bg_prefetch_zero_footer_wakeups,
    test_ux02_b5_non_weread_local_book_footer_unaffected,
    test_ux03_b1_swipe_at_first_and_last_page_clamps,
    test_ux03_b2_search_clear_when_empty_and_overlong_query,
    test_ux03_b3_direct_open_missing_book_id_clears_requested,
    test_ux03_b4_direct_open_while_unauth_defers_until_login,
    test_ux03_b5_search_pinyin_match_empty_shelf_zero_results,
    test_ux04_b1_undo_banner_expires_exactly_at_6000ms,
    test_ux04_b2_cloud_same_position_as_local_no_banner,
    test_ux04_b3_user_turn_across_save_persists_pages_gt_zero,
    test_ux04_b4_undo_when_prev_chapter_evicted_handles_gracefully,
    test_ux04_b5_multiple_cloud_updates_after_turn_coalesce_rebase,
    test_ux05_b1_cancel_when_not_pending_is_noop,
    test_ux05_b2_402_notice_expires_after_5s_and_allows_back_turn,
    test_ux05_b3_toc_cold_jump_from_page_N_restores_exact_page,
    test_ux05_b4_prepare_open_double_2063_stops_after_one_heal,
    test_ux05_b5_499_sold_out_does_not_loop_version_heal,
]


def run_all_tests() -> None:
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 2 WR-UX P0 Boundaries Suite: {len(ALL_TESTS)} tests ===")
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
