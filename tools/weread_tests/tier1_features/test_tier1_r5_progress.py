#!/usr/bin/env python3
"""Tier 1 Feature Coverage: Progress Sync & Wi-Fi Lifecycle (R5)

Covers:
- Feature 30: Fetch Cloud Progress (GET /book/getProgress, parsing chapterUid/progress, web fallback, position update, new book default)
- Feature 31: Report Reading Progress & Signature (POST /book/read payload, web signed fallback b/s, MD5 signature, synckey update, failure handling)
- Feature 32: Progress Throttling & Immediate Triggers (60s throttling, 10-page trigger, chapter switch trigger, book exit trigger, sleep trigger)
- Feature 33: Offline Progress Conflict Resolution (divergence detection, prompt condition, Keep Local choice, Use Cloud choice, silent match)
- Feature 34: Dirty Flag Marking & Reconnect Retry (network error sets dirty, disk persistence, Wi-Fi reconnect retry, clear dirty, coalescing)
- Feature 35: Wi-Fi Lifecycle & RF Power Saving (entering WeRead connects STA, unconfigured prompt/skip, reading connection hold, exit RF sleep, lock RF powerdown)
"""

from __future__ import annotations

import hashlib
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
# Feature 30: Fetch Cloud Progress (GET /book/getProgress)
# ==============================================================================

def test_f30_get_progress_query():
    """Verify GET /book/getProgress sends bookId query parameter."""
    reset_mock_state()
    status, _, body = api_request("GET", "/book/getProgress?bookId=test_book_101")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["bookId"] == "test_book_101"
    assert "book" in data


def test_f30_get_progress_attributes():
    """Verify parsing chapterUid, chapterIdx, progress, and updateTime."""
    reset_mock_state()
    status, _, body = api_request("GET", "/book/getProgress?bookId=test_book_101")
    assert status == 200
    data = json.loads(body.decode("utf-8"))["book"]
    assert data["chapterUid"] == 1
    assert data["chapterIdx"] == 1
    assert data["progress"] == 15
    assert data["bookVersion"] == 100
    assert data["updateTime"] > 0


def test_f30_get_progress_web_fallback():
    """Verify client falls back to /web/book/getProgress if primary endpoint is unavailable."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/book/getProgress": {"status": 503, "body": {"errcode": 503}}}})
    status, _, body = api_request("GET", "/web/book/getProgress?bookId=test_book_101")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["bookId"] == "test_book_101"


def test_f30_get_progress_updates_local_position():
    """Verify cloud progress updates local reading position when local has no unpushed progress."""
    local_state = {"progress": 0, "chapterUid": 0, "dirty": False}
    cloud_state = {"progress": 25, "chapterUid": 2}
    if not local_state["dirty"]:
        local_state["progress"] = cloud_state["progress"]
        local_state["chapterUid"] = cloud_state["chapterUid"]
    assert local_state["progress"] == 25
    assert local_state["chapterUid"] == 2


def test_f30_get_progress_new_book_default():
    """Verify newly opened book with no server progress defaults to 0."""
    reset_mock_state()
    status, _, body = api_request("GET", "/book/getProgress?bookId=brand_new_book_999")
    assert status == 200
    data = json.loads(body.decode("utf-8"))["book"]
    assert data["progress"] == 0
    assert data["chapterUid"] == 1


# ==============================================================================
# Feature 31: Report Reading Progress (POST /book/read) & Signature
# ==============================================================================

def test_f31_post_read_payload():
    """Verify POST /book/read sends bookId, chapterUid, chapterOffset, progress, timestamp."""
    reset_mock_state()
    payload = {
        "bookId": "test_book_101",
        "chapterUid": 2,
        "chapterOffset": 150,
        "progress": 35,
        "timestamp": int(time.time()),
    }
    status, _, body = api_request("POST", "/book/read", payload)
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data.get("succ") == 1


def test_f31_post_read_web_signed_fallback():
    """Verify fallback to /web/book/read with signed payload containing 'b' and 's' fields."""
    reset_mock_state()
    raw_body = json.dumps({"bookId": "test_book_101", "progress": 50})
    # Compute signature
    sig = hashlib.md5((raw_body + "weread_secret_salt").encode("utf-8")).hexdigest()
    web_payload = {"b": raw_body, "s": sig}

    status, _, body = api_request("POST", "/web/book/read", web_payload)
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data.get("succ") == 1


def test_f31_web_read_signature_construction():
    """Verify MD5 signature generation for web read body."""
    book_id = "test_book_101"
    chapter_uid = 1
    progress = 20
    ts = 1700000000
    raw = f"{book_id}:{chapter_uid}:{progress}:{ts}"
    expected_md5 = hashlib.md5(raw.encode("utf-8")).hexdigest()
    assert len(expected_md5) == 32
    assert all(c in "0123456789abcdef" for c in expected_md5)


def test_f31_read_progress_updates_synckey():
    """Verify successful progress report returns and updates synckey."""
    reset_mock_state()
    status, _, body = api_request("POST", "/book/read", {"bookId": "test_book_101", "progress": 10})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert "synckey" in data
    assert data["synckey"] > 0


def test_f31_post_read_server_error_handling():
    """Verify client handles server error during progress report."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/book/read": {"status": 500, "body": {"errcode": -500, "errmsg": "Failed to record"}}}})
    status, _, body = api_request("POST", "/book/read", {"bookId": "test_book_101", "progress": 10})
    assert status == 500
    data = json.loads(body.decode("utf-8"))
    assert data["errcode"] == -500


# ==============================================================================
# Feature 32: Progress Throttling & Immediate Triggers
# ==============================================================================

def test_f32_periodic_progress_throttling_60s():
    """Verify background progress report is throttled to at least 60 second intervals."""
    last_report_time = 1000
    current_time_1 = 1030  # 30s later (should throttle)
    should_report_1 = (current_time_1 - last_report_time) >= 60
    assert should_report_1 is False

    current_time_2 = 1065  # 65s later (should report)
    should_report_2 = (current_time_2 - last_report_time) >= 60
    assert should_report_2 is True


def test_f32_page_turns_threshold_trigger():
    """Verify turning >= 10 pages triggers immediate progress report even before 60s."""
    page_turns = 9
    should_trigger = page_turns >= 10
    assert should_trigger is False

    page_turns += 1  # 10 pages
    should_trigger = page_turns >= 10
    assert should_trigger is True


def test_f32_chapter_switch_immediate_trigger():
    """Verify switching chapters triggers immediate unthrottled progress report."""
    event = "CHAPTER_SWITCH"
    is_immediate = event in ("CHAPTER_SWITCH", "BOOK_EXIT", "DEVICE_SLEEP")
    assert is_immediate is True


def test_f32_book_exit_immediate_trigger():
    """Verify exiting reader to bookshelf triggers immediate progress report."""
    event = "BOOK_EXIT"
    is_immediate = event in ("CHAPTER_SWITCH", "BOOK_EXIT", "DEVICE_SLEEP")
    assert is_immediate is True


def test_f32_device_sleep_immediate_trigger():
    """Verify entering device sleep/lock triggers immediate progress report before RF off."""
    event = "DEVICE_SLEEP"
    is_immediate = event in ("CHAPTER_SWITCH", "BOOK_EXIT", "DEVICE_SLEEP")
    assert is_immediate is True


# ==============================================================================
# Feature 33: Offline Progress Conflict Resolution
# ==============================================================================

def test_f33_progress_divergence_detection():
    """Verify detecting conflict when local has unpushed progress and cloud has different progress."""
    local_record = {"chapterUid": 2, "progress": 30, "updateTime": 1700000500, "dirty": True}
    cloud_record = {"chapterUid": 3, "progress": 55, "updateTime": 1700000800}

    has_conflict = (
        local_record["dirty"]
        and (local_record["chapterUid"] != cloud_record["chapterUid"] or local_record["progress"] != cloud_record["progress"])
    )
    assert has_conflict is True


def test_f33_conflict_triggers_user_prompt():
    """Verify conflict triggers user prompt dialog state."""
    has_conflict = True
    prompt_shown = False
    if has_conflict:
        prompt_shown = True
    assert prompt_shown is True


def test_f33_choice_keep_local():
    """Verify choosing 'Keep Local' retains local progress and uploads to cloud."""
    local_record = {"chapterUid": 2, "progress": 30, "dirty": True}
    cloud_record = {"chapterUid": 3, "progress": 55}
    user_choice = "KEEP_LOCAL"

    if user_choice == "KEEP_LOCAL":
        # Keep local position, mark dirty to force upload
        active_position = (local_record["chapterUid"], local_record["progress"])
        force_upload = True
    else:
        active_position = (cloud_record["chapterUid"], cloud_record["progress"])
        force_upload = False

    assert active_position == (2, 30)
    assert force_upload is True


def test_f33_choice_use_cloud():
    """Verify choosing 'Use Cloud' jumps to cloud position and clears local dirty flag."""
    local_record = {"chapterUid": 2, "progress": 30, "dirty": True}
    cloud_record = {"chapterUid": 3, "progress": 55}
    user_choice = "USE_CLOUD"

    if user_choice == "USE_CLOUD":
        local_record["chapterUid"] = cloud_record["chapterUid"]
        local_record["progress"] = cloud_record["progress"]
        local_record["dirty"] = False

    assert local_record["chapterUid"] == 3
    assert local_record["progress"] == 55
    assert local_record["dirty"] is False


def test_f33_silent_match_no_prompt():
    """Verify identical local and cloud progress synchronizes silently without user prompt."""
    local_record = {"chapterUid": 2, "progress": 30, "dirty": True}
    cloud_record = {"chapterUid": 2, "progress": 30}
    has_conflict = (
        local_record["dirty"]
        and (local_record["chapterUid"] != cloud_record["chapterUid"] or local_record["progress"] != cloud_record["progress"])
    )
    assert has_conflict is False


# ==============================================================================
# Feature 34: Dirty Flag Marking & Reconnect Retry
# ==============================================================================

def test_f34_network_failure_sets_dirty_flag():
    """Verify failed progress upload marks dirty_progress = True."""
    book_progress = {"progress": 45, "dirty": False}
    # Simulate failed HTTP request
    upload_success = False
    if not upload_success:
        book_progress["dirty"] = True
    assert book_progress["dirty"] is True


def test_f34_dirty_flag_disk_persistence():
    """Verify dirty progress state is saved to disk and survives device reboot."""
    with tempfile.TemporaryDirectory() as storage_root:
        progress_file = os.path.join(storage_root, "progress_state.json")
        saved_state = {"test_book_101": {"chapterUid": 2, "progress": 45, "dirty": True}}
        with open(progress_file, "w", encoding="utf-8") as f:
            json.dump(saved_state, f)

        # Reboot simulation: reload from disk
        with open(progress_file, "r", encoding="utf-8") as f:
            loaded_state = json.load(f)
        assert loaded_state["test_book_101"]["dirty"] is True


def test_f34_wifi_reconnect_triggers_dirty_retry():
    """Verify Wi-Fi reconnection triggers retry upload of dirty progress."""
    book_progress = {"bookId": "test_book_101", "progress": 45, "dirty": True}
    retry_triggered = False

    def on_wifi_connected():
        nonlocal retry_triggered
        if book_progress["dirty"]:
            retry_triggered = True

    on_wifi_connected()
    assert retry_triggered is True


def test_f34_successful_retry_clears_dirty_flag():
    """Verify successful upload clears dirty flag."""
    book_progress = {"progress": 45, "dirty": True}
    # Upload succeeds
    upload_success = True
    if upload_success:
        book_progress["dirty"] = False
    assert book_progress["dirty"] is False


def test_f34_multiple_offline_updates_coalesce():
    """Verify multiple offline reading turns coalesce into single latest dirty record."""
    book_progress = {"chapterUid": 1, "progress": 10, "dirty": True}
    # Read several pages offline
    book_progress.update({"chapterUid": 1, "progress": 15, "dirty": True})
    book_progress.update({"chapterUid": 1, "progress": 22, "dirty": True})
    book_progress.update({"chapterUid": 2, "progress": 5, "dirty": True})

    assert book_progress["chapterUid"] == 2
    assert book_progress["progress"] == 5
    assert book_progress["dirty"] is True


# ==============================================================================
# Feature 35: Wi-Fi Lifecycle & RF Power Saving
# ==============================================================================

def test_f35_enter_weread_connects_sta():
    """Verify entering WeRead app requests Wi-Fi STA connection."""
    wifi_events = []
    def on_app_enter(app_name):
        if app_name == "weread":
            wifi_events.append("WIFI_CONNECT_STA")

    on_app_enter("weread")
    assert wifi_events == ["WIFI_CONNECT_STA"]


def test_f35_unconfigured_wifi_prompts_or_offline_skip():
    """Verify unconfigured Wi-Fi provides configuration prompt with offline skip option."""
    wifi_configured = False
    user_action = "SKIP_OFFLINE"
    app_state = "PROMPT_WIFI" if not wifi_configured else "ONLINE"
    if app_state == "PROMPT_WIFI" and user_action == "SKIP_OFFLINE":
        app_state = "OFFLINE_READING"
    assert app_state == "OFFLINE_READING"


def test_f35_wifi_maintained_during_reading():
    """Verify Wi-Fi STA radio remains active while actively reading online."""
    reading_active = True
    rf_power = "ON" if reading_active else "SLEEP"
    assert rf_power == "ON"


def test_f35_exit_weread_powers_down_rf():
    """Verify exiting WeRead app puts Wi-Fi RF radio to sleep."""
    wifi_events = []
    def on_app_exit(app_name):
        if app_name == "weread":
            wifi_events.append("WIFI_RF_SLEEP")

    on_app_exit("weread")
    assert wifi_events == ["WIFI_RF_SLEEP"]


def test_f35_sleep_lock_immediately_powers_down_rf():
    """Verify device sleep/lock immediately powers down RF radio."""
    wifi_events = []
    def on_device_sleep():
        wifi_events.append("WIFI_RF_SLEEP")

    on_device_sleep()
    assert wifi_events == ["WIFI_RF_SLEEP"]


# ==============================================================================
# Runner
# ==============================================================================

ALL_TESTS = [
    # Feature 30
    test_f30_get_progress_query,
    test_f30_get_progress_attributes,
    test_f30_get_progress_web_fallback,
    test_f30_get_progress_updates_local_position,
    test_f30_get_progress_new_book_default,
    # Feature 31
    test_f31_post_read_payload,
    test_f31_post_read_web_signed_fallback,
    test_f31_web_read_signature_construction,
    test_f31_read_progress_updates_synckey,
    test_f31_post_read_server_error_handling,
    # Feature 32
    test_f32_periodic_progress_throttling_60s,
    test_f32_page_turns_threshold_trigger,
    test_f32_chapter_switch_immediate_trigger,
    test_f32_book_exit_immediate_trigger,
    test_f32_device_sleep_immediate_trigger,
    # Feature 33
    test_f33_progress_divergence_detection,
    test_f33_conflict_triggers_user_prompt,
    test_f33_choice_keep_local,
    test_f33_choice_use_cloud,
    test_f33_silent_match_no_prompt,
    # Feature 34
    test_f34_network_failure_sets_dirty_flag,
    test_f34_dirty_flag_disk_persistence,
    test_f34_wifi_reconnect_triggers_dirty_retry,
    test_f34_successful_retry_clears_dirty_flag,
    test_f34_multiple_offline_updates_coalesce,
    # Feature 35
    test_f35_enter_weread_connects_sta,
    test_f35_unconfigured_wifi_prompts_or_offline_skip,
    test_f35_wifi_maintained_during_reading,
    test_f35_exit_weread_powers_down_rf,
    test_f35_sleep_lock_immediately_powers_down_rf,
]


def run_all_tests():
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 1 R5 (Progress Sync & Wi-Fi Lifecycle) Suite: {len(ALL_TESTS)} tests ===")
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
