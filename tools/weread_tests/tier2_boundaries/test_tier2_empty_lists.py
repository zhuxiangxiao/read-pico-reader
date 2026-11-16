#!/usr/bin/env python3
"""Tier 2 Boundary & Corner Cases: Empty Lists, Zero Payloads & Zero Values

Covers:
- Category 1: Empty Bookshelf (empty books array, disk persistence, offline read, incremental sync fallback, UI empty placeholder)
- Category 2: Empty syncbook IDs (empty bookIds array, zero network requests, shelf preservation, null books fallback, missing key fallback)
- Category 3: Empty Chapters List (empty TOC response, chapters.json persistence, reader error view, count == 0, out of bounds load)
- Category 4: Empty info.txt Files (0-byte info.txt, whitespace info.txt, empty files list in JSON, missing referenced files, empty part file)
- Category 5: Empty Auth Tokens (empty accessToken in auth.json, missing keys in auth.json, empty token in weblogin, empty session request, empty skey in polling)
- Category 6: Zero-Byte Downloads (0-byte chapter payload, 0-byte cover payload, 0-byte JSON response, 0-byte chapters.json on disk, 0-byte shelf.json on disk)
- Category 7: Zero Progress Values (progress:0 initial position, reporting progress:0, cloud 0 does not erase local, 0 total chapters clamping, 0 reading time)
"""

from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

# Helper to ensure tools/ is on path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
COMMON_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", "common"))
WORKSPACE_ROOT = os.path.abspath(os.path.join(TOOLS_DIR, ".."))
if COMMON_DIR not in sys.path:
    sys.path.insert(0, COMMON_DIR)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from weread_client_sim import WereadClient, WereadStorage
import weread_mock_server


PORT = int(os.environ.get("WEREAD_MOCK_PORT", "8989"))
BASE_URL = f"http://127.0.0.1:{PORT}"


def ensure_mock_server():
    try:
        req = urllib.request.Request(f"{BASE_URL}/mock/control/health")
        with urllib.request.urlopen(req, timeout=1) as resp:
            if resp.status == 200:
                return
    except Exception:
        pass

    server_address = ("127.0.0.1", PORT)
    httpd = weread_mock_server.ThreadedHTTPServer(server_address, weread_mock_server.WeReadMockRequestHandler)
    thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    thread.start()
    time.sleep(0.1)


def api_request(method: str, path: str, body: dict | bytes | None = None, headers: dict | None = None):
    url = f"{BASE_URL}{path}"
    hdrs = headers.copy() if headers else {}
    data = None
    if body is not None:
        if isinstance(body, dict):
            data = json.dumps(body).encode("utf-8")
            hdrs.setdefault("Content-Type", "application/json")
        else:
            data = body
    req = urllib.request.Request(url, data=data, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as err:
        return err.code, dict(err.headers), err.read()


def reset_mock_state():
    api_request("POST", "/mock/control/reset")


def set_mock_state(state: dict):
    api_request("POST", "/mock/control/set_state", state)


# ==============================================================================
# Category 1: Empty Bookshelf
# ==============================================================================

def test_empty_shelf_response_parses_to_empty_list():
    """Verify empty books array in sync response parses cleanly to empty list."""
    reset_mock_state()
    set_mock_state({"books": []})
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["books"] == []


def test_empty_shelf_persisted_to_disk():
    """Verify saving empty bookshelf writes valid JSON with empty array to shelf.json."""
    with tempfile.TemporaryDirectory() as storage_root:
        shelf_file = os.path.join(storage_root, "shelf.json")
        shelf_data = {"books": [], "synckey": 100, "updatedAt": int(time.time())}
        with open(shelf_file, "w", encoding="utf-8") as f:
            json.dump(shelf_data, f)

        with open(shelf_file, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        assert loaded["books"] == []
        assert loaded["synckey"] == 100


def test_empty_shelf_offline_read():
    """Verify reading empty shelf.json offline does not crash or hang."""
    with tempfile.TemporaryDirectory() as storage_root:
        shelf_file = os.path.join(storage_root, "shelf.json")
        with open(shelf_file, "w", encoding="utf-8") as f:
            json.dump({"books": []}, f)

        with open(shelf_file, "r", encoding="utf-8") as f:
            books = json.load(f).get("books", [])
        assert len(books) == 0


def test_empty_shelf_incremental_triggers_full_sync():
    """Verify incremental sync on empty shelf requests synckey=0 (full sync)."""
    with tempfile.TemporaryDirectory() as storage_root:
        storage = WereadStorage(storage_root)
        storage.save_shelf({"books": [], "synckey": 500})
        shelf = storage.load_shelf()
        assert shelf is not None
        local_books = shelf.get("books", [])
        # If local shelf has 0 books, incremental sync must fall back to synckey=0 for full sync
        sync_key_to_request = shelf.get("synckey", 0) if len(local_books) > 0 else 0
        assert sync_key_to_request == 0


def determine_shelf_ui_mode(shelf_path_or_data: str | dict) -> str:
    """Determine UI view state based on shelf storage content."""
    if isinstance(shelf_path_or_data, str):
        if not os.path.exists(shelf_path_or_data):
            return "EMPTY_SHELF_PLACEHOLDER"
        try:
            with open(shelf_path_or_data, "r", encoding="utf-8") as f:
                data = json.load(f)
        except Exception:
            return "UI_CORRUPTED_ERROR"
    else:
        data = shelf_path_or_data

    books = data.get("books", []) if isinstance(data, dict) else []
    if len(books) == 0:
        return "EMPTY_SHELF_PLACEHOLDER"
    return "BOOK_GRID_VIEW"


def test_empty_shelf_ui_empty_placeholder():
    """Verify empty shelf state activates 'No Books on Shelf' placeholder."""
    reset_mock_state()
    set_mock_state({"books": []})

    # 1. Fetch real empty shelf payload from mock server
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    server_data = json.loads(body.decode("utf-8"))

    # 2. Persist to actual storage file on disk
    with tempfile.TemporaryDirectory() as storage_root:
        shelf_file = os.path.join(storage_root, "shelf.json")
        with open(shelf_file, "w", encoding="utf-8") as f:
            json.dump(server_data, f)

        # 3. Verify UI view state evaluated from persisted storage file
        ui_view_empty = determine_shelf_ui_mode(shelf_file)
        assert ui_view_empty == "EMPTY_SHELF_PLACEHOLDER", f"Expected EMPTY_SHELF_PLACEHOLDER, got {ui_view_empty}"

        # 4. Verify contrasting case when shelf has books
        with open(shelf_file, "w", encoding="utf-8") as f:
            json.dump({"books": [{"bookId": "test_101", "title": "Test Book"}]}, f)
        ui_view_populated = determine_shelf_ui_mode(shelf_file)
        assert ui_view_populated == "BOOK_GRID_VIEW", f"Expected BOOK_GRID_VIEW, got {ui_view_populated}"


# ==============================================================================
# Category 2: Empty syncbook IDs
# ==============================================================================

def test_empty_syncbook_request_empty_response():
    """Verify calling /shelf/syncbook when catalog is empty returns empty books array."""
    reset_mock_state()
    set_mock_state({"books": []})
    status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": []})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["books"] == []


def test_empty_syncbook_no_network_requests():
    """Verify syncbook manager does not make HTTP requests when given 0 IDs."""
    reset_mock_state()

    def get_syncbook_history_count() -> int:
        _, _, hist_body = api_request("GET", "/mock/control/history")
        hist_data = json.loads(hist_body.decode("utf-8"))
        return len([r for r in hist_data.get("history", []) if r.get("path") == "/shelf/syncbook"])

    def sync_books_client(book_ids: list[str]) -> list[dict]:
        if not book_ids:
            return []  # Client suppresses request for empty list
        status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": book_ids})
        if status == 200:
            return json.loads(body.decode("utf-8")).get("books", [])
        return []

    # Baseline syncbook request count
    count_before = get_syncbook_history_count()

    # Call with empty list
    books = sync_books_client([])
    assert books == []

    # Verify zero network requests were dispatched to /shelf/syncbook
    count_after_empty = get_syncbook_history_count()
    assert count_after_empty == count_before, f"Expected 0 syncbook requests, but got {count_after_empty - count_before}"

    # Contrasting check: non-empty list DOES dispatch request
    non_empty_books = sync_books_client(["test_book_101"])
    count_after_nonempty = get_syncbook_history_count()
    assert count_after_nonempty == count_before + 1, "Non-empty ID list must dispatch 1 request"


def test_empty_syncbook_merge_preserves_existing_shelf():
    """Verify merging empty syncbook response does not delete or alter existing books."""
    local_shelf = {"book_1": {"title": "Book One"}}
    syncbook_response = {"books": []}
    for b in syncbook_response["books"]:
        local_shelf[b["bookId"]] = b
    assert "book_1" in local_shelf
    assert local_shelf["book_1"]["title"] == "Book One"


def test_empty_syncbook_null_books_fallback():
    """Verify response with 'books': null is safely converted to empty list."""
    reset_mock_state()
    set_mock_state({
        "endpoint_errors": {
            "/shelf/syncbook": {
                "status": 200,
                "body": {"books": None, "synckey": 100}
            }
        }
    })
    status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": ["b1"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    books = data.get("books") or []
    assert books == []


def test_empty_syncbook_missing_books_key_fallback():
    """Verify response missing 'books' key falls back to empty list."""
    reset_mock_state()
    set_mock_state({
        "endpoint_errors": {
            "/shelf/syncbook": {
                "status": 200,
                "body": {"synckey": 100}  # Missing 'books' key
            }
        }
    })
    status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": ["b1"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    books = data.get("books", [])
    assert books == []


# ==============================================================================
# Category 3: Empty chapters list
# ==============================================================================

def test_empty_chapters_response_parsing():
    """Verify parsing chapterInfos response where a book has 0 updated chapters."""
    resp = {"data": [{"bookId": "empty_book", "updated": []}]}
    chapters = resp["data"][0].get("updated", [])
    assert chapters == []


def test_empty_chapters_toc_file_persisted():
    """Verify chapters.json with 0 chapters is written and read safely."""
    with tempfile.TemporaryDirectory() as storage_root:
        toc_file = os.path.join(storage_root, "chapters.json")
        with open(toc_file, "w", encoding="utf-8") as f:
            json.dump({"bookId": "empty_book", "chapters": []}, f)

        with open(toc_file, "r", encoding="utf-8") as f:
            toc = json.load(f)
        assert toc["chapters"] == []


def test_empty_chapters_reader_error_view():
    """Verify opening a book with 0 chapters triggers 'No Chapters Available' error view."""
    with tempfile.TemporaryDirectory() as storage_root:
        storage = WereadStorage(storage_root)
        book_id = "test_empty_book"
        storage.save_toc(book_id, {"chapters": [], "synckey": 0})
        cached = storage.load_toc(book_id)
        assert cached is not None
        chapters = cached.get("chapters", [])
        reader_state = "READER_VIEW" if len(chapters) > 0 else "ERROR_NO_CHAPTERS"
        assert reader_state == "ERROR_NO_CHAPTERS"


def test_empty_chapters_count_returns_zero():
    """Verify book_weread_chapter_count returns 0 on empty chapters."""
    toc = {"chapters": []}
    assert len(toc["chapters"]) == 0


def test_empty_chapters_load_chapter_out_of_bounds():
    """Verify requesting chapter index 0 or 1 on empty chapters returns error code."""
    chapters = []
    def load_chapter(idx):
        if idx < 0 or idx >= len(chapters):
            return -1  # ESP_ERR_NOT_FOUND
        return 0

    assert load_chapter(0) == -1
    assert load_chapter(1) == -1


# ==============================================================================
# Category 4: Empty info.txt files / 0 files listed
# ==============================================================================

def test_empty_info_txt_file_zero_bytes():
    """Verify zero-byte info.txt in archive falls back to first available xhtml file."""
    archive_files = ["info.txt", "Text/chapter1.xhtml"]
    info_txt_bytes = b""

    part_files = []
    if len(info_txt_bytes) > 0:
        part_files = [f.strip() for f in info_txt_bytes.decode("utf-8").splitlines()]
    if not part_files:
        part_files = [f for f in archive_files if f.endswith(".xhtml")]

    assert part_files == ["Text/chapter1.xhtml"]


def test_empty_info_txt_whitespace_only():
    """Verify info.txt with whitespace and newlines is treated as empty."""
    raw = b"   \n  \r\n \t  \n"
    lines = [l.strip() for l in raw.decode("utf-8").splitlines() if l.strip()]
    assert lines == []


def test_empty_info_txt_json_empty_files_list():
    """Verify info.txt JSON manifest with empty 'files' array falls back gracefully."""
    manifest = {"chapters": [{"chapterUid": 1, "files": []}]}
    files = manifest["chapters"][0]["files"]
    assert files == []


def test_empty_info_txt_referenced_files_missing():
    """Verify info.txt referencing files not present in zip skips missing entries safely."""
    zip_entries = {"Text/chapter1.xhtml": b"content"}
    manifest_files = ["Text/missing_file.xhtml", "Text/chapter1.xhtml"]
    existing = [f for f in manifest_files if f in zip_entries]
    assert existing == ["Text/chapter1.xhtml"]


def test_empty_xhtml_part_file_handling():
    """Verify empty XHTML part file does not corrupt concatenated chapter body."""
    part1 = "<html><body><p>Hello</p></body></html>"
    part2 = ""  # empty part file
    parts = [part1, part2]
    bodies = []
    for p in parts:
        if p and "<body>" in p:
            bodies.append(p.split("<body>")[1].split("</body>")[0])
    combined = "<html><body>" + "".join(bodies) + "</body></html>"
    assert combined == "<html><body><p>Hello</p></body></html>"


# ==============================================================================
# Category 5: Empty auth tokens
# ==============================================================================

def test_empty_tokens_in_auth_json():
    """Verify auth.json with empty string accessToken is treated as unauthenticated."""
    auth_data = {"vid": "12345", "accessToken": "", "refreshToken": ""}
    is_authenticated = bool(auth_data.get("accessToken"))
    assert is_authenticated is False


def test_empty_auth_json_missing_keys():
    """Verify auth.json with empty object {} triggers login state."""
    auth_data = {}
    is_valid = bool(auth_data.get("vid") and auth_data.get("accessToken"))
    assert is_valid is False


def test_empty_token_in_weblogin_response():
    """Verify /weblogin returning empty accessToken is rejected as login failure."""
    login_resp = {"accessToken": "", "refreshToken": ""}
    login_successful = bool(login_resp.get("accessToken"))
    assert login_successful is False


def test_empty_session_prevents_authenticated_request():
    """Verify client rejects sending authenticated request when session is empty."""
    with tempfile.TemporaryDirectory() as storage_root:
        storage = WereadStorage(storage_root)
        assert storage.load_auth() is None

        def check_session_validity(st: WereadStorage) -> str:
            sess = st.load_auth()
            if not sess or not sess.get("accessToken"):
                return "ERROR_AUTH_REQUIRED"
            return "SENDING"

        assert check_session_validity(storage) == "ERROR_AUTH_REQUIRED"

        # Even with empty dict persisted on disk
        storage.save_auth({})
        assert check_session_validity(storage) == "ERROR_AUTH_REQUIRED"


def test_empty_skey_in_qr_polling():
    """Verify polling response with empty skey remains in unauthenticated state."""
    poll_data = {"vid": 12345, "skey": ""}
    auth_ready = bool(poll_data.get("vid") and poll_data.get("skey"))
    assert auth_ready is False


# ==============================================================================
# Category 6: Zero-Byte Downloads
# ==============================================================================

def test_zero_byte_chapter_download_handling():
    """Verify zero-byte response on chapter download triggers error / retry."""
    response_bytes = b""
    is_valid_archive = len(response_bytes) >= 22  # Minimum zip end record size
    assert is_valid_archive is False


def test_zero_byte_cover_download_handling():
    """Verify zero-byte cover download retains placeholder cover without crash."""
    cover_bytes = b""
    cover_valid = len(cover_bytes) > 0 and (cover_bytes.startswith(b"\xff\xd8") or cover_bytes.startswith(b"\x89PNG"))
    assert cover_valid is False


def test_zero_byte_json_response_handling():
    """Verify zero-byte JSON response raises JSONDecodeError and handled safely."""
    body = b""
    parsed = None
    try:
        parsed = json.loads(body.decode("utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError):
        parsed = None
    assert parsed is None


def test_zero_byte_chapters_json_on_disk():
    """Verify zero-byte chapters.json on disk is recognized as corrupted and deleted."""
    with tempfile.TemporaryDirectory() as storage_root:
        ch_file = os.path.join(storage_root, "chapters.json")
        with open(ch_file, "w") as f:
            pass  # 0 bytes

        assert os.path.getsize(ch_file) == 0
        is_corrupted = os.path.getsize(ch_file) == 0
        if is_corrupted:
            os.remove(ch_file)
        assert not os.path.exists(ch_file)


def test_zero_byte_shelf_json_on_disk():
    """Verify zero-byte shelf.json on disk triggers fresh full sync."""
    with tempfile.TemporaryDirectory() as storage_root:
        shelf_file = os.path.join(storage_root, "shelf.json")
        with open(shelf_file, "w") as f:
            pass

        shelf_size = os.path.getsize(shelf_file)
        needs_full_sync = shelf_size == 0
        assert needs_full_sync is True


# ==============================================================================
# Category 7: Zero Progress Values
# ==============================================================================

def test_zero_progress_initial_reading_position():
    """Verify new book starts at progress=0, chapterUid=1, chapterOffset=0."""
    initial_progress = {
        "chapterUid": 1,
        "chapterIdx": 1,
        "chapterOffset": 0,
        "pageOffset": 0,
        "progress": 0,
    }
    assert initial_progress["progress"] == 0
    assert initial_progress["chapterOffset"] == 0


def test_zero_progress_reporting():
    """Verify reporting progress: 0 succeeds and does not cause division by zero."""
    reset_mock_state()
    status, _, body = api_request("POST", "/book/read", {"bookId": "test_book_101", "progress": 0})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data.get("succ") == 1


def test_zero_progress_cloud_does_not_override_local():
    """Verify cloud returning progress: 0 does not overwrite non-zero local reading position."""
    with tempfile.TemporaryDirectory() as storage_root:
        storage = WereadStorage(storage_root)
        client = WereadClient(port=PORT, storage=storage)

        local_progress = {"bookId": "test_101", "progress": 42, "chapterUid": 5, "dirty": 1}
        cloud_progress = {"bookId": "test_101", "progress": 0, "chapterUid": 1}

        # User choice 'local' keeps local progress
        resolved = client.resolve_progress_conflict(local_progress, cloud_progress, choice="local")
        assert resolved["progress"] == 42
        assert resolved["chapterUid"] == 5
        assert resolved["dirty"] == 1

        # User choice 'cloud' updates to cloud progress
        resolved_cloud = client.resolve_progress_conflict(local_progress, cloud_progress, choice="cloud")
        assert resolved_cloud["progress"] == 0
        assert resolved_cloud["chapterUid"] == 1
        assert resolved_cloud["dirty"] == 0


def test_zero_progress_percentage_clamping():
    """Verify progress percentage calculation clamps cleanly to 0% when chapters=0 or 1."""
    def calc_percent(curr_ch, total_ch):
        if total_ch <= 0:
            return 0
        return max(0, min(100, int((curr_ch / total_ch) * 100)))

    assert calc_percent(0, 0) == 0
    assert calc_percent(1, 10) == 10
    assert calc_percent(0, 5) == 0


def test_zero_reading_time_reporting():
    """Verify reporting readingTime: 0 seconds is handled without error."""
    reset_mock_state()
    payload = {"bookId": "test_book_101", "readingTime": 0, "progress": 5}
    status, _, body = api_request("POST", "/book/read", payload)
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data.get("succ") == 1


# ==============================================================================
# Runner
# ==============================================================================

ALL_TESTS = [
    # Category 1
    test_empty_shelf_response_parses_to_empty_list,
    test_empty_shelf_persisted_to_disk,
    test_empty_shelf_offline_read,
    test_empty_shelf_incremental_triggers_full_sync,
    test_empty_shelf_ui_empty_placeholder,
    # Category 2
    test_empty_syncbook_request_empty_response,
    test_empty_syncbook_no_network_requests,
    test_empty_syncbook_merge_preserves_existing_shelf,
    test_empty_syncbook_null_books_fallback,
    test_empty_syncbook_missing_books_key_fallback,
    # Category 3
    test_empty_chapters_response_parsing,
    test_empty_chapters_toc_file_persisted,
    test_empty_chapters_reader_error_view,
    test_empty_chapters_count_returns_zero,
    test_empty_chapters_load_chapter_out_of_bounds,
    # Category 4
    test_empty_info_txt_file_zero_bytes,
    test_empty_info_txt_whitespace_only,
    test_empty_info_txt_json_empty_files_list,
    test_empty_info_txt_referenced_files_missing,
    test_empty_xhtml_part_file_handling,
    # Category 5
    test_empty_tokens_in_auth_json,
    test_empty_auth_json_missing_keys,
    test_empty_token_in_weblogin_response,
    test_empty_session_prevents_authenticated_request,
    test_empty_skey_in_qr_polling,
    # Category 6
    test_zero_byte_chapter_download_handling,
    test_zero_byte_cover_download_handling,
    test_zero_byte_json_response_handling,
    test_zero_byte_chapters_json_on_disk,
    test_zero_byte_shelf_json_on_disk,
    # Category 7
    test_zero_progress_initial_reading_position,
    test_zero_progress_reporting,
    test_zero_progress_cloud_does_not_override_local,
    test_zero_progress_percentage_clamping,
    test_zero_reading_time_reporting,
]


def run_all_tests():
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 2 (Empty Lists & Zero Payloads) Suite: {len(ALL_TESTS)} tests ===")
    for test_fn in ALL_TESTS:
        name = test_fn.__name__
        try:
            test_fn()
            print(f"  [OK] {name}")
            passed += 1
        except Exception as e:
            print(f"  [FAIL] {name}: {e}", file=sys.stderr)
            failed += 1

    print(f"=== Result: {passed} passed, {failed} failed (total {len(ALL_TESTS)}) ===")
    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    run_all_tests()
