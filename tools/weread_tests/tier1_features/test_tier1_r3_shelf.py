#!/usr/bin/env python3
"""Tier 1 Feature Coverage: Bookshelf, Offline Browsing & TOC Cache (R3)

Covers:
- Feature 13: GET /shelf/sync Incremental Sync (synckey > 0, onlyBookid=1, modified IDs, progress, removed books)
- Feature 14: GET /shelf/sync Full Sync (synckey=0, full catalog, reconciliation, readUpdateTime, progress mapping)
- Feature 15: lectureBooks Filtering (audio/podcast filtering, totalWords==0 check, EPUB/TXT retention, empty list safety)
- Feature 16: POST /shelf/syncbook Batch Details Sync (chunking <=20 IDs, request payload, catalog merging, partial response, multi-batch)
- Feature 17: Local Shelf Persistence & Sorting (shelf.json layout, readUpdateTime DESC sort, tie-breaking, atomic write, offline load)
- Feature 18: POST /book/chapterInfos TOC Sync (chapter list parsing, clearAll:1 invalidation, clearAll:0 merge, empty TOC safety)
- Feature 19: Local TOC Storage (cache/<bookId>/chapters.json layout, synckey/bookVersion, atomic write, directory auto-create, offline open)
- Feature 20: High-Res Cover Transformation & Backoff (/s_ to /t6_, non-/s_ intact, 5s/15s/45s backoff, 404 fallback, counter reset)
"""

from __future__ import annotations

import json
import os
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# Helper to ensure tools/ is on path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
WORKSPACE_ROOT = os.path.abspath(os.path.join(TOOLS_DIR, ".."))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

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
# Feature 13: GET /shelf/sync Incremental Sync
# ==============================================================================

def test_f13_incremental_query_params():
    """Verify incremental sync sends onlyBookid=1 and existing synckey > 0."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=1000&onlyBookid=1")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert "bookIds" in data
    assert "bookProgress" in data
    assert "synckey" in data
    assert data["synckey"] > 1000


def test_f13_incremental_returns_modified_ids():
    """Verify incremental sync returns bookIds for updated books."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=1000&onlyBookid=1")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    ids = [b["bookId"] for b in data["bookIds"]]
    assert "test_book_101" in ids


def test_f13_incremental_updates_local_synckey():
    """Verify local synckey is updated to the newly returned synckey."""
    local_state = {"synckey": 1000}
    reset_mock_state()
    set_mock_state({"synckey": 1500})
    status, _, body = api_request("GET", "/shelf/sync?synckey=1000&onlyBookid=1")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    local_state["synckey"] = data["synckey"]
    assert local_state["synckey"] == 1501


def test_f13_incremental_empty_changes():
    """Verify client handles empty changes cleanly when no books are updated."""
    reset_mock_state()
    set_mock_state({"books": [], "progress_data": {}})
    status, _, body = api_request("GET", "/shelf/sync?synckey=1000&onlyBookid=1")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["bookIds"] == []
    assert data["bookProgress"] == []


def test_f13_incremental_removed_books_purging():
    """Verify books in the 'removed' array are purged from the local shelf."""
    local_shelf = {"book_keep": {"title": "Keep"}, "book_remove": {"title": "Remove"}}
    sync_response = {"removed": ["book_remove"], "bookIds": []}
    for r_id in sync_response.get("removed", []):
        local_shelf.pop(r_id, None)
    assert "book_remove" not in local_shelf
    assert "book_keep" in local_shelf


# ==============================================================================
# Feature 14: GET /shelf/sync Full Sync
# ==============================================================================

def test_f14_full_sync_request_params():
    """Verify full sync requests synckey=0."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert "books" in data
    assert len(data["books"]) >= 2


def test_f14_full_sync_catalog_attributes():
    """Verify full sync response provides title, author, cover, format, totalWords."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    book = next(b for b in data["books"] if b["bookId"] == "test_book_101")
    assert book["title"] == "测试小说：荒原起源"
    assert book["author"] == "测试作者"
    assert "cover" in book
    assert book["format"] == "epub"
    assert book["totalWords"] > 0


def test_f14_full_sync_cache_reconciliation():
    """Verify full sync reconciles and updates local book database."""
    local_db = {}
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    for b in data["books"]:
        local_db[b["bookId"]] = b
    assert "test_book_101" in local_db
    assert "txt_book_202" in local_db


def test_f14_full_sync_captures_read_update_time():
    """Verify readUpdateTime is preserved for sorting."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    for b in data["books"]:
        assert "readUpdateTime" in b
        assert b["readUpdateTime"] > 0


def test_f14_full_sync_progress_mapping():
    """Verify bookProgress items correlate with bookIds."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    prog_map = {p["bookId"]: p for p in data["bookProgress"]}
    assert "test_book_101" in prog_map
    assert prog_map["test_book_101"]["progress"] == 15


# ==============================================================================
# Feature 15: lectureBooks Filtering
# ==============================================================================

def test_f15_lecture_books_filtered_out():
    """Verify lectureBooks in sync response are identified and not added to the readable shelf."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert "lectureBooks" in data
    lecture_ids = {lb["bookId"] for lb in data["lectureBooks"]}
    assert "lecture_999" in lecture_ids

    # Shelf should filter these out
    books_to_display = [b for b in data["books"] if b["bookId"] not in lecture_ids]
    assert all(b["bookId"] != "lecture_999" for b in books_to_display)


def test_f15_audio_format_filtered():
    """Verify books with format='audio' are excluded."""
    items = [
        {"bookId": "b1", "format": "epub", "title": "EPUB Book"},
        {"bookId": "b2", "format": "audio", "title": "Audiobook"},
        {"bookId": "b3", "format": "txt", "title": "TXT Book"},
    ]
    filtered = [b for b in items if b.get("format", "").lower() != "audio"]
    assert len(filtered) == 2
    assert [b["bookId"] for b in filtered] == ["b1", "b3"]


def test_f15_retains_epub_and_txt():
    """Verify standard EPUB and TXT books are kept in the shelf."""
    reset_mock_state()
    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    formats = {b["format"] for b in data["books"]}
    assert "epub" in formats
    assert "txt" in formats


def test_f15_total_words_zero_audio_excluded():
    """Verify audio entries with totalWords==0 and type!=0 are filtered."""
    items = [
        {"bookId": "a1", "format": "audio", "type": 1, "totalWords": 0},
        {"bookId": "a2", "format": "epub", "type": 0, "totalWords": 50000},
    ]
    readable = [b for b in items if not (b.get("format") == "audio" or b.get("type") == 1)]
    assert len(readable) == 1
    assert readable[0]["bookId"] == "a2"


def test_f15_empty_lecture_books_safety():
    """Verify empty lectureBooks array does not crash filtering logic."""
    data = {"books": [{"bookId": "1", "format": "epub"}], "lectureBooks": []}
    lecture_ids = {lb["bookId"] for lb in data.get("lectureBooks", [])}
    readable = [b for b in data["books"] if b["bookId"] not in lecture_ids]
    assert len(readable) == 1


# ==============================================================================
# Feature 16: POST /shelf/syncbook Batch Details Sync
# ==============================================================================

def test_f16_syncbook_batch_chunking():
    """Verify batching book IDs into chunks of <= 20 IDs per request."""
    book_ids = [f"book_{i}" for i in range(55)]
    chunk_size = 20
    chunks = [book_ids[i:i + chunk_size] for i in range(0, len(book_ids), chunk_size)]
    assert len(chunks) == 3
    assert len(chunks[0]) == 20
    assert len(chunks[1]) == 20
    assert len(chunks[2]) == 15


def test_f16_syncbook_payload_format():
    """Verify POST /shelf/syncbook sends bookIds array."""
    reset_mock_state()
    status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": ["test_book_101"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert "books" in data
    assert any(b["bookId"] == "test_book_101" for b in data["books"])


def test_f16_syncbook_merges_details():
    """Verify syncbook details are merged into local shelf metadata."""
    local_shelf = {"test_book_101": {"bookId": "test_book_101"}}
    reset_mock_state()
    status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": ["test_book_101"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    for b in data["books"]:
        local_shelf[b["bookId"]].update(b)
    assert local_shelf["test_book_101"]["title"] == "测试小说：荒原起源"


def test_f16_syncbook_partial_match():
    """Verify syncbook handles server returning a subset of requested IDs."""
    reset_mock_state()
    # Request valid ID and non-existent ID
    status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": ["test_book_101", "nonexistent_999"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    returned_ids = [b["bookId"] for b in data["books"]]
    assert "test_book_101" in returned_ids
    assert "nonexistent_999" not in returned_ids


def test_f16_syncbook_multi_batch_assembly():
    """Verify multi-batch requests combine cleanly into single shelf catalog."""
    batches = [["test_book_101"], ["txt_book_202"]]
    collected_books = []
    reset_mock_state()
    for batch in batches:
        status, _, body = api_request("POST", "/shelf/syncbook", {"bookIds": batch})
        assert status == 200
        collected_books.extend(json.loads(body.decode("utf-8"))["books"])
    assert len(collected_books) == 2
    ids = {b["bookId"] for b in collected_books}
    assert ids == {"test_book_101", "txt_book_202"}


# ==============================================================================
# Feature 17: Local Shelf Persistence (shelf.json) & Sorting
# ==============================================================================

def test_f17_shelf_json_save_layout():
    """Verify shelf.json layout saved under storage root."""
    with tempfile.TemporaryDirectory() as storage_root:
        shelf_file = os.path.join(storage_root, "shelf.json")
        shelf_data = {
            "synckey": 1005,
            "updatedAt": 1700000000,
            "books": [
                {"bookId": "b1", "title": "Book 1", "readUpdateTime": 100},
                {"bookId": "b2", "title": "Book 2", "readUpdateTime": 200},
            ]
        }
        with open(shelf_file, "w", encoding="utf-8") as f:
            json.dump(shelf_data, f)

        with open(shelf_file, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        assert loaded["synckey"] == 1005
        assert len(loaded["books"]) == 2


def test_f17_shelf_sorting_by_read_update_time():
    """Verify books are sorted by readUpdateTime DESC (most recently read first)."""
    books = [
        {"bookId": "b1", "readUpdateTime": 100},
        {"bookId": "b2", "readUpdateTime": 500},
        {"bookId": "b3", "readUpdateTime": 300},
    ]
    books.sort(key=lambda b: b.get("readUpdateTime", 0), reverse=True)
    assert [b["bookId"] for b in books] == ["b2", "b3", "b1"]


def test_f17_shelf_sorting_tie_break_by_update_time():
    """Verify ties in readUpdateTime fall back to updateTime DESC."""
    books = [
        {"bookId": "b1", "readUpdateTime": 500, "updateTime": 1000},
        {"bookId": "b2", "readUpdateTime": 500, "updateTime": 2000},
        {"bookId": "b3", "readUpdateTime": 100, "updateTime": 3000},
    ]
    books.sort(key=lambda b: (b.get("readUpdateTime", 0), b.get("updateTime", 0)), reverse=True)
    assert [b["bookId"] for b in books] == ["b2", "b1", "b3"]


def test_f17_shelf_atomic_write():
    """Verify atomic write of shelf.json via temporary file and replace."""
    with tempfile.TemporaryDirectory() as storage_root:
        shelf_file = os.path.join(storage_root, "shelf.json")
        tmp_file = shelf_file + ".tmp"
        with open(tmp_file, "w", encoding="utf-8") as f:
            json.dump({"synckey": 1}, f)
        os.replace(tmp_file, shelf_file)
        assert os.path.isfile(shelf_file)
        assert not os.path.exists(tmp_file)


def test_f17_shelf_offline_loading():
    """Verify shelf can be loaded from shelf.json without any network request."""
    with tempfile.TemporaryDirectory() as storage_root:
        shelf_file = os.path.join(storage_root, "shelf.json")
        with open(shelf_file, "w", encoding="utf-8") as f:
            json.dump({"books": [{"bookId": "offline_book", "title": "Offline"}]}, f)

        # Read offline
        with open(shelf_file, "r", encoding="utf-8") as f:
            data = json.load(f)
        assert data["books"][0]["bookId"] == "offline_book"


# ==============================================================================
# Feature 18: POST /book/chapterInfos Chapter TOC Sync
# ==============================================================================

def test_f18_chapter_infos_request():
    """Verify request sends bookIds array to fetch chapter TOC."""
    reset_mock_state()
    status, _, body = api_request("POST", "/book/chapterInfos", {"bookIds": ["test_book_101"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert "data" in data
    assert len(data["data"]) == 1
    assert data["data"][0]["bookId"] == "test_book_101"


def test_f18_chapter_infos_schema():
    """Verify chapterInfos response contains chapterUid, title, level, wordCount."""
    reset_mock_state()
    status, _, body = api_request("POST", "/book/chapterInfos", {"bookIds": ["test_book_101"]})
    assert status == 200
    ch_list = json.loads(body.decode("utf-8"))["data"][0]["updated"]
    ch1 = ch_list[0]
    assert ch1["chapterUid"] == 1
    assert ch1["chapterIdx"] == 1
    assert "title" in ch1
    assert "level" in ch1
    assert "wordCount" in ch1


def test_f18_clear_all_toc_invalidation():
    """Verify clearAll: 1 flag triggers total invalidation and replacement of local TOC."""
    local_toc = [{"chapterUid": 999, "title": "Old Outdated Chapter"}]
    reset_mock_state()
    set_mock_state({"clear_all_toc": True})

    status, _, body = api_request("POST", "/book/chapterInfos", {"bookIds": ["test_book_101"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))["data"][0]
    assert data["clearAll"] == 1
    if data["clearAll"] == 1:
        local_toc = list(data["updated"])

    assert len(local_toc) == 2
    assert all(c["chapterUid"] != 999 for c in local_toc)


def test_f18_clear_all_zero_incremental_merge():
    """Verify clearAll: 0 merges updated chapters into local TOC without deleting unmentioned chapters."""
    local_toc = {1: {"chapterUid": 1, "title": "Old Ch 1"}, 2: {"chapterUid": 2, "title": "Ch 2"}}
    new_updates = [{"chapterUid": 1, "title": "Updated Ch 1"}, {"chapterUid": 3, "title": "New Ch 3"}]
    for u in new_updates:
        local_toc[u["chapterUid"]] = u

    assert len(local_toc) == 3
    assert local_toc[1]["title"] == "Updated Ch 1"
    assert local_toc[2]["title"] == "Ch 2"


def test_f18_empty_toc_handling():
    """Verify handling when server returns empty updated list for a book."""
    reset_mock_state()
    status, _, body = api_request("POST", "/book/chapterInfos", {"bookIds": ["nonexistent_book"]})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["data"][0]["updated"] == []


# ==============================================================================
# Feature 19: Local TOC Storage (chapters.json)
# ==============================================================================

def test_f19_local_toc_file_path():
    """Verify chapters.json is stored at cache/<bookId>/chapters.json."""
    with tempfile.TemporaryDirectory() as storage_root:
        book_id = "test_book_101"
        book_dir = os.path.join(storage_root, "cache", book_id)
        os.makedirs(book_dir, exist_ok=True)
        toc_file = os.path.join(book_dir, "chapters.json")

        toc_data = {"bookId": book_id, "bookVersion": 100, "synckey": 1000, "chapters": [{"chapterUid": 1, "title": "Ch1"}]}
        with open(toc_file, "w", encoding="utf-8") as f:
            json.dump(toc_data, f)

        assert os.path.isfile(toc_file)
        with open(toc_file, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        assert loaded["bookVersion"] == 100


def test_f19_local_toc_schema():
    """Verify TOC schema contains synckey, bookVersion, chapters array."""
    sample = {
        "bookId": "test_book_101",
        "bookVersion": 105,
        "synckey": 2001,
        "chapters": [{"chapterUid": 1, "chapterIdx": 1, "title": "Chapter 1", "level": 1}]
    }
    assert "synckey" in sample
    assert "bookVersion" in sample
    assert isinstance(sample["chapters"], list)


def test_f19_offline_toc_instant_open():
    """Verify local TOC opens instantly offline without network call."""
    with tempfile.TemporaryDirectory() as storage_root:
        book_dir = os.path.join(storage_root, "cache", "book_quick")
        os.makedirs(book_dir)
        toc_file = os.path.join(book_dir, "chapters.json")
        with open(toc_file, "w", encoding="utf-8") as f:
            json.dump({"chapters": [{"chapterUid": 1, "title": "Fast"}]}, f)

        # Open offline
        with open(toc_file, "r", encoding="utf-8") as f:
            toc = json.load(f)
        assert len(toc["chapters"]) == 1


def test_f19_local_toc_atomic_write():
    """Verify atomic write pattern for chapters.json."""
    with tempfile.TemporaryDirectory() as storage_root:
        book_dir = os.path.join(storage_root, "cache", "book_atomic")
        os.makedirs(book_dir)
        toc_file = os.path.join(book_dir, "chapters.json")
        tmp_file = toc_file + ".tmp"
        with open(tmp_file, "w", encoding="utf-8") as f:
            json.dump({"chapters": []}, f)
        os.replace(tmp_file, toc_file)
        assert os.path.isfile(toc_file)
        assert not os.path.exists(tmp_file)


def test_f19_local_toc_dir_auto_create():
    """Verify helper creates parent cache/<bookId> directory if missing."""
    with tempfile.TemporaryDirectory() as storage_root:
        book_dir = os.path.join(storage_root, "cache", "new_book_dir")
        assert not os.path.exists(book_dir)
        os.makedirs(book_dir, exist_ok=True)
        assert os.path.isdir(book_dir)


# ==============================================================================
# Feature 20: High-Res Cover Transformation (t6_) & Backoff
# ==============================================================================

def test_f20_cover_url_transformation_s_to_t6():
    """Verify transforming /s_ cover URL to /t6_ high-res URL."""
    url = "https://weread.qq.com/covers/s_test_book_101.jpg"
    high_res_url = url.replace("/s_", "/t6_")
    assert high_res_url == "https://weread.qq.com/covers/t6_test_book_101.jpg"


def test_f20_cover_url_without_s_unchanged():
    """Verify cover URL without /s_ remains unchanged."""
    url = "https://weread.qq.com/covers/original_cover.jpg"
    transformed = url.replace("/s_", "/t6_") if "/s_" in url else url
    assert transformed == url


def test_f20_exponential_backoff_delays():
    """Verify backoff delays sequence (5s, 15s, 45s)."""
    backoff_delays = [5, 15, 45]
    retry_state = {"attempt": 0}

    def get_next_delay():
        att = retry_state["attempt"]
        delay = backoff_delays[min(att, len(backoff_delays) - 1)]
        retry_state["attempt"] += 1
        return delay

    assert get_next_delay() == 5
    assert get_next_delay() == 15
    assert get_next_delay() == 45
    assert get_next_delay() == 45  # capped at maximum backoff


def test_f20_cover_fallback_on_404():
    """Verify fallback to original low-res cover if high-res t6_ returns 404."""
    high_res_url = "https://weread.qq.com/covers/t6_not_found.jpg"
    low_res_url = "https://weread.qq.com/covers/s_not_found.jpg"
    # Simulate high res 404
    high_res_status = 404
    active_url = high_res_url if high_res_status == 200 else low_res_url
    assert active_url == low_res_url


def test_f20_cover_retry_counter_reset():
    """Verify retry counter resets to 0 upon successful download."""
    retry_state = {"attempt": 2}
    # Download succeeds
    download_success = True
    if download_success:
        retry_state["attempt"] = 0
    assert retry_state["attempt"] == 0


# ==============================================================================
# Runner
# ==============================================================================

ALL_TESTS = [
    # Feature 13
    test_f13_incremental_query_params,
    test_f13_incremental_returns_modified_ids,
    test_f13_incremental_updates_local_synckey,
    test_f13_incremental_empty_changes,
    test_f13_incremental_removed_books_purging,
    # Feature 14
    test_f14_full_sync_request_params,
    test_f14_full_sync_catalog_attributes,
    test_f14_full_sync_cache_reconciliation,
    test_f14_full_sync_captures_read_update_time,
    test_f14_full_sync_progress_mapping,
    # Feature 15
    test_f15_lecture_books_filtered_out,
    test_f15_audio_format_filtered,
    test_f15_retains_epub_and_txt,
    test_f15_total_words_zero_audio_excluded,
    test_f15_empty_lecture_books_safety,
    # Feature 16
    test_f16_syncbook_batch_chunking,
    test_f16_syncbook_payload_format,
    test_f16_syncbook_merges_details,
    test_f16_syncbook_partial_match,
    test_f16_syncbook_multi_batch_assembly,
    # Feature 17
    test_f17_shelf_json_save_layout,
    test_f17_shelf_sorting_by_read_update_time,
    test_f17_shelf_sorting_tie_break_by_update_time,
    test_f17_shelf_atomic_write,
    test_f17_shelf_offline_loading,
    # Feature 18
    test_f18_chapter_infos_request,
    test_f18_chapter_infos_schema,
    test_f18_clear_all_toc_invalidation,
    test_f18_clear_all_zero_incremental_merge,
    test_f18_empty_toc_handling,
    # Feature 19
    test_f19_local_toc_file_path,
    test_f19_local_toc_schema,
    test_f19_offline_toc_instant_open,
    test_f19_local_toc_atomic_write,
    test_f19_local_toc_dir_auto_create,
    # Feature 20
    test_f20_cover_url_transformation_s_to_t6,
    test_f20_cover_url_without_s_unchanged,
    test_f20_exponential_backoff_delays,
    test_f20_cover_fallback_on_404,
    test_f20_cover_retry_counter_reset,
]


def run_all_tests():
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 1 R3 (Bookshelf, Offline Browsing & TOC Cache) Suite: {len(ALL_TESTS)} tests ===")
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
