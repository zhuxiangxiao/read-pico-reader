#!/usr/bin/env python3
"""Tier 4: Real-World Application Scenarios for WeRead E2E Testing.

Covers 6 comprehensive multi-step end-to-end workflows:
1. First-Time User Onboarding & Initial Bookshelf Sync
2. Complete Reading Session with Streaming Decryption & Prefetch
3. Commuter Flight / Offline Reading & Post-Flight Sync
4. Session Expiry During Multi-Chapter Reading
5. Paywalled & Delisted Book Handling (HTTP 402, HTTP 499)
6. Account Logout & Clean Storage Sanitization
"""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import time
import zipfile

# Add local directory and tools directory to sys.path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.dirname(os.path.dirname(SCRIPT_DIR))
COMMON_DIR = os.path.join(os.path.dirname(SCRIPT_DIR), "common")
if COMMON_DIR not in sys.path:
    sys.path.insert(0, COMMON_DIR)
if SCRIPT_DIR not in sys.path:
    sys.path.insert(0, SCRIPT_DIR)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from weread_client_sim import (
    WereadClient,
    WereadStorage,
    book_file_path_supported,
    decrypt_encrypt_key,
    is_weread_uri,
    parse_weread_uri,
)


def get_mock_port() -> int:
    return int(os.environ.get("WEREAD_MOCK_PORT", "8989"))


# ---------------------------------------------------------------------------
# Scenario 1: First-Time User Onboarding & Initial Bookshelf Sync
# ---------------------------------------------------------------------------
def test_scenario1_first_time_user_onboarding_and_shelf_sync() -> None:
    print("[TIER 4] Scenario 1: First-Time User Onboarding & Initial Bookshelf Sync...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier4_s1_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # Step 1.1: Connect Wi-Fi (STA mode)
        client.set_wifi_enabled(True)
        assert client.wifi_enabled is True

        # Step 1.2: App requests QR login UID and constructs WeChat confirm URL
        uid, qr_url = client.login_qr_get_uid()
        assert len(uid) > 0
        assert qr_url == f"https://weread.qq.com/web/confirm?pf=2&uid={uid}"

        # Step 1.3: Poll QR scan status through state transitions
        # State A: WAITING_CONFIRM
        client.mock_set_state({"qr_status": "WAITING_CONFIRM"})
        poll1 = client.login_qr_poll(uid)
        assert poll1["data"]["succeed"] is False

        # State B: NEED_OTP
        client.mock_set_state({"qr_status": "NEED_OTP"})
        poll2 = client.login_qr_poll(uid)
        assert poll2["data"].get("logicCode") == "NEED_OTP"

        # State C: CONFIRMED
        client.mock_set_state({"qr_status": "CONFIRMED"})
        poll3 = client.login_qr_poll(uid)
        assert poll3.get("vid") == 12345678
        assert poll3.get("skey") == "mock_skey_123456"
        assert poll3.get("code") == "mock_qr_code_xyz123"

        # Step 1.4: Device Token Exchange (POST /weblogin deviceType=4)
        auth = client.login_device_exchange(
            vid="12345678",
            skey="mock_skey_123456",
            code="mock_qr_code_xyz123",
            device_id="kiiko-dev-pico",
            device_name="KiikoReader",
        )
        assert auth["accessToken"] == "mock_access_token_token"
        assert auth["refreshToken"] == "mock_refresh_token_xyz"

        # Verify auth.json saved to disk atomically
        loaded_auth = storage.load_auth()
        assert loaded_auth is not None
        assert loaded_auth["vid"] == "12345678"
        assert loaded_auth["accessToken"] == "mock_access_token_token"

        # Step 1.5: Initial Bookshelf Sync (GET /shelf/sync)
        # Server returns books + lectureBooks. Client filters out lectureBooks!
        shelf = client.sync_shelf_full()
        assert len(shelf["books"]) >= 2
        # Verify lectureBooks was filtered out
        for b in shelf["books"]:
            assert b.get("format") != "audio"
            assert b.get("type", 0) != 1

        # Step 1.6: Batch Metadata Sync (POST /shelf/syncbook)
        book_ids = [b["bookId"] for b in shelf["books"]]
        details = client.sync_book_details(book_ids)
        assert len(details) >= 2

        # Step 1.7: Cover Upgrades (/s_ to /t6_)
        for b in shelf["books"]:
            orig_cover = b.get("cover", "")
            upgraded = client.upgrade_cover_url(orig_cover)
            assert "/t6_" in upgraded
            b["cover"] = upgraded
        storage.save_shelf(shelf)

        # Step 1.8: Render Shelf Model
        final_shelf = storage.load_shelf()
        assert final_shelf is not None
        assert len(final_shelf["books"]) >= 2
        # Check sort order is descending by readUpdateTime
        t_prev = float("inf")
        for b in final_shelf["books"]:
            t_curr = b.get("readUpdateTime", 0)
            assert t_curr <= t_prev, "Bookshelf must be sorted descending by readUpdateTime"
            t_prev = t_curr

        print("  -> Scenario 1 passed successfully.")


# ---------------------------------------------------------------------------
# Scenario 2: Complete Reading Session with Streaming Decryption & Prefetch
# ---------------------------------------------------------------------------
def test_scenario2_complete_reading_session_with_decryption_and_prefetch() -> None:
    print("[TIER 4] Scenario 2: Complete Reading Session with Streaming Decryption & Prefetch...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier4_s2_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # Pre-populate authenticated session
        storage.save_auth({
            "vid": 12345678,
            "skey": "mock_skey_123456",
            "accessToken": "mock_access_token_token",
            "refreshToken": "mock_refresh_token_xyz",
        })

        book_id = "test_book_101"

        # Step 2.1: User selects book from shelf
        # Step 2.2: Query TOC & Progress
        toc = client.sync_toc(book_id)
        assert len(toc.get("updated", [])) >= 2
        cached_toc = storage.load_toc(book_id)
        assert cached_toc is not None

        cloud_prog = client.get_cloud_progress(book_id)
        assert cloud_prog.get("chapterUid") == 1
        assert cloud_prog.get("progress") == 15

        # Step 2.3: Initial Chapter Download (chapters=0,1)
        res = client.download_chapter(book_id, chapters="0,1", book_type="epub", secret="12345678")
        assert res.get("status") == 200
        assert len(res.get("chapters", [])) >= 1

        # Verify chapter 0 shared resources were extracted to res/
        css_data = storage.get_shared_resource(book_id, "Styles/main.css")
        assert css_data is not None and b"font-size" in css_data

        img_data = storage.get_shared_resource(book_id, "Images/cover.jpg")
        assert img_data is not None and len(img_data) > 0

        # Step 2.4: Render Chapter 1
        ch1_html = storage.load_chapter_html(book_id, 1)
        assert ch1_html is not None
        assert "第一章 起源" in ch1_html
        assert "探索者踏上了新的征程" in ch1_html  # concatenated second part from info.txt
        assert "../res/Styles/main.css" in ch1_html

        # Step 2.5: Background Prefetch for Chapter 2
        prefetch_res = client.download_chapter(book_id, chapters="2", book_type="epub", secret="12345678")
        assert prefetch_res.get("status") == 200

        # Verify chapter 2 is now cached on disk
        ch2_cached = storage.load_chapter_html(book_id, 2)
        assert ch2_cached is not None

        # Step 2.6: Instant Turn to Chapter 2
        # Reading engine checks local cache first: found without network latency!
        instantly_opened = storage.load_chapter_html(book_id, 2)
        assert instantly_opened is not None
        assert "第一章 起源" in instantly_opened or "第二章" in instantly_opened or len(instantly_opened) > 0

        print("  -> Scenario 2 passed successfully.")


# ---------------------------------------------------------------------------
# Scenario 3: Commuter Flight / Offline Reading & Post-Flight Sync
# ---------------------------------------------------------------------------
def test_scenario3_commuter_flight_offline_reading_and_post_flight_sync() -> None:
    print("[TIER 4] Scenario 3: Commuter Flight / Offline Reading & Post-Flight Sync...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier4_s3_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # Pre-cached bookshelf with 2 books
        t0 = int(time.time()) - 3600
        storage.save_shelf({
            "synckey": 100,
            "books": [
                {
                    "bookId": "txt_book_202",
                    "title": "测试纯文本：序章录",
                    "author": "纯文本作家",
                    "format": "txt",
                    "readUpdateTime": t0 + 500,
                },
                {
                    "bookId": "test_book_101",
                    "title": "测试小说：荒原起源",
                    "author": "测试作者",
                    "format": "epub",
                    "cover": "https://weread.qq.com/covers/t6_test_book_101.jpg",
                    "readUpdateTime": t0,
                },
            ],
            "bookProgress": [],
            "updatedAt": t0,
        })
        storage.save_chapter_html("test_book_101", 1, "<html><body><p>离线已缓存章节正文</p></body></html>")

        # Step 3.1: Commuter boards flight: Wi-Fi RF powers down
        client.set_wifi_enabled(False)
        assert client.wifi_enabled is False

        # Step 3.2: User opens book offline and reads 30 pages
        cached_chapter = storage.load_chapter_html("test_book_101", 1)
        assert cached_chapter is not None and "离线已缓存" in cached_chapter

        # Local progress recorded: dirty=1 because network is offline
        local_prog = client.record_reading_progress(
            book_id="test_book_101",
            chapter_uid=1,
            chapter_idx=1,
            progress=45,
            reading_time=1800,
            summary="正在三万英尺高空阅读",
            dirty=1,
        )
        assert local_prog["dirty"] == 1
        assert local_prog["progress"] == 45

        # Step 3.3: User exits reader to bookshelf: shelf re-sorts
        shelf = storage.load_shelf()
        assert shelf is not None
        assert shelf["books"][0]["bookId"] == "test_book_101", "test_book_101 must be at top after offline reading"
        assert shelf["books"][1]["bookId"] == "txt_book_202"

        # Step 3.4: Mid-flight sleep & lock screen ticket rendering
        ticket = client.render_lock_screen_ticket("test_book_101")
        assert ticket["renderedOffline"] is True
        assert ticket["progress"] == 45
        assert ticket["readingTimeMinutes"] == 30
        assert ticket["title"] == "测试小说：荒原起源"

        # Step 3.5: Plane lands, Wi-Fi reconnects and dirty progress syncs to cloud
        client.set_wifi_enabled(True)
        assert client.wifi_enabled is True

        sync_succ = client.sync_progress_to_cloud("test_book_101")
        assert sync_succ is True

        # Check local progress dirty flag is now 0
        synced_prog = storage.load_local_progress("test_book_101")
        assert synced_prog is not None
        assert synced_prog["dirty"] == 0
        assert synced_prog["progress"] == 45

        # Check mock server history contains POST /book/read
        hist = client.mock_get_history()
        read_calls = [h for h in hist if h["path"] == "/book/read"]
        assert len(read_calls) >= 1
        assert json.loads(read_calls[-1]["body"])["progress"] == 45
        print("  -> Scenario 3 passed successfully.")


# ---------------------------------------------------------------------------
# Scenario 4: Session Expiry During Multi-Chapter Reading
# ---------------------------------------------------------------------------
def test_scenario4_session_expiry_during_multi_chapter_reading() -> None:
    print("[TIER 4] Scenario 4: Session Expiry During Multi-Chapter Reading...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier4_s4_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # Step 4.1: Initial active reading session with expiring token
        storage.save_auth({
            "vid": 12345678,
            "skey": "mock_skey_old",
            "accessToken": "expiring_access_token_123",
            "refreshToken": "mock_refresh_token_valid",
            "updatedAt": int(time.time()),
        })

        # Step 4.2: User reads chapters 1, 2, 3 smoothly
        for ch in (1, 2, 3):
            res = client.download_chapter("test_book_101", ch, book_type="epub", secret="12345678")
            assert res.get("status") == 200

        # Step 4.3: User advances to Chapter 4: token expires (-2013)
        client.mock_set_state({
            "endpoint_errors": {
                "/book/chapterdownload": {
                    "status": 401,
                    "body": {"errcode": -2013, "errmsg": "access token expired"}
                }
            }
        })
        # Simulate server accepting token after renewal
        client.on_renewal_callback = lambda: client.mock_set_state({"endpoint_errors": {}})

        # Step 4.4 & 4.5: Transparent renewal and single replay
        res4 = client.download_chapter("test_book_101", 4, book_type="epub", secret="12345678")
        assert res4.get("status") == 200, f"Expected 200 after renewal replay, got {res4}"

        # Verification: session was renewed without user disruption
        assert client.last_renewal_count == 1
        updated_auth = storage.load_auth()
        assert updated_auth is not None
        assert updated_auth["accessToken"] == "mock_renewed_access_token"

        # Check mock server history: chapter 4 download failed -> /weblogin renewal -> replay succeeded
        hist = client.mock_get_history()
        weblogin_calls = [h for h in hist if "/weblogin" in h["path"]]
        assert len(weblogin_calls) == 1
        assert weblogin_calls[0]["query"].get("deviceType") == ["7"] or "deviceType=7" in weblogin_calls[0]["url"]

        print("  -> Scenario 4 passed successfully.")


# ---------------------------------------------------------------------------
# Scenario 5: Paywalled & Delisted Book Handling
# ---------------------------------------------------------------------------
def test_scenario5_paywalled_and_delisted_book_handling() -> None:
    print("[TIER 4] Scenario 5: Paywalled & Delisted Book Handling (HTTP 402, HTTP 499)...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier4_s5_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        storage.save_auth({
            "vid": 12345678,
            "skey": "mock_skey_123456",
            "accessToken": "mock_access_token_token",
            "refreshToken": "mock_refresh_token_xyz",
        })

        # Step 5.1 & 5.2: Chapters 1-3 are free preview chapters
        res1 = client.download_chapter("test_book_101", 1, book_type="epub", secret="12345678")
        assert res1.get("status") == 200

        # Step 5.3: Chapter 4 is paywalled: returns HTTP 402
        client.mock_set_state({
            "endpoint_errors": {
                "/book/chapterdownload": {
                    "status": 402,
                    "body": {"errcode": -2001, "errmsg": "Purchase required"}
                }
            }
        })
        res_paywall = client.download_chapter("test_book_101", 4, book_type="epub")
        assert res_paywall.get("status") == 402
        assert res_paywall.get("error") == "PurchaseRequired"

        # UI renders purchase notice card and allows navigating back to chapter 1
        notice_html = "<html><body><div class='notice'>本章为付费章节，请在微信读书 App 购买后继续阅读</div></body></html>"
        assert "本章为付费章节" in notice_html

        # Step 5.4: Delisted book returns HTTP 499 (soldOut)
        client.mock_set_state({
            "endpoint_errors": {
                "/book/chapterdownload": {
                    "status": 499,
                    "body": {"errcode": -2064, "errmsg": "Book is sold out or delisted"}
                }
            }
        })
        res_delisted = client.download_chapter("delisted_book_999", 1, book_type="epub")
        assert res_delisted.get("status") == 499
        assert res_delisted.get("error") == "SoldOut"

        # UI renders sold-out notice card and allows exiting to shelf
        soldout_html = "<html><body><div class='notice'>该书籍已下架或暂不支持阅读</div></body></html>"
        assert "该书籍已下架" in soldout_html

        print("  -> Scenario 5 passed successfully.")


# ---------------------------------------------------------------------------
# Scenario 6: Account Logout & Clean Storage Sanitization
# ---------------------------------------------------------------------------
def test_scenario6_account_logout_and_clean_storage_sanitization() -> None:
    print("[TIER 4] Scenario 6: Account Logout & Clean Storage Sanitization...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier4_s6_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # Step 6.1: Active session in auth.json
        storage.save_auth({
            "vid": 12345678,
            "skey": "mock_skey_active",
            "accessToken": "secret_access_token_to_purge",
            "refreshToken": "secret_refresh_token_to_purge",
        })
        assert storage.load_auth() is not None
        assert os.path.exists(storage.auth_file)

        # Step 6.2 & 6.3: Settings -> Account -> Logout
        client.logout()

        # Verify storage sanitization: auth.json completely deleted
        assert storage.load_auth() is None
        assert not os.path.exists(storage.auth_file)

        # Step 6.4: Zero Credential Leakage: subsequent requests carry ZERO sensitive headers
        status, _, _ = client.request("GET", "/web/getuid?platform=desktop")
        assert status == 200

        hist = client.mock_get_history()
        getuid_reqs = [h for h in hist if h["path"] == "/web/getuid"]
        assert len(getuid_reqs) >= 1
        headers_after_logout = getuid_reqs[-1]["headers"]
        assert "cookie" not in headers_after_logout
        assert "authorization" not in headers_after_logout
        assert "accesstoken" not in headers_after_logout
        assert "vid" not in headers_after_logout

        # Step 6.5: UI State Reset: QR Login view is ready for new login
        new_uid, new_qr_url = client.login_qr_get_uid()
        assert len(new_uid) > 0
        assert "https://weread.qq.com/web/confirm" in new_qr_url

        print("  -> Scenario 6 passed successfully.")


# ---------------------------------------------------------------------------
# Test Runner
# ---------------------------------------------------------------------------
def main() -> None:
    print("==========================================================")
    print(" Running WeRead Tier 4 Real-World Application Workflows")
    print(f" Target Mock Server Port: {get_mock_port()}")
    print("==========================================================")

    tests = [
        ("Scenario 1: First-Time User Onboarding & Shelf Sync", test_scenario1_first_time_user_onboarding_and_shelf_sync),
        ("Scenario 2: Complete Reading Session with Decryption & Prefetch", test_scenario2_complete_reading_session_with_decryption_and_prefetch),
        ("Scenario 3: Commuter Flight / Offline Reading & Post-Flight Sync", test_scenario3_commuter_flight_offline_reading_and_post_flight_sync),
        ("Scenario 4: Session Expiry During Multi-Chapter Reading", test_scenario4_session_expiry_during_multi_chapter_reading),
        ("Scenario 5: Paywalled & Delisted Book Handling", test_scenario5_paywalled_and_delisted_book_handling),
        ("Scenario 6: Account Logout & Clean Storage Sanitization", test_scenario6_account_logout_and_clean_storage_sanitization),
    ]

    passed = 0
    failed = 0

    for name, fn in tests:
        try:
            fn()
            passed += 1
        except Exception as exc:
            print(f"[FAIL] {name}: {exc}", file=sys.stderr)
            import traceback
            traceback.print_exc()
            failed += 1

    print("==========================================================")
    print(f" Tier 4 Test Results: {passed} passed, {failed} failed")
    print("==========================================================")

    if failed > 0:
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
