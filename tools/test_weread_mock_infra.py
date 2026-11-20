#!/usr/bin/env python3
"""Smoke test and verification suite for WeRead test mock harness.

Validates:
1. Control endpoints: /mock/control/reset, /mock/control/set_state, /mock/control/history
2. WeRead Auth endpoints: /web/getuid, /web/getlogininfo (all states), /weblogin (type 4 & 7)
3. Bookshelf & TOC: /shelf/sync (full & incremental), /shelf/syncbook, /book/chapterInfos
4. Chapter download & Crypto:
   - EPUB download with encryptkey header
   - AES-128-CBC key unwrapping to password
   - ZipCrypto ZIP unpacking with decrypted password
   - TXT TAR download and member extraction
5. Progress: /book/getProgress, /book/read (primary & web signed fallback)
6. Error injections: -2013, -2012, 402, 499 (-2063)
7. Security checks: Sensitive header stripping on untrusted domains
8. C Host Stub integration: Compiles and executes C test using esp_http_client_mock + book_weread_crypto with ASan/UBSan.
"""

from __future__ import annotations

import base64
import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import tarfile
import io

# Pure python AES-128-CBC decrypt for self-contained test assertion
from weread_mock_server import _aes128_expand_key, _AES_INV_SBOX, _gf_mul


def _aes128_decrypt_block(block: bytes, round_keys: list[list[int]]) -> bytes:
    state = [bytearray(block[r::4]) for r in range(4)]

    def add_round_key(rk: list[int]) -> None:
        for c in range(4):
            for r in range(4):
                state[r][c] ^= rk[4 * c + r]

    add_round_key(round_keys[10])
    for rnd in range(9, -1, -1):
        state[1] = bytearray([state[1][3], state[1][0], state[1][1], state[1][2]])
        state[2] = bytearray([state[2][2], state[2][3], state[2][0], state[2][1]])
        state[3] = bytearray([state[3][1], state[3][2], state[3][3], state[3][0]])
        for r in range(4):
            for c in range(4):
                state[r][c] = _AES_INV_SBOX[state[r][c]]
        add_round_key(round_keys[rnd])
        if rnd > 0:
            for c in range(4):
                s0, s1, s2, s3 = state[0][c], state[1][c], state[2][c], state[3][c]
                state[0][c] = _gf_mul(0x0E, s0) ^ _gf_mul(0x0B, s1) ^ _gf_mul(0x0D, s2) ^ _gf_mul(0x09, s3)
                state[1][c] = _gf_mul(0x09, s0) ^ _gf_mul(0x0E, s1) ^ _gf_mul(0x0B, s2) ^ _gf_mul(0x0D, s3)
                state[2][c] = _gf_mul(0x0D, s0) ^ _gf_mul(0x09, s1) ^ _gf_mul(0x0E, s2) ^ _gf_mul(0x0B, s3)
                state[3][c] = _gf_mul(0x0B, s0) ^ _gf_mul(0x0D, s1) ^ _gf_mul(0x09, s2) ^ _gf_mul(0x0E, s3)
    return bytes(state[r][c] for c in range(4) for r in range(4))


def aes128_cbc_decrypt(key: bytes, iv: bytes, ct: bytes) -> bytes:
    rk = _aes128_expand_key(key)
    out = bytearray()
    prev = iv
    for off in range(0, len(ct), 16):
        blk = ct[off : off + 16]
        dec = _aes128_decrypt_block(blk, rk)
        out.extend(dec[i] ^ prev[i] for i in range(16))
        prev = blk
    pad_len = out[-1]
    if pad_len < 1 or pad_len > 16 or any(b != pad_len for b in out[-pad_len:]):
        raise ValueError("Invalid PKCS#7 padding")
    return bytes(out[:-pad_len])


def decrypt_encrypt_key(encrypt_key_b64: str, secret: str) -> bytes:
    buf = bytearray()
    s_bytes = secret.encode("utf-8")
    while len(buf) < 32:
        buf.extend(s_bytes)
    key = bytes(buf[:16])
    iv = bytes(buf[16:32])
    ct = base64.b64decode(encrypt_key_b64)
    return aes128_cbc_decrypt(key, iv, ct)


class MockHarnessTester:
    def __init__(self, port: int = 8989) -> None:
        self.port = port
        self.base_url = f"http://127.0.0.1:{port}"

    def req(
        self,
        method: str,
        path: str,
        body: dict | bytes | None = None,
        headers: dict | None = None,
    ) -> tuple[int, dict, bytes]:
        url = f"{self.base_url}{path}"
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
                resp_headers = dict(resp.headers)
                resp_body = resp.read()
                return resp.status, resp_headers, resp_body
        except urllib.error.HTTPError as err:
            return err.code, dict(err.headers), err.read()

    def test_control_endpoints(self) -> None:
        print("[TEST] 1. Control endpoints: health, reset, set_state, history...")
        # Health
        code, _, body = self.req("GET", "/mock/control/health")
        assert code == 200, f"Expected 200, got {code}"
        assert json.loads(body)["status"] == "ok"

        # Reset
        code, _, _ = self.req("POST", "/mock/control/reset")
        assert code == 200

        # Set state
        code, _, _ = self.req("POST", "/mock/control/set_state", {"qr_status": "NEED_OTP", "synckey": 2000})
        assert code == 200

        # Make a sample API request to record in history
        code, _, _ = self.req("GET", "/web/getuid")
        assert code == 200

        # History check
        code, _, body = self.req("GET", "/mock/control/history")
        assert code == 200
        hist = json.loads(body)
        assert hist["count"] >= 3
        paths = [item["path"] for item in hist["history"]]
        assert "/web/getuid" in paths
        print("  -> Control endpoints verified.")

    def test_auth_flow(self) -> None:
        print("[TEST] 2. WeRead Auth endpoints: getuid, getlogininfo, weblogin...")
        self.req("POST", "/mock/control/reset")

        # GET /web/getuid
        code, _, body = self.req("GET", "/web/getuid?platform=desktop")
        assert code == 200
        uid_data = json.loads(body)
        assert "uid" in uid_data["data"]
        uid = uid_data["data"]["uid"]
        assert len(uid) > 0

        # POST /web/getlogininfo across all states
        states = [
            ("WAITING_CONFIRM", lambda d: d["data"]["succeed"] is False),
            ("NEED_OTP", lambda d: d["data"].get("logicCode") == "NEED_OTP"),
            ("LOGIN_TIMEOUT", lambda d: d["data"].get("logicCode") == "LOGIN_TIMEOUT"),
            ("LOGIN_CANCELLED", lambda d: d["data"].get("logicCode") == "LOGIN_CANCELLED"),
            ("CONFIRMED", lambda d: d.get("vid") == 12345678 and d.get("skey")),
        ]
        for st, validator in states:
            self.req("POST", "/mock/control/set_state", {"qr_status": st})
            code, _, body = self.req("POST", "/web/getlogininfo?platform=desktop", {"uid": uid})
            assert code == 200
            data = json.loads(body)
            assert validator(data), f"State {st} failed validation: {data}"

        # POST /weblogin (deviceType=4)
        login_body = {
            "vid": "12345678",
            "skey": "mock_skey_123456",
            "code": "mock_code_abc",
            "deviceId": "eink-test-device",
            "deviceType": 4,
            "deviceName": "KiikoReader",
        }
        code, headers, body = self.req("POST", "/weblogin?platform=desktop&deviceType=4", login_body)
        assert code == 200
        tokens = json.loads(body)
        assert tokens.get("accessToken")
        assert tokens.get("refreshToken")
        assert "Set-Cookie" in headers or "set-cookie" in headers

        # POST /weblogin (deviceType=7 session renewal)
        renew_body = {
            "vid": "12345678",
            "refreshToken": tokens["refreshToken"],
            "deviceId": "eink-test-device",
            "deviceType": 7,
            "isFromweb": 0,
        }
        code, _, body = self.req("POST", "/weblogin?deviceType=7", renew_body)
        assert code == 200
        renewed = json.loads(body)
        assert renewed.get("accessToken") == "mock_renewed_access_token"
        print("  -> Auth flow verified.")

    def test_shelf_and_toc(self) -> None:
        print("[TEST] 3. WeRead Bookshelf & TOC endpoints...")
        self.req("POST", "/mock/control/reset")

        # Full sync: must include books and lectureBooks
        code, _, body = self.req("GET", "/shelf/sync?synckey=0&lectureSynckey=0")
        assert code == 200
        shelf = json.loads(body)
        assert len(shelf["books"]) >= 2
        assert len(shelf["lectureBooks"]) >= 1, "lectureBooks must be present for filtering tests"
        assert shelf["lectureBooks"][0]["type"] == 1

        # Incremental sync
        code, _, body = self.req("GET", "/shelf/sync?synckey=1000&onlyBookid=1")
        assert code == 200
        inc = json.loads(body)
        assert "bookIds" in inc
        assert inc["synckey"] == 1001

        # Batch fetch book details (/shelf/syncbook)
        code, _, body = self.req("POST", "/shelf/syncbook", {"bookIds": ["test_book_101"], "albumIds": []})
        assert code == 200
        details = json.loads(body)
        assert len(details["books"]) == 1
        assert details["books"][0]["bookId"] == "test_book_101"

        # Chapter TOC (/book/chapterInfos)
        code, _, body = self.req("POST", "/book/chapterInfos", {"bookIds": ["test_book_101"], "synckeys": [0]})
        assert code == 200
        toc = json.loads(body)
        assert len(toc["data"]) == 1
        assert toc["data"][0]["clearAll"] == 0
        assert len(toc["data"][0]["updated"]) >= 2

        # TOC with clearAll flag
        self.req("POST", "/mock/control/set_state", {"clear_all_toc": True})
        code, _, body = self.req("POST", "/book/chapterInfos", {"bookIds": ["test_book_101"]})
        assert code == 200
        toc2 = json.loads(body)
        assert toc2["data"][0]["clearAll"] == 1
        print("  -> Bookshelf and TOC verified.")

    def test_chapter_download_and_crypto(self) -> None:
        print("[TEST] 4. Chapter download, authentic encryptkey and ZipCrypto unpack...")
        self.req("POST", "/mock/control/reset")

        # 1. EPUB download: serves encrypted zip with encryptkey header
        code, headers, body = self.req("GET", "/book/chapterdownload?bookId=test_book_101&chapters=0,1&bookType=epub")
        assert code == 200, f"Expected 200, got {code}"
        enc_hdr = headers.get("encryptkey") or headers.get("Encryptkey")
        assert enc_hdr, "encryptkey header must be present in response"

        # Decrypt encryptkey via AES-128-CBC with vid="12345678"
        plain_pwd = decrypt_encrypt_key(enc_hdr, "12345678")
        assert plain_pwd == b"test_password_123", f"Expected test_password_123, got {plain_pwd}"

        # Unpack ZipCrypto ZIP using decrypted password
        with zipfile.ZipFile(io.BytesIO(body)) as zf:
            namelist = zf.namelist()
            assert "info.txt" in namelist
            assert "Styles/main.css" in namelist
            assert "Text/chapter1_part1.xhtml" in namelist

            info_txt = zf.read("info.txt", pwd=plain_pwd).decode("utf-8")
            assert "test_book_101" in info_txt

            part1 = zf.read("Text/chapter1_part1.xhtml", pwd=plain_pwd).decode("utf-8")
            assert "第一章 起源" in part1

        # 2. TXT download: serves plaintext TAR
        code, headers, body = self.req("GET", "/book/chapterdownload?bookId=txt_book_202&chapters=1&bookType=txt")
        assert code == 200
        ct_header = headers.get("Content-Type") or headers.get("content-type")
        assert "tar" in ct_header

        with tarfile.open(fileobj=io.BytesIO(body)) as tf:
            names = tf.getnames()
            assert "info.txt" in names
            assert "txt_book_202_1_o" in names
            txt_content = tf.extractfile("txt_book_202_1_o").read().decode("utf-8")
            assert "这是一个纯文本书籍的测试章节" in txt_content

        print("  -> Chapter download and cryptographic unpack verified.")

    def test_progress_and_signatures(self) -> None:
        print("[TEST] 5. Progress query and report (primary & web signed fallback)...")
        self.req("POST", "/mock/control/reset")

        # GET /book/getProgress
        code, _, body = self.req("GET", "/book/getProgress?bookId=test_book_101")
        assert code == 200
        prog = json.loads(body)
        assert prog["bookId"] == "test_book_101"
        assert prog["book"]["progress"] == 15

        # POST /book/read primary
        code, _, body = self.req("POST", "/book/read", {"bookId": "test_book_101", "progress": 20})
        assert code == 200
        res = json.loads(body)
        assert res["succ"] == 1

        # Web signed fallback /web/book/read
        # 1) unsigned body returns empty 200 (silent drop simulation)
        code, _, body = self.req("POST", "/web/book/read", {"bookId": "test_book_101"})
        assert code == 200
        assert json.loads(body) == {}

        # 2) signed body returns success
        signed_body = {
            "appId": "wb_test_id",
            "b": "test_book_101_enc",
            "c": "ch1_enc",
            "s": "valid_signature_hash",
        }
        code, _, body = self.req("POST", "/web/book/read", signed_body)
        assert code == 200
        res2 = json.loads(body)
        assert res2["succ"] == 1
        print("  -> Reading progress endpoints verified.")

    def test_error_injections(self) -> None:
        print("[TEST] 6. Error injections: -2013, -2012, 402, 499 (-2063)...")
        # Global force -2013
        self.req("POST", "/mock/control/set_state", {
            "force_error": {"status": 401, "body": {"errcode": -2013, "errmsg": "access token expired"}}
        })
        code, _, body = self.req("GET", "/shelf/sync")
        assert code == 401
        assert json.loads(body)["errcode"] == -2013

        # Endpoint-specific 402 payment required on chapter download
        self.req("POST", "/mock/control/reset")
        self.req("POST", "/mock/control/set_state", {
            "endpoint_errors": {
                "/book/chapterdownload": {"status": 402, "body": {"errcode": -2001, "errmsg": "Purchase required"}}
            }
        })
        code, _, body = self.req("GET", "/book/chapterdownload?bookId=test_book_101&chapters=3")
        assert code == 402
        assert json.loads(body)["errcode"] == -2001

        # Version expired -2063
        self.req("POST", "/mock/control/set_state", {
            "endpoint_errors": {
                "/book/chapterdownload": {"status": 499, "body": {"errcode": -2063, "errmsg": "Book version expired"}}
            }
        })
        code, _, body = self.req("GET", "/book/chapterdownload?bookId=test_book_101&chapters=1")
        assert code == 499
        assert json.loads(body)["errcode"] == -2063

        self.req("POST", "/mock/control/reset")
        print("  -> Error injections verified.")

    def test_security_checks(self) -> None:
        print("[TEST] 7. Security: Outbound sensitive header stripping detection...")
        self.req("POST", "/mock/control/reset")

        # Request to untrusted domain WITH sensitive headers -> Violation flagged!
        code, _, body = self.req(
            "GET",
            "/thirdparty/cover.jpg",
            headers={"accessToken": "leaked_secret_token", "Cookie": "wr_vid=123"}
        )
        assert code == 403, f"Expected 403 on security violation, got {code}"
        assert "leaked" in json.loads(body)

        # Check history recorded violation
        code, _, body = self.req("GET", "/mock/control/history")
        hist = json.loads(body)
        assert len(hist.get("violations", [])) >= 1
        print("  -> Security violation detection verified.")

        # Request to untrusted domain WITHOUT sensitive headers -> Allowed!
        code, _, _ = self.req(
            "GET",
            "/thirdparty/cover.jpg",
            headers={"User-Agent": "WeRead/1.0.0"}
        )
        assert code == 200
        print("  -> Safe outbound request allowed.")

    def test_c_host_stub_integration(self) -> None:
        print("[TEST] 8. C Host Stub integration: compiling and executing test against mock server...")
        self.req("POST", "/mock/control/reset")

        # Write a small standalone C test program exercising esp_http_client_mock + book_weread_crypto
        c_test_src = """
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "esp_http_client.h"
#include "book_weread_crypto.h"
#include "kiiko_storage_mock.h"

static bool s_found_info = false;
static bool s_found_text = false;

static bool on_zip_entry(void *ctx, const char *name, const uint8_t *data, size_t size) {
    (void)ctx; (void)data; (void)size;
    if (strcmp(name, "info.txt") == 0) s_found_info = true;
    if (strstr(name, "chapter1_part1.xhtml")) s_found_text = true;
    return true;
}

int main(void) {
    printf("[C_TEST] Starting C host test integration...\\n");

    // Test storage mock
    assert(weread_storage_init() == 0);
    char path_buf[256];
    assert(weread_storage_path("auth.json", path_buf, sizeof(path_buf)) == 0);
    assert(strstr(path_buf, "auth.json") != NULL);

    // Test esp_http_client GET /web/getuid
    esp_http_client_config_t cfg = {
        .url = "http://127.0.0.1:8989/web/getuid?platform=desktop",
        .method = HTTP_METHOD_GET,
        .timeout_ms = 3000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    assert(client != NULL);

    assert(esp_http_client_open(client, 0) == ESP_OK);
    int64_t content_len = esp_http_client_fetch_headers(client);
    assert(content_len > 0);
    assert(esp_http_client_get_status_code(client) == 200);

    char resp[1024];
    int r = esp_http_client_read(client, resp, sizeof(resp) - 1);
    assert(r > 0);
    resp[r] = '\\0';
    assert(strstr(resp, "uid") != NULL);
    esp_http_client_cleanup(client);
    printf("[C_TEST] GET /web/getuid OK\\n");

    // Test esp_http_client GET /book/chapterdownload with encryptkey header
    cfg.url = "http://127.0.0.1:8989/book/chapterdownload?bookId=test_book_101&chapters=0,1";
    client = esp_http_client_init(&cfg);
    assert(esp_http_client_open(client, 0) == ESP_OK);
    content_len = esp_http_client_fetch_headers(client);
    assert(content_len > 0);
    assert(esp_http_client_get_status_code(client) == 200);

    char *enc_key_hdr = NULL;
    assert(esp_http_client_get_header(client, "encryptkey", &enc_key_hdr) == ESP_OK);
    assert(enc_key_hdr != NULL);

    // Read full zip payload
    uint8_t *zip_payload = (uint8_t *)malloc((size_t)content_len);
    assert(zip_payload != NULL);
    size_t total = 0;
    while (total < (size_t)content_len) {
        int n = esp_http_client_read(client, (char *)zip_payload + total, (int)(content_len - total));
        if (n <= 0) break;
        total += (size_t)n;
    }
    assert(total == (size_t)content_len);

    // Decrypt encryptkey with secret "12345678" before cleaning up client
    uint8_t pwd[64];
    size_t pwd_len = 0;
    assert(book_weread_decrypt_encrypt_key(enc_key_hdr, "12345678", pwd, sizeof(pwd), &pwd_len));
    assert(pwd_len == 17);
    assert(memcmp(pwd, "test_password_123", 17) == 0);

    // Now cleanup client
    esp_http_client_cleanup(client);

    // Unpack ZipCrypto ZIP
    esp_err_t unpack_err = book_weread_unpack_zip(zip_payload, total, pwd, pwd_len, on_zip_entry, NULL);
    assert(unpack_err == ESP_OK);
    assert(s_found_info && s_found_text);
    free(zip_payload);

    printf("[C_TEST] Authentic chapter decryption & ZipCrypto unpack OK!\\n");
    return 0;
}
"""
        c_src_path = "/tmp/test_weread_c_host_smoke.c"
        c_bin_path = "/tmp/test_weread_c_host_smoke"
        with open(c_src_path, "w") as f:
            f.write(c_test_src)

        cmd = [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-fsanitize=address,undefined",
            "-g",
            "-Itools/weread_stubs",
            "-Imain/book",
            c_src_path,
            "tools/weread_stubs/esp_http_client_mock.c",
            "main/book/book_weread_crypto.c",
            "-lz",
            "-o",
            c_bin_path,
        ]
        comp = subprocess.run(cmd, capture_output=True, text=True)
        assert comp.returncode == 0, f"Compilation failed:\nSTDOUT: {comp.stdout}\nSTDERR: {comp.stderr}"

        # Run the binary with ASan/UBSan
        run_res = subprocess.run([c_bin_path], capture_output=True, text=True, env=os.environ.copy())
        assert run_res.returncode == 0, f"C host test failed:\nSTDOUT: {run_res.stdout}\nSTDERR: {run_res.stderr}"
        assert "Authentic chapter decryption & ZipCrypto unpack OK!" in run_res.stdout
        print("  -> C host stub integration with ASan/UBSan passed cleanly (zero leaks, zero errors).")


def main() -> None:
    port = int(os.environ.get("WEREAD_MOCK_PORT", "8989"))
    tester = MockHarnessTester(port)

    print(f"=== Running WeRead Mock Harness Smoke Suite on port {port} ===")
    tester.test_control_endpoints()
    tester.test_auth_flow()
    tester.test_shelf_and_toc()
    tester.test_chapter_download_and_crypto()
    tester.test_progress_and_signatures()
    tester.test_error_injections()
    tester.test_security_checks()
    tester.test_c_host_stub_integration()
    print("=== All WeRead Mock Harness Smoke Tests PASSED successfully! ===")


if __name__ == "__main__":
    main()
