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

    # Shelf storage
    def save_shelf(self, shelf_data: dict) -> None:
        self._atomic_write_json(self.shelf_file, shelf_data)

    def load_shelf(self) -> dict | None:
        return self._read_json(self.shelf_file)

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
        p = os.path.join(self.book_cache_dir(book_id), "chapters", f"{chapter_uid}.html")
        if os.path.exists(p):
            with open(p, "r", encoding="utf-8") as f:
                return f.read()
        return None

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

    def set_wifi_enabled(self, enabled: bool) -> None:
        self.wifi_enabled = enabled

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

        # Attach auth headers if stored and target is trusted
        auth = self.storage.load_auth()
        if auth:
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
            return False

        renewed = json.loads(body.decode("utf-8"))
        if "accessToken" in renewed:
            auth["accessToken"] = renewed["accessToken"]
            if "refreshToken" in renewed and renewed["refreshToken"]:
                auth["refreshToken"] = renewed["refreshToken"]
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
        self.storage.delete_auth()

    # Bookshelf routines
    def sync_shelf_full(self) -> dict:
        status, _, body = self.request("GET", "/shelf/sync?synckey=0&lectureSynckey=0")
        assert status == 200
        raw = json.loads(body.decode("utf-8"))
        # Filter out lectureBooks and sort by readUpdateTime DESC
        filtered_books = [b for b in raw.get("books", []) if b.get("format") != "audio" and b.get("type", 0) != 1]
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
    ) -> dict:
        """Download chapter archive, decrypt encryptkey, unpack ZipCrypto/TAR, extract shared res and HTML."""
        ch_str = str(chapters)
        path = f"/book/chapterdownload?bookId={book_id}&chapters={ch_str}&bookType={book_type}"
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
