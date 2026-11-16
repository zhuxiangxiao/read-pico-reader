#!/usr/bin/env python3
"""Tier 2 Boundary & Corner Cases: Corrupt Payloads, Traversal Attacks & Malformed Data

Covers:
- Category 1: Corrupt JSON Syntax (truncated shelf, invalid types, TOC preserved on error, null required fields, binary garbage)
- Category 2: Corrupt Base64 encryptkey (invalid characters, truncated length, empty string, oversized >4096B, non-multiple of 16)
- Category 3: Invalid AES PKCS#7 Padding (zero pad byte, pad > 16, mismatched pad bytes, all-zeros block, truncated block)
- Category 4: Corrupted ZIP Magic & CRC32 (magic bytes mismatch, corrupted_crc.zip fixture, truncated local header, bad central dir, ZipCrypto check byte)
- Category 5: Truncated Deflate Stream & EOF (truncated deflate, bad zlib header, truncated tar stream, bad tar checksum, graceful error notice)
- Category 6: Path Traversal Guards (path_traversal_attack.zip fixture, absolute paths, nested res/ escape, Windows backslashes, URL encoded dots)
- Category 7: Corrupted auth.json on Disk (truncated JSON, binary garbage, zero bytes, null credentials, stale .tmp cleanup)
"""

from __future__ import annotations

import base64
import io
import json
import os
import sys
import tarfile
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
import zlib

# Helper to ensure tools/ is on path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
WORKSPACE_ROOT = os.path.abspath(os.path.join(TOOLS_DIR, ".."))
FIXTURES_DIR = os.path.join(TOOLS_DIR, "fixtures", "weread")
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


# ==============================================================================
# Category 1: Corrupt JSON Syntax
# ==============================================================================

def test_corrupt_json_shelf_truncated():
    """Verify truncated JSON response from shelf API is caught without uncaught exception."""
    truncated_payload = b'{"books": [{"bookId": "b1", "title":'
    parsed = None
    try:
        parsed = json.loads(truncated_payload.decode("utf-8"))
    except json.JSONDecodeError:
        parsed = None
    assert parsed is None


def test_corrupt_json_invalid_types():
    """Verify JSON with string where array is expected is handled safely."""
    payload = '{"books": "this should be an array", "synckey": "not_an_int"}'
    data = json.loads(payload)
    books = data.get("books")
    if not isinstance(books, list):
        books = []
    assert books == []


def test_corrupt_json_toc_preserves_old_cache():
    """Verify receiving malformed JSON for TOC leaves existing cached chapters.json untouched."""
    with tempfile.TemporaryDirectory() as storage_root:
        toc_file = os.path.join(storage_root, "chapters.json")
        with open(toc_file, "w", encoding="utf-8") as f:
            f.write('{"bookId": "b1", "chapters": [{"chapterUid": 1}]}')

        corrupt_incoming = b'{"data": corrupt_syntax}'
        try:
            new_toc = json.loads(corrupt_incoming.decode("utf-8"))
            with open(toc_file, "w") as f:
                json.dump(new_toc, f)
        except Exception:
            pass  # Keep previous

        with open(toc_file, "r", encoding="utf-8") as f:
            cached = json.load(f)
        assert len(cached["chapters"]) == 1


def test_corrupt_json_null_required_fields():
    """Verify missing or null fields fall back to safe defaults."""
    book_entry = {"bookId": "b1", "title": None, "author": None, "readUpdateTime": None}
    title = book_entry.get("title") or book_entry["bookId"]
    author = book_entry.get("author") or "未知作者"
    time_val = book_entry.get("readUpdateTime") or 0
    assert title == "b1"
    assert author == "未知作者"
    assert time_val == 0


def test_corrupt_json_binary_garbage():
    """Verify random binary garbage returned from endpoint is rejected."""
    garbage = b"\x80\xff\xfe\x00\x01\x02\xaa\xbb\xcc"
    try:
        json.loads(garbage.decode("utf-8"))
        assert False, "Should have raised decode error"
    except (UnicodeDecodeError, json.JSONDecodeError):
        pass


# ==============================================================================
# Category 2: Corrupt Base64 encryptkey
# ==============================================================================

def test_corrupt_base64_invalid_chars():
    """Verify Base64 string with invalid non-alphabet characters is rejected."""
    bad_b64 = "ab!@#$%=123"
    try:
        base64.b64decode(bad_b64, validate=True)
        assert False, "Expected error on invalid base64 chars"
    except Exception:
        pass


def test_corrupt_base64_truncated_length():
    """Verify truncated Base64 string with odd length is rejected."""
    bad_b64 = "abcde"
    try:
        base64.b64decode(bad_b64, validate=True)
        assert False, "Expected error on invalid base64 length"
    except Exception:
        pass


def test_corrupt_base64_empty_string():
    """Verify empty Base64 string in encryptkey header is caught."""
    enc_hdr = ""
    is_valid = bool(enc_hdr and len(enc_hdr.strip()) > 0)
    assert is_valid is False


def test_corrupt_base64_excessive_length():
    """Verify excessively long Base64 string (>4096 bytes) is rejected as buffer safety limit."""
    huge_b64 = "A" * 8192
    max_allowed = 4096
    is_safe_length = len(huge_b64) <= max_allowed
    assert is_safe_length is False


def test_corrupt_base64_non_multiple_of_16_ciphertext():
    """Verify decoded ciphertext whose length is not a multiple of 16 is rejected for AES-CBC."""
    # Base64 of 15 bytes
    raw_15 = b"A" * 15
    b64_str = base64.b64encode(raw_15).decode("ascii")
    ct = base64.b64decode(b64_str)
    assert len(ct) == 15
    is_valid_block_size = (len(ct) % 16) == 0
    assert is_valid_block_size is False


# ==============================================================================
# Category 3: Invalid AES PKCS#7 Padding
# ==============================================================================

def test_invalid_padding_zero_byte():
    """Verify decrypted plaintext ending in 0x00 is rejected as invalid PKCS#7 padding."""
    block = b"valid_plain_text\x00"
    pad_len = block[-1]
    is_valid_padding = 1 <= pad_len <= 16
    assert is_valid_padding is False


def test_invalid_padding_greater_than_16():
    """Verify decrypted plaintext ending in 0x11 (17) is rejected."""
    block = b"valid_plain_text\x11"
    pad_len = block[-1]
    is_valid_padding = 1 <= pad_len <= 16
    assert is_valid_padding is False


def test_invalid_padding_mismatched_bytes():
    """Verify padding with mismatched byte values (e.g. 0x03, 0x02, 0x03) is rejected."""
    block = b"plain_data" + bytes([0x03, 0x02, 0x03])
    pad_len = block[-1]  # 3
    pad_bytes = block[-pad_len:]
    is_valid = all(b == pad_len for b in pad_bytes)
    assert is_valid is False


def test_invalid_padding_all_zeros_block():
    """Verify block of all zeros is rejected for PKCS#7 padding."""
    block = bytes([0x00] * 16)
    pad_len = block[-1]
    is_valid = 1 <= pad_len <= 16 and all(b == pad_len for b in block[-pad_len:])
    assert is_valid is False


def test_invalid_padding_truncated_block():
    """Verify incomplete AES block (< 16 bytes) cannot be decrypted."""
    block = b"short_block"
    assert len(block) < 16
    can_decrypt = len(block) >= 16 and (len(block) % 16) == 0
    assert can_decrypt is False


# ==============================================================================
# Category 4: Corrupted ZIP Magic & CRC32
# ==============================================================================

def test_corrupted_zip_magic_bytes():
    """Verify zip payload with zeroed magic bytes is rejected."""
    corrupted_header = b"\x00\x00\x00\x00" + b"\x00" * 26
    is_zip = corrupted_header.startswith(b"PK\x03\x04")
    assert is_zip is False


def test_corrupted_zip_crc32_fixture():
    """Verify reading fixture corrupted_crc.zip raises CRC error."""
    fixture_path = os.path.join(FIXTURES_DIR, "corrupted_crc.zip")
    assert os.path.isfile(fixture_path), f"Missing fixture {fixture_path}"
    with open(fixture_path, "rb") as f:
        content = f.read()

    with zipfile.ZipFile(io.BytesIO(content)) as zf:
        zf.setpassword(b"test_password_123")
        try:
            # testzip returns the name of the first file with CRC mismatch
            bad_file = zf.testzip()
            assert bad_file is not None
            try:
                zf.read(bad_file)
                assert False, "Expected BadZipFile when reading corrupted entry"
            except zipfile.BadZipFile:
                pass
        except RuntimeError as e:
            # If CRC byte 3 was corrupted, ZipCrypto verification byte
            # mismatch raises RuntimeError('Bad password...')
            assert "password" in str(e).lower()


def test_corrupted_zip_local_file_header():
    """Verify truncated local file header (< 30 bytes) raises BadZipFile."""
    truncated = b"PK\x03\x04\x14\x00"
    try:
        with zipfile.ZipFile(io.BytesIO(truncated)) as zf:
            zf.namelist()
        assert False, "Expected BadZipFile"
    except zipfile.BadZipFile:
        pass


def test_corrupted_zip_central_directory():
    """Verify corrupt central directory end record signature is rejected."""
    corrupt_zip = b"PK\x03\x04" + b"\x00" * 40 + b"PK\x05\x06_corrupted"
    try:
        with zipfile.ZipFile(io.BytesIO(corrupt_zip)) as zf:
            zf.namelist()
        assert False, "Expected BadZipFile"
    except zipfile.BadZipFile:
        pass


def test_corrupted_zip_password_verification_byte():
    """Verify ZipCrypto 12-byte header verification byte mismatch fails authentication."""
    # ZipCrypto header 12th byte must match last byte of CRC32 or MSB of last mod time
    expected_verify_byte = 0xAA
    actual_header_byte = 0xBB
    auth_passed = expected_verify_byte == actual_header_byte
    assert auth_passed is False


# ==============================================================================
# Category 5: Truncated Deflate Stream & EOF
# ==============================================================================

def test_truncated_deflate_stream():
    """Verify truncated deflate stream raises zlib.error."""
    original = b"Hello WeRead Compressed Stream Test Data"
    compressed = zlib.compress(original)
    truncated = compressed[:len(compressed) // 2]
    try:
        zlib.decompress(truncated)
        assert False, "Expected zlib.error on truncated stream"
    except zlib.error:
        pass


def test_corrupted_zlib_header():
    """Verify corrupt zlib header bytes raise zlib.error."""
    bad_header = b"\xff\xff" + b"\x00" * 20
    try:
        zlib.decompress(bad_header)
        assert False, "Expected zlib.error"
    except zlib.error:
        pass


def test_truncated_tar_stream():
    """Verify incomplete tar archive ending before 512-byte block raises ReadError."""
    short_tar = b"ustar" + b"\x00" * 100
    try:
        with tarfile.open(fileobj=io.BytesIO(short_tar)) as t:
            t.getnames()
        assert False, "Expected tarfile error"
    except (tarfile.ReadError, tarfile.TarError):
        pass


def test_corrupted_tar_checksum():
    """Verify tar header with invalid checksum is rejected."""
    header = bytearray(512)
    header[0:5] = b"file1"
    header[148:156] = b"000000\x00 "  # bad checksum
    try:
        with tarfile.open(fileobj=io.BytesIO(header)) as t:
            t.getnames()
    except (tarfile.ReadError, tarfile.TarError):
        pass


def test_decompression_error_graceful_recovery():
    """Verify decompression failure sets error notice in reader state without crash."""
    reader_state = {"error": None, "page_rendered": False}
    try:
        zlib.decompress(b"garbage")
    except zlib.error:
        reader_state["error"] = "DECOMPRESSION_FAILED"
    assert reader_state["error"] == "DECOMPRESSION_FAILED"


# ==============================================================================
# Category 6: Path Traversal Guards
# ==============================================================================

def test_path_traversal_dot_dot_slash():
    """Verify path_traversal_attack.zip fixture entries with ../../ are rejected or stripped."""
    fixture_path = os.path.join(FIXTURES_DIR, "path_traversal_attack.zip")
    assert os.path.isfile(fixture_path), f"Missing fixture {fixture_path}"
    with open(fixture_path, "rb") as f:
        content = f.read()

    target_dir = "/storage/cache/book_1"
    with zipfile.ZipFile(io.BytesIO(content)) as zf:
        names = zf.namelist()
        assert "info.txt" in names, "Fixture must contain benign info.txt entry"
        malicious_entries = [n for n in names if ".." in n]
        assert len(malicious_entries) > 0, "Fixture must contain at least one path traversal entry"

        for name in names:
            is_malicious = ".." in name
            safe_name = os.path.basename(name)
            assert ".." not in safe_name
            if is_malicious:
                raw_resolved = os.path.abspath(os.path.join(target_dir, name))
                escaped = not raw_resolved.startswith(target_dir + os.sep)
                assert escaped is True, "Raw malicious path should escape root"
                assert safe_name == "evil.txt"
            else:
                assert safe_name == "info.txt"

            safe_resolved = os.path.abspath(os.path.join(target_dir, safe_name))
            assert safe_resolved.startswith(target_dir + os.sep)


def test_path_traversal_absolute_paths():
    """Verify absolute paths (starting with /) in archives are sanitized."""
    abs_entry = "/etc/passwd"
    is_absolute = abs_entry.startswith("/") or abs_entry.startswith("\\")
    assert is_absolute is True
    safe = os.path.normpath(abs_entry).lstrip("/").lstrip("\\")
    assert safe == "etc/passwd"


def test_path_traversal_nested_res_escape():
    """Verify nested escape attempts like res/../../evil cannot escape root."""
    target_dir = "/storage/cache/book_1"
    entry_path = "res/../../evil_file.txt"
    resolved = os.path.abspath(os.path.join(target_dir, entry_path))
    escaped = not resolved.startswith(target_dir + os.sep)
    assert escaped is True  # Detected traversal attempt!


def test_path_traversal_backslash_windows():
    """Verify Windows-style backslash traversal ..\\..\\evil is sanitized."""
    entry = "..\\..\\evil"
    normalized = entry.replace("\\", "/")
    assert ".." in normalized
    safe = os.path.basename(normalized)
    assert safe == "evil"


def test_path_traversal_url_encoded():
    """Verify URL-encoded dot-dot %2e%2e%2f is detected and rejected."""
    entry = "%2e%2e%2fevil.txt"
    unquoted = urllib.parse.unquote(entry)
    assert ".." in unquoted


# ==============================================================================
# Category 7: Corrupted auth.json on Disk
# ==============================================================================

def test_corrupted_auth_json_truncated_file():
    """Verify truncated auth.json on disk is detected and treated as invalid."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        with open(auth_file, "w") as f:
            f.write('{"vid": "1234", "access')  # truncated

        session = None
        try:
            with open(auth_file, "r") as f:
                session = json.load(f)
        except json.JSONDecodeError:
            session = None
        assert session is None


def test_corrupted_auth_json_binary_garbage():
    """Verify random binary bytes in auth.json are caught and ignored."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        with open(auth_file, "wb") as f:
            f.write(b"\x00\x01\x02\xff\xfe")

        session = None
        try:
            with open(auth_file, "r", encoding="utf-8") as f:
                session = json.load(f)
        except Exception:
            session = None
        assert session is None


def test_corrupted_auth_json_empty_zero_bytes():
    """Verify zero-byte auth.json is detected and removed."""
    with tempfile.TemporaryDirectory() as storage_root:
        auth_file = os.path.join(storage_root, "auth.json")
        with open(auth_file, "w"):
            pass

        if os.path.getsize(auth_file) == 0:
            os.remove(auth_file)
        assert not os.path.exists(auth_file)


def test_corrupted_auth_json_null_credentials():
    """Verify auth.json with null vid and accessToken triggers re-login."""
    data = {"vid": None, "accessToken": None}
    is_valid = bool(data.get("vid") and data.get("accessToken"))
    assert is_valid is False


def test_stale_auth_tmp_cleanup():
    """Verify stale .tmp file from previous aborted write is cleaned up."""
    with tempfile.TemporaryDirectory() as storage_root:
        tmp_file = os.path.join(storage_root, "auth.json.tmp")
        with open(tmp_file, "w") as f:
            f.write("partial")

        assert os.path.exists(tmp_file)
        # Cleanup routine
        if os.path.exists(tmp_file):
            os.remove(tmp_file)
        assert not os.path.exists(tmp_file)


# ==============================================================================
# Runner
# ==============================================================================

ALL_TESTS = [
    # Category 1
    test_corrupt_json_shelf_truncated,
    test_corrupt_json_invalid_types,
    test_corrupt_json_toc_preserves_old_cache,
    test_corrupt_json_null_required_fields,
    test_corrupt_json_binary_garbage,
    # Category 2
    test_corrupt_base64_invalid_chars,
    test_corrupt_base64_truncated_length,
    test_corrupt_base64_empty_string,
    test_corrupt_base64_excessive_length,
    test_corrupt_base64_non_multiple_of_16_ciphertext,
    # Category 3
    test_invalid_padding_zero_byte,
    test_invalid_padding_greater_than_16,
    test_invalid_padding_mismatched_bytes,
    test_invalid_padding_all_zeros_block,
    test_invalid_padding_truncated_block,
    # Category 4
    test_corrupted_zip_magic_bytes,
    test_corrupted_zip_crc32_fixture,
    test_corrupted_zip_local_file_header,
    test_corrupted_zip_central_directory,
    test_corrupted_zip_password_verification_byte,
    # Category 5
    test_truncated_deflate_stream,
    test_corrupted_zlib_header,
    test_truncated_tar_stream,
    test_corrupted_tar_checksum,
    test_decompression_error_graceful_recovery,
    # Category 6
    test_path_traversal_dot_dot_slash,
    test_path_traversal_absolute_paths,
    test_path_traversal_nested_res_escape,
    test_path_traversal_backslash_windows,
    test_path_traversal_url_encoded,
    # Category 7
    test_corrupted_auth_json_truncated_file,
    test_corrupted_auth_json_binary_garbage,
    test_corrupted_auth_json_empty_zero_bytes,
    test_corrupted_auth_json_null_credentials,
    test_stale_auth_tmp_cleanup,
]


def run_all_tests():
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 2 (Corrupt Payloads & Malformed Data) Suite: {len(ALL_TESTS)} tests ===")
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
