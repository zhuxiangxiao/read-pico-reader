#!/usr/bin/env python3
"""Standalone loopback mock HTTP server for WeRead (微信读书) E2E test harness.

Implements:
- Control API: /mock/control/reset, /mock/control/set_state, /mock/control/history, /mock/control/health
- WeRead Auth API: /web/getuid, /web/getlogininfo, /weblogin
- WeRead Shelf & TOC: /shelf/sync, /shelf/syncbook, /book/chapterInfos
- WeRead Content & Progress: /book/chapterdownload, /book/getProgress, /book/read
- Security checks: Outbound sensitive header stripping detection on untrusted paths.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import signal
import struct
import sys
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, HTTPServer
from socketserver import ThreadingMixIn

# AES-128 primitives in pure standard library
_AES_SBOX = (
    0x63, 0x7C, 0x77, 0x7B, 0xF2, 0x6B, 0x6F, 0xC5, 0x30, 0x01, 0x67, 0x2B, 0xFE, 0xD7, 0xAB, 0x76,
    0xCA, 0x82, 0xC9, 0x7D, 0xFA, 0x59, 0x47, 0xF0, 0xAD, 0xD4, 0xA2, 0xAF, 0x9C, 0xA4, 0x72, 0xC0,
    0xB7, 0xFD, 0x93, 0x26, 0x36, 0x3F, 0xF7, 0xCC, 0x34, 0xA5, 0xE5, 0xF1, 0x71, 0xD8, 0x31, 0x15,
    0x04, 0xC7, 0x23, 0xC3, 0x18, 0x96, 0x05, 0x9A, 0x07, 0x12, 0x80, 0xE2, 0xEB, 0x27, 0xB2, 0x75,
    0x09, 0x83, 0x2C, 0x1A, 0x1B, 0x6E, 0x5A, 0xA0, 0x52, 0x3B, 0xD6, 0xB3, 0x29, 0xE3, 0x2F, 0x84,
    0x53, 0xD1, 0x00, 0xED, 0x20, 0xFC, 0xB1, 0x5B, 0x6A, 0xCB, 0xBE, 0x39, 0x4A, 0x4C, 0x58, 0xCF,
    0xD0, 0xEF, 0xAA, 0xFB, 0x43, 0x4D, 0x33, 0x85, 0x45, 0xF9, 0x02, 0x7F, 0x50, 0x3C, 0x9F, 0xA8,
    0x51, 0xA3, 0x40, 0x8F, 0x92, 0x9D, 0x38, 0xF5, 0xBC, 0xB6, 0xDA, 0x21, 0x10, 0xFF, 0xF3, 0xD2,
    0xCD, 0x0C, 0x13, 0xEC, 0x5F, 0x97, 0x44, 0x17, 0xC4, 0xA7, 0x7E, 0x3D, 0x64, 0x5D, 0x19, 0x73,
    0x60, 0x81, 0x4F, 0xDC, 0x22, 0x2A, 0x90, 0x88, 0x46, 0xEE, 0xB8, 0x14, 0xDE, 0x5E, 0x0B, 0xDB,
    0xE0, 0x32, 0x3A, 0x0A, 0x49, 0x06, 0x24, 0x5C, 0xC2, 0xD3, 0xAC, 0x62, 0x91, 0x95, 0xE4, 0x79,
    0xE7, 0xC8, 0x37, 0x6D, 0x8D, 0xD5, 0x4E, 0xA9, 0x6C, 0x56, 0xF4, 0xEA, 0x65, 0x7A, 0xAE, 0x08,
    0xBA, 0x78, 0x25, 0x2E, 0x1C, 0xA6, 0xB4, 0xC6, 0xE8, 0xDD, 0x74, 0x1F, 0x4B, 0xBD, 0x8B, 0x8A,
    0x70, 0x3E, 0xB5, 0x66, 0x48, 0x03, 0xF6, 0x0E, 0x61, 0x35, 0x57, 0xB9, 0x86, 0xC1, 0x1D, 0x9E,
    0xE1, 0xF8, 0x98, 0x11, 0x69, 0xD9, 0x8E, 0x94, 0x9B, 0x1E, 0x87, 0xE9, 0xCE, 0x55, 0x28, 0xDF,
    0x8C, 0xA1, 0x89, 0x0D, 0xBF, 0xE6, 0x42, 0x68, 0x41, 0x99, 0x2D, 0x0F, 0xB0, 0x54, 0xBB, 0x16,
)
_RCON = (0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1B, 0x36)

_AES_INV_SBOX = (
    0x52, 0x09, 0x6A, 0xD5, 0x30, 0x36, 0xA5, 0x38, 0xBF, 0x40, 0xA3, 0x9E, 0x81, 0xF3, 0xD7, 0xFB,
    0x7C, 0xE3, 0x39, 0x82, 0x9B, 0x2F, 0xFF, 0x87, 0x34, 0x8E, 0x43, 0x44, 0xC4, 0xDE, 0xE9, 0xCB,
    0x54, 0x7B, 0x94, 0x32, 0xA6, 0xC2, 0x23, 0x3D, 0xEE, 0x4C, 0x95, 0x0B, 0x42, 0xFA, 0xC3, 0x4E,
    0x08, 0x2E, 0xA1, 0x66, 0x28, 0xD9, 0x24, 0xB2, 0x76, 0x5B, 0xA2, 0x49, 0x6D, 0x8B, 0xD1, 0x25,
    0x72, 0xF8, 0xF6, 0x64, 0x86, 0x68, 0x98, 0x16, 0xD4, 0xA4, 0x5C, 0xCC, 0x5D, 0x65, 0xB6, 0x92,
    0x6C, 0x70, 0x48, 0x50, 0xFD, 0xED, 0xB9, 0xDA, 0x5E, 0x15, 0x46, 0x57, 0xA7, 0x8D, 0x9D, 0x84,
    0x90, 0xD8, 0xAB, 0x00, 0x8C, 0xBC, 0xD3, 0x0A, 0xF7, 0xE4, 0x58, 0x05, 0xB8, 0xB3, 0x45, 0x06,
    0xD0, 0x2C, 0x1E, 0x8F, 0xCA, 0x3F, 0x0F, 0x02, 0xC1, 0xAF, 0xBD, 0x03, 0x01, 0x13, 0x8A, 0x6B,
    0x3A, 0x91, 0x11, 0x41, 0x4F, 0x67, 0xDC, 0xEA, 0x97, 0xF2, 0xCF, 0xCE, 0xF0, 0xB4, 0xE6, 0x73,
    0x96, 0xAC, 0x74, 0x22, 0xE7, 0xAD, 0x35, 0x85, 0xE2, 0xF9, 0x37, 0xE8, 0x1C, 0x75, 0xDF, 0x6E,
    0x47, 0xF1, 0x1A, 0x71, 0x1D, 0x29, 0xC5, 0x89, 0x6F, 0xB7, 0x62, 0x0E, 0xAA, 0x18, 0xBE, 0x1B,
    0xFC, 0x56, 0x3E, 0x4B, 0xC6, 0xD2, 0x79, 0x20, 0x9A, 0xDB, 0xC0, 0xFE, 0x78, 0xCD, 0x5A, 0xF4,
    0x1F, 0xDD, 0xA8, 0x33, 0x88, 0x07, 0xC7, 0x31, 0xB1, 0x12, 0x10, 0x59, 0x27, 0x80, 0xEC, 0x5F,
    0x60, 0x51, 0x7F, 0xA9, 0x19, 0xB5, 0x4A, 0x0D, 0x2D, 0xE5, 0x7A, 0x9F, 0x93, 0xC9, 0x9C, 0xEF,
    0xA0, 0xE0, 0x3B, 0x4D, 0xAE, 0x2A, 0xF5, 0xB0, 0xC8, 0xEB, 0xBB, 0x3C, 0x83, 0x53, 0x99, 0x61,
    0x17, 0x2B, 0x04, 0x7E, 0xBA, 0x77, 0xD6, 0x26, 0xE1, 0x69, 0x14, 0x63, 0x55, 0x21, 0x0C, 0x7D,
)


def _xtime(val: int) -> int:
    return ((val << 1) ^ 0x1B) & 0xFF if (val & 0x80) else (val << 1)


def _gf_mul(a: int, b: int) -> int:
    res = 0
    for _ in range(8):
        if b & 1:
            res ^= a
        a = _xtime(a)
        b >>= 1
    return res


def _aes128_expand_key(key: bytes) -> list[list[int]]:
    words = [list(key[i : i + 4]) for i in range(0, 16, 4)]
    for i in range(4, 44):
        temp = list(words[i - 1])
        if i % 4 == 0:
            temp = [_AES_SBOX[temp[1]], _AES_SBOX[temp[2]], _AES_SBOX[temp[3]], _AES_SBOX[temp[0]]]
            temp[0] ^= _RCON[(i // 4) - 1]
        words.append([words[i - 4][j] ^ temp[j] for j in range(4)])
    return [sum(words[4 * r : 4 * (r + 1)], []) for r in range(11)]


def _aes128_encrypt_block(block: bytes, round_keys: list[list[int]]) -> bytes:
    state = [bytearray(block[r::4]) for r in range(4)]

    def add_round_key(rk: list[int]) -> None:
        for c in range(4):
            for r in range(4):
                state[r][c] ^= rk[4 * c + r]

    add_round_key(round_keys[0])
    for rnd in range(1, 10):
        for r in range(4):
            for c in range(4):
                state[r][c] = _AES_SBOX[state[r][c]]
        state[1] = bytearray([state[1][1], state[1][2], state[1][3], state[1][0]])
        state[2] = bytearray([state[2][2], state[2][3], state[2][0], state[2][1]])
        state[3] = bytearray([state[3][3], state[3][0], state[3][1], state[3][2]])
        for c in range(4):
            s0, s1, s2, s3 = state[0][c], state[1][c], state[2][c], state[3][c]
            state[0][c] = _xtime(s0) ^ (_xtime(s1) ^ s1) ^ s2 ^ s3
            state[1][c] = s0 ^ _xtime(s1) ^ (_xtime(s2) ^ s2) ^ s3
            state[2][c] = s0 ^ s1 ^ _xtime(s2) ^ (_xtime(s3) ^ s3)
            state[3][c] = (_xtime(s0) ^ s0) ^ s1 ^ s2 ^ _xtime(s3)
        add_round_key(round_keys[rnd])

    for r in range(4):
        for c in range(4):
            state[r][c] = _AES_SBOX[state[r][c]]
    state[1] = bytearray([state[1][1], state[1][2], state[1][3], state[1][0]])
    state[2] = bytearray([state[2][2], state[2][3], state[2][0], state[2][1]])
    state[3] = bytearray([state[3][3], state[3][0], state[3][1], state[3][2]])
    add_round_key(round_keys[10])
    return bytes(state[r][c] for c in range(4) for r in range(4))


def _aes128_cbc_encrypt(key: bytes, iv: bytes, plain: bytes) -> bytes:
    pad_len = 16 - (len(plain) % 16)
    padded = plain + bytes([pad_len] * pad_len)
    rk = _aes128_expand_key(key)
    out = bytearray()
    prev = iv
    for off in range(0, len(padded), 16):
        blk = bytes(padded[off + i] ^ prev[i] for i in range(16))
        enc = _aes128_encrypt_block(blk, rk)
        out.extend(enc)
        prev = enc
    return bytes(out)


def encrypt_encrypt_key(password: bytes | str, secret: str) -> str:
    """Generate Base64 encryptkey header using AES-128-CBC with repeated secret bytes."""
    secret_bytes = secret.encode("utf-8")
    buf = bytearray()
    while len(buf) < 32:
        buf.extend(secret_bytes)
    key = bytes(buf[:16])
    iv = bytes(buf[16:32])
    raw_pwd = password.encode("utf-8") if isinstance(password, str) else password
    cipher = _aes128_cbc_encrypt(key, iv, raw_pwd)
    return base64.b64encode(cipher).decode("ascii")


class ThreadedHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


class MockServerState:
    """Thread-safe mutable state container for WeRead mock server."""

    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.reset()

    def reset(self) -> None:
        with self.lock:
            self.history: list[dict] = []
            self.qr_status = "CONFIRMED"  # WAITING_CONFIRM, NEED_OTP, CONFIRMED, LOGIN_TIMEOUT, LOGIN_CANCELLED
            self.current_vid = 12345678
            self.current_skey = "mock_skey_123456"
            self.access_token = "mock_access_token_token"
            self.refresh_token = "mock_refresh_token_xyz"
            self.token_valid = True
            self.force_error: dict | None = None
            self.endpoint_errors: dict[str, dict] = {}
            self.latency: float = 0.0
            self.disconnect: bool = False
            self.clear_all_toc: bool = False
            self.book_version: int = 100
            self.synckey: int = 1000
            self.progress_data: dict[str, dict] = {
                "test_book_101": {
                    "chapterUid": 1,
                    "chapterIdx": 1,
                    "chapterOffset": 0,
                    "pageOffset": 0,
                    "bookVersion": 100,
                    "progress": 15,
                    "readingTime": 600,
                    "updateTime": 1700000000,
                    "summary": "第一章 起源",
                }
            }
            self.books: list[dict] = [
                {
                    "bookId": "test_book_101",
                    "title": "测试小说：荒原起源",
                    "author": "测试作者",
                    "cover": "https://weread.qq.com/covers/s_test_book_101.jpg",
                    "format": "epub",
                    "totalWords": 120000,
                    "updateTime": 1700000000,
                    "readUpdateTime": 1700000500,
                    "bookVersion": 100,
                    "paid": 0,
                    "finished": 0,
                },
                {
                    "bookId": "txt_book_202",
                    "title": "测试纯文本：序章录",
                    "author": "纯文本作家",
                    "cover": "https://weread.qq.com/covers/s_txt_book_202.jpg",
                    "format": "txt",
                    "totalWords": 50000,
                    "updateTime": 1700001000,
                    "readUpdateTime": 1700001200,
                    "bookVersion": 1,
                    "paid": 0,
                    "finished": 0,
                },
            ]
            self.lecture_books: list[dict] = [
                {
                    "bookId": "lecture_999",
                    "title": "音频讲座：不应出现在书架",
                    "author": "主讲人",
                    "format": "audio",
                    "type": 1,
                    "totalWords": 0,
                    "updateTime": 1700000000,
                }
            ]
            self.chapters: dict[str, list[dict]] = {
                "test_book_101": [
                    {
                        "chapterUid": 1,
                        "chapterIdx": 1,
                        "title": "第一章 起源",
                        "level": 1,
                        "wordCount": 3200,
                        "paid": 0,
                        "updateTime": 1700000000,
                    },
                    {
                        "chapterUid": 2,
                        "chapterIdx": 2,
                        "title": "第二章 征途",
                        "level": 1,
                        "wordCount": 4100,
                        "paid": 0,
                        "updateTime": 1700000000,
                    },
                ],
                "txt_book_202": [
                    {
                        "chapterUid": 1,
                        "chapterIdx": 1,
                        "title": "第一章 序章",
                        "level": 1,
                        "wordCount": 1800,
                        "paid": 0,
                        "updateTime": 1700000000,
                    }
                ],
            }
            self.security_violations: list[dict] = []

    def set_state(self, updates: dict) -> None:
        with self.lock:
            for k, v in updates.items():
                if hasattr(self, k):
                    setattr(self, k, v)
                elif k == "endpoint_errors" and isinstance(v, dict):
                    self.endpoint_errors.update(v)

    def record_request(self, item: dict) -> None:
        with self.lock:
            self.history.append(item)

    def record_security_violation(self, violation: dict) -> None:
        with self.lock:
            self.security_violations.append(violation)

    def get_history(self) -> list[dict]:
        with self.lock:
            return list(self.history)


GLOBAL_STATE = MockServerState()


class WeReadMockRequestHandler(BaseHTTPRequestHandler):
    server_version = "WeReadMock/1.0"

    def log_message(self, format: str, *args) -> None:
        # Suppress noisy standard HTTP logs in test runs unless verbose
        if os.environ.get("WEREAD_MOCK_VERBOSE") == "1":
            sys.stderr.write(f"[WeReadMock] {format % args}\n")

    def _read_body(self) -> bytes:
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length > 0:
            return self.rfile.read(content_length)
        return b""

    def _record_and_check_security(self, body_bytes: bytes) -> bool:
        """Record request and check sensitive header stripping on untrusted endpoints."""
        parsed_url = urllib.parse.urlparse(self.path)
        path = parsed_url.path
        query = urllib.parse.parse_qs(parsed_url.query)

        headers_dict = {k.lower(): v for k, v in self.headers.items()}
        try:
            body_str = body_bytes.decode("utf-8")
        except UnicodeDecodeError:
            body_str = base64.b64encode(body_bytes).decode("ascii")

        req_record = {
            "method": self.command,
            "url": self.path,
            "path": path,
            "query": query,
            "headers": headers_dict,
            "body": body_str,
            "timestamp": time.time(),
        }
        GLOBAL_STATE.record_request(req_record)

        # Security check: sensitive headers on untrusted endpoints
        is_untrusted = (
            path.startswith("/untrusted")
            or path.startswith("/thirdparty")
            or "untrusted" in self.headers.get("Host", "")
        )
        if is_untrusted:
            sensitive = ["cookie", "authorization", "accesstoken", "vid"]
            leaked = [h for h in sensitive if h in headers_dict]
            if leaked:
                GLOBAL_STATE.record_security_violation(
                    {"path": path, "leaked_headers": leaked, "headers": headers_dict}
                )
                self._send_json(403, {"error": "SecurityViolation", "leaked": leaked})
                return False
        return True

    def _send_json(self, status: int, data: dict, extra_headers: dict | None = None) -> None:
        body = json.dumps(data, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        if extra_headers:
            for k, v in extra_headers.items():
                self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _handle_forced_errors(self, path: str) -> bool:
        """Check if global force_error or endpoint_errors should intercept this request."""
        if GLOBAL_STATE.latency > 0:
            time.sleep(GLOBAL_STATE.latency)

        if GLOBAL_STATE.disconnect:
            # Abruptly close connection
            self.close_connection = True
            return True

        # Check endpoint-specific error
        if path in GLOBAL_STATE.endpoint_errors:
            err = GLOBAL_STATE.endpoint_errors[path]
            status = err.get("status", 500)
            body = err.get("body", {"errcode": -1, "errmsg": "injected error"})
            headers = err.get("headers")
            self._send_json(status, body, headers)
            return True

        # Check global force_error (ignores control paths)
        if not path.startswith("/mock/control") and GLOBAL_STATE.force_error:
            err = GLOBAL_STATE.force_error
            status = err.get("status", 401)
            body = err.get("body", {"errcode": -2013, "errmsg": "session expired"})
            headers = err.get("headers")
            self._send_json(status, body, headers)
            return True

        return False

    # ---------------------------------------------------------
    # Route Handlers
    # ---------------------------------------------------------
    def do_GET(self) -> None:
        body = self._read_body()
        if not self._record_and_check_security(body):
            return

        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if self._handle_forced_errors(path):
            return

        # Control endpoints
        if path == "/mock/control/health":
            self._send_json(200, {"status": "ok", "running": True})
            return
        if path == "/mock/control/history":
            self._send_json(200, {
                "status": "ok",
                "count": len(GLOBAL_STATE.get_history()),
                "history": GLOBAL_STATE.get_history(),
                "violations": list(GLOBAL_STATE.security_violations),
            })
            return

        # WeRead endpoints
        if path == "/web/getuid":
            # Returns QR login session UID
            self._send_json(200, {"data": {"uid": "mock_uid_1234567890abcdef"}})
            return

        if path == "/shelf/sync":
            # Bookshelf sync (full or incremental)
            synckey_req = int(query.get("synckey", ["0"])[0])
            only_book_id = query.get("onlyBookid", ["0"])[0] == "1"

            if only_book_id and synckey_req > 0:
                # Incremental sync
                book_ids = [{"bookId": b["bookId"], "type": 0} for b in GLOBAL_STATE.books]
                progress_list = [
                    {"bookId": bid, "chapterUid": p.get("chapterUid", 1), "progress": p.get("progress", 0), "updateTime": p.get("updateTime", 1700000000)}
                    for bid, p in GLOBAL_STATE.progress_data.items()
                ]
                self._send_json(200, {
                    "bookIds": book_ids,
                    "bookProgress": progress_list,
                    "removed": [],
                    "synckey": GLOBAL_STATE.synckey + 1,
                })
            else:
                # Full sync: includes lectureBooks to test filtering
                book_ids = [b["bookId"] for b in GLOBAL_STATE.books]
                progress_list = [
                    {"bookId": bid, "chapterUid": p.get("chapterUid", 1), "progress": p.get("progress", 0), "updateTime": p.get("updateTime", 1700000000)}
                    for bid, p in GLOBAL_STATE.progress_data.items()
                ]
                self._send_json(200, {
                    "books": GLOBAL_STATE.books,
                    "bookIds": book_ids,
                    "bookProgress": progress_list,
                    "lectureBooks": GLOBAL_STATE.lecture_books,
                    "removed": [],
                    "synckey": GLOBAL_STATE.synckey,
                })
            return

        if path == "/book/chapterdownload":
            self._handle_chapter_download(query)
            return

        if path in ("/book/getProgress", "/web/book/getProgress"):
            book_id = query.get("bookId", ["test_book_101"])[0]
            prog = GLOBAL_STATE.progress_data.get(book_id, {
                "chapterUid": 1,
                "chapterIdx": 1,
                "chapterOffset": 0,
                "pageOffset": 0,
                "bookVersion": 100,
                "progress": 0,
                "readingTime": 0,
                "updateTime": int(time.time()),
                "summary": "第一章",
            })
            self._send_json(200, {"bookId": book_id, "book": prog})
            return

        if path.startswith("/untrusted") or path.startswith("/thirdparty"):
            self._send_json(200, {"status": "ok", "message": "Untrusted asset fetched safely"})
            return

        # 404 default
        self._send_json(404, {"errcode": -404, "errmsg": f"Endpoint not found: {path}"})

    def do_POST(self) -> None:
        body = self._read_body()
        if not self._record_and_check_security(body):
            return

        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        query = urllib.parse.parse_qs(parsed.query)

        if self._handle_forced_errors(path):
            return

        # Control endpoints
        if path == "/mock/control/reset":
            GLOBAL_STATE.reset()
            self._send_json(200, {"status": "ok", "message": "Mock server state reset successfully"})
            return

        if path == "/mock/control/set_state":
            try:
                updates = json.loads(body.decode("utf-8")) if body else {}
                GLOBAL_STATE.set_state(updates)
                self._send_json(200, {"status": "ok", "message": "State updated"})
            except Exception as exc:
                self._send_json(400, {"status": "error", "message": str(exc)})
            return

        # WeRead Auth endpoints
        if path == "/web/getlogininfo":
            # Polling QR confirmation status
            qr = GLOBAL_STATE.qr_status
            if qr == "WAITING_CONFIRM":
                self._send_json(200, {"data": {"succeed": False}})
            elif qr == "NEED_OTP":
                self._send_json(200, {"data": {"succeed": False, "logicCode": "NEED_OTP"}})
            elif qr == "LOGIN_TIMEOUT":
                self._send_json(200, {"data": {"succeed": False, "logicCode": "LOGIN_TIMEOUT"}})
            elif qr == "LOGIN_CANCELLED":
                self._send_json(200, {"data": {"succeed": False, "logicCode": "LOGIN_CANCELLED"}})
            else:  # CONFIRMED
                self._send_json(200, {
                    "vid": GLOBAL_STATE.current_vid,
                    "skey": GLOBAL_STATE.current_skey,
                    "code": "mock_qr_code_xyz123",
                    "redirect_uri": "https://weread.qq.com",
                    "pf": "2",
                    "expireMode": 0,
                    "isAutoLogout": 0,
                    "wxNickName": "KiikoReader",
                })
            return

        if path == "/weblogin":
            # Handles initial device login (deviceType=4) and renewal (deviceType=7)
            dt_q = query.get("deviceType", [""])[0]
            dt_body = 0
            try:
                b_json = json.loads(body.decode("utf-8")) if body else {}
                dt_body = b_json.get("deviceType", 0)
            except Exception:
                pass

            if dt_q == "7" or dt_body == 7:
                # Renewal
                self._send_json(200, {
                    "accessToken": "mock_renewed_access_token",
                    "refreshToken": "mock_renewed_refresh_token",
                })
            else:
                # Initial device login (deviceType=4)
                extra = {
                    "Set-Cookie": f"wr_vid={GLOBAL_STATE.current_vid}; Path=/; Domain=.weread.qq.com; Secure; HttpOnly, "
                                  f"wr_skey={GLOBAL_STATE.access_token}; Path=/; Domain=.weread.qq.com; Secure; HttpOnly, "
                                  f"wr_rt={GLOBAL_STATE.refresh_token}; Path=/; Domain=.weread.qq.com; Secure; HttpOnly"
                }
                self._send_json(200, {
                    "webLoginVid": str(GLOBAL_STATE.current_vid),
                    "accessToken": GLOBAL_STATE.access_token,
                    "refreshToken": GLOBAL_STATE.refresh_token,
                }, extra_headers=extra)
            return

        if path == "/shelf/syncbook":
            # Batch fetch book details
            try:
                b_json = json.loads(body.decode("utf-8")) if body else {}
                req_bids = set(b_json.get("bookIds", []))
            except Exception:
                req_bids = set()

            matched = [b for b in GLOBAL_STATE.books if b["bookId"] in req_bids] if req_bids else GLOBAL_STATE.books
            self._send_json(200, {
                "books": matched,
                "bookProgress": [
                    {"bookId": b["bookId"], "progress": 15, "chapterUid": 1}
                    for b in matched
                ]
            })
            return

        if path == "/book/chapterInfos":
            try:
                b_json = json.loads(body.decode("utf-8")) if body else {}
                req_bids = b_json.get("bookIds", ["test_book_101"])
            except Exception:
                req_bids = ["test_book_101"]

            out_data = []
            for bid in req_bids:
                ch_list = GLOBAL_STATE.chapters.get(bid, [])
                out_data.append({
                    "bookId": bid,
                    "synckey": GLOBAL_STATE.synckey,
                    "clearAll": 1 if GLOBAL_STATE.clear_all_toc else 0,
                    "updated": ch_list,
                    "removed": [],
                })
            self._send_json(200, {"data": out_data})
            return

        if path in ("/book/read", "/web/book/read"):
            # Reading progress report
            if path == "/web/book/read":
                # Verify signed fallback parameters
                try:
                    b_json = json.loads(body.decode("utf-8")) if body else {}
                    if "s" not in b_json or "b" not in b_json:
                        # Direct unauthenticated body on web endpoint returns empty 200 (silent drop)
                        self._send_json(200, {})
                        return
                except Exception:
                    pass
            self._send_json(200, {"succ": 1, "synckey": GLOBAL_STATE.synckey + 5})
            return

        self._send_json(404, {"errcode": -404, "errmsg": f"Endpoint not found: {path}"})

    def _handle_chapter_download(self, query: dict) -> None:
        """Serve authentic encrypted chapter payloads or injected errors."""
        book_type = query.get("bookType", ["epub"])[0].lower()
        fix_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "fixtures", "weread")

        if book_type == "txt":
            tar_path = os.path.join(fix_dir, "sample_txt_chapter1.tar")
            if not os.path.exists(tar_path):
                self._send_json(500, {"errcode": -500, "errmsg": "Missing sample_txt_chapter1.tar fixture"})
                return
            with open(tar_path, "rb") as f:
                content = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "application/x-tar")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)
            return

        # Default EPUB download with authentic encryptkey header
        zip_path = os.path.join(fix_dir, "sample_epub_chapter0_and_1.zip")
        if not os.path.exists(zip_path):
            self._send_json(500, {"errcode": -500, "errmsg": "Missing sample_epub_chapter0_and_1.zip fixture"})
            return

        with open(zip_path, "rb") as f:
            content = f.read()

        # Generate encryptkey header using user's vid (or skey)
        secret = str(GLOBAL_STATE.current_vid)
        enc_key_hdr = encrypt_encrypt_key("test_password_123", secret)

        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("encryptkey", enc_key_hdr)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)


def run_server(port: int = 8989) -> None:
    ready_file = f"/tmp/weread_mock_{port}.ready"
    if os.path.exists(ready_file):
        os.remove(ready_file)

    server_address = ("127.0.0.1", port)
    httpd = ThreadedHTTPServer(server_address, WeReadMockRequestHandler)

    with open(ready_file, "w") as f:
        f.write(f"READY:{port}\n")

    print(f"[WeReadMockServer] Listening on http://127.0.0.1:{port}", flush=True)

    def sig_handler(sig, frame):
        print("\n[WeReadMockServer] Shutting down...", flush=True)
        if os.path.exists(ready_file):
            try:
                os.remove(ready_file)
            except OSError:
                pass
        threading.Thread(target=httpd.shutdown).start()

    signal.signal(signal.SIGINT, sig_handler)
    signal.signal(signal.SIGTERM, sig_handler)

    try:
        httpd.serve_forever()
    finally:
        httpd.server_close()
        if os.path.exists(ready_file):
            try:
                os.remove(ready_file)
            except OSError:
                pass
        print("[WeReadMockServer] Server closed.", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description="WeRead Mock Server")
    default_port = int(os.environ.get("WEREAD_MOCK_PORT", "8989"))
    parser.add_argument("--port", type=int, default=default_port, help="Port to listen on (default 8989)")
    args = parser.parse_args()
    run_server(args.port)


if __name__ == "__main__":
    main()
