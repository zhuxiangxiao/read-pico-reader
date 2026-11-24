#!/usr/bin/env python3
"""WeRead Client & Storage Simulator for E2E Test Tiers.

Simulates the Kiikoread WeRead client runtime on e-ink devices:
- Cryptographic operations: AES-128-CBC key unwrapping, ZipCrypto unpack, USTAR unpack
- Atomically managed persistent storage (auth.json, shelf.json, chapters.json, cache)
- Security enforcement: sensitive header stripping on untrusted domains
- Session renewal and single-replay on -2013/-2012
- Offline reading, progress throttling, conflict resolution, and dirty sync
- Virtual URI validation (weread://) and lock screen ticket generation
"""

from __future__ import annotations

import base64
import io
import json
import os
import re
import sys
import tarfile
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

# Add tools directory to path to load AES primitives from weread_mock_server
TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

try:
    from weread_mock_server import _aes128_expand_key, _AES_INV_SBOX, _gf_mul
except ImportError:
    raise RuntimeError(f"Failed to import AES primitives from weread_mock_server at {TOOLS_DIR}")


# ---------------------------------------------------------------------------
# Cryptographic Primitives
# ---------------------------------------------------------------------------

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


# ---------------------------------------------------------------------------
# Virtual URI & Local File Helpers
# ---------------------------------------------------------------------------

def is_weread_uri(uri: str) -> bool:
    if not uri or not uri.startswith("weread://"):
        return False
    book_id = uri[len("weread://"):]
    if not book_id:
        return False
    return bool(re.match(r"^[0-9a-zA-Z_]+$", book_id))


def parse_weread_uri(uri: str) -> str | None:
    if is_weread_uri(uri):
        return uri[len("weread://"):]
    return None


def book_file_path_supported(path: str) -> bool:
    """Mock of Kiikoread book_file_path_supported(): supports epub/txt/pdf files, rejects weread:// URIs."""
    if is_weread_uri(path):
        return False
    ext = os.path.splitext(path)[1].lower()
    return ext in (".epub", ".txt", ".pdf", ".mobi", ".azw3")


# ---------------------------------------------------------------------------
# Storage Layer Simulator
# ---------------------------------------------------------------------------

class WereadStorage:
    """Manages virtual e-ink persistent filesystem state at WEREAD_STORAGE_ROOT."""

    def __init__(self, root_dir: str) -> None:
        self.root_dir = os.path.abspath(root_dir)
        self.auth_file = os.path.join(self.root_dir, "auth.json")
        self.shelf_file = os.path.join(self.root_dir, "shelf.json")
        self.cache_dir = os.path.join(self.root_dir, "cache")
        self.io_read_count: int = 0
        os.makedirs(self.cache_dir, exist_ok=True)

    def _atomic_write_json(self, target_path: str, data: dict | list) -> None:
        os.makedirs(os.path.dirname(target_path), exist_ok=True)
        tmp_path = target_path + ".tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(data, f, ensure_ascii=False, indent=2)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_path, target_path)

    def _read_json(self, target_path: str) -> dict | list | None:
        self.io_read_count += 1
        if not os.path.exists(target_path):
            return None
        with open(target_path, "r", encoding="utf-8") as f:
            return json.load(f)

    # Auth storage
    def save_auth(self, auth_data: dict) -> None:
        self._atomic_write_json(self.auth_file, auth_data)

    def load_auth(self) -> dict | None:
        return self._read_json(self.auth_file)

    def delete_auth(self) -> None:
        if os.path.exists(self.auth_file):
            os.remove(self.auth_file)

    def mark_auth_expired(self) -> dict:
        """WR-UX-01: Clear tokens/skey, set expired=True, retain read-only vid/deviceId."""
        existing = self.load_auth() or {}
        retained = {
            "expired": True,
            "updatedAt": int(time.time()),
        }
        for k in ("vid", "deviceId", "deviceName", "wxNickName", "wxHeadImg"):
            if k in existing and existing[k] not in (None, ""):
                retained[k] = existing[k]
        self.save_auth(retained)
        return retained

    def is_auth_expired(self) -> bool:
        auth = self.load_auth()
        return bool(auth and auth.get("expired") is True)

    def owner_matches(self, vid: str | int) -> bool:
        """Check whether auth.json retains a matching owner vid (valid even when expired=True)."""
        auth = self.load_auth()
        if not auth or not auth.get("vid"):
            return False
        return str(auth["vid"]) == str(vid)

    # Shelf storage
    def save_shelf(self, shelf_data: dict) -> None:
        self._atomic_write_json(self.shelf_file, shelf_data)

    def load_shelf(self) -> dict | None:
        return self._read_json(self.shelf_file)

    def update_shelf_chapter_counts(self, book_id: str, cached: int, total: int) -> None:
        shelf = self.load_shelf()
        if not shelf or "books" not in shelf:
            return
        changed = False
        for b in shelf["books"]:
            if b.get("bookId") == book_id:
                b["cachedChapters"] = int(cached)
                b["totalChapters"] = int(total)
                changed = True
                break
        if changed:
            self.save_shelf(shelf)

    # Cover & legacy artifacts for WR-UX-09 cleanup verification
    def save_cover_gray(self, book_id: str, data: bytes) -> str:
        p = os.path.join(self.book_cache_dir(book_id), f"cover-{book_id}.gray")
        with open(p, "wb") as f:
            f.write(data)
        return p

    def has_cover_gray(self, book_id: str) -> bool:
        self.io_read_count += 1
        p = os.path.join(self.book_cache_dir(book_id), f"cover-{book_id}.gray")
        return os.path.isfile(p)

    def load_cover_gray(self, book_id: str) -> bytes | None:
        self.io_read_count += 1
        p = os.path.join(self.book_cache_dir(book_id), f"cover-{book_id}.gray")
        if os.path.isfile(p):
            with open(p, "rb") as f:
                return f.read()
        return None

    def save_cover_img(self, book_id: str, data: bytes) -> str:
        p = os.path.join(self.book_cache_dir(book_id), f"cover-{book_id}.img")
        with open(p, "wb") as f:
            f.write(data)
        return p

    def save_chapter_json(self, book_id: str, uid: int, data: dict) -> str:
        p = os.path.join(self.book_cache_dir(book_id), f"chapter-{uid}.json")
        self._atomic_write_json(p, data)
        return p

    def save_resources_manifest(self, book_id: str, data: dict) -> str:
        p = os.path.join(self.book_cache_dir(book_id), "resources.json")
        self._atomic_write_json(p, data)
        return p

    def save_progress_backup(self, book_id: str, data: dict) -> str:
        p = os.path.join(self.book_cache_dir(book_id), "progress.json.backup")
        self._atomic_write_json(p, data)
        return p

    def compute_cache_stats(self, free_bytes: int = 512 * 1024 * 1024) -> dict:
        """WR-UX-09: Compute cached book count, cache size MB, free space MB, and 104 MB reserve note."""
        cached_books = 0
        total_bytes = 0
        if os.path.isdir(self.cache_dir):
            for bid in os.listdir(self.cache_dir):
                bdir = os.path.join(self.cache_dir, bid)
                if not os.path.isdir(bdir):
                    continue
                book_has_chapter = False
                book_bytes = 0
                for sub in ("chapters", "res"):
                    sp = os.path.join(bdir, sub)
                    if os.path.isdir(sp):
                        for root, _, files in os.walk(sp):
                            for fn in files:
                                fp = os.path.join(root, fn)
                                sz = os.path.getsize(fp)
                                book_bytes += sz
                                if sub == "chapters":
                                    book_has_chapter = True
                for fn in os.listdir(bdir):
                    if (fn.startswith("chapter-") and fn.endswith(".json")) or fn == "resources.json" or (fn.startswith("cover-") and fn.endswith(".img")):
                        fp = os.path.join(bdir, fn)
                        if os.path.isfile(fp):
                            book_bytes += os.path.getsize(fp)
                            if fn.startswith("chapter-"):
                                book_has_chapter = True
                if book_has_chapter:
                    cached_books += 1
                total_bytes += book_bytes
        return {
            "cached_books": cached_books,
            "total_bytes": total_bytes,
            "total_mb": round(total_bytes / (1024.0 * 1024.0), 2),
            "free_mb": round(free_bytes / (1024.0 * 1024.0), 2),
            "reserve_mb": 104,
        }

    def clean_book_chapters(self, book_id: str) -> None:
        """WR-UX-09: Delete chapter/resource caches for one book; keep progress.json*, cover-*.gray, shelf/auth."""
        bdir = os.path.join(self.cache_dir, book_id)
        if not os.path.isdir(bdir):
            return
        self.purge_cached_chapters(book_id)
        ch_dir = os.path.join(bdir, "chapters")
        if os.path.isdir(ch_dir) and not os.listdir(ch_dir):
            os.rmdir(ch_dir)
        self.purge_shared_resources(book_id)
        for fn in list(os.listdir(bdir)):
            if (fn.startswith("chapter-") and fn.endswith(".json")) or fn == "resources.json" or (fn.startswith("cover-") and fn.endswith(".img")):
                fp = os.path.join(bdir, fn)
                if os.path.isfile(fp):
                    os.remove(fp)
        shelf = self.load_shelf()
        if shelf and "books" in shelf:
            for b in shelf["books"]:
                if b.get("bookId") == book_id:
                    b["cachedChapters"] = 0
            self.save_shelf(shelf)

    def clean_finished_books(self) -> list[str]:
        """WR-UX-09: Clean chapter caches only for books with finished==1 or progress>=100."""
        shelf = self.load_shelf() or {}
        cleaned: list[str] = []
        for b in shelf.get("books", []):
            bid = b.get("bookId")
            if not bid:
                continue
            prog = self.load_local_progress(bid) or {}
            is_finished = bool(b.get("finished") == 1 or b.get("progress", 0) >= 100 or prog.get("progress", 0) >= 100)
            if is_finished:
                self.clean_book_chapters(bid)
                cleaned.append(bid)
        return cleaned

    def clean_all_offline_chapters(self) -> list[str]:
        """WR-UX-09: Clean chapter caches across all books while preserving auth.json, shelf.json, progress.json*, cover-*.gray."""
        cleaned: list[str] = []
        if os.path.isdir(self.cache_dir):
            for bid in os.listdir(self.cache_dir):
                if os.path.isdir(os.path.join(self.cache_dir, bid)):
                    self.clean_book_chapters(bid)
                    cleaned.append(bid)
        return cleaned

    # TOC storage
    def book_cache_dir(self, book_id: str) -> str:
        d = os.path.join(self.cache_dir, book_id)
        os.makedirs(d, exist_ok=True)
        return d

    def save_toc(self, book_id: str, toc_data: dict) -> None:
        p = os.path.join(self.book_cache_dir(book_id), "chapters.json")
        self._atomic_write_json(p, toc_data)

    def load_toc(self, book_id: str) -> dict | None:
        p = os.path.join(self.book_cache_dir(book_id), "chapters.json")
        return self._read_json(p)

    def purge_cached_chapters(self, book_id: str) -> None:
        ch_dir = os.path.join(self.book_cache_dir(book_id), "chapters")
        if os.path.exists(ch_dir):
            for fname in os.listdir(ch_dir):
                fpath = os.path.join(ch_dir, fname)
                if os.path.isfile(fpath):
                    os.remove(fpath)

    def purge_shared_resources(self, book_id: str) -> None:
        res_dir = os.path.join(self.book_cache_dir(book_id), "res")
        if os.path.exists(res_dir):
            for root, dirs, files in os.walk(res_dir, topdown=False):
                for f in files:
                    os.remove(os.path.join(root, f))
                for d in dirs:
                    os.rmdir(os.path.join(root, d))
            os.rmdir(res_dir)

    def save_shared_resource(self, book_id: str, relative_path: str, data: bytes) -> None:
        res_path = os.path.join(self.book_cache_dir(book_id), "res", relative_path)
        os.makedirs(os.path.dirname(res_path), exist_ok=True)
        with open(res_path, "wb") as f:
            f.write(data)

    def get_shared_resource(self, book_id: str, relative_path: str) -> bytes | None:
        res_path = os.path.join(self.book_cache_dir(book_id), "res", relative_path)
        if os.path.exists(res_path):
            with open(res_path, "rb") as f:
                return f.read()
        return None

    def save_chapter_html(self, book_id: str, chapter_uid: int, html_content: str) -> None:
        ch_dir = os.path.join(self.book_cache_dir(book_id), "chapters")
        os.makedirs(ch_dir, exist_ok=True)
        p = os.path.join(ch_dir, f"{chapter_uid}.html")
        tmp_p = p + ".tmp"
        with open(tmp_p, "w", encoding="utf-8") as f:
            f.write(html_content)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp_p, p)

    def load_chapter_html(self, book_id: str, chapter_uid: int) -> str | None:
        self.io_read_count += 1
        p = os.path.join(self.book_cache_dir(book_id), "chapters", f"{chapter_uid}.html")
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as f:
                return f.read()
        return None

    def get_cached_chapters(self, book_id: str) -> list[int]:
        self.io_read_count += 1
        ch_dir = os.path.join(self.cache_dir, book_id, "chapters")
        if not os.path.isdir(ch_dir):
            return []
        uids: list[int] = []
        for fn in os.listdir(ch_dir):
            if fn.endswith(".html"):
                stem = fn[:-5]
                if stem.isdigit():
                    uids.append(int(stem))
        return sorted(uids)

    # Local Progress & Dirty tracking
    def save_local_progress(self, book_id: str, prog: dict) -> None:
        p = os.path.join(self.book_cache_dir(book_id), "progress.json")
        self._atomic_write_json(p, prog)

    def load_local_progress(self, book_id: str) -> dict | None:
        p = os.path.join(self.book_cache_dir(book_id), "progress.json")
        return self._read_json(p)


# ---------------------------------------------------------------------------
# WeRead Client API Simulator
# ---------------------------------------------------------------------------

class WereadClient:
    """WeRead HTTP client with token renewal, sensitive header stripping, and storage integration."""

    def __init__(self, port: int, storage: WereadStorage) -> None:
        self.port = port
        self.base_url = f"http://127.0.0.1:{port}"
        self.storage = storage
        self.wifi_enabled = True
        self.last_renewal_count = 0
        self.on_renewal_callback = None
        # WR-UX additive runtime fields (legacy callers unaffected)
        self.rf_idle_timeout_ms: int = 600000
        self.owns_network: bool = False
        self.last_network_activity_ms: int = 0
        self.last_page_move_ms: int = 0
        self.last_note_report_ms: int = 0
        self.now_ms: int = 0
        self.mark_expired_on_2013: bool = False
        self.background_jobs: list[str] = []

    @property
    def is_authenticated(self) -> bool:
        auth = self.storage.load_auth()
        if not auth or auth.get("expired"):
            return False
        return bool(auth.get("accessToken") and auth.get("refreshToken"))

    def set_wifi_enabled(self, enabled: bool) -> None:
        self.wifi_enabled = enabled
        if not enabled:
            self.owns_network = False

    def connect_network(self, now_ms: int | None = None) -> None:
        if now_ms is not None:
            self.now_ms = now_ms
        self.wifi_enabled = True
        self.owns_network = True
        self.last_network_activity_ms = self.now_ms

    def check_rf_idle_disconnect(self, now_ms: int) -> bool:
        """WR-UX-06: Preserve 600000ms (10 min) RF idle disconnect threshold."""
        if (
            self.owns_network
            and self.last_network_activity_ms > 0
            and now_ms >= self.last_network_activity_ms
            and (now_ms - self.last_network_activity_ms) >= self.rf_idle_timeout_ms
        ):
            self.owns_network = False
            return True
        return False

    def handle_terminal_2013(self) -> dict:
        """WR-UX-01: Passive -2013 marks auth.json expired=True while retaining read-only vid/deviceId."""
        self.background_jobs.clear()
        return self.storage.mark_auth_expired()

    # Low-level HTTP transport
    def _raw_request(
        self,
        method: str,
        url_or_path: str,
        body: dict | bytes | None = None,
        headers: dict | None = None,
        timeout: float = 5.0,
    ) -> tuple[int, dict, bytes]:
        is_control = "/mock/control" in url_or_path
        if not self.wifi_enabled and not is_control:
            raise ConnectionError("Wi-Fi RF is powered down (offline)")

        if url_or_path.startswith("http://") or url_or_path.startswith("https://"):
            full_url = url_or_path
        else:
            full_url = f"{self.base_url}{url_or_path}"

        if not is_control and self.now_ms > 0:
            self.last_network_activity_ms = self.now_ms

        hdrs = headers.copy() if headers else {}
        data = None
        if body is not None:
            if isinstance(body, dict):
                data = json.dumps(body).encode("utf-8")
                hdrs.setdefault("Content-Type", "application/json")
            else:
                data = body

        req = urllib.request.Request(full_url, data=data, headers=hdrs, method=method)
        try:
            with urllib.request.urlopen(req, timeout=timeout) as resp:
                resp_headers = dict(resp.headers)
                resp_body = resp.read()
                return resp.status, resp_headers, resp_body
        except urllib.error.HTTPError as err:
            return err.code, dict(err.headers), err.read()

    def strip_sensitive_headers_if_untrusted(self, url: str, headers: dict) -> dict:
        """Strip Cookie, Authorization, accessToken, vid if target is untrusted or non-https non-qq."""
        parsed = urllib.parse.urlparse(url)
        path = parsed.path
        host = parsed.netloc.split(":")[0].lower()

        # Check if trusted domain
        is_trusted = (
            host == "127.0.0.1" and not (path.startswith("/thirdparty") or path.startswith("/untrusted"))
        ) or (
            host == "weread.qq.com" or host.endswith(".weread.qq.com") or host.endswith(".qq.com")
        )

        clean_headers = headers.copy()
        if not is_trusted or path.startswith("/thirdparty") or path.startswith("/untrusted"):
            sensitive_keys = ["cookie", "authorization", "accesstoken", "vid"]
            for k in list(clean_headers.keys()):
                if k.lower() in sensitive_keys:
                    del clean_headers[k]
        return clean_headers

    def request(
        self,
        method: str,
        path: str,
        body: dict | bytes | None = None,
        headers: dict | None = None,
        auto_renew: bool = True,
    ) -> tuple[int, dict, bytes]:
        """Authenticated request with automatic -2013 renewal and single replay."""
        full_url = f"{self.base_url}{path}" if not path.startswith("http") else path
        hdrs = headers.copy() if headers else {}

        # Attach auth headers if stored and unexpired and target is trusted
        auth = self.storage.load_auth()
        if auth and not auth.get("expired"):
            if "accessToken" in auth:
                hdrs.setdefault("accessToken", auth["accessToken"])
            if "vid" in auth:
                hdrs.setdefault("vid", str(auth["vid"]))
            if "skey" in auth:
                hdrs.setdefault("Cookie", f"wr_vid={auth.get('vid')}; wr_skey={auth.get('skey')}")

        # Enforce sensitive header stripping
        hdrs = self.strip_sensitive_headers_if_untrusted(full_url, hdrs)

        status, resp_headers, resp_body = self._raw_request(method, full_url, body, hdrs)

        # Check for -2013 / -2012 token expiration
        if auto_renew and status in (401, 400, 200):
            try:
                data = json.loads(resp_body.decode("utf-8"))
                if isinstance(data, dict) and data.get("errcode") in (-2013, -2012):
                    renewed = self.renew_session()
                    if renewed:
                        self.last_renewal_count += 1
                        # Single replay with renewed tokens
                        auth = self.storage.load_auth()
                        hdrs["accessToken"] = auth.get("accessToken", "")
                        hdrs = self.strip_sensitive_headers_if_untrusted(full_url, hdrs)
                        status, resp_headers, resp_body = self._raw_request(method, full_url, body, hdrs)
            except Exception:
                pass

        return status, resp_headers, resp_body

    # Mock server control API
    def mock_reset(self) -> None:
        self._raw_request("POST", "/mock/control/reset")

    def mock_set_state(self, updates: dict) -> None:
        self._raw_request("POST", "/mock/control/set_state", body=updates)

    def mock_get_history(self) -> list[dict]:
        _, _, body = self._raw_request("GET", "/mock/control/history")
        data = json.loads(body.decode("utf-8"))
        return data.get("history", [])

    def mock_get_violations(self) -> list[dict]:
        _, _, body = self._raw_request("GET", "/mock/control/history")
        data = json.loads(body.decode("utf-8"))
        return data.get("violations", [])

    # Auth routines
    def login_qr_get_uid(self) -> tuple[str, str]:
        status, _, body = self.request("GET", "/web/getuid?platform=desktop", auto_renew=False)
        assert status == 200
        data = json.loads(body.decode("utf-8"))
        uid = data["data"]["uid"]
        qr_url = f"https://weread.qq.com/web/confirm?pf=2&uid={uid}"
        return uid, qr_url

    def login_qr_poll(self, uid: str) -> dict:
        status, _, body = self.request("POST", "/web/getlogininfo?platform=desktop", body={"uid": uid}, auto_renew=False)
        assert status == 200
        return json.loads(body.decode("utf-8"))

    def login_device_exchange(self, vid: str, skey: str, code: str, device_id: str = "eink-dev-1", device_name: str = "KiikoReader") -> dict:
        payload = {
            "vid": vid,
            "skey": skey,
            "code": code,
            "deviceId": device_id,
            "deviceType": 4,
            "deviceName": device_name,
        }
        status, _, body = self.request("POST", "/weblogin?platform=desktop&deviceType=4", body=payload, auto_renew=False)
        assert status == 200
        tokens = json.loads(body.decode("utf-8"))
        auth_record = {
            "vid": vid,
            "skey": skey,
            "accessToken": tokens.get("accessToken"),
            "refreshToken": tokens.get("refreshToken"),
            "deviceId": device_id,
            "deviceName": device_name,
            "updatedAt": int(time.time()),
        }
        self.storage.save_auth(auth_record)
        return auth_record

    def renew_session(self) -> bool:
        auth = self.storage.load_auth()
        if not auth or "refreshToken" not in auth:
            return False

        renew_body = {
            "vid": str(auth.get("vid")),
            "refreshToken": auth["refreshToken"],
            "deviceId": auth.get("deviceId", "eink-dev-1"),
            "deviceType": 7,
            "isFromweb": 0,
        }
        status, _, body = self._raw_request("POST", f"{self.base_url}/weblogin?deviceType=7", body=renew_body)
        if status != 200:
            if self.mark_expired_on_2013:
                try:
                    err_j = json.loads(body.decode("utf-8"))
                    if err_j.get("errcode") == -2013:
                        self.handle_terminal_2013()
                except Exception:
                    pass
            return False

        renewed = json.loads(body.decode("utf-8"))
        if renewed.get("errcode") == -2013:
            if self.mark_expired_on_2013:
                self.handle_terminal_2013()
            return False
        if "accessToken" in renewed:
            auth["accessToken"] = renewed["accessToken"]
            if "refreshToken" in renewed and renewed["refreshToken"]:
                auth["refreshToken"] = renewed["refreshToken"]
            auth.pop("expired", None)
            auth["updatedAt"] = int(time.time())
            self.storage.save_auth(auth)
            if self.on_renewal_callback:
                try:
                    self.on_renewal_callback()
                except Exception:
                    pass
            return True
        return False

    def logout(self) -> None:
        self.background_jobs.clear()
        self.storage.delete_auth()

    # Bookshelf routines
    def sync_shelf_full(self) -> dict:
        status, _, body = self.request("GET", "/shelf/sync?synckey=0&lectureSynckey=0")
        assert status == 200
        raw = json.loads(body.decode("utf-8"))
        # Preserve cachedChapters / totalChapters from existing shelf.json across syncs (WR-UX-07)
        prior_shelf = self.storage.load_shelf() or {}
        prior_counts = {
            b.get("bookId"): (b.get("cachedChapters", 0), b.get("totalChapters", 0))
            for b in prior_shelf.get("books", [])
            if b.get("bookId")
        }
        # Filter out lectureBooks and sort by readUpdateTime DESC
        filtered_books = [b for b in raw.get("books", []) if b.get("format") != "audio" and b.get("type", 0) != 1]
        for b in filtered_books:
            bid = b.get("bookId")
            if bid in prior_counts:
                c_prev, t_prev = prior_counts[bid]
                b.setdefault("cachedChapters", c_prev)
                b.setdefault("totalChapters", t_prev)
        filtered_books.sort(key=lambda b: b.get("readUpdateTime", 0), reverse=True)
        shelf_data = {
            "synckey": raw.get("synckey", 0),
            "books": filtered_books,
            "bookProgress": raw.get("bookProgress", []),
            "updatedAt": int(time.time()),
        }
        self.storage.save_shelf(shelf_data)
        return shelf_data

    def sync_book_details(self, book_ids: list[str]) -> list[dict]:
        status, _, body = self.request("POST", "/shelf/syncbook", body={"bookIds": book_ids, "albumIds": []})
        assert status == 200
        data = json.loads(body.decode("utf-8"))
        return data.get("books", [])

    def upgrade_cover_url(self, cover_url: str) -> str:
        """Upgrade /s_ to /t6_ high resolution cover."""
        return cover_url.replace("/s_", "/t6_")

    # TOC routines
    def sync_toc(self, book_id: str, synckey: int = 0) -> dict:
        status, _, body = self.request("POST", "/book/chapterInfos", body={"bookIds": [book_id], "synckeys": [synckey]})
        assert status == 200
        data = json.loads(body.decode("utf-8"))
        toc_entry = data["data"][0]

        # Handle clearAll: 1 TOC invalidation
        if toc_entry.get("clearAll") == 1:
            self.storage.purge_cached_chapters(book_id)

        self.storage.save_toc(book_id, toc_entry)
        return toc_entry

    # Chapter download, decryption, unpacking & concatenation
    def download_chapter(
        self,
        book_id: str,
        chapters: str | int,
        book_type: str = "epub",
        secret: str | None = None,
        book_version: int | None = None,
    ) -> dict:
        """Download chapter archive, decrypt encryptkey, unpack ZipCrypto/TAR, extract shared res and HTML."""
        ch_str = str(chapters)
        path = f"/book/chapterdownload?bookId={book_id}&chapters={ch_str}&bookType={book_type}"
        if book_version is not None:
            path += f"&bookVersion={int(book_version)}"
        status, headers, body = self.request("GET", path)

        if status == 402:
            return {"status": 402, "error": "PurchaseRequired"}
        if status == 499:
            try:
                err_data = json.loads(body.decode("utf-8"))
                if err_data.get("errcode") == -2063:
                    return {"status": 499, "errcode": -2063, "error": "VersionExpired"}
                return {"status": 499, "errcode": err_data.get("errcode", -2064), "error": "SoldOut"}
            except Exception:
                return {"status": 499, "error": "SoldOut"}
        if status == 200 and headers.get("Content-Type", "").startswith("application/json"):
            try:
                err_data = json.loads(body.decode("utf-8"))
                if err_data.get("errcode") == -2063:
                    return {"status": 200, "errcode": -2063, "error": "VersionExpired"}
            except Exception:
                pass
        if status != 200:
            return {"status": status, "error": f"HTTP {status}"}

        # Successful payload
        auth = self.storage.load_auth()
        effective_secret = secret or (str(auth["vid"]) if auth and "vid" in auth else "12345678")

        extracted_chapters: list[dict] = []
        if book_type == "epub":
            enc_key_hdr = headers.get("encryptkey") or headers.get("Encryptkey")
            plain_pwd = decrypt_encrypt_key(enc_key_hdr, effective_secret) if enc_key_hdr else b""

            with zipfile.ZipFile(io.BytesIO(body)) as zf:
                namelist = zf.namelist()

                # Save chapter 0 shared resources (Styles/, Images/, Fonts/)
                for name in namelist:
                    if name.startswith("Styles/") or name.startswith("Images/") or name.startswith("Fonts/"):
                        entry_bytes = zf.read(name, pwd=plain_pwd)
                        self.storage.save_shared_resource(book_id, name, entry_bytes)

                # Read info.txt to concatenate multi-file chapters
                if "info.txt" in namelist:
                    info_json = json.loads(zf.read("info.txt", pwd=plain_pwd).decode("utf-8"))
                    for ch_info in info_json.get("chapters", []):
                        ch_uid = ch_info.get("chapterUid", 1)
                        if ch_str.isdigit() and int(ch_str) != 0 and ch_uid != int(ch_str):
                            ch_uid = int(ch_str)
                        ch_title = ch_info.get("title", "")
                        files = ch_info.get("files", [])

                        # Concatenate all parts in exact order
                        part_bodies: list[str] = []
                        for part_file in files:
                            if part_file in namelist:
                                raw_part = zf.read(part_file, pwd=plain_pwd).decode("utf-8")
                                # Extract inner body markup
                                m = re.search(r"<body[^>]*>(.*?)</body>", raw_part, re.DOTALL | re.IGNORECASE)
                                if m:
                                    part_bodies.append(m.group(1).strip())
                                else:
                                    part_bodies.append(raw_part)

                        # Wrap into unified HTML
                        joined_body = "\n<hr class=\"part-divider\"/>\n".join(part_bodies)
                        unified_html = (
                            f"<!DOCTYPE html>\n<html>\n<head>\n"
                            f"<title>{ch_title}</title>\n"
                            f"<link rel=\"stylesheet\" href=\"../res/Styles/main.css\"/>\n"
                            f"</head>\n<body>\n{joined_body}\n</body>\n</html>"
                        )
                        self.storage.save_chapter_html(book_id, ch_uid, unified_html)
                        extracted_chapters.append({
                            "chapterUid": ch_uid,
                            "title": ch_title,
                            "html": unified_html,
                            "parts_count": len(files),
                        })

        elif book_type == "txt":
            with tarfile.open(fileobj=io.BytesIO(body)) as tf:
                info_member = tf.extractfile("info.txt")
                if info_member:
                    info_json = json.loads(info_member.read().decode("utf-8"))
                    for ch_info in info_json.get("chapters", []):
                        ch_uid = ch_info.get("chapterUid", 1)
                        if ch_str.isdigit() and int(ch_str) != 0 and ch_uid != int(ch_str):
                            ch_uid = int(ch_str)
                        ch_title = ch_info.get("title", "")
                        files = ch_info.get("files", [])
                        text_parts: list[str] = []
                        for f_name in files:
                            f_member = tf.extractfile(f_name)
                            if f_member:
                                text_parts.append(f_member.read().decode("utf-8"))
                        joined_txt = "\n".join(text_parts)
                        html_doc = f"<html><head><title>{ch_title}</title></head><body><pre>{joined_txt}</pre></body></html>"
                        self.storage.save_chapter_html(book_id, ch_uid, html_doc)
                        extracted_chapters.append({
                            "chapterUid": ch_uid,
                            "title": ch_title,
                            "html": html_doc,
                            "parts_count": len(files),
                        })

        return {"status": 200, "chapters": extracted_chapters}

    # Progress & Conflict Resolution
    def get_cloud_progress(self, book_id: str) -> dict:
        status, _, body = self.request("GET", f"/book/getProgress?bookId={book_id}")
        assert status == 200
        return json.loads(body.decode("utf-8")).get("book", {})

    def record_reading_progress(
        self,
        book_id: str,
        chapter_uid: int,
        chapter_idx: int,
        progress: int,
        reading_time: int,
        summary: str = "",
        dirty: int = 1,
    ) -> dict:
        now = int(time.time())
        local_prog = {
            "bookId": book_id,
            "chapterUid": chapter_uid,
            "chapterIdx": chapter_idx,
            "progress": progress,
            "readingTime": reading_time,
            "summary": summary,
            "updateTime": now,
            "readUpdateTime": now,
            "dirty": dirty,
        }
        self.storage.save_local_progress(book_id, local_prog)

        # Update shelf book's readUpdateTime and re-sort
        shelf = self.storage.load_shelf()
        if shelf and "books" in shelf:
            for b in shelf["books"]:
                if b.get("bookId") == book_id:
                    b["readUpdateTime"] = now
            # Sort books by readUpdateTime DESC
            shelf["books"].sort(key=lambda b: b.get("readUpdateTime", 0), reverse=True)
            self.storage.save_shelf(shelf)

        # Attempt immediate cloud sync if online and dirty
        if self.wifi_enabled and dirty == 1:
            try:
                sync_ok = self.sync_progress_to_cloud(book_id)
                if sync_ok:
                    local_prog["dirty"] = 0
                    self.storage.save_local_progress(book_id, local_prog)
            except Exception:
                pass

        return local_prog

    def sync_progress_to_cloud(self, book_id: str) -> bool:
        local_prog = self.storage.load_local_progress(book_id)
        if not local_prog or not self.wifi_enabled:
            return False

        payload = {
            "bookId": book_id,
            "chapterUid": local_prog.get("chapterUid", 1),
            "chapterIdx": local_prog.get("chapterIdx", 1),
            "progress": local_prog.get("progress", 0),
            "readingTime": local_prog.get("readingTime", 0),
            "updateTime": local_prog.get("updateTime", int(time.time())),
        }
        status, _, body = self.request("POST", "/book/read", body=payload)
        if status == 200:
            res = json.loads(body.decode("utf-8"))
            if res.get("succ") == 1:
                local_prog["dirty"] = 0
                self.storage.save_local_progress(book_id, local_prog)
                return True
        return False

    def resolve_progress_conflict(self, local_prog: dict, cloud_prog: dict, choice: str = "local") -> dict:
        """Simulate user dialog: 'local' keeps local progress and keeps dirty=1; 'cloud' accepts cloud progress."""
        if choice == "local":
            local_prog["dirty"] = 1
            return local_prog
        else:
            resolved = {
                "bookId": local_prog.get("bookId"),
                "chapterUid": cloud_prog.get("chapterUid", 1),
                "chapterIdx": cloud_prog.get("chapterIdx", 1),
                "progress": cloud_prog.get("progress", 0),
                "readingTime": cloud_prog.get("readingTime", 0),
                "summary": cloud_prog.get("summary", ""),
                "updateTime": cloud_prog.get("updateTime", int(time.time())),
                "readUpdateTime": int(time.time()),
                "dirty": 0,
            }
            return resolved

    # Lock screen ticket rendering
    def render_lock_screen_ticket(self, book_id: str) -> dict:
        """Simulate book_ticket.c rendering: builds lockscreen stats buffer purely from local storage."""
        shelf = self.storage.load_shelf() or {}
        matched_book = None
        for b in shelf.get("books", []):
            if b.get("bookId") == book_id:
                matched_book = b
                break

        prog = self.storage.load_local_progress(book_id) or {}
        ticket_data = {
            "bookId": book_id,
            "title": matched_book.get("title", "未知书名") if matched_book else "未知书名",
            "author": matched_book.get("author", "未知作者") if matched_book else "未知作者",
            "progress": prog.get("progress", 0),
            "readingTimeMinutes": prog.get("readingTime", 0) // 60,
            "renderedOffline": not self.wifi_enabled,
            "hasCachedCover": bool(matched_book and matched_book.get("cover")),
        }
        return ticket_data

    # WR-UX-06: 8s pause silent report, exit note, Home continue-card pending badge
    def note_page_move(self, now_ms: int) -> None:
        self.now_ms = now_ms
        self.last_page_move_ms = now_ms

    def tick_reader_pause(self, book_id: str, now_ms: int) -> bool:
        """WR-UX-06: When s_owns_network is online-ready, 8s pause + >=15s interval fires silent WR_JOB_NOTE."""
        self.now_ms = now_ms
        if not self.owns_network or not self.wifi_enabled:
            return False
        local_prog = self.storage.load_local_progress(book_id)
        if not local_prog or local_prog.get("dirty", 0) != 1:
            return False
        if now_ms < self.last_page_move_ms or (now_ms - self.last_page_move_ms) < 8000:
            return False
        if self.last_note_report_ms > 0 and (now_ms - self.last_note_report_ms) < 15000:
            return False
        ok = self.sync_progress_to_cloud(book_id)
        self.last_note_report_ms = now_ms
        return ok

    def end_reader_session(self, book_id: str, now_ms: int, busy: bool = False) -> bool:
        """WR-UX-06: Flush locally, then if owns_network & !busy & dirty==1, fire non-blocking WR_JOB_NOTE."""
        self.now_ms = now_ms
        local_prog = self.storage.load_local_progress(book_id)
        if not local_prog or local_prog.get("dirty", 0) != 1:
            return False
        if not self.owns_network or not self.wifi_enabled or busy:
            return False
        ok = self.sync_progress_to_cloud(book_id)
        if ok:
            self.last_note_report_ms = now_ms
        return ok

    def render_home_continue_card(self, book_path_or_id: str) -> dict:
        """WR-UX-06: Home continue-reading card appends '· 待同步' iff WeRead book has WR_PROGRESS_PENDING."""
        is_wr = is_weread_uri(book_path_or_id) or (
            "://" not in book_path_or_id
            and "." not in book_path_or_id
            and not book_file_path_supported(book_path_or_id)
        )
        bid = parse_weread_uri(book_path_or_id) or book_path_or_id
        prog = self.storage.load_local_progress(bid) or {} if is_wr else {}
        pct = prog.get("progress", 0)
        pending = is_wr and prog.get("dirty", 0) == 1
        badge = "· 待同步" if pending else ""
        display = f"已读 {pct}% {badge}".strip() if pending else f"已读 {pct}%"
        return {
            "path": book_path_or_id,
            "isWeread": is_wr,
            "progress": pct,
            "pending": pending,
            "pendingBadge": badge,
            "statusDisplay": display,
        }


# ---------------------------------------------------------------------------
# WR-UX Shelf & Reader State Machine Simulators (Additive, Zero-Breakage)
# ---------------------------------------------------------------------------

class WereadShelfModel:
    """Models app_weread.c WR-UX-01/03/07/08/09 state transitions and zero-IO render."""

    FILTER_LABELS = {
        "ALL": "全部书籍",
        "READING": "在读",
        "OFFLINE": "已离线",
        "UNREAD": "未读",
        "FINISHED": "已读完",
    }
    DROPDOWN_RECT = (24, 79, 260, 74)
    QR_AREA_RECT = (72, 218, 540, 540)
    SEARCH_CONFIRM_RECT = (560, 76, 124, 80)
    SEARCH_HINT = "可输入首字母（如 st）、拼音或中文"
    OFFLINE_READONLY_PROMPT = "登录已过期，可读已缓存书；点「更多」重新扫码"
    QR_EXPIRED_PROMPT = "二维码已过期，点击刷新"
    PINYIN_DICT = {
        "三": ("san", "s"),
        "体": ("ti", "t"),
        "测": ("ce", "c"),
        "试": ("shi", "s"),
        "小": ("xiao", "x"),
        "说": ("shuo", "s"),
        "荒": ("huang", "h"),
        "原": ("yuan", "y"),
        "起": ("qi", "q"),
        "序": ("xu", "x"),
        "章": ("zhang", "z"),
        "录": ("lu", "l"),
    }

    def __init__(self, storage: WereadStorage, style: int = 6) -> None:
        self.storage = storage
        self.style = style  # 6 = 9-grid, 5 = list (8 rows)
        self.books: list[dict] = []
        self.visible_indices: list[int] = []
        self.s_page: int = 0
        self.filter_mode: str = "ALL"
        self.dropdown_open: bool = False
        self.more_menu_open: bool = False
        self.search_active: bool = False
        self.search_draft: str = ""
        self.query: str = ""
        self.search_page: int = 0
        self.requested_book_id: str = ""
        self.qr_uid: str = ""
        self.qr_url: str = ""
        self.qr_expired_prompt: bool = False
        self.offline_readonly: bool = False
        self.screen: str = "SHELF"  # SHELF, QR_LOGIN_PROMPT, DIRECT_OPEN, ACTION_CARD, CACHE_MGMT, SLIDER
        self.syncing: bool = False
        self.pending_sync: bool = False
        self.error_message: str = ""
        self.bottom_message: str = ""
        self.active_job: str | None = None
        self.cache_job_book_id: str | None = None
        self.last_cache_ui_update_ms: int = 0
        self.action_card_book_id: str | None = None
        self.action_card_info: dict | None = None
        self.storage_blocked: bool = False
        self.kb_page_count: int = 3
        self.primed_cover_pages: list[int] = []
        self.screen_history: list[str] = []
        self.cache_stats: dict | None = None
        self.io_calls_during_render: int = 0

    def page_rows(self) -> int:
        return 9 if self.style == 6 else 8

    def page_count(self) -> int:
        if not self.visible_indices:
            return 1
        r = self.page_rows()
        return (len(self.visible_indices) + r - 1) // r

    def load_snapshot(self, books: list[dict], authenticated: bool, expired: bool = False) -> None:
        self.books = [dict(b) for b in books]
        if expired and len(self.books) > 0:
            self.offline_readonly = True
            self.screen = "DIRECT_OPEN" if self.requested_book_id else "SHELF"
            self.bottom_message = self.OFFLINE_READONLY_PROMPT
        elif not authenticated and (not expired or len(self.books) == 0):
            self.offline_readonly = False
            self.screen = "QR_LOGIN_PROMPT"
            self.bottom_message = ""
        else:
            self.offline_readonly = False
            self.screen = "DIRECT_OPEN" if self.requested_book_id else "SHELF"
            self.bottom_message = ""
        self.screen_history.append(self.screen)
        self.filter_page(reset_page=True)

    @classmethod
    def _classify_status(cls, book: dict) -> set[str]:
        statuses = {"ALL"}
        prog = int(book.get("progress", 0))
        finished = bool(book.get("finished") == 1 or prog >= 100)
        cached = int(book.get("cachedChapters", 0))
        total = int(book.get("totalChapters", 0))
        if finished:
            statuses.add("FINISHED")
        elif prog == 0:
            statuses.add("UNREAD")
        else:
            statuses.add("READING")
        if total > 0 and cached >= total:
            statuses.add("OFFLINE")
        return statuses

    def filter_counts(self) -> dict[str, int]:
        counts = {k: 0 for k in self.FILTER_LABELS}
        for b in self.books:
            for st in self._classify_status(b):
                counts[st] += 1
        return counts

    def _pinyin_match(self, title: str, query: str) -> bool:
        if not query:
            return True
        q = query.lower()
        t_lower = title.lower()
        if q in t_lower:
            return True
        initials = "".join(self.PINYIN_DICT.get(ch, (ch, ch[0]))[1] for ch in title).lower()
        full_py = "".join(self.PINYIN_DICT.get(ch, (ch, ch[0]))[0] for ch in title).lower()
        return q in initials or q in full_py

    def filter_page(self, reset_page: bool = False) -> None:
        matched = []
        for idx, b in enumerate(self.books):
            if self.filter_mode not in self._classify_status(b):
                continue
            if self.query:
                hay = f"{b.get('title', '')} {b.get('author', '')}"
                if not self._pinyin_match(b.get("title", ""), self.query) and self.query.lower() not in hay.lower():
                    continue
            matched.append(idx)
        self.visible_indices = matched
        if reset_page:
            self.s_page = 0
        else:
            pc = self.page_count()
            if self.s_page >= pc:
                self.s_page = max(0, pc - 1)

    def set_filter(self, mode: str) -> None:
        if mode not in self.FILTER_LABELS:
            raise ValueError(f"Unknown filter mode: {mode}")
        self.filter_mode = mode
        self.dropdown_open = False
        self.filter_page(reset_page=True)

    def title_label(self) -> str:
        if self.filter_mode == "ALL":
            return "微信读书 ▾"
        return f"{self.FILTER_LABELS[self.filter_mode]} ▾"

    def hit_dropdown(self, x: int, y: int) -> bool:
        rx, ry, rw, rh = self.DROPDOWN_RECT
        if rx <= x < rx + rw and ry <= y < ry + rh:
            self.dropdown_open = not self.dropdown_open
            return True
        return False

    def can_open_page_slider(self) -> bool:
        return self.page_count() > 3

    def tap_footer_page_indicator(self) -> bool:
        if self.can_open_page_slider():
            self.screen = "SLIDER"
            return True
        return False

    def slider_jump_to(self, target_page: int) -> int:
        pc = self.page_count()
        self.s_page = max(0, min(int(target_page), pc - 1))
        self.screen = "SHELF"
        return self.s_page

    def swipe(self, direction: str) -> bool:
        if self.search_active:
            if direction == "L":
                if self.search_page + 1 < self.kb_page_count:
                    self.search_page += 1
                    return True
                return False
            if direction == "R" and self.search_page > 0:
                self.search_page -= 1
                return True
            return False
        pc = self.page_count()
        if direction == "L" and self.s_page + 1 < pc:
            self.s_page += 1
            self.prime_covers_from_ram()
            return True
        if direction == "R" and self.s_page > 0:
            self.s_page -= 1
            self.prime_covers_from_ram()
            return True
        return False

    def prime_covers_from_ram(self) -> None:
        self.primed_cover_pages.append(self.s_page)

    def set_search_draft(self, text: str) -> None:
        # Enforce 64-byte cap cleanly
        b = text.encode("utf-8")[:64]
        self.search_draft = b.decode("utf-8", errors="ignore")

    def clear_search(self) -> None:
        self.search_draft = ""
        self.query = ""
        self.search_page = 0
        self.filter_page(reset_page=True)

    def commit_search(self) -> None:
        self.query = self.search_draft
        self.search_active = False
        self.search_page = 0
        self.filter_page(reset_page=True)

    def tap_search_confirm(self, x: int, y: int) -> bool:
        if not self.search_active:
            return False
        rx, ry, rw, rh = self.SEARCH_CONFIRM_RECT
        if rx <= x < rx + rw and ry <= y < ry + rh:
            self.commit_search()
            return True
        return False

    def visible_books(self) -> list[dict]:
        r = self.page_rows()
        start = self.s_page * r
        return [self.books[i] for i in self.visible_indices[start : start + r]]

    def visible_rows_needing_probe(self) -> list[str]:
        return [
            b["bookId"]
            for b in self.visible_books()
            if int(b.get("cachedChapters", 0)) == 0 and int(b.get("totalChapters", 0)) == 0
        ]

    @staticmethod
    def classify_badge(book: dict) -> str:
        cached = int(book.get("cachedChapters", 0))
        total = int(book.get("totalChapters", 0))
        if total > 0 and cached >= total:
            return "OFFLINE_PILL"
        if cached > 0:
            return "DOT"
        return "NONE"

    @staticmethod
    def list_chapter_caption(book: dict) -> str:
        cached = int(book.get("cachedChapters", 0))
        total = int(book.get("totalChapters", 0))
        if total > 0 and cached >= total:
            return f"整本离线 · {total} 章"
        if cached > 0 and total > 0:
            return f"已存 {cached}/{total} 章"
        return ""

    def long_press_book(self, book_id: str) -> list[str]:
        b = next((x for x in self.books if x.get("bookId") == book_id), None)
        if not b:
            return []
        self.action_card_book_id = book_id
        self.screen = "ACTION_CARD"
        cached = int(b.get("cachedChapters", 0))
        total = int(b.get("totalChapters", 0))
        actions = ["取消"]
        if cached > 0:
            actions.append("清理缓存")
        if total == 0 or cached < total:
            actions.append("缓存整本")
        self.action_card_info = {
            "bookId": book_id,
            "title": b.get("title", book_id),
            "cachedChapters": cached,
            "totalChapters": total,
            "progress": int(b.get("progress", 0)),
            "actions": list(actions),
        }
        return actions

    def tap_action_card(self, action_label: str, now_ms: int = 0) -> str:
        if self.screen != "ACTION_CARD" or not self.action_card_book_id:
            return "NOOP"
        bid = self.action_card_book_id
        if action_label == "取消":
            self.action_card_book_id = None
            self.action_card_info = None
            self.screen = "SHELF"
            return "CANCELLED"
        if action_label == "清理缓存":
            self.storage.clean_book_chapters(bid)
            for b in self.books:
                if b.get("bookId") == bid:
                    b["cachedChapters"] = 0
            self.action_card_book_id = None
            self.action_card_info = None
            self.screen = "SHELF"
            self.filter_page(reset_page=False)
            return "CLEANED"
        if action_label == "缓存整本":
            self.action_card_book_id = None
            self.action_card_info = None
            self.start_whole_book_cache(bid, now_ms=now_ms)
            return "STARTED_CACHE"
        return "NOOP"

    def cache_whole_book(
        self,
        client: WereadClient,
        book_id: str,
        chapter_uids: list[int],
        free_bytes_provider,
        book_type: str = "epub",
        now_ms: int = 1000,
    ) -> dict:
        """WR-UX-07: Whole-book cache loop gated by 104 MiB free-space floor."""
        floor_bytes = 104 * 1024 * 1024
        self.storage_blocked = False
        self.start_whole_book_cache(book_id, now_ms=now_ms)
        total = len(chapter_uids)
        cached_count = 0
        for uid in chapter_uids:
            if self.storage.load_chapter_html(book_id, uid) is not None:
                cached_count += 1
                continue
            if free_bytes_provider() < floor_bytes:
                self.storage_blocked = True
                self.active_job = None
                self.cache_job_book_id = None
                self.bottom_message = "存储空间不足（需保留 104 MB）"
                self.storage.update_shelf_chapter_counts(book_id, cached_count, total)
                for b in self.books:
                    if b.get("bookId") == book_id:
                        b["cachedChapters"] = cached_count
                        b["totalChapters"] = total
                return {
                    "cached": cached_count,
                    "total": total,
                    "storage_blocked": True,
                    "stopped_at": uid,
                }
            res = client.download_chapter(book_id, uid, book_type=book_type)
            if res.get("status") != 200:
                self.active_job = None
                self.cache_job_book_id = None
                self.storage.update_shelf_chapter_counts(book_id, cached_count, total)
                return {
                    "cached": cached_count,
                    "total": total,
                    "storage_blocked": False,
                    "error_status": res.get("status"),
                    "stopped_at": uid,
                }
            cached_count += 1
            now_ms += 1000
            self.tick_whole_book_cache_progress(cached_count, total, now_ms=now_ms)
        for b in self.books:
            if b.get("bookId") == book_id:
                b["cachedChapters"] = cached_count
                b["totalChapters"] = total
        self.active_job = None
        self.cache_job_book_id = None
        self.bottom_message = ""
        self.storage.update_shelf_chapter_counts(book_id, cached_count, total)
        return {"cached": cached_count, "total": total, "storage_blocked": False}

    def start_whole_book_cache(self, book_id: str, now_ms: int) -> None:
        self.active_job = "WR_JOB_CACHE_BOOK"
        self.cache_job_book_id = book_id
        self.last_cache_ui_update_ms = now_ms
        self.screen = "SHELF"
        b = next((x for x in self.books if x.get("bookId") == book_id), {})
        self.bottom_message = f"正在缓存《{b.get('title', book_id)}》 {b.get('cachedChapters', 0)}/{b.get('totalChapters', 0)} 章…"

    def tick_whole_book_cache_progress(self, cached: int, total: int, now_ms: int) -> bool:
        if self.active_job != "WR_JOB_CACHE_BOOK" or not self.cache_job_book_id:
            return False
        for b in self.books:
            if b.get("bookId") == self.cache_job_book_id:
                b["cachedChapters"] = cached
                b["totalChapters"] = total
                title = b.get("title", self.cache_job_book_id)
                break
        else:
            title = self.cache_job_book_id
        if cached >= total and total > 0:
            self.active_job = None
            self.cache_job_book_id = None
            self.bottom_message = ""
            return True
        if now_ms - self.last_cache_ui_update_ms >= 3000:
            self.last_cache_ui_update_ms = now_ms
            self.bottom_message = f"正在缓存《{title}》 {cached}/{total} 章…"
            return True
        return False

    def preempt_for_open(self, book_id: str) -> str:
        """WR-UX-07 / WR-UX-01: Tapping any book cancels cache job and handles expired/cached vs uncached."""
        if self.active_job == "WR_JOB_CACHE_BOOK":
            self.active_job = None
            self.cache_job_book_id = None
        b = next((x for x in self.books if x.get("bookId") == book_id), None)
        if self.offline_readonly:
            if b and int(b.get("cachedChapters", 0)) > 0:
                self.active_job = "WR_JOB_OPEN"
                return "WR_JOB_OPEN"
            self.screen = "QR_LOGIN_PROMPT"
            return "QR_LOGIN_PROMPT"
        self.active_job = "WR_JOB_OPEN"
        return "WR_JOB_OPEN"

    def handle_qr_poll_result(self, logic_code: str) -> None:
        if logic_code in ("LOGIN_TIMEOUT", "EXPIRED", "LOGIN_CANCELLED", "LOGIN_FAILED"):
            self.qr_url = ""
            self.qr_uid = ""
            self.qr_expired_prompt = True
            self.bottom_message = self.QR_EXPIRED_PROMPT

    def tap_qr_area(self, x: int, y: int) -> bool:
        rx, ry, rw, rh = self.QR_AREA_RECT
        if rx <= x < rx + rw and ry <= y < ry + rh and self.qr_expired_prompt:
            self.active_job = "WR_JOB_QR"
            self.qr_expired_prompt = False
            return True
        return False

    def more_menu_items(self) -> list[str]:
        first = "重新扫码登录" if self.offline_readonly else "刷新书架"
        return [first, "搜索书籍", "退出登录", "缓存管理"]

    def open_more_item(self, index_or_label: int | str) -> str:
        items = self.more_menu_items()
        if isinstance(index_or_label, int):
            if index_or_label < 0 or index_or_label >= len(items):
                raise IndexError(f"More menu index out of range: {index_or_label}")
            label = items[index_or_label]
        else:
            label = str(index_or_label)
        self.more_menu_open = False
        if label == "重新扫码登录":
            self.screen = "QR_LOGIN_PROMPT"
            self.active_job = "WR_JOB_QR"
            return "QR_LOGIN_PROMPT"
        if label == "刷新书架":
            self.active_job = "WR_JOB_SYNC"
            return "WR_JOB_SYNC"
        if label == "搜索书籍":
            self.search_active = True
            return "SEARCH"
        if label == "退出登录":
            self.active_job = "WR_JOB_LOGOUT"
            return "WR_JOB_LOGOUT"
        if label == "缓存管理":
            self.open_cache_management()
            return "CACHE_MGMT"
        raise ValueError(f"Unknown more menu item: {index_or_label}")

    def qr_back_to_shelf(self) -> bool:
        if self.screen == "QR_LOGIN_PROMPT" and self.offline_readonly and len(self.books) > 0:
            self.screen = "SHELF"
            self.active_job = None
            self.bottom_message = self.OFFLINE_READONLY_PROMPT
            return True
        return False

    def open_cache_management(self, free_bytes: int = 512 * 1024 * 1024) -> dict:
        self.active_job = "WR_JOB_CACHE_STATS"
        self.cache_stats = self.storage.compute_cache_stats(free_bytes=free_bytes)
        self.screen = "CACHE_MGMT"
        return self.cache_stats

    def render(self) -> dict:
        """Zero-IO snapshot render returning observable UI elements."""
        io_before = self.storage.io_read_count
        try:
            if self.screen == "QR_LOGIN_PROMPT":
                return {
                    "screen": "QR_LOGIN_PROMPT",
                    "qrCleared": not bool(self.qr_url),
                    "qrPrompt": self.QR_EXPIRED_PROMPT if self.qr_expired_prompt else "",
                    "backToShelf": len(self.books) > 0 and self.offline_readonly,
                }
            if self.requested_book_id:
                if any(b.get("bookId") == self.requested_book_id for b in self.books):
                    grid_flashed = "SHELF" in self.screen_history
                    if self.offline_readonly:
                        target = next(b for b in self.books if b.get("bookId") == self.requested_book_id)
                        if int(target.get("cachedChapters", 0)) == 0:
                            return {"screen": "QR_LOGIN_PROMPT", "backToShelf": True, "gridFlashed": grid_flashed}
                    return {"screen": "DIRECT_OPEN", "centeredText": "正在打开阅读…", "gridFlashed": grid_flashed}
                self.requested_book_id = ""
                if self.screen == "DIRECT_OPEN":
                    self.screen = "SHELF"
                    self.screen_history.append("SHELF")
            # Idle bottom line: "" when healthy/idle, only populated on sync/pending/error/expired
            if self.offline_readonly:
                bottom = self.OFFLINE_READONLY_PROMPT
            elif self.active_job == "WR_JOB_CACHE_BOOK":
                bottom = self.bottom_message
            elif self.syncing:
                bottom = "后台同步中…"
            elif self.pending_sync:
                bottom = "有进度待同步"
            elif self.error_message:
                bottom = self.error_message
            else:
                bottom = ""

            vis = self.visible_books()
            out = {
                "screen": self.screen,
                "title": self.title_label(),
                "bottomMessage": bottom,
                "page": self.s_page,
                "pageCount": self.page_count(),
                "badges": {b["bookId"]: self.classify_badge(b) for b in vis},
                "listCaptions": {b["bookId"]: self.list_chapter_caption(b) for b in vis},
                "searchHint": self.SEARCH_HINT if self.search_active else "",
                "moreItems": self.more_menu_items(),
            }
            if self.screen == "ACTION_CARD" and self.action_card_info is not None:
                out["actionCard"] = dict(self.action_card_info)
            if self.dropdown_open:
                counts = self.filter_counts()
                out["dropdownItems"] = [
                    {"mode": k, "label": v, "count": counts[k]}
                    for k, v in self.FILTER_LABELS.items()
                ]
            return out
        finally:
            self.io_calls_during_render += self.storage.io_read_count - io_before


class WereadReaderModel:
    """Models book_weread_bridge.inc + book_weread_chapter.inc WR-UX-02/04/05 transitions."""

    def __init__(self, current_chapter: int = 1, current_offset: int = 0) -> None:
        self.current_chapter: int = current_chapter
        self.current_offset: int = current_offset
        self.pages_turned_since_open: int = 0
        self.jump_pending: bool = False
        self.jump_target_chapter: int | None = None
        self.jump_toc_origin_page: int | None = None
        self.undo_banner_active: bool = False
        self.undo_banner_type: str = ""  # "UNDO" or "OPT_IN"
        self.undo_banner_text: str = ""
        self.undo_banner_expires_ms: int = 0
        self.undo_prev_pos: tuple[int, int] | None = None
        self.opt_in_target_pos: tuple[int, int] | None = None
        self.notice_text: str = ""
        self.notice_expires_ms: int = 0
        self.view: str = "READING"
        self.toc_page: int = 0
        self.heal_attempts: int = 0
        self.local_nav_dirty: bool = False
        self.s_reader_footer_pending: bool = False
        self.footer_wake_count: int = 0
        self.cached_chapters: int = 0
        self.total_chapters: int = 0
        self._footer_initialized: bool = False
        self._last_sync_tuple: tuple | None = None
        self._last_paused: bool = False
        self._last_storage_blocked: bool = False
        self._last_cached_count: int = 0

    @staticmethod
    def footer_sync_prefix(
        state: str,
        save_failed: bool = False,
        pending_save: bool = False,
        rescued: bool = False,
        moved: bool = False,
    ) -> str:
        if save_failed:
            return "未保存 · "
        if pending_save:
            return "待保存 · "
        if state == "WR_PROGRESS_CONFLICT":
            return "进度冲突 · "
        if rescued:
            return "暂存机内 · "
        if state == "WR_PROGRESS_PENDING":
            return "待同步 · "
        if state == "WR_PROGRESS_LOCAL" and moved:
            return "本地保存 · "
        return ""

    @staticmethod
    def footer_cache_prefix(
        cached: int,
        total: int,
        paused: bool = False,
        storage_blocked: bool = False,
    ) -> str:
        if storage_blocked:
            return "空间不足 · "
        return ""

    @staticmethod
    def footer_should_wake(
        before_state: str,
        after_state: str,
        before_err: bool = False,
        after_err: bool = False,
        cache_delta: bool = False,
    ) -> bool:
        """WR-UX-02: cache_delta alone MUST NOT wake footer refresh."""
        return (before_state != after_state) or (before_err != after_err)

    def sample_footer(
        self,
        storage: WereadStorage,
        book_id: str,
        moved: bool = False,
        sync_state: str = "WR_PROGRESS_SYNCED",
        save_failed: bool = False,
        pending_save: bool = False,
        rescued: bool = False,
        paused: bool = False,
        storage_blocked: bool = False,
        total_chapters: int = 0,
    ) -> bool:
        """WR-UX-02: Stateful footer sampling inspecting storage cached chapters; cache_delta alone NEVER sets s_reader_footer_pending."""
        new_cached = len(storage.get_cached_chapters(book_id))
        sync_tuple = (sync_state, save_failed, pending_save, rescued, moved if sync_state == "WR_PROGRESS_LOCAL" else False)
        if not self._footer_initialized:
            self._footer_initialized = True
            self._last_sync_tuple = sync_tuple
            self._last_paused = paused
            self._last_storage_blocked = storage_blocked
            self._last_cached_count = new_cached
            self.cached_chapters = new_cached
            self.total_chapters = total_chapters
            self.s_reader_footer_pending = False
            return False
        wake = (
            sync_tuple != self._last_sync_tuple
            or paused != self._last_paused
            or storage_blocked != self._last_storage_blocked
        )
        self._last_sync_tuple = sync_tuple
        self._last_paused = paused
        self._last_storage_blocked = storage_blocked
        self._last_cached_count = new_cached
        self.cached_chapters = new_cached
        self.total_chapters = total_chapters
        self.s_reader_footer_pending = wake
        if wake:
            self.footer_wake_count += 1
        return wake

    @staticmethod
    def toolbar_button6_label(
        cached: int,
        total: int,
        paused: bool = False,
        stalled: bool = False,
        storage_blocked: bool = False,
    ) -> str:
        if total > 0 and cached >= total:
            return "已全存"
        if paused:
            return "已暂停"
        if stalled or storage_blocked:
            return "重试缓存"
        if total > 0:
            return f"缓存 {cached}/{total}"
        return "缓存中"

    @classmethod
    def non_fullscreen_topbar_cache_label(
        cls,
        cached: int,
        total: int,
        paused: bool = False,
        stalled: bool = False,
        storage_blocked: bool = False,
    ) -> str:
        if total == 0 and not paused and not stalled and not storage_blocked:
            return ""
        return cls.toolbar_button6_label(cached, total, paused=paused, stalled=stalled, storage_blocked=storage_blocked)

    def save_local_only(self) -> None:
        """Saving progress MUST NOT reset pages_turned_since_open."""
        pass

    def turn_page(self, new_offset: int, now_ms: int) -> None:
        self.current_offset = new_offset
        self.pages_turned_since_open += 1
        if self.undo_banner_active and self.undo_banner_type == "UNDO":
            self.undo_banner_active = False
            self.undo_banner_text = ""

    def on_cloud_jump_arrival(self, target_chapter: int, target_offset: int, now_ms: int) -> str:
        if (target_chapter, target_offset) == (self.current_chapter, self.current_offset):
            return "NOOP"
        if self.pages_turned_since_open == 0:
            self.undo_prev_pos = (self.current_chapter, self.current_offset)
            self.current_chapter = target_chapter
            self.current_offset = target_offset
            self.undo_banner_active = True
            self.undo_banner_type = "UNDO"
            self.undo_banner_text = "已接力至手机最新进度 [返回原处]"
            self.undo_banner_expires_ms = now_ms + 6000
            return "AUTO_JUMPED"
        self.opt_in_target_pos = (target_chapter, target_offset)
        self.undo_banner_active = True
        self.undo_banner_type = "OPT_IN"
        self.undo_banner_text = f"发现更新的手机进度（第 {target_chapter} 章） [点击跳转]"
        self.undo_banner_expires_ms = now_ms + 6000
        return "REBASED_OPT_IN"

    def tap_banner(self, storage: WereadStorage | None = None, book_id: str | None = None) -> str:
        if not self.undo_banner_active:
            return "NOOP"
        if self.undo_banner_type == "UNDO" and self.undo_prev_pos is not None:
            ch, off = self.undo_prev_pos
            self.undo_banner_active = False
            self.undo_banner_text = ""
            if storage and book_id and storage.load_chapter_html(book_id, ch) is None:
                self.jump_pending = True
                self.jump_target_chapter = ch
                return "QUEUED_COLD_UNDO"
            self.current_chapter = ch
            self.current_offset = off
            self.local_nav_dirty = True
            self.pages_turned_since_open += 1
            return "RESTORED_PREV"
        if self.undo_banner_type == "OPT_IN" and self.opt_in_target_pos is not None:
            ch, off = self.opt_in_target_pos
            self.undo_banner_active = False
            self.undo_banner_text = ""
            if storage and book_id and storage.load_chapter_html(book_id, ch) is None:
                self.jump_pending = True
                self.jump_target_chapter = ch
                return "QUEUED_COLD_OPT_IN"
            self.current_chapter = ch
            self.current_offset = off
            return "JUMPED_TO_CLOUD"
        return "NOOP"

    def tick(self, now_ms: int) -> None:
        if self.undo_banner_active and now_ms >= self.undo_banner_expires_ms:
            self.undo_banner_active = False
            self.undo_banner_text = ""
        if self.notice_text and now_ms >= self.notice_expires_ms:
            self.notice_text = ""

    def start_cold_jump(self, target_chapter: int, toc_origin_page: int | None = None) -> None:
        self.jump_pending = True
        self.jump_target_chapter = target_chapter
        self.jump_toc_origin_page = toc_origin_page

    def wr_chapter_cancel(self) -> bool:
        if not self.jump_pending:
            return False
        self.jump_pending = False
        self.jump_target_chapter = None
        self.jump_toc_origin_page = None
        self.notice_text = ""
        return True

    def forward_turn_cold(self, client: WereadClient, book_id: str, next_uid: int, now_ms: int, book_type: str = "epub") -> dict:
        if not client.wifi_enabled:
            self.notice_text = "本章未缓存且网络不可用，连网后再翻页重试"
            self.notice_expires_ms = now_ms + 5000
            return {"status": "offline", "notice": self.notice_text}
        res = client.download_chapter(book_id, next_uid, book_type=book_type)
        if res.get("status") == 402:
            self.notice_text = "手机端购买本章后，再翻页即可加载"
            self.notice_expires_ms = now_ms + 5000
            return {"status": 402, "notice": self.notice_text}
        if res.get("status") == 200:
            self.current_chapter = next_uid
            self.current_offset = 0
            self.notice_text = ""
        return res

    def complete_toc_cold_jump(self, client: WereadClient, book_id: str, target_uid: int, origin_toc_page: int, now_ms: int, book_type: str = "epub") -> bool:
        self.toc_page = origin_toc_page
        self.start_cold_jump(target_uid, toc_origin_page=origin_toc_page)
        if not client.wifi_enabled:
            self.view = "TOC"
            self.toc_page = origin_toc_page
            self.notice_text = "本章未缓存且网络不可用，连网后再翻页重试"
            self.notice_expires_ms = now_ms + 5000
            self.jump_pending = False
            return False
        res = client.download_chapter(book_id, target_uid, book_type=book_type)
        if res.get("status") != 200:
            self.view = "TOC"
            self.toc_page = origin_toc_page
            self.notice_text = "手机端购买本章后，再翻页即可加载" if res.get("status") == 402 else f"章节加载失败 ({res.get('status')})"
            self.notice_expires_ms = now_ms + 5000
            self.jump_pending = False
            return False
        self.view = "READING"
        self.current_chapter = target_uid
        self.current_offset = 0
        self.jump_pending = False
        return True

    def prepare_open(self, client: WereadClient, book_id: str, chapter_uid: int, book_type: str = "epub", book_version: int | None = None) -> dict:
        """WR-UX-05: On -2063 / WEREAD_ERR_VERSION, auto-heal ONCE via sync_book_details + sync_toc."""
        self.heal_attempts = 0
        res = client.download_chapter(book_id, chapter_uid, book_type=book_type, book_version=book_version)
        if res.get("errcode") == -2063:
            self.heal_attempts += 1
            details = client.sync_book_details([book_id])
            new_ver = details[0].get("bookVersion", book_version) if details else book_version
            client.sync_toc(book_id)
            res = client.download_chapter(book_id, chapter_uid, book_type=book_type, book_version=new_ver)
        return res
