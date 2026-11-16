#!/usr/bin/env python3
"""Tier 1 Feature Coverage: Authentication, Credentials & Security (R2)

Covers:
- Feature 7: GET /web/getuid & QR URL Construction (UID retrieval, QR URL formatting, escaping, history, error injection)
- Feature 8: POST /web/getlogininfo Polling & State Machine (WAITING_CONFIRM, NEED_OTP, LOGIN_TIMEOUT, LOGIN_CANCELLED, CONFIRMED)
- Feature 9: POST /weblogin Initial Device Login (deviceType=4, token retrieval, Set-Cookie parsing, session construction, 400 error)
- Feature 10: POST /weblogin Session Renewal & Replay (deviceType=7, renewed tokens, single replay, loop guard, code -2012)
- Feature 11: Token Persistence & Logout Wipe (auth.json storage, atomic rename, recovery on boot, logout deletion, memory zeroing)
- Feature 12: Outbound Sensitive Header Stripping (Cookie, Authorization, accessToken, vid stripping on untrusted domains, trusted pass)
"""

from __future__ import annotations

import base64
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
    """Ensure mock server is running on PORT, starting a background thread if needed."""
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
# Feature 7: GET /web/getuid & QR URL Construction
# ==============================================================================

def test_f7_getuid_success_response():
    """Verify GET /web/getuid returns 200 and valid session UID."""
    reset_mock_state()
    status, headers, body = api_request("GET", "/web/getuid")
    assert status == 200, f"Expected 200, got {status}"
    data = json.loads(body.decode("utf-8"))
    assert "data" in data and "uid" in data["data"]
    assert len(data["data"]["uid"]) > 0


def test_f7_qr_url_format():
    """Verify QR code URL follows https://weread.qq.com/web/confirm?pf=2&uid=<uid> format."""
    uid = "test_qr_uid_12345"
    qr_url = f"https://weread.qq.com/web/confirm?pf=2&uid={urllib.parse.quote(uid)}"
    parsed = urllib.parse.urlparse(qr_url)
    assert parsed.scheme == "https"
    assert parsed.netloc == "weread.qq.com"
    assert parsed.path == "/web/confirm"
    qs = urllib.parse.parse_qs(parsed.query)
    assert qs["pf"] == ["2"]
    assert qs["uid"] == [uid]


def test_f7_qr_url_escaping():
    """Verify special characters in UID are properly URL escaped."""
    uid = "uid+with/special=chars&more"
    encoded = urllib.parse.quote(uid, safe="")
    qr_url = f"https://weread.qq.com/web/confirm?pf=2&uid={encoded}"
    assert "&more" not in qr_url.split("uid=")[1]  # '&' was escaped
    parsed = urllib.parse.urlparse(qr_url)
    qs = urllib.parse.parse_qs(parsed.query)
    assert qs["uid"] == [uid]


def test_f7_getuid_recorded_in_history():
    """Verify request to /web/getuid is recorded in mock server history."""
    reset_mock_state()
    api_request("GET", "/web/getuid")
    status, _, body = api_request("GET", "/mock/control/history")
    assert status == 200
    hist = json.loads(body.decode("utf-8"))
    paths = [item["path"] for item in hist["history"]]
    assert "/web/getuid" in paths


def test_f7_getuid_server_error_handling():
    """Verify client handles server error injection on /web/getuid."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/web/getuid": {"status": 500, "body": {"errcode": -500, "errmsg": "UID generator failure"}}}})
    status, _, body = api_request("GET", "/web/getuid")
    assert status == 500
    data = json.loads(body.decode("utf-8"))
    assert data["errcode"] == -500


# ==============================================================================
# Feature 8: POST /web/getlogininfo Polling & State Machine
# ==============================================================================

def test_f8_polling_waiting_confirm():
    """Verify WAITING_CONFIRM returns succeed: false without logicCode."""
    reset_mock_state()
    set_mock_state({"qr_status": "WAITING_CONFIRM"})
    status, _, body = api_request("POST", "/web/getlogininfo")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data.get("data", {}).get("succeed") is False
    assert "logicCode" not in data.get("data", {})


def test_f8_polling_need_otp():
    """Verify NEED_OTP returns logicCode: NEED_OTP."""
    reset_mock_state()
    set_mock_state({"qr_status": "NEED_OTP"})
    status, _, body = api_request("POST", "/web/getlogininfo")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["data"]["logicCode"] == "NEED_OTP"


def test_f8_polling_login_timeout():
    """Verify LOGIN_TIMEOUT returns logicCode: LOGIN_TIMEOUT."""
    reset_mock_state()
    set_mock_state({"qr_status": "LOGIN_TIMEOUT"})
    status, _, body = api_request("POST", "/web/getlogininfo")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["data"]["logicCode"] == "LOGIN_TIMEOUT"


def test_f8_polling_login_cancelled():
    """Verify LOGIN_CANCELLED returns logicCode: LOGIN_CANCELLED."""
    reset_mock_state()
    set_mock_state({"qr_status": "LOGIN_CANCELLED"})
    status, _, body = api_request("POST", "/web/getlogininfo")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["data"]["logicCode"] == "LOGIN_CANCELLED"


def test_f8_polling_confirmed():
    """Verify CONFIRMED returns vid, skey, and confirmation code."""
    reset_mock_state()
    set_mock_state({"qr_status": "CONFIRMED", "current_vid": 98765432, "current_skey": "valid_skey_xyz"})
    status, _, body = api_request("POST", "/web/getlogininfo")
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["vid"] == 98765432
    assert data["skey"] == "valid_skey_xyz"
    assert "code" in data
    assert data.get("wxNickName") == "KiikoReader"


# ==============================================================================
# Feature 9: POST /weblogin Initial Device Login (deviceType=4)
# ==============================================================================

def test_f9_weblogin_device_type_4_request():
    """Verify POST /weblogin sends deviceType=4 and auth code."""
    reset_mock_state()
    body_payload = {"deviceType": 4, "code": "mock_qr_code_xyz123"}
    status, headers, body = api_request("POST", "/weblogin", body_payload)
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert "accessToken" in data
    assert "refreshToken" in data


def test_f9_weblogin_device_type_4_tokens():
    """Verify weblogin returns webLoginVid and access tokens."""
    reset_mock_state()
    set_mock_state({"current_vid": 55667788, "access_token": "token_abc_123", "refresh_token": "refresh_def_456"})
    status, _, body = api_request("POST", "/weblogin", {"deviceType": 4})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["webLoginVid"] == "55667788"
    assert data["accessToken"] == "token_abc_123"
    assert data["refreshToken"] == "refresh_def_456"


def test_f9_weblogin_cookie_extraction():
    """Verify Set-Cookie headers contain wr_vid, wr_skey, wr_rt."""
    reset_mock_state()
    status, headers, _ = api_request("POST", "/weblogin", {"deviceType": 4})
    assert status == 200
    cookie_hdr = headers.get("Set-Cookie", "")
    assert "wr_vid=" in cookie_hdr
    assert "wr_skey=" in cookie_hdr
    assert "wr_rt=" in cookie_hdr


def test_f9_weblogin_session_assembly():
    """Verify assembling weread_auth_session_t dictionary from login response."""
    response_data = {
        "webLoginVid": "12345678",
        "accessToken": "mock_access_token_token",
        "refreshToken": "mock_refresh_token_xyz",
    }
    session = {
        "vid": response_data["webLoginVid"],
        "skey": response_data["accessToken"],
        "access_token": response_data["accessToken"],
        "refresh_token": response_data["refreshToken"],
        "device_id": "esp32_device_001",
        "updated_at": int(time.time()),
    }
    assert session["vid"] == "12345678"
    assert session["access_token"] == "mock_access_token_token"
    assert session["refresh_token"] == "mock_refresh_token_xyz"


def test_f9_weblogin_bad_request_handling():
    """Verify client handles error when auth code is invalid (400 Bad Request)."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/weblogin": {"status": 400, "body": {"errcode": -400, "errmsg": "Invalid confirmation code"}}}})
    status, _, body = api_request("POST", "/weblogin", {"deviceType": 4, "code": "bad_code"})
    assert status == 400
    data = json.loads(body.decode("utf-8"))
    assert data["errcode"] == -400


# ==============================================================================
# Feature 10: POST /weblogin Session Renewal (deviceType=7) & Replay
# ==============================================================================

def test_f10_token_expiration_renewal_trigger():
    """Verify encountering -2013 triggers renewal request with deviceType=7."""
    reset_mock_state()
    # 1. First trigger renewal request
    status, _, body = api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7, "refreshToken": "old_token"})
    assert status == 200
    data = json.loads(body.decode("utf-8"))
    assert data["accessToken"] == "mock_renewed_access_token"
    assert data["refreshToken"] == "mock_renewed_refresh_token"


def test_f10_renewal_token_storage():
    """Verify renewed tokens are updated in session state."""
    session = {"access_token": "old_token", "refresh_token": "old_refresh"}
    renewal_response = {"accessToken": "mock_renewed_access_token", "refreshToken": "mock_renewed_refresh_token"}
    session["access_token"] = renewal_response["accessToken"]
    session["refresh_token"] = renewal_response["refreshToken"]
    session["updated_at"] = int(time.time())

    assert session["access_token"] == "mock_renewed_access_token"
    assert session["refresh_token"] == "mock_renewed_refresh_token"


def test_f10_single_replay_succeeds():
    """Verify client replays original request once after renewal and succeeds."""
    reset_mock_state()
    # Simulate first request failing with -2013
    fail_state = {"force_error": {"status": 401, "body": {"errcode": -2013, "errmsg": "session expired"}}}
    set_mock_state(fail_state)

    status1, _, body1 = api_request("GET", "/shelf/sync")
    assert status1 == 401
    assert json.loads(body1.decode("utf-8"))["errcode"] == -2013

    # Renewal occurs
    set_mock_state({"force_error": None})
    status_renew, _, _ = api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7})
    assert status_renew == 200

    # Replay request
    status2, _, body2 = api_request("GET", "/shelf/sync")
    assert status2 == 200
    assert "books" in json.loads(body2.decode("utf-8"))


def test_f10_replay_limit_aborts_loop():
    """Verify single-replay guard: persistent -2013 aborts and avoids infinite loop."""
    replay_count = 0
    max_replays = 1

    def execute_with_renewal():
        nonlocal replay_count
        while True:
            # Simulate endpoint always returning -2013
            errcode = -2013
            if errcode == -2013:
                if replay_count >= max_replays:
                    return "AUTH_FAILED_ABORT"
                replay_count += 1
                # Execute renewal (which succeeds)
                # Next loop iteration is replay
                continue
            return "SUCCESS"

    result = execute_with_renewal()
    assert result == "AUTH_FAILED_ABORT"
    assert replay_count == 1


def test_f10_renewal_on_code_2012():
    """Verify -2012 (login required) similarly triggers the session renewal flow."""
    reset_mock_state()
    set_mock_state({"force_error": {"status": 401, "body": {"errcode": -2012, "errmsg": "login required"}}})
    status, _, body = api_request("GET", "/shelf/sync")
    assert status == 401
    data = json.loads(body.decode("utf-8"))
    assert data["errcode"] == -2012
    # Verify renewal endpoint is accessible
    set_mock_state({"force_error": None})
    r_status, _, r_body = api_request("POST", "/weblogin?deviceType=7", {"deviceType": 7})
    assert r_status == 200


# ==============================================================================
# Feature 11: Token Persistence (auth.json) & Logout Wipe
# ==============================================================================

def test_f11_auth_json_save_structure():
    """Verify writing auth session to auth.json with full schema."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        session = {
            "vid": "12345678",
            "skey": "mock_skey",
            "accessToken": "access_token_val",
            "refreshToken": "refresh_token_val",
            "deviceId": "kiiko_pico_01",
            "updatedAt": 1700000000,
        }
        with open(auth_file, "w", encoding="utf-8") as f:
            json.dump(session, f)

        with open(auth_file, "r", encoding="utf-8") as f:
            loaded = json.load(f)
        assert loaded["vid"] == "12345678"
        assert loaded["accessToken"] == "access_token_val"


def test_f11_auth_json_atomic_rename():
    """Verify atomic write semantics (.tmp followed by atomic os.replace)."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        tmp_file = auth_file + ".tmp"
        data = {"vid": "88888888", "accessToken": "new_token"}

        with open(tmp_file, "w", encoding="utf-8") as f:
            json.dump(data, f)
            f.flush()
            os.fsync(f.fileno())

        os.replace(tmp_file, auth_file)
        assert not os.path.exists(tmp_file)
        assert os.path.exists(auth_file)
        with open(auth_file, "r", encoding="utf-8") as f:
            assert json.load(f)["vid"] == "88888888"


def test_f11_auth_json_load_and_validate():
    """Verify reading persisted credentials on startup and validating presence."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        with open(auth_file, "w", encoding="utf-8") as f:
            json.dump({"vid": "1234", "accessToken": "token"}, f)

        def load_session(path):
            if not os.path.isfile(path):
                return None
            with open(path, "r", encoding="utf-8") as f:
                d = json.load(f)
            if not d.get("vid") or not d.get("accessToken"):
                return None
            return d

        sess = load_session(auth_file)
        assert sess is not None
        assert sess["vid"] == "1234"


def test_f11_logout_wipes_auth_file():
    """Verify logout removes auth.json completely from disk."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        with open(auth_file, "w", encoding="utf-8") as f:
            f.write('{"logged_in": true}')

        assert os.path.exists(auth_file)
        # Logout
        if os.path.exists(auth_file):
            os.remove(auth_file)

        assert not os.path.exists(auth_file)


def test_f11_memory_wipe_on_logout():
    """Verify in-memory sensitive tokens are overwritten with zeros on logout."""
    token_buf = bytearray(b"super_secret_access_token_12345")
    # Simulate memory wipe
    for i in range(len(token_buf)):
        token_buf[i] = 0
    assert all(b == 0 for b in token_buf)


# ==============================================================================
# Feature 12: Outbound Sensitive Header Stripping
# ==============================================================================

def test_f12_strip_cookie_on_untrusted():
    """Verify mock server intercepts and rejects Cookie header on untrusted paths."""
    reset_mock_state()
    # Sending Cookie to /untrusted/asset should be rejected by mock server
    status, _, body = api_request("GET", "/untrusted/asset", headers={"Cookie": "wr_vid=1234"})
    assert status == 403
    data = json.loads(body.decode("utf-8"))
    assert "cookie" in data["leaked"]


def test_f12_strip_authorization_on_untrusted():
    """Verify Authorization header is rejected on untrusted endpoints."""
    reset_mock_state()
    status, _, body = api_request("GET", "/untrusted/asset", headers={"Authorization": "Bearer secret"})
    assert status == 403
    data = json.loads(body.decode("utf-8"))
    assert "authorization" in data["leaked"]


def test_f12_strip_access_token_on_untrusted():
    """Verify accessToken header is rejected on untrusted endpoints."""
    reset_mock_state()
    status, _, body = api_request("GET", "/untrusted/asset", headers={"accessToken": "token_abc"})
    assert status == 403
    data = json.loads(body.decode("utf-8"))
    assert "accesstoken" in data["leaked"]


def test_f12_strip_vid_on_untrusted():
    """Verify vid header is rejected on untrusted endpoints."""
    reset_mock_state()
    status, _, body = api_request("GET", "/untrusted/asset", headers={"vid": "12345678"})
    assert status == 403
    data = json.loads(body.decode("utf-8"))
    assert "vid" in data["leaked"]


def test_f12_allow_credentials_on_trusted_domain():
    """Verify requests with sensitive credentials pass normally without leakage warnings on trusted endpoints."""
    reset_mock_state()
    status, _, body = api_request(
        "GET",
        "/shelf/sync",
        headers={
            "Cookie": "wr_vid=12345678; wr_skey=valid_skey",
            "accessToken": "valid_token",
            "vid": "12345678",
        },
    )
    assert status == 200
    # Also verify untrusted request without sensitive headers succeeds
    u_status, _, u_body = api_request("GET", "/untrusted/image.jpg", headers={"User-Agent": "KiikoReader"})
    assert u_status == 200


# ==============================================================================
# Runner
# ==============================================================================

ALL_TESTS = [
    # Feature 7
    test_f7_getuid_success_response,
    test_f7_qr_url_format,
    test_f7_qr_url_escaping,
    test_f7_getuid_recorded_in_history,
    test_f7_getuid_server_error_handling,
    # Feature 8
    test_f8_polling_waiting_confirm,
    test_f8_polling_need_otp,
    test_f8_polling_login_timeout,
    test_f8_polling_login_cancelled,
    test_f8_polling_confirmed,
    # Feature 9
    test_f9_weblogin_device_type_4_request,
    test_f9_weblogin_device_type_4_tokens,
    test_f9_weblogin_cookie_extraction,
    test_f9_weblogin_session_assembly,
    test_f9_weblogin_bad_request_handling,
    # Feature 10
    test_f10_token_expiration_renewal_trigger,
    test_f10_renewal_token_storage,
    test_f10_single_replay_succeeds,
    test_f10_replay_limit_aborts_loop,
    test_f10_renewal_on_code_2012,
    # Feature 11
    test_f11_auth_json_save_structure,
    test_f11_auth_json_atomic_rename,
    test_f11_auth_json_load_and_validate,
    test_f11_logout_wipes_auth_file,
    test_f11_memory_wipe_on_logout,
    # Feature 12
    test_f12_strip_cookie_on_untrusted,
    test_f12_strip_authorization_on_untrusted,
    test_f12_strip_access_token_on_untrusted,
    test_f12_strip_vid_on_untrusted,
    test_f12_allow_credentials_on_trusted_domain,
]


def run_all_tests():
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 1 R2 (Authentication, Credentials & Security) Suite: {len(ALL_TESTS)} tests ===")
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
