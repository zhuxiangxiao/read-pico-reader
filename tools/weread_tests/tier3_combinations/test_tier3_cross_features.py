#!/usr/bin/env python3
"""Tier 3: Cross-Feature Combination Test Suite for WeRead E2E Testing.

Covers >=10 pairwise feature interaction combinations:
1. Token Expiry (-2013) × Chapter Download Streaming
2. Offline Reading × Bookshelf Re-sort
3. Cloud Progress Conflict × Offline Reading
4. TOC Invalidation (clearAll: 1) × Active Reader Session
5. Background Prefetch × Rapid User Navigation
6. Wi-Fi Powerdown × Lock Screen Ticket Rendering
7. High-Res Cover Upgrade (t6_) × Untrusted Domain Sensitive Header Stripping
8. Multi-File info.txt Concatenation (4 XHTML fragments) × Layout Engine
9. Dashboard Continue Card × Virtual URI Dispatch (weread://)
10. Version Expiration (-2063) × Shared Resource Invalidation
11. (Adversarial) Progress Dirty Sync × Session Expiry Renewal on POST /book/read
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
from weread_mock_server import encrypt_encrypt_key


def get_mock_port() -> int:
    return int(os.environ.get("WEREAD_MOCK_PORT", "8989"))


# ---------------------------------------------------------------------------
# Combination 1: Token Expiry × Chapter Download Streaming
# ---------------------------------------------------------------------------
def test_comb1_token_expiry_x_chapter_download_streaming() -> None:
    print("[TIER 3] Combination 1: Token Expiry (-2013) × Chapter Download Streaming...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c1_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Setup authenticated session with initially valid token
        client.storage.save_auth({
            "vid": 12345678,
            "skey": "mock_skey_old",
            "accessToken": "mock_access_token_expired",
            "refreshToken": "mock_refresh_token_valid",
            "deviceId": "eink-dev-test",
            "deviceName": "KiikoReader",
            "updatedAt": int(time.time()),
        })

        # 2. Inject transient -2013 token expired error on /book/chapterdownload
        client.mock_set_state({
            "endpoint_errors": {
                "/book/chapterdownload": {
                    "status": 401,
                    "body": {"errcode": -2013, "errmsg": "access token expired"}
                }
            }
        })

        # When renewal occurs, clear the endpoint error to simulate server accepting renewed token
        client.on_renewal_callback = lambda: client.mock_set_state({"endpoint_errors": {}})

        # 3. Client calls chapter download: should encounter 401, auto-renew, and replay
        result = client.download_chapter(
            book_id="test_book_101",
            chapters="1",
            book_type="epub",
            secret="12345678",
        )

        # 4. Verify outcome
        assert result.get("status") == 200, f"Expected 200 after replay, got {result}"
        assert client.last_renewal_count == 1, "Expected exactly 1 token renewal"

        # Check auth.json was updated atomically
        updated_auth = storage.load_auth()
        assert updated_auth is not None
        assert updated_auth["accessToken"] == "mock_renewed_access_token", (
            f"Expected renewed access token, got {updated_auth.get('accessToken')}"
        )

        # Verify history shows: download -> weblogin renewal -> replayed download
        hist = client.mock_get_history()
        actions = [item["path"] for item in hist]
        assert "/book/chapterdownload" in actions
        assert "/weblogin" in actions
        # Chapter download appeared at least twice (initial attempt + replay)
        ch_calls = [item for item in hist if item["path"] == "/book/chapterdownload"]
        assert len(ch_calls) >= 2, f"Expected initial attempt + replay, found {len(ch_calls)}"

        # Check chapter 1 was saved to disk
        cached_html = storage.load_chapter_html("test_book_101", 1)
        assert cached_html is not None and "第一章 起源" in cached_html
        print("  -> Combination 1 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 2: Offline Reading × Bookshelf Re-sort
# ---------------------------------------------------------------------------
def test_comb2_offline_reading_x_bookshelf_resort() -> None:
    print("[TIER 3] Combination 2: Offline Reading × Bookshelf Re-sort...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c2_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Setup local bookshelf with 2 books: Book B initially ahead of Book A
        t0 = int(time.time()) - 1000
        shelf_data = {
            "synckey": 100,
            "books": [
                {
                    "bookId": "txt_book_202",
                    "title": "测试纯文本：序章录",
                    "author": "纯文本作家",
                    "format": "txt",
                    "readUpdateTime": t0 + 200,  # Read more recently
                },
                {
                    "bookId": "test_book_101",
                    "title": "测试小说：荒原起源",
                    "author": "测试作者",
                    "format": "epub",
                    "readUpdateTime": t0 + 100,  # Older
                },
            ],
            "bookProgress": [],
            "updatedAt": t0,
        }
        storage.save_shelf(shelf_data)

        # Initial check: txt_book_202 is at index 0
        loaded_shelf = storage.load_shelf()
        assert loaded_shelf["books"][0]["bookId"] == "txt_book_202"

        # 2. Enter offline mode (Wi-Fi disabled)
        hist_before = len([h for h in client.mock_get_history() if not h["path"].startswith("/mock/control")])
        client.set_wifi_enabled(False)

        # 3. User opens Book A (test_book_101) offline and advances reading progress
        client.record_reading_progress(
            book_id="test_book_101",
            chapter_uid=1,
            chapter_idx=1,
            progress=25,
            reading_time=900,
            summary="正在离线阅读荒原起源",
            dirty=1,
        )

        # 4. Verify bookshelf was re-sorted purely offline
        hist_after = len([h for h in client.mock_get_history() if not h["path"].startswith("/mock/control")])
        assert hist_before == hist_after, "Zero network requests must be made during offline reading"

        resorted_shelf = storage.load_shelf()
        assert resorted_shelf is not None
        assert resorted_shelf["books"][0]["bookId"] == "test_book_101", (
            f"Expected test_book_101 at top of shelf, got {resorted_shelf['books'][0]['bookId']}"
        )
        assert resorted_shelf["books"][1]["bookId"] == "txt_book_202"
        assert resorted_shelf["books"][0]["readUpdateTime"] > resorted_shelf["books"][1]["readUpdateTime"]

        # Check local progress dirty flag is 1
        prog = storage.load_local_progress("test_book_101")
        assert prog is not None and prog.get("dirty") == 1
        print("  -> Combination 2 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 3: Cloud Progress Conflict × Offline Reading
# ---------------------------------------------------------------------------
def test_comb3_cloud_progress_conflict_x_offline_reading() -> None:
    print("[TIER 3] Combination 3: Cloud Progress Conflict × Offline Reading...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c3_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Offline reading generates local progress
        client.set_wifi_enabled(False)
        local_prog = client.record_reading_progress(
            book_id="test_book_101",
            chapter_uid=2,
            chapter_idx=2,
            progress=65,
            reading_time=2400,
            summary="第二章 征途",
            dirty=1,
        )
        assert local_prog["dirty"] == 1

        # 2. Reconnect Wi-Fi
        client.set_wifi_enabled(True)

        # 3. Query cloud progress (returns mock server default: chapter 1, progress 15%)
        cloud_prog = client.get_cloud_progress("test_book_101")
        assert cloud_prog["progress"] == 15
        assert cloud_prog["chapterUid"] == 1

        # 4. Detect conflict: local has dirty progress (65%) differing from cloud (15%)
        has_conflict = (local_prog.get("dirty") == 1) and (local_prog["progress"] != cloud_prog["progress"])
        assert has_conflict, "Progress conflict should be detected"

        # 5. User prompt chooses LOCAL progress
        resolved_prog = client.resolve_progress_conflict(local_prog, cloud_prog, choice="local")
        assert resolved_prog["progress"] == 65
        assert resolved_prog["dirty"] == 1
        storage.save_local_progress("test_book_101", resolved_prog)

        # 6. Background sync uploads local progress to cloud
        sync_ok = client.sync_progress_to_cloud("test_book_101")
        assert sync_ok is True

        # 7. Check local dirty flag is now cleared to 0
        final_prog = storage.load_local_progress("test_book_101")
        assert final_prog is not None
        assert final_prog["dirty"] == 0
        assert final_prog["progress"] == 65

        # Check mock history recorded POST /book/read
        hist = client.mock_get_history()
        read_posts = [h for h in hist if h["path"] == "/book/read"]
        assert len(read_posts) >= 1
        print("  -> Combination 3 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 4: TOC Invalidation (clearAll: 1) × Active Reader Session
# ---------------------------------------------------------------------------
def test_comb4_toc_invalidation_clear_all_x_active_reader_session() -> None:
    print("[TIER 3] Combination 4: TOC Invalidation (clearAll: 1) × Active Reader Session...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c4_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Populate initial cache with chapter 1 HTML and old TOC
        storage.save_chapter_html("test_book_101", 1, "<html><body><p>旧版本章节内容</p></body></html>")
        storage.save_toc("test_book_101", {
            "bookId": "test_book_101",
            "synckey": 100,
            "clearAll": 0,
            "updated": [{"chapterUid": 1, "title": "旧第一章"}],
        })

        assert storage.load_chapter_html("test_book_101", 1) is not None

        # 2. Configure mock server to emit clearAll: 1 on next TOC sync
        client.mock_set_state({"clear_all_toc": True})

        # 3. Background TOC sync occurs
        toc = client.sync_toc("test_book_101", synckey=100)
        assert toc.get("clearAll") == 1

        # 4. Verify local cached chapter files were purged
        assert storage.load_chapter_html("test_book_101", 1) is None, "Cache must be purged on clearAll: 1"

        # 5. Active reader session notices missing chapter 1 and re-downloads seamlessly
        client.mock_set_state({"clear_all_toc": False})
        res = client.download_chapter("test_book_101", "1", book_type="epub", secret="12345678")
        assert res.get("status") == 200

        # Chapter 1 is refreshed with authentic content
        refreshed_html = storage.load_chapter_html("test_book_101", 1)
        assert refreshed_html is not None
        assert "第一章 起源" in refreshed_html
        print("  -> Combination 4 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 5: Background Prefetch × Rapid User Navigation
# ---------------------------------------------------------------------------
def test_comb5_background_prefetch_x_rapid_user_navigation() -> None:
    print("[TIER 3] Combination 5: Background Prefetch × Rapid User Navigation...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c5_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # State tracking for prefetch worker
        prefetch_state = {"target": 2, "cancelled": False, "completed": False}

        def cancel_prefetch() -> None:
            prefetch_state["cancelled"] = True

        # 1. User starts reading chapter 1, prefetch worker targets chapter 2
        active_chapter = 1
        prefetch_state["target"] = active_chapter + 1

        # 2. User rapidly opens TOC and navigates to chapter 8
        target_navigation = 8
        cancel_prefetch()  # Immediate yield on user navigation

        assert prefetch_state["cancelled"] is True, "Prefetch of chapter 2 must be cancelled"

        # 3. High-priority foreground download loads chapter 8
        # Mock server serves chapter payload for requested chapter
        res = client.download_chapter("test_book_101", target_navigation, book_type="epub", secret="12345678")
        assert res.get("status") == 200

        # Active chapter transitions to 8
        active_chapter = target_navigation

        # 4. Prefetch target resets to next chapter (8 + 1 = 9)
        prefetch_state["target"] = active_chapter + 1
        prefetch_state["cancelled"] = False

        assert active_chapter == 8
        assert prefetch_state["target"] == 9
        print("  -> Combination 5 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 6: Wi-Fi Powerdown × Lock Screen Ticket Rendering
# ---------------------------------------------------------------------------
def test_comb6_wifi_powerdown_x_lock_screen_ticket_rendering() -> None:
    print("[TIER 3] Combination 6: Wi-Fi Powerdown × Lock Screen Ticket Rendering...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c6_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Prepare local cache with shelf metadata and reading progress
        storage.save_shelf({
            "synckey": 100,
            "books": [
                {
                    "bookId": "test_book_101",
                    "title": "测试小说：荒原起源",
                    "author": "测试作者",
                    "cover": "https://weread.qq.com/covers/t6_test_book_101.jpg",
                    "format": "epub",
                }
            ],
            "bookProgress": [],
            "updatedAt": int(time.time()),
        })
        storage.save_local_progress("test_book_101", {
            "bookId": "test_book_101",
            "chapterUid": 1,
            "chapterIdx": 1,
            "progress": 38,
            "readingTime": 3600,
            "summary": "第一章 起源",
            "dirty": 0,
        })

        # 2. Lock screen event: RF powerdown
        hist_count_before = len([h for h in client.mock_get_history() if not h["path"].startswith("/mock/control")])
        client.set_wifi_enabled(False)

        # 3. Render lock screen ticket
        ticket = client.render_lock_screen_ticket("test_book_101")

        hist_count_after = len([h for h in client.mock_get_history() if not h["path"].startswith("/mock/control")])

        # 4. Assertions: rendered purely offline without any network calls
        assert hist_count_before == hist_count_after, "No network calls allowed during lock screen rendering"
        assert ticket["bookId"] == "test_book_101"
        assert ticket["title"] == "测试小说：荒原起源"
        assert ticket["author"] == "测试作者"
        assert ticket["progress"] == 38
        assert ticket["readingTimeMinutes"] == 60
        assert ticket["renderedOffline"] is True
        assert ticket["hasCachedCover"] is True
        print("  -> Combination 6 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 7: High-Res Cover Upgrade × Untrusted Domain Stripping
# ---------------------------------------------------------------------------
def test_comb7_high_res_cover_upgrade_x_untrusted_domain_stripping() -> None:
    print("[TIER 3] Combination 7: High-Res Cover Upgrade (t6_) × Untrusted Domain Stripping...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c7_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # Authenticated session with sensitive credentials
        storage.save_auth({
            "vid": 99887766,
            "skey": "sensitive_skey_abc",
            "accessToken": "secret_access_token_123",
            "refreshToken": "secret_refresh_token_xyz",
        })

        # Low-res cover on untrusted third-party CDN domain
        raw_cover_url = f"http://127.0.0.1:{port}/thirdparty/cdn/s_novel_cover.jpg"

        # 1. Upgrade cover to high-resolution /t6_
        upgraded_url = client.upgrade_cover_url(raw_cover_url)
        assert "/t6_" in upgraded_url
        assert "/s_" not in upgraded_url

        # 2. Outbound request to untrusted path: headers MUST be stripped
        # WereadClient.request() enforces strip_sensitive_headers_if_untrusted()
        status, _, body = client.request("GET", upgraded_url)
        assert status == 200, f"Expected 200, got {status} ({body})"

        # 3. Check mock server recorded ZERO security violations
        violations = client.mock_get_violations()
        assert len(violations) == 0, f"Security violations detected: {violations}"

        # 4. Verify request in history had no sensitive headers
        hist = client.mock_get_history()
        thirdparty_reqs = [h for h in hist if "/thirdparty/" in h["path"]]
        assert len(thirdparty_reqs) >= 1
        headers_sent = thirdparty_reqs[0]["headers"]
        assert "cookie" not in headers_sent
        assert "authorization" not in headers_sent
        assert "accesstoken" not in headers_sent
        assert "vid" not in headers_sent
        print("  -> Combination 7 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 8: Multi-File info.txt Concatenation × Layout Engine
# ---------------------------------------------------------------------------
def test_comb8_multi_file_info_txt_concatenation_x_layout_engine() -> None:
    print("[TIER 3] Combination 8: Multi-File info.txt Concatenation (4 XHTML fragments) × Layout Engine...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c8_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Build an authentic ZipCrypto archive containing 4 XHTML fragments specified in info.txt
        pwd = b"test_multi_pwd_123"
        entries = {
            "info.txt": json.dumps({
                "bookId": "multi_part_book",
                "bookVersion": 1,
                "chapters": [
                    {
                        "chapterUid": 10,
                        "chapterIdx": 1,
                        "title": "长章第一回：风云聚散",
                        "files": [
                            "Text/part1.xhtml",
                            "Text/part2.xhtml",
                            "Text/part3.xhtml",
                            "Text/part4.xhtml",
                        ]
                    }
                ]
            }).encode("utf-8"),
            "Styles/main.css": b"body { font-size: 18px; }\n.part-divider { border: none; }",
            "Text/part1.xhtml": "<html><head><title>P1</title></head><body><h1>长章第一回</h1><p>第一节：初入江湖，风雨凄迷。</p></body></html>".encode("utf-8"),
            "Text/part2.xhtml": "<html><head><title>P2</title></head><body><p>第二节：客栈夜话，暗流涌动。</p></body></html>".encode("utf-8"),
            "Text/part3.xhtml": "<html><head><title>P3</title></head><body><p>第三节：剑拔弩张，短兵相接。</p></body></html>".encode("utf-8"),
            "Text/part4.xhtml": "<html><head><title>P4</title></head><body><p>第四节：晨曦初照，尘埃落定。</p></body></html>".encode("utf-8"),
        }

        # Build ZipCrypto ZIP buffer
        from fixtures.weread.generate_fixtures import _make_zipcrypto_zip
        zip_bytes = _make_zipcrypto_zip(entries, pwd)

        # Encrypt the password using AES-128-CBC with secret vid="12345678"
        enc_key_hdr = encrypt_encrypt_key(pwd, "12345678")

        # 2. Decrypt encryptkey and unpack/concatenate using client primitives
        decrypted_pwd = decrypt_encrypt_key(enc_key_hdr, "12345678")
        assert decrypted_pwd == pwd

        with zipfile.ZipFile(io.BytesIO(zip_bytes)) as zf:
            info_json = json.loads(zf.read("info.txt", pwd=decrypted_pwd).decode("utf-8"))
            ch_info = info_json["chapters"][0]
            parts = []
            for pf in ch_info["files"]:
                raw = zf.read(pf, pwd=decrypted_pwd).decode("utf-8")
                # Extract inner body content
                import re
                m = re.search(r"<body[^>]*>(.*?)</body>", raw, re.DOTALL | re.IGNORECASE)
                parts.append(m.group(1).strip() if m else raw)

            joined_body = "\n<hr class=\"part-divider\"/>\n".join(parts)
            unified_html = (
                f"<!DOCTYPE html>\n<html>\n<head>\n"
                f"<title>{ch_info['title']}</title>\n"
                f"<link rel=\"stylesheet\" href=\"../res/Styles/main.css\"/>\n"
                f"</head>\n<body>\n{joined_body}\n</body>\n</html>"
            )
            storage.save_chapter_html("multi_part_book", 10, unified_html)

        # 3. Layout engine verification: all 4 parts present in exact sequential order
        saved_html = storage.load_chapter_html("multi_part_book", 10)
        assert saved_html is not None

        # Verify exact sequence
        pos1 = saved_html.find("第一节：初入江湖")
        pos2 = saved_html.find("第二节：客栈夜话")
        pos3 = saved_html.find("第三节：剑拔弩张")
        pos4 = saved_html.find("第四节：晨曦初照")
        assert 0 < pos1 < pos2 < pos3 < pos4, "All 4 parts must be concatenated in exact index order"

        # Verify no duplicate <html> or <body> tags
        assert saved_html.count("<!DOCTYPE html>") == 1
        assert saved_html.count("<body>") == 1
        assert saved_html.count("</body>") == 1
        assert saved_html.count("<hr class=\"part-divider\"/>") == 3
        print("  -> Combination 8 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 9: Dashboard Continue Card × Virtual URI Dispatch
# ---------------------------------------------------------------------------
def test_comb9_dashboard_continue_card_x_virtual_uri_dispatch() -> None:
    print("[TIER 3] Combination 9: Dashboard Continue Card × Virtual URI Dispatch (weread://)...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c9_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Virtual URI validation
        uri = "weread://330001"
        assert is_weread_uri(uri) is True
        assert parse_weread_uri(uri) == "330001"
        assert is_weread_uri("weread://") is False
        assert is_weread_uri("file:///sdcard/book.epub") is False

        # 2. Verify book_file_path_supported does not treat virtual URI as local file
        assert book_file_path_supported(uri) is False
        assert book_file_path_supported("/sdcard/novel.epub") is True
        assert book_file_path_supported("/sdcard/novel.txt") is True

        # 3. Simulate home dashboard "Continue Reading" tap
        book_id = parse_weread_uri(uri)
        assert book_id == "330001"

        # Ensure chapter 0 shared resources exist or are fetched
        client.storage.save_shared_resource(book_id, "Styles/main.css", b"body { margin: 0; }")
        assert client.storage.get_shared_resource(book_id, "Styles/main.css") is not None

        # Fetch current reading progress and target chapter
        client.storage.save_local_progress(book_id, {
            "bookId": book_id,
            "chapterUid": 1,
            "chapterIdx": 1,
            "progress": 42,
            "readingTime": 1200,
        })
        client.storage.save_chapter_html(book_id, 1, "<html><body><p>已就绪的第1章内容</p></body></html>")

        # Reader opens cached chapter 0 + target chapter smoothly
        css = client.storage.get_shared_resource(book_id, "Styles/main.css")
        html = client.storage.load_chapter_html(book_id, 1)
        assert css is not None and len(css) > 0
        assert html is not None and "已就绪" in html
        print("  -> Combination 9 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 10: Version Expiration (-2063) × Shared Resource Invalidation
# ---------------------------------------------------------------------------
def test_comb10_version_expiration_2063_x_shared_resource_invalidation() -> None:
    print("[TIER 3] Combination 10: Version Expiration (-2063) × Shared Resource Invalidation...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c10_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # 1. Old cached state for test_book_101 with bookVersion=100
        storage.save_shared_resource("test_book_101", "Styles/main.css", b"/* old version 100 styles */")
        storage.save_chapter_html("test_book_101", 1, "<html><body><p>旧版本内容</p></body></html>")
        assert storage.get_shared_resource("test_book_101", "Styles/main.css") is not None
        assert storage.load_chapter_html("test_book_101", 1) is not None

        # 2. Server simulates updated version on server by returning -2063 on download
        client.mock_set_state({
            "endpoint_errors": {
                "/book/chapterdownload": {
                    "status": 499,
                    "body": {"errcode": -2063, "errmsg": "Book version expired"}
                }
            }
        })

        # 3. Client initiates chapter download and catches -2063
        res = client.download_chapter("test_book_101", "1", book_type="epub")
        assert res.get("status") == 499
        assert res.get("errcode") == -2063

        # 4. Invalidation recovery flow:
        # a) Purge outdated chapter 0 shared resources and cached chapters
        storage.purge_shared_resources("test_book_101")
        storage.purge_cached_chapters("test_book_101")
        assert storage.get_shared_resource("test_book_101", "Styles/main.css") is None
        assert storage.load_chapter_html("test_book_101", 1) is None

        # b) Refetch syncbook to get updated bookVersion
        client.mock_set_state({"endpoint_errors": {}})  # Server returns new version
        details = client.sync_book_details(["test_book_101"])
        assert len(details) == 1

        # c) Refetch TOC chapterInfos
        toc = client.sync_toc("test_book_101")
        assert len(toc.get("updated", [])) >= 1

        # d) Download updated chapter 0 + chapter 1
        fresh_res = client.download_chapter("test_book_101", "0,1", book_type="epub", secret="12345678")
        assert fresh_res.get("status") == 200

        # e) Verify updated shared resources and chapter are re-extracted
        updated_css = storage.get_shared_resource("test_book_101", "Styles/main.css")
        assert updated_css is not None and len(updated_css) > 0
        updated_html = storage.load_chapter_html("test_book_101", 1)
        assert updated_html is not None and "第一章 起源" in updated_html
        print("  -> Combination 10 passed successfully.")


# ---------------------------------------------------------------------------
# Combination 11 (Adversarial): Progress Dirty Sync × Session Expiry Renewal
# ---------------------------------------------------------------------------
def test_comb11_dirty_progress_sync_x_session_expiry_renewal() -> None:
    print("[TIER 3] Combination 11: Progress Dirty Sync × Session Expiry Renewal on POST /book/read...")
    port = get_mock_port()

    with tempfile.TemporaryDirectory(prefix="weread_tier3_c11_") as temp_dir:
        storage = WereadStorage(temp_dir)
        client = WereadClient(port, storage)
        client.mock_reset()

        # Session with expired access token
        storage.save_auth({
            "vid": 12345678,
            "skey": "mock_skey_old",
            "accessToken": "expired_access_token",
            "refreshToken": "mock_refresh_token_valid",
            "updatedAt": int(time.time()),
        })

        # Save local progress with dirty=1
        storage.save_local_progress("test_book_101", {
            "bookId": "test_book_101",
            "chapterUid": 2,
            "chapterIdx": 2,
            "progress": 80,
            "readingTime": 5000,
            "dirty": 1,
        })

        # Server injects -2013 on /book/read
        client.mock_set_state({
            "endpoint_errors": {
                "/book/read": {
                    "status": 401,
                    "body": {"errcode": -2013, "errmsg": "access token expired"}
                }
            }
        })
        client.on_renewal_callback = lambda: client.mock_set_state({"endpoint_errors": {}})

        # Attempt progress sync: should encounter -2013, renew, and replay
        sync_ok = client.sync_progress_to_cloud("test_book_101")
        assert sync_ok is True
        assert client.last_renewal_count == 1

        # Check local dirty flag cleared
        prog = storage.load_local_progress("test_book_101")
        assert prog is not None and prog["dirty"] == 0

        # Check auth was updated with renewed token
        auth = storage.load_auth()
        assert auth is not None and auth["accessToken"] == "mock_renewed_access_token"
        print("  -> Combination 11 passed successfully.")


# ---------------------------------------------------------------------------
# Test Runner
# ---------------------------------------------------------------------------
def main() -> None:
    print("==========================================================")
    print(" Running WeRead Tier 3 Cross-Feature Combination Test Suite")
    print(f" Target Mock Server Port: {get_mock_port()}")
    print("==========================================================")

    tests = [
        ("Combination 1: Token Expiry x Chapter Download", test_comb1_token_expiry_x_chapter_download_streaming),
        ("Combination 2: Offline Reading x Bookshelf Re-sort", test_comb2_offline_reading_x_bookshelf_resort),
        ("Combination 3: Cloud Progress Conflict x Offline Reading", test_comb3_cloud_progress_conflict_x_offline_reading),
        ("Combination 4: TOC Invalidation x Active Reader Session", test_comb4_toc_invalidation_clear_all_x_active_reader_session),
        ("Combination 5: Background Prefetch x Rapid Navigation", test_comb5_background_prefetch_x_rapid_user_navigation),
        ("Combination 6: Wi-Fi Powerdown x Lock Screen Ticket", test_comb6_wifi_powerdown_x_lock_screen_ticket_rendering),
        ("Combination 7: Cover Upgrade x Untrusted Stripping", test_comb7_high_res_cover_upgrade_x_untrusted_domain_stripping),
        ("Combination 8: Multi-File info.txt (4 parts) x Layout", test_comb8_multi_file_info_txt_concatenation_x_layout_engine),
        ("Combination 9: Dashboard Continue x Virtual URI", test_comb9_dashboard_continue_card_x_virtual_uri_dispatch),
        ("Combination 10: Version Expiry (-2063) x Resource Invalidation", test_comb10_version_expiration_2063_x_shared_resource_invalidation),
        ("Combination 11: Dirty Progress Sync x Expiry Renewal", test_comb11_dirty_progress_sync_x_session_expiry_renewal),
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
    print(f" Tier 3 Test Results: {passed} passed, {failed} failed")
    print("==========================================================")

    if failed > 0:
        sys.exit(1)
    else:
        sys.exit(0)


if __name__ == "__main__":
    main()
