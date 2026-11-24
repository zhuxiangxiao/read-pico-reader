#!/usr/bin/env python3
"""Tier 1 Feature Coverage: WeRead P0 UX Optimizations (WR-UX-01 .. WR-UX-05).

25 tests (5 per feature):
- WR-UX-01: -2013 Offline Readonly & QR Expiry/Refresh
- WR-UX-02: Reader Footer Simplification & Silent Prefetch
- WR-UX-03: Shelf/Keyboard Swipe, Search UX, Direct Open, Idle Tagline
- WR-UX-04: Cloud-Jump Non-Interrupt Guard & 6s Cross-Chapter Undo
- WR-UX-05: Cold-Jump Cancel, 402/Offline Retry, TOC Rollback, -2063 Self-Heal
"""

from __future__ import annotations

import json
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
    client.login_device_exchange(
        vid="12345678",
        skey="mock_skey_123456",
        code="mock_qr_code_xyz123",
        device_id="eink-ux-p0",
    )
    return storage, client


# ==============================================================================
# WR-UX-01: -2013 Offline Readonly & QR Expiry/Refresh (5 tests)
# ==============================================================================

def test_ux01_passive_2013_marks_expired_retains_vid() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.mark_expired_on_2013 = True
        client.mock_set_state({
            "endpoint_errors": {"/shelf/sync": {"status": 401, "body": {"errcode": -2013}}},
            "renew_error": {"status": 401, "body": {"errcode": -2013, "errmsg": "refresh expired"}},
        })
        status, _, _ = client.request("GET", "/shelf/sync?synckey=0")
        assert status == 401
        auth = storage.load_auth()
        assert auth is not None and auth.get("expired") is True
        assert "accessToken" not in auth and "refreshToken" not in auth and "skey" not in auth
        assert auth.get("vid") == "12345678" and auth.get("deviceId") == "eink-ux-p0"
        assert storage.owner_matches("12345678") is True
        assert client.is_authenticated is False
        assert not client.mock_get_violations()


def test_ux01_logout_wipes_auth_and_expired_flag() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.handle_terminal_2013()
        assert storage.is_auth_expired() is True
        client.logout()
        assert not os.path.exists(storage.auth_file)
        assert storage.is_auth_expired() is False
        assert storage.owner_matches("12345678") is False


def test_ux01_expired_with_shelf_enters_offline_readonly() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        shelf_data = client.sync_shelf_full()
        books = shelf_data["books"]
        for b in books:
            if b["bookId"] == "test_book_101":
                b["cachedChapters"] = 1
                b["totalChapters"] = 2
        client.handle_terminal_2013()
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot(books, authenticated=client.is_authenticated, expired=storage.is_auth_expired())
        ui = shelf.render()
        assert ui["screen"] == "SHELF"
        assert shelf.offline_readonly is True
        assert ui["bottomMessage"] == WereadShelfModel.OFFLINE_READONLY_PROMPT
        assert shelf.more_menu_items()[0] == "重新扫码登录"
        assert shelf.open_more_item("重新扫码登录") == "QR_LOGIN_PROMPT"
        assert shelf.render()["backToShelf"] is True
        assert shelf.qr_back_to_shelf() is True
        assert shelf.render()["screen"] == "SHELF"
        assert shelf.render()["bottomMessage"] == WereadShelfModel.OFFLINE_READONLY_PROMPT
        assert not client.mock_get_violations()


def test_ux01_expired_opens_cached_book_writes_local_progress() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        shelf_data = client.sync_shelf_full()
        res = client.download_chapter("test_book_101", 1, book_type="epub")
        assert res["status"] == 200
        client.handle_terminal_2013()
        client.set_wifi_enabled(False)
        client.mock_set_state({})
        hist_before = len(client.mock_get_history())
        shelf = WereadShelfModel(storage)
        books = shelf_data["books"]
        for b in books:
            if b["bookId"] == "test_book_101":
                b["cachedChapters"] = 1
                b["totalChapters"] = 2
        shelf.load_snapshot(books, authenticated=False, expired=True)
        assert shelf.preempt_for_open("test_book_101") == "WR_JOB_OPEN"
        html = storage.load_chapter_html("test_book_101", 1)
        assert html is not None and "第一章 起源" in html
        prog = client.record_reading_progress("test_book_101", 1, 1, 35, 180, summary="第一章", dirty=1)
        assert prog["dirty"] == 1
        loaded = storage.load_local_progress("test_book_101")
        assert loaded is not None and loaded["progress"] == 35 and loaded["dirty"] == 1
        # Zero WeRead API calls made while reading offline (exclude /mock/control/history query itself)
        hist_after = [h for h in client.mock_get_history()[hist_before:] if not h["path"].startswith("/mock/control")]
        assert hist_after == []


def test_ux01_qr_timeout_cancel_clears_qr_and_shows_refresh() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.logout()
        client.mock_set_state({"dynamic_uid": True, "qr_sequence": ["LOGIN_TIMEOUT"]})
        uid1, qr_url1 = client.login_qr_get_uid()
        poll1 = client.login_qr_poll(uid1)
        assert poll1["data"]["logicCode"] == "LOGIN_TIMEOUT"
        shelf = WereadShelfModel(storage)
        shelf.screen = "QR_LOGIN_PROMPT"
        shelf.qr_uid = uid1
        shelf.qr_url = qr_url1
        shelf.handle_qr_poll_result(poll1["data"]["logicCode"])
        ui = shelf.render()
        assert ui["qrCleared"] is True
        assert shelf.qr_uid == "" and shelf.qr_url == ""
        assert ui["qrPrompt"] == "二维码已过期，点击刷新"
        assert shelf.tap_qr_area(300, 400) is True
        assert shelf.active_job == "WR_JOB_QR"
        uid2, qr_url2 = client.login_qr_get_uid()
        assert uid2 != uid1 and qr_url2 != qr_url1


# ==============================================================================
# WR-UX-02: Reader Footer Simplification & Silent Prefetch (5 tests)
# ==============================================================================

def test_ux02_footer_synced_and_stationary_local_empty() -> None:
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_SYNCED") == ""
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", moved=False) == ""
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_PENDING") == "待同步 · "


def test_ux02_footer_abnormal_states_explicit() -> None:
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", save_failed=True) == "未保存 · "
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", pending_save=True) == "待保存 · "
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_LOCAL", rescued=True) == "暂存机内 · "
    assert WereadReaderModel.footer_sync_prefix("WR_PROGRESS_CONFLICT") == "进度冲突 · "


def test_ux02_footer_cache_prefix_silent_when_full_or_paused() -> None:
    assert WereadReaderModel.footer_cache_prefix(cached=10, total=10) == ""
    assert WereadReaderModel.footer_cache_prefix(cached=3, total=10, paused=True) == ""
    assert WereadReaderModel.footer_cache_prefix(cached=3, total=10) == ""
    assert WereadReaderModel.footer_cache_prefix(cached=3, total=10, storage_blocked=True) == "空间不足 · "


def test_ux02_cache_delta_does_not_wake_footer_refresh() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.mock_set_state({
            "chapters": {
                "test_book_101": [
                    {"chapterUid": i, "chapterIdx": i, "title": f"第{i}章"} for i in range(1, 5)
                ]
            }
        })
        reader = WereadReaderModel(current_chapter=1, current_offset=0)
        assert reader.sample_footer(storage, "test_book_101", sync_state="WR_PROGRESS_SYNCED", total_chapters=4) is False
        assert reader.s_reader_footer_pending is False
        for uid in (1, 2, 3):
            res = client.download_chapter("test_book_101", uid, book_type="epub")
            assert res["status"] == 200
            # Real cached_chapters incremented on disk, yet footer wakeup stays False
            woke = reader.sample_footer(storage, "test_book_101", sync_state="WR_PROGRESS_SYNCED", total_chapters=4)
            assert woke is False and reader.s_reader_footer_pending is False
            assert reader.cached_chapters == uid
            assert reader.non_fullscreen_topbar_cache_label(reader.cached_chapters, 4) == f"缓存 {uid}/4"
        assert reader.footer_wake_count == 0
        # Sync state change DOES wake footer refresh
        assert reader.sample_footer(storage, "test_book_101", sync_state="WR_PROGRESS_PENDING", total_chapters=4) is True
        assert reader.s_reader_footer_pending is True and reader.footer_wake_count == 1


def test_ux02_toolbar_button6_dynamic_cache_labels() -> None:
    assert WereadReaderModel.toolbar_button6_label(3, 10) == "缓存 3/10"
    assert WereadReaderModel.toolbar_button6_label(10, 10) == "已全存"
    assert WereadReaderModel.toolbar_button6_label(3, 10, paused=True) == "已暂停"
    assert WereadReaderModel.toolbar_button6_label(3, 10, stalled=True) == "重试缓存"
    assert WereadReaderModel.toolbar_button6_label(3, 10, storage_blocked=True) == "重试缓存"
    assert WereadReaderModel.non_fullscreen_topbar_cache_label(3, 10) == "缓存 3/10"
    assert WereadReaderModel.non_fullscreen_topbar_cache_label(10, 10) == "已全存"
    assert WereadReaderModel.non_fullscreen_topbar_cache_label(3, 10, paused=True) == "已暂停"
    assert WereadReaderModel.non_fullscreen_topbar_cache_label(3, 10, storage_blocked=True) == "重试缓存"


# ==============================================================================
# WR-UX-03: Shelf/Keyboard Swipe, Search UX, Direct Open, Idle Tagline (5 tests)
# ==============================================================================

def test_ux03_main_shelf_swipe_lr_paging() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage, style=6)
        books = [{"bookId": f"b_{i}", "title": f"Book {i}"} for i in range(20)]
        shelf.load_snapshot(books, authenticated=True)
        assert shelf.page_count() == 3
        assert shelf.swipe("L") is True and shelf.s_page == 1
        assert shelf.swipe("L") is True and shelf.s_page == 2
        assert shelf.swipe("R") is True and shelf.s_page == 1
        assert shelf.primed_cover_pages == [1, 2, 1]


def test_ux03_search_pinyin_hint_clear_confirm_and_kb_swipe() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        books = [
            {"bookId": "santi", "title": "三体", "author": "刘慈欣"},
            {"bookId": "other", "title": "荒原起源", "author": "作者"},
        ]
        shelf.load_snapshot(books, authenticated=True)
        shelf.search_active = True
        ui = shelf.render()
        assert ui["searchHint"] == "可输入首字母（如 st）、拼音或中文"
        shelf.set_search_draft("st")
        assert shelf.tap_search_confirm(200, 100) is False
        assert shelf.tap_search_confirm(600, 100) is True
        assert shelf.search_active is False and shelf.search_page == 0
        vis = shelf.visible_books()
        assert len(vis) == 1 and vis[0]["bookId"] == "santi"
        shelf.search_active = True
        assert shelf.swipe("L") is True and shelf.search_page == 1
        assert shelf.swipe("L") is True and shelf.search_page == 2
        assert shelf.swipe("L") is False and shelf.search_page == 2
        assert shelf.swipe("R") is True and shelf.search_page == 1
        assert shelf.swipe("R") is True and shelf.search_page == 0
        shelf.clear_search()
        assert shelf.query == "" and shelf.search_draft == "" and len(shelf.visible_books()) == 2


def test_ux03_direct_open_centered_transition_no_grid_flash() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.requested_book_id = "test_book_101"
        shelf.load_snapshot([{"bookId": "test_book_101", "title": "测试小说"}], authenticated=True)
        ui = shelf.render()
        assert ui["screen"] == "DIRECT_OPEN"
        assert ui["centeredText"] == "正在打开阅读…"
        assert "SHELF" not in shelf.screen_history
        assert ui["gridFlashed"] is False


def test_ux03_idle_normal_shelf_clears_static_tagline() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([{"bookId": "b1", "title": "Book 1"}], authenticated=True)
        ui = shelf.render()
        assert ui["bottomMessage"] == ""
        assert "单击阅读" not in ui["bottomMessage"]


def test_ux03_shelf_status_line_shown_only_on_sync_pending_error() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        shelf = WereadShelfModel(storage)
        shelf.load_snapshot([{"bookId": "b1", "title": "Book 1"}], authenticated=True)
        assert shelf.render()["bottomMessage"] == ""
        shelf.syncing = True
        assert "同步" in shelf.render()["bottomMessage"]
        shelf.syncing = False
        shelf.pending_sync = True
        assert "待同步" in shelf.render()["bottomMessage"]
        shelf.pending_sync = False
        shelf.error_message = "网络异常"
        assert shelf.render()["bottomMessage"] == "网络异常"


# ==============================================================================
# WR-UX-04: Cloud-Jump Non-Interrupt Guard & 6s Cross-Chapter Undo (5 tests)
# ==============================================================================

def test_ux04_pages_zero_auto_jumps_cloud_with_6s_undo_banner() -> None:
    reader = WereadReaderModel(current_chapter=2, current_offset=120)
    action = reader.on_cloud_jump_arrival(target_chapter=5, target_offset=400, now_ms=10000)
    assert action == "AUTO_JUMPED"
    assert (reader.current_chapter, reader.current_offset) == (5, 400)
    assert reader.undo_banner_active is True
    assert reader.undo_banner_type == "UNDO"
    assert "已接力至手机最新进度" in reader.undo_banner_text and "[返回原处]" in reader.undo_banner_text
    reader.tick(15999)
    assert reader.undo_banner_active is True
    reader.tick(16000)
    assert reader.undo_banner_active is False


def test_ux04_undo_banner_returns_cross_chapter_position() -> None:
    reader = WereadReaderModel(current_chapter=2, current_offset=120)
    reader.on_cloud_jump_arrival(target_chapter=5, target_offset=400, now_ms=1000)
    res = reader.tap_banner()
    assert res == "RESTORED_PREV"
    assert (reader.current_chapter, reader.current_offset) == (2, 120)
    assert reader.undo_banner_active is False
    assert reader.local_nav_dirty is True
    assert reader.pages_turned_since_open == 1


def test_ux04_normal_page_turn_dismisses_undo_banner() -> None:
    reader = WereadReaderModel(current_chapter=1, current_offset=0)
    reader.on_cloud_jump_arrival(target_chapter=3, target_offset=50, now_ms=1000)
    assert reader.undo_banner_active is True
    reader.turn_page(new_offset=150, now_ms=1500)
    assert reader.undo_banner_active is False
    assert reader.pages_turned_since_open == 1


def test_ux04_pages_gt_zero_forbids_forced_jump_shows_opt_in_banner() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.mock_set_state({"endpoint_delays": {"/shelf/syncbook": 0.05}})
        reader = WereadReaderModel(current_chapter=1, current_offset=100)
        reader.turn_page(new_offset=220, now_ms=1200)
        details = client.sync_book_details(["test_book_101"])
        assert len(details) == 1
        action = reader.on_cloud_jump_arrival(target_chapter=4, target_offset=80, now_ms=1300)
        assert action == "REBASED_OPT_IN"
        assert (reader.current_chapter, reader.current_offset) == (1, 220)
        assert reader.undo_banner_type == "OPT_IN"
        assert "发现更新的手机进度（第 4 章）" in reader.undo_banner_text


def test_ux04_opt_in_banner_tap_jumps_to_cloud_chapter() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage = WereadStorage(tmp)
        storage.save_chapter_html("b1", 6, "<p>cached ch6</p>")
        reader = WereadReaderModel(current_chapter=1, current_offset=100)
        reader.turn_page(new_offset=200, now_ms=1000)
        reader.on_cloud_jump_arrival(target_chapter=6, target_offset=550, now_ms=1200)
        res = reader.tap_banner(storage=storage, book_id="b1")
        assert res == "JUMPED_TO_CLOUD"
        assert (reader.current_chapter, reader.current_offset) == (6, 550)
        assert reader.undo_banner_active is False
        # Uncached OPT_IN target enters cold load queue instead of jumping directly
        reader.on_cloud_jump_arrival(target_chapter=7, target_offset=10, now_ms=1500)
        res_cold = reader.tap_banner(storage=storage, book_id="b1")
        assert res_cold == "QUEUED_COLD_OPT_IN"
        assert reader.jump_pending is True and reader.jump_target_chapter == 7


# ==============================================================================
# WR-UX-05: Cold-Jump Cancel, 402/Offline Retry, TOC Rollback, -2063 Self-Heal (5 tests)
# ==============================================================================

def test_ux05_cold_jump_cancel_on_capsule_back_or_center() -> None:
    reader = WereadReaderModel(current_chapter=2, current_offset=80)
    reader.start_cold_jump(target_chapter=9)
    assert reader.jump_pending is True
    assert reader.wr_chapter_cancel() is True
    assert reader.jump_pending is False
    assert (reader.current_chapter, reader.current_offset) == (2, 80)


def test_ux05_forward_turn_402_notice_and_retry() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.mock_set_state({
            "chapter_errors": {"test_book_101:2": {"status": 402, "body": {"errcode": -2001, "errmsg": "Purchase required"}}}
        })
        reader = WereadReaderModel(current_chapter=1, current_offset=100)
        res1 = reader.forward_turn_cold(client, "test_book_101", next_uid=2, now_ms=2000)
        assert res1["status"] == 402
        assert reader.notice_text == "手机端购买本章后，再翻页即可加载"
        assert reader.current_chapter == 1
        # User purchases chapter -> mock clears 402 -> next forward turn retries & loads chapter 2
        client.mock_set_state({"chapter_errors": {}})
        res2 = reader.forward_turn_cold(client, "test_book_101", next_uid=2, now_ms=3000)
        assert res2["status"] == 200
        assert reader.current_chapter == 2
        assert storage.load_chapter_html("test_book_101", 2) is not None


def test_ux05_forward_turn_offline_notice_and_retry() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.set_wifi_enabled(False)
        reader = WereadReaderModel(current_chapter=1, current_offset=50)
        res1 = reader.forward_turn_cold(client, "test_book_101", next_uid=2, now_ms=1000)
        assert res1["status"] == "offline"
        assert reader.notice_text == "本章未缓存且网络不可用，连网后再翻页重试"
        client.set_wifi_enabled(True)
        res2 = reader.forward_turn_cold(client, "test_book_101", next_uid=2, now_ms=2000)
        assert res2["status"] == 200 and reader.current_chapter == 2


def test_ux05_toc_cold_jump_failure_returns_origin_page_and_retries() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.mock_set_state({
            "chapter_errors": {"test_book_101:2": {"status": 402, "body": {"errcode": -2001}}}
        })
        reader = WereadReaderModel(current_chapter=1, current_offset=0)
        ok1 = reader.complete_toc_cold_jump(client, "test_book_101", target_uid=2, origin_toc_page=3, now_ms=1000)
        assert ok1 is False
        assert reader.view == "TOC" and reader.toc_page == 3
        assert "购买" in reader.notice_text
        client.mock_set_state({"chapter_errors": {}})
        ok2 = reader.complete_toc_cold_jump(client, "test_book_101", target_uid=2, origin_toc_page=3, now_ms=2000)
        assert ok2 is True
        assert reader.view == "READING" and reader.current_chapter == 2


def test_ux05_prepare_open_version_error_single_auto_heal() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        storage, client = _authed_client(tmp)
        client.mock_set_state({
            "enforce_book_version": True,
            "book_versions": {"test_book_101": 105},
        })
        reader = WereadReaderModel()
        res = reader.prepare_open(client, "test_book_101", chapter_uid=1, book_type="epub", book_version=100)
        assert res["status"] == 200
        assert reader.heal_attempts == 1
        hist = [h["path"] for h in client.mock_get_history()]
        assert hist.count("/shelf/syncbook") == 1
        assert hist.count("/book/chapterInfos") == 1
        assert not client.mock_get_violations()


ALL_TESTS = [
    test_ux01_passive_2013_marks_expired_retains_vid,
    test_ux01_logout_wipes_auth_and_expired_flag,
    test_ux01_expired_with_shelf_enters_offline_readonly,
    test_ux01_expired_opens_cached_book_writes_local_progress,
    test_ux01_qr_timeout_cancel_clears_qr_and_shows_refresh,
    test_ux02_footer_synced_and_stationary_local_empty,
    test_ux02_footer_abnormal_states_explicit,
    test_ux02_footer_cache_prefix_silent_when_full_or_paused,
    test_ux02_cache_delta_does_not_wake_footer_refresh,
    test_ux02_toolbar_button6_dynamic_cache_labels,
    test_ux03_main_shelf_swipe_lr_paging,
    test_ux03_search_pinyin_hint_clear_confirm_and_kb_swipe,
    test_ux03_direct_open_centered_transition_no_grid_flash,
    test_ux03_idle_normal_shelf_clears_static_tagline,
    test_ux03_shelf_status_line_shown_only_on_sync_pending_error,
    test_ux04_pages_zero_auto_jumps_cloud_with_6s_undo_banner,
    test_ux04_undo_banner_returns_cross_chapter_position,
    test_ux04_normal_page_turn_dismisses_undo_banner,
    test_ux04_pages_gt_zero_forbids_forced_jump_shows_opt_in_banner,
    test_ux04_opt_in_banner_tap_jumps_to_cloud_chapter,
    test_ux05_cold_jump_cancel_on_capsule_back_or_center,
    test_ux05_forward_turn_402_notice_and_retry,
    test_ux05_forward_turn_offline_notice_and_retry,
    test_ux05_toc_cold_jump_failure_returns_origin_page_and_retries,
    test_ux05_prepare_open_version_error_single_auto_heal,
]


def run_all_tests() -> None:
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 1 WR-UX P0 (WR-UX-01..05) Suite: {len(ALL_TESTS)} tests ===")
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
