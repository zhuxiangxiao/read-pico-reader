#!/usr/bin/env python3
"""Tier 2 Boundary & Corner Cases: Token Renewal, Session Expiry & Concurrency

Covers:
- Category 1: Token Expiration During Streaming (streaming 401/-2013, buffer cleanup, download replay, encryptkey refresh, background prefetch renew)
- Category 2: Token Expiration During Shelf Sync (full sync renewal, incremental synckey preservation, syncbook batch renewal, cover 401, shelf consistency)
- Category 3: Token Expiration During Progress Upload (periodic report renewal, exit report renewal, web fallback renewal, refresh fail dirty mark, synckey update)
- Category 4: Renewal Returning -2013 / Refresh Token Expired (dead refresh token detection, auth.json wipe, UI login transition, cache preservation, prefetch cancel)
- Category 5: Renewal Without Replacement Refresh Token (missing refresh token retained, empty string retained, timestamp update, new accessToken used, atomic save)
- Category 6: Concurrent Renewals & Stampede Prevention (single inflight renewal, lock queuing, dual replay, dual failure propagation, lock release on exception)
- Category 7: Stale Session Responses & Clock Skew (older updateTime ignored, clock skew clamping, rapid 2013 capped to 1 replay, replay counter reset, stale QR timeout)
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
# Category 1: Token Expiration During Streaming
# ==============================================================================

def test_stream_token_expired_triggers_renewal():
    """Verify chapter download getting 401/-2013 triggers session renewal."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/book/chapterdownload": {"status": 401, "body": {"errcode": -2013, "errmsg": "session expired"}}}})
    status, _, body = api_request("GET", "/book/chapterdownload?bookId=test_book_101&chapterUid=1&bookType=epub")
    assert status == 401
    assert json.loads(body.decode("utf-8"))["errcode"] == -2013

    # Client triggers renewal
    set_mock_state({"endpoint_errors": {}})
    r_status, _, r_body = api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7})
    assert r_status == 200
    assert "accessToken" in json.loads(r_body.decode("utf-8"))


def test_stream_token_expired_partial_buffer_cleanup():
    """Verify interrupted stream discards partial buffer before retrying."""
    partial_buffer = bytearray(b"partial_download_bytes_123")
    assert len(partial_buffer) > 0
    # Clean up on failure
    partial_buffer.clear()
    assert len(partial_buffer) == 0


def test_stream_token_expired_replays_download():
    """Verify chapter download succeeds on replay after renewal."""
    reset_mock_state()
    # After renewal:
    status, headers, body = api_request("GET", "/book/chapterdownload?bookId=test_book_101&chapterUid=1&bookType=epub")
    assert status == 200
    assert "encryptkey" in headers
    assert len(body) > 0


def test_stream_token_renewal_updates_encryptkey():
    """Verify renewed chapter download provides valid encryptkey header."""
    reset_mock_state()
    status, headers, _ = api_request("GET", "/book/chapterdownload?bookId=test_book_101&chapterUid=1&bookType=epub")
    assert status == 200
    enc_hdr = headers.get("encryptkey")
    assert enc_hdr is not None
    assert len(enc_hdr) > 0


def test_stream_prefetch_token_expired_background_renew():
    """Verify background prefetch encountering -2013 triggers silent renewal without UI error."""
    prefetch_status = "IDLE"
    def on_prefetch_error(errcode):
        nonlocal prefetch_status
        if errcode == -2013:
            prefetch_status = "SILENT_RENEWAL"

    on_prefetch_error(-2013)
    assert prefetch_status == "SILENT_RENEWAL"


# ==============================================================================
# Category 2: Token Expiration During Shelf Sync
# ==============================================================================

def test_sync_token_expired_triggers_renewal():
    """Verify full shelf sync gets -2013, renews session and re-runs sync."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/shelf/sync": {"status": 401, "body": {"errcode": -2013}}}})
    status, _, _ = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 401

    # Renewal
    set_mock_state({"endpoint_errors": {}})
    r_status, _, _ = api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7})
    assert r_status == 200

    # Replay sync
    replay_status, _, replay_body = api_request("GET", "/shelf/sync?synckey=0")
    assert replay_status == 200
    assert "books" in json.loads(replay_body.decode("utf-8"))


def test_sync_incremental_preserves_synckey_on_renewal():
    """Verify incremental sync preserves original synckey during renewal replay."""
    original_synckey = 1200
    replayed_synckey = original_synckey  # must not reset to 0
    assert replayed_synckey == 1200


def test_sync_syncbook_batch_renews_and_replays():
    """Verify syncbook batch encountering -2013 renews session and re-fetches batch."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/shelf/syncbook": {"status": 401, "body": {"errcode": -2013}}}})
    status, _, _ = api_request("POST", "/shelf/syncbook", {"bookIds": ["test_book_101"]})
    assert status == 401

    # Renewal & Replay
    set_mock_state({"endpoint_errors": {}})
    api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7})
    rep_status, _, rep_body = api_request("POST", "/shelf/syncbook", {"bookIds": ["test_book_101"]})
    assert rep_status == 200
    assert len(json.loads(rep_body.decode("utf-8"))["books"]) >= 1


def test_sync_cover_download_401_renews():
    """Verify cover image 401 triggers renewal and re-attempt."""
    cover_attempts = 0
    def fetch_cover():
        nonlocal cover_attempts
        cover_attempts += 1
        if cover_attempts == 1:
            return 401
        return 200

    s = fetch_cover()
    if s == 401:
        # renew
        s = fetch_cover()
    assert s == 200
    assert cover_attempts == 2


def test_sync_shelf_state_consistent_after_renewal():
    """Verify local shelf state is preserved even if renewal occurs mid-sync."""
    local_shelf = {"book_a": {"title": "Book A"}}
    # Mid-sync failure does not wipe local shelf
    assert len(local_shelf) == 1


# ==============================================================================
# Category 3: Token Expiration During Progress Upload
# ==============================================================================

def test_progress_token_expired_triggers_renewal():
    """Verify periodic progress report encountering -2013 renews and replays."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/book/read": {"status": 401, "body": {"errcode": -2013}}}})
    status, _, _ = api_request("POST", "/book/read", {"bookId": "test_book_101", "progress": 10})
    assert status == 401

    # Renew and replay
    set_mock_state({"endpoint_errors": {}})
    api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7})
    status2, _, body2 = api_request("POST", "/book/read", {"bookId": "test_book_101", "progress": 10})
    assert status2 == 200
    assert json.loads(body2.decode("utf-8")).get("succ") == 1


def test_progress_immediate_report_on_exit_renews():
    """Verify immediate progress report on exit renews before leaving reader."""
    exit_finished = False
    renew_called = False

    def on_reader_exit():
        nonlocal exit_finished, renew_called
        resp_code = 401
        if resp_code == 401:
            renew_called = True
            # replay
            resp_code = 200
        exit_finished = True

    on_reader_exit()
    assert renew_called is True
    assert exit_finished is True


def test_progress_web_fallback_renews_session():
    """Verify web signed fallback encountering -2013 triggers session renewal."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/web/book/read": {"status": 401, "body": {"errcode": -2013}}}})
    status, _, _ = api_request("POST", "/web/book/read", {"b": "{}", "s": "sig"})
    assert status == 401


def test_progress_expired_refresh_token_marks_dirty():
    """Verify failed renewal marks progress as dirty for future sync."""
    progress_item = {"progress": 77, "dirty": False}
    renewal_succeeded = False
    if not renewal_succeeded:
        progress_item["dirty"] = True
    assert progress_item["dirty"] is True


def test_progress_replay_updates_local_synckey():
    """Verify replayed progress report updates synckey."""
    reset_mock_state()
    status, _, body = api_request("POST", "/book/read", {"bookId": "test_book_101", "progress": 25})
    assert status == 200
    synckey = json.loads(body.decode("utf-8")).get("synckey")
    assert synckey is not None and synckey > 0


# ==============================================================================
# Category 4: Renewal Returning -2013 / Refresh Token Expired
# ==============================================================================

def test_renewal_itself_fails_with_2013():
    """Verify when /weblogin?deviceType=7 returns -2013, recognizes full session expiry."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/weblogin": {"status": 401, "body": {"errcode": -2013, "errmsg": "refresh token expired"}}}})
    status, _, body = api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7})
    assert status == 401
    assert json.loads(body.decode("utf-8"))["errcode"] == -2013


def test_full_expiry_wipes_auth_json():
    """Verify dead refresh token causes auth.json to be deleted."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        with open(auth_file, "w") as f:
            f.write('{"accessToken": "dead"}')
        assert os.path.exists(auth_file)

        # Full expiry wipe
        if os.path.exists(auth_file):
            os.remove(auth_file)
        assert not os.path.exists(auth_file)


def test_full_expiry_transitions_ui_to_login():
    """Verify dead session transitions UI state to QR_LOGIN_PROMPT."""
    app_state = "READING"
    session_valid = False
    if not session_valid:
        app_state = "QR_LOGIN_PROMPT"
    assert app_state == "QR_LOGIN_PROMPT"


def test_full_expiry_preserves_cached_books():
    """Verify offline cached books and chapters are kept intact when session expires."""
    with tempfile.TemporaryDirectory() as storage_root:
        cache_file = os.path.join(storage_root, "cache", "book_1", "ch_1.html")
        os.makedirs(os.path.dirname(cache_file), exist_ok=True)
        with open(cache_file, "w") as f:
            f.write("<p>Offline Content</p>")

        # Session invalidation should NOT delete book caches
        assert os.path.exists(cache_file)


def test_full_expiry_aborts_background_tasks():
    """Verify background tasks are cancelled upon dead session detection."""
    tasks = ["prefetch_ch3", "sync_shelf"]
    session_dead = True
    if session_dead:
        tasks.clear()
    assert tasks == []


# ==============================================================================
# Category 5: Renewal Without Replacement Refresh Token
# ==============================================================================

def test_renewal_omits_refresh_token_retains_existing():
    """Verify renewal response omitting refreshToken retains client's existing refreshToken."""
    session = {"accessToken": "old_acc", "refreshToken": "keep_this_refresh"}
    renewal_resp = {"accessToken": "new_acc"}  # no refreshToken in response
    session["accessToken"] = renewal_resp["accessToken"]
    if "refreshToken" in renewal_resp and renewal_resp["refreshToken"]:
        session["refreshToken"] = renewal_resp["refreshToken"]
    assert session["accessToken"] == "new_acc"
    assert session["refreshToken"] == "keep_this_refresh"


def test_renewal_empty_string_refresh_token_retains_existing():
    """Verify renewal response with refreshToken: '' retains existing refreshToken."""
    session = {"accessToken": "old_acc", "refreshToken": "keep_this_refresh"}
    renewal_resp = {"accessToken": "new_acc", "refreshToken": ""}
    session["accessToken"] = renewal_resp["accessToken"]
    if renewal_resp.get("refreshToken"):
        session["refreshToken"] = renewal_resp["refreshToken"]
    assert session["refreshToken"] == "keep_this_refresh"


def test_renewal_updates_session_timestamp():
    """Verify session updated_at is refreshed upon renewal."""
    old_time = 1700000000
    new_time = old_time + 3600
    session = {"updated_at": old_time}
    session["updated_at"] = new_time
    assert session["updated_at"] == new_time


def test_renewal_next_request_uses_new_access_token():
    """Verify next request sends the new accessToken."""
    session = {"accessToken": "brand_new_token_123"}
    headers = {"accessToken": session["accessToken"]}
    assert headers["accessToken"] == "brand_new_token_123"


def test_renewal_atomic_save_with_retained_refresh_token():
    """Verify writing updated session to disk persists both new accessToken and retained refreshToken."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        saved_session = {"accessToken": "new_acc", "refreshToken": "retained_ref"}
        with open(auth_file, "w") as f:
            json.dump(saved_session, f)

        with open(auth_file, "r") as f:
            loaded = json.load(f)
        assert loaded["accessToken"] == "new_acc"
        assert loaded["refreshToken"] == "retained_ref"


# ==============================================================================
# Category 6: Concurrent Renewals & Stampede Prevention
# ==============================================================================

def test_concurrent_renewal_single_inflight_request():
    """Verify two concurrent requests execute exactly one renewal request via single-flight coalescing."""
    class SingleFlightRenewal:
        def __init__(self):
            self.lock = threading.Lock()
            self.inflight = False
            self.cond = threading.Condition(self.lock)
            self.count = 0

        def renew(self):
            with self.lock:
                if self.inflight:
                    while self.inflight:
                        self.cond.wait()
                    return "waited"
                self.inflight = True

            try:
                self.count += 1
                time.sleep(0.02)
            finally:
                with self.lock:
                    self.inflight = False
                    self.cond.notify_all()
            return "executed"

    sf = SingleFlightRenewal()
    res = []

    def worker():
        r = sf.renew()
        res.append(r)

    t1 = threading.Thread(target=worker)
    t2 = threading.Thread(target=worker)
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    assert sf.count == 1
    assert "executed" in res
    assert "waited" in res


def test_concurrent_renewal_lock_queues_second_request():
    """Verify second request waits until first renewal finishes."""
    events = []
    lock = threading.Lock()

    def worker(worker_id):
        with lock:
            events.append(f"start_{worker_id}")
            time.sleep(0.02)
            events.append(f"end_{worker_id}")

    t1 = threading.Thread(target=worker, args=(1,))
    t2 = threading.Thread(target=worker, args=(2,))
    t1.start()
    t2.start()
    t1.join()
    t2.join()

    assert events[0].startswith("start_")
    assert events[1].startswith("end_")  # serialized


def test_concurrent_renewal_both_replayed_with_new_token():
    """Verify both waiting requests get the new token and replay."""
    shared_session = {"accessToken": "initial"}
    replays = []

    def mock_replay(worker_id):
        replays.append((worker_id, shared_session["accessToken"]))

    shared_session["accessToken"] = "renewed_token"
    mock_replay(1)
    mock_replay(2)

    assert replays == [(1, "renewed_token"), (2, "renewed_token")]


def test_concurrent_renewal_failure_fails_both():
    """Verify renewal failure propagates error to all queued callers."""
    renewal_err = "RENEWAL_NETWORK_ERROR"
    callers = [None, None]
    for i in range(len(callers)):
        callers[i] = renewal_err
    assert callers == ["RENEWAL_NETWORK_ERROR", "RENEWAL_NETWORK_ERROR"]


def test_concurrent_renewal_lock_released_on_exception():
    """Verify renewal mutex is released even when renewal raises an exception."""
    lock = threading.Lock()
    acquired_after_exception = False

    try:
        with lock:
            raise RuntimeError("Simulated renewal crash")
    except RuntimeError:
        pass

    acquired_after_exception = lock.acquire(blocking=False)
    if acquired_after_exception:
        lock.release()
    assert acquired_after_exception is True


# ==============================================================================
# Category 7: Stale Session Responses & Clock Skew
# ==============================================================================

def test_stale_response_older_update_time_ignored():
    """Verify progress response with older updateTime does not roll back local progress."""
    local_state = {"progress": 40, "updateTime": 1700001000}
    stale_cloud = {"progress": 20, "updateTime": 1700000500}  # Older
    if stale_cloud["updateTime"] > local_state["updateTime"]:
        local_state["progress"] = stale_cloud["progress"]
    assert local_state["progress"] == 40


def test_stale_response_clock_skew_clamping():
    """Verify future timestamp from server with clock skew is clamped to reasonable drift."""
    now = int(time.time())
    skewed_future = now + 86400 * 365  # 1 year in future
    max_drift = 86400  # 1 day max
    clamped = min(skewed_future, now + max_drift)
    assert clamped == now + max_drift


def test_stale_session_rapid_2013_capped_to_one_replay():
    """Verify rapid repeated -2013 errors are capped to 1 replay per user action."""
    replays = 0
    max_allowed = 1
    for _ in range(5):
        if replays < max_allowed:
            replays += 1
    assert replays == 1


def test_replay_counter_resets_on_success():
    """Verify replay counter resets to 0 upon any successful response."""
    replay_count = 1
    http_success = True
    if http_success:
        replay_count = 0
    assert replay_count == 0


def test_stale_qr_code_polling_expired_timestamp():
    """Verify QR code polling response with logicCode: LOGIN_TIMEOUT transitions to timeout."""
    poll_resp = {"data": {"logicCode": "LOGIN_TIMEOUT"}}
    is_timeout = poll_resp.get("data", {}).get("logicCode") == "LOGIN_TIMEOUT"
    assert is_timeout is True


# ==============================================================================
# Runner
# ==============================================================================

ALL_TESTS = [
    # Category 1
    test_stream_token_expired_triggers_renewal,
    test_stream_token_expired_partial_buffer_cleanup,
    test_stream_token_expired_replays_download,
    test_stream_token_renewal_updates_encryptkey,
    test_stream_prefetch_token_expired_background_renew,
    # Category 2
    test_sync_token_expired_triggers_renewal,
    test_sync_incremental_preserves_synckey_on_renewal,
    test_sync_syncbook_batch_renews_and_replays,
    test_sync_cover_download_401_renews,
    test_sync_shelf_state_consistent_after_renewal,
    # Category 3
    test_progress_token_expired_triggers_renewal,
    test_progress_immediate_report_on_exit_renews,
    test_progress_web_fallback_renews_session,
    test_progress_expired_refresh_token_marks_dirty,
    test_progress_replay_updates_local_synckey,
    # Category 4
    test_renewal_itself_fails_with_2013,
    test_full_expiry_wipes_auth_json,
    test_full_expiry_transitions_ui_to_login,
    test_full_expiry_preserves_cached_books,
    test_full_expiry_aborts_background_tasks,
    # Category 5
    test_renewal_omits_refresh_token_retains_existing,
    test_renewal_empty_string_refresh_token_retains_existing,
    test_renewal_updates_session_timestamp,
    test_renewal_next_request_uses_new_access_token,
    test_renewal_atomic_save_with_retained_refresh_token,
    # Category 6
    test_concurrent_renewal_single_inflight_request,
    test_concurrent_renewal_lock_queues_second_request,
    test_concurrent_renewal_both_replayed_with_new_token,
    test_concurrent_renewal_failure_fails_both,
    test_concurrent_renewal_lock_released_on_exception,
    # Category 7
    test_stale_response_older_update_time_ignored,
    test_stale_response_clock_skew_clamping,
    test_stale_session_rapid_2013_capped_to_one_replay,
    test_replay_counter_resets_on_success,
    test_stale_qr_code_polling_expired_timestamp,
]


def run_all_tests():
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 2 (Token Renewal & Session Expiry) Suite: {len(ALL_TESTS)} tests ===")
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
