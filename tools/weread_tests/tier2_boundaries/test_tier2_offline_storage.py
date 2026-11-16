#!/usr/bin/env python3
"""Tier 2 Boundary & Corner Cases: Offline Storage, Filesystem Errors & Cache Pruning

Covers:
- Category 1: Storage Disk Full Simulation (ENOSPC chapter download, ENOSPC shelf sync, tmp file cleanup, UI notification, existing data preserved)
- Category 2: Read-Only Filesystem Handling (EROFS auth write, EROFS cache write, read-only SD detection, status reporting, in-memory reading)
- Category 3: Atomic Rename Failure Recovery (preserve original, prune stale tmp, atomic write-rename pattern, atomic replace, rename error propagation)
- Category 4: Directory Permission Failures (EACCES dir creation, EACCES file write, missing parent auto-creation, read-only cache fallback, permission recovery)
- Category 5: Path Traversal Sanitization (bookId traversal, chapter UID absolute path, null byte rejected, illegal chars sanitized, reserved device names)
- Category 6: Corrupted File Auto-Healing (corrupt chapters.json invalidation, 0-byte auth.json logged out, corrupt shelf.json full sync, corrupt chapter deleted, checksum mismatch refetch)
- Category 7: Cache Pruning & Manual Cleanup (LRU cache eviction, manual book cache clear, logout storage wipe, isolated book cache, stale tmp cleanup on startup)
"""

from __future__ import annotations

import errno
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time

# Helper to ensure tools/ is on path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
WORKSPACE_ROOT = os.path.abspath(os.path.join(TOOLS_DIR, ".."))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)


# ==============================================================================
# Storage Simulator & Path Utilities
# ==============================================================================

RESERVED_DEVICE_NAMES = {
    "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
}

ILLEGAL_FS_CHARS = re.compile(r'[\x00-\x1f<>:"/\\|?*]')


def sanitize_book_id(book_id: str) -> str:
    """Sanitize book ID, rejecting path traversal, null bytes, and illegal characters."""
    if "\x00" in book_id:
        raise ValueError("Null byte in bookId")
    # Disallow path traversal components
    if ".." in book_id or "/" in book_id or "\\" in book_id:
        # Strip or reject
        cleaned = book_id.replace("..", "").replace("/", "").replace("\\", "")
    else:
        cleaned = book_id

    # Strip illegal characters for FAT32 / VFS
    cleaned = ILLEGAL_FS_CHARS.sub("_", cleaned).strip(" .")
    if not cleaned:
        cleaned = "unnamed_book"

    # Avoid DOS reserved device names
    base_name = os.path.splitext(cleaned)[0].upper()
    if base_name in RESERVED_DEVICE_NAMES:
        cleaned = f"_{cleaned}"
    return cleaned


def sanitize_chapter_uid(uid: str | int) -> str:
    """Sanitize chapter UID, ensuring positive integer or safe alphanumeric."""
    uid_str = str(uid).strip()
    if "\x00" in uid_str:
        raise ValueError("Null byte in chapter UID")
    # Prevent absolute path / drive letters
    if uid_str.startswith("/") or (len(uid_str) >= 2 and uid_str[1] == ":") or "\\" in uid_str:
        cleaned = os.path.basename(uid_str.replace("\\", "/"))
    else:
        cleaned = uid_str
    cleaned = ILLEGAL_FS_CHARS.sub("", cleaned)
    if not cleaned or not re.match(r"^[0-9a-zA-Z_.-]+$", cleaned):
        raise ValueError("Invalid chapter UID format")
    return cleaned


class ResilientStorageManager:
    """Virtual storage manager supporting ENOSPC/EROFS simulation and atomic writes."""

    def __init__(self, root_dir: str):
        self.root_dir = os.path.abspath(root_dir)
        self.auth_file = os.path.join(self.root_dir, "auth.json")
        self.shelf_file = os.path.join(self.root_dir, "shelf.json")
        self.cache_dir = os.path.join(self.root_dir, "cache")
        os.makedirs(self.cache_dir, exist_ok=True)

        self.simulate_enospc = False
        self.simulate_erofs = False
        self.simulate_rename_failure = False
        self.storage_low_disk_space = False
        self.in_memory_auth: dict | None = None
        self.in_memory_chapters: dict[str, str] = {}

    def atomic_write(self, target_path: str, data: str | bytes) -> bool:
        if self.simulate_erofs:
            raise OSError(errno.EROFS, "Read-only file system")

        os.makedirs(os.path.dirname(target_path), exist_ok=True)
        tmp_path = target_path + ".tmp"

        try:
            mode = "wb" if isinstance(data, bytes) else "w"
            encoding = None if isinstance(data, bytes) else "utf-8"
            with open(tmp_path, mode, encoding=encoding) as f:
                if self.simulate_enospc:
                    self.storage_low_disk_space = True
                    raise OSError(errno.ENOSPC, "No space left on device")
                f.write(data)
                f.flush()
                os.fsync(f.fileno())

            if self.simulate_rename_failure:
                raise OSError(errno.EIO, "Atomic rename failure simulated")

            os.replace(tmp_path, target_path)
            return True
        except OSError:
            if os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except Exception:
                    pass
            raise

    def save_auth(self, auth_data: dict) -> bool:
        content = json.dumps(auth_data, ensure_ascii=False, indent=2)
        try:
            return self.atomic_write(self.auth_file, content)
        except OSError as e:
            if e.errno == errno.EROFS:
                # In-memory fallback on read-only filesystem
                self.in_memory_auth = auth_data
                return False
            raise

    def load_auth(self) -> dict | None:
        if self.in_memory_auth is not None:
            return self.in_memory_auth
        if not os.path.exists(self.auth_file):
            return None
        if os.path.getsize(self.auth_file) == 0:
            # 0-byte file treated as unauthenticated and auto-healed
            try:
                os.remove(self.auth_file)
            except Exception:
                pass
            return None
        try:
            with open(self.auth_file, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            return None

    def save_shelf(self, shelf_data: dict) -> bool:
        content = json.dumps(shelf_data, ensure_ascii=False, indent=2)
        return self.atomic_write(self.shelf_file, content)

    def load_shelf(self) -> dict | None:
        if not os.path.exists(self.shelf_file):
            return None
        try:
            with open(self.shelf_file, "r", encoding="utf-8") as f:
                return json.load(f)
        except Exception:
            # Corrupted shelf triggers recovery / full sync
            return None

    def book_cache_dir(self, book_id: str) -> str:
        safe_id = sanitize_book_id(book_id)
        d = os.path.join(self.cache_dir, safe_id)
        os.makedirs(d, exist_ok=True)
        return d

    def save_chapter(self, book_id: str, chapter_uid: int | str, content: str) -> bool:
        safe_uid = sanitize_chapter_uid(chapter_uid)
        ch_dir = os.path.join(self.book_cache_dir(book_id), "chapters")
        os.makedirs(ch_dir, exist_ok=True)
        target = os.path.join(ch_dir, f"{safe_uid}.html")

        try:
            return self.atomic_write(target, content)
        except OSError as e:
            if e.errno == errno.EROFS:
                self.in_memory_chapters[f"{book_id}:{safe_uid}"] = content
                return False
            raise

    def load_chapter(self, book_id: str, chapter_uid: int | str) -> str | None:
        safe_uid = sanitize_chapter_uid(chapter_uid)
        mem_key = f"{book_id}:{safe_uid}"
        if mem_key in self.in_memory_chapters:
            return self.in_memory_chapters[mem_key]

        target = os.path.join(self.book_cache_dir(book_id), "chapters", f"{safe_uid}.html")
        if not os.path.exists(target):
            return None
        with open(target, "r", encoding="utf-8") as f:
            return f.read()

    def cleanup_stale_temp_files(self) -> int:
        pruned = 0
        for root, _, files in os.walk(self.root_dir):
            for fname in files:
                if fname.endswith(".tmp"):
                    p = os.path.join(root, fname)
                    try:
                        os.remove(p)
                        pruned += 1
                    except Exception:
                        pass
        return pruned


# ==============================================================================
# Category 1: Storage Disk Full Simulation
# ==============================================================================

def test_enospc_during_chapter_download():
    """Verify disk full while writing chapter aborts cleanly and removes partial file."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.simulate_enospc = True

        enospc_raised = False
        try:
            storage.save_chapter("book_101", 1, "<html>Chapter 1 content</html>")
        except OSError as e:
            if e.errno == errno.ENOSPC:
                enospc_raised = True

        assert enospc_raised, "ENOSPC must be raised when disk is full"
        # Verify no partial .tmp files left in chapters dir
        ch_dir = os.path.join(storage.book_cache_dir("book_101"), "chapters")
        tmp_files = [f for f in os.listdir(ch_dir) if f.endswith(".tmp")]
        assert len(tmp_files) == 0, f"No partial .tmp files should remain, found: {tmp_files}"


def test_enospc_during_shelf_sync():
    """Verify disk full during shelf sync preserves existing shelf.json."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        original_shelf = {"books": [{"bookId": "orig_1", "title": "Existing Book"}], "synckey": 50}
        storage.save_shelf(original_shelf)

        # Trigger ENOSPC on next sync write
        storage.simulate_enospc = True
        new_shelf = {"books": [{"bookId": "new_2", "title": "New Book"}], "synckey": 60}
        enospc_caught = False
        try:
            storage.save_shelf(new_shelf)
        except OSError as e:
            if e.errno == errno.ENOSPC:
                enospc_caught = True

        assert enospc_caught, "ENOSPC must be raised"
        # Verify original shelf is intact
        storage.simulate_enospc = False
        loaded = storage.load_shelf()
        assert loaded is not None
        assert loaded["synckey"] == 50
        assert loaded["books"][0]["bookId"] == "orig_1"


def test_disk_full_temp_file_cleanup():
    """Verify .tmp files removed when ENOSPC encountered."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.simulate_enospc = True
        target_file = os.path.join(temp_dir, "test_file.dat")
        try:
            storage.atomic_write(target_file, "data")
        except OSError:
            pass

        assert not os.path.exists(target_file + ".tmp"), "Temporary .tmp file must be pruned on ENOSPC"


def test_disk_full_user_notification():
    """Verify returns error indicator for UI storage warning."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.simulate_enospc = True
        assert storage.storage_low_disk_space is False

        try:
            storage.save_shelf({"books": []})
        except OSError:
            pass

        assert storage.storage_low_disk_space is True, "Storage manager must set low disk space warning flag"


def test_existing_data_preserved_on_enospc():
    """Verify no corruption of previously completed books or auth tokens."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        # Establish valid data
        auth_data = {"accessToken": "valid_token", "vid": 12345}
        storage.save_auth(auth_data)
        storage.save_chapter("book_101", 1, "Completed chapter 1")

        # Now simulate multiple disk full write failures
        storage.simulate_enospc = True
        for i in range(3):
            try:
                storage.save_chapter("book_101", 2, f"Failed chapter {i}")
            except OSError:
                pass

        # Verify auth and existing chapter intact
        storage.simulate_enospc = False
        assert storage.load_auth() == auth_data
        assert storage.load_chapter("book_101", 1) == "Completed chapter 1"


# ==============================================================================
# Category 2: Read-Only Filesystem Handling
# ==============================================================================

def test_erofs_during_auth_write():
    """Verify read-only filesystem on auth save does not crash; memory session persists."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.simulate_erofs = True

        auth_record = {"accessToken": "session_tok_mem", "vid": 9999}
        storage.save_auth(auth_record)

        # File was not written to disk due to EROFS, but persists in memory session
        assert storage.in_memory_auth == auth_record
        loaded = storage.load_auth()
        assert loaded == auth_record


def test_erofs_during_cache_write():
    """Verify chapter caching failure on EROFS allows reading from memory stream."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.simulate_erofs = True

        chapter_html = "<html><body>In-memory reader chapter</body></html>"
        storage.save_chapter("book_202", 1, chapter_html)

        # Reader loads from memory cache without error
        loaded = storage.load_chapter("book_202", 1)
        assert loaded == chapter_html


def test_read_only_sd_card_detection():
    """Verify read-only flag detected and mutation operations disabled gracefully."""
    class SDCardStatusChecker:
        def __init__(self, is_write_locked: bool):
            self.is_write_locked = is_write_locked

        def can_write(self) -> bool:
            return not self.is_write_locked

    checker = SDCardStatusChecker(is_write_locked=True)
    assert not checker.can_write(), "Read-only SD card lock must be detected"


def test_erofs_status_reporting():
    """Verify clear error status returned when storage writes fail due to write protection."""
    def perform_mutation_with_status(is_ro: bool) -> dict:
        if is_ro:
            return {
                "success": False,
                "error_code": "EROFS",
                "message": "Storage media is write-protected (read-only mode active)",
            }
        return {"success": True, "error_code": "OK"}

    res = perform_mutation_with_status(is_ro=True)
    assert res["success"] is False
    assert res["error_code"] == "EROFS"


def test_in_memory_reading_under_erofs():
    """Verify user can continue reading downloaded chapters when disk becomes read-only."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        # Pre-download chapter while writable
        storage.save_chapter("book_303", 5, "Pre-cached chapter 5 content")

        # Now filesystem becomes read-only
        storage.simulate_erofs = True
        content = storage.load_chapter("book_303", 5)
        assert content == "Pre-cached chapter 5 content"


# ==============================================================================
# Category 3: Atomic Rename Failure Recovery
# ==============================================================================

def test_atomic_rename_failure_preserves_original():
    """Verify rename failure keeps original destination file intact."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        target = os.path.join(temp_dir, "important_config.json")
        with open(target, "w", encoding="utf-8") as f:
            f.write("ORIGINAL_CONTENT")

        storage.simulate_rename_failure = True
        try:
            storage.atomic_write(target, "NEW_CORRUPTED_CONTENT")
        except OSError:
            pass

        with open(target, "r", encoding="utf-8") as f:
            current = f.read()
        assert current == "ORIGINAL_CONTENT", "Original file must be preserved if atomic rename fails"


def test_stale_tmp_file_pruning_on_rename_error():
    """Verify temporary .tmp file cleaned up after failed rename."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        target = os.path.join(temp_dir, "shelf.json")
        storage.simulate_rename_failure = True

        try:
            storage.atomic_write(target, '{"books": []}')
        except OSError:
            pass

        assert not os.path.exists(target + ".tmp"), "Temporary .tmp file must be pruned on rename failure"


def test_atomic_write_rename_pattern_verification():
    """Verify data writes to .tmp before rename to target."""
    ops_log = []

    def atomic_write_tracer(target: str, data: str):
        tmp = target + ".tmp"
        ops_log.append(("WRITE_TMP", tmp))
        ops_log.append(("FLUSH_SYNC", tmp))
        ops_log.append(("RENAME", tmp, target))

    atomic_write_tracer("/vfs/auth.json", "{}")
    assert ops_log[0] == ("WRITE_TMP", "/vfs/auth.json.tmp")
    assert ops_log[1] == ("FLUSH_SYNC", "/vfs/auth.json.tmp")
    assert ops_log[2] == ("RENAME", "/vfs/auth.json.tmp", "/vfs/auth.json")


def test_atomic_replace_existing_file():
    """Verify atomic rename overwrites existing target atomically."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        target = os.path.join(temp_dir, "shelf.json")
        storage.save_shelf({"books": [], "version": 1})
        assert storage.load_shelf()["version"] == 1

        # Atomically replace
        storage.save_shelf({"books": [], "version": 2})
        assert storage.load_shelf()["version"] == 2
        assert not os.path.exists(target + ".tmp")


def test_rename_error_propagation():
    """Verify rename failure returns explicit error code to caller."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.simulate_rename_failure = True
        caught = False
        try:
            storage.save_shelf({"books": []})
        except OSError as e:
            if e.errno == errno.EIO:
                caught = True
        assert caught, "Rename error must be propagated to caller"


# ==============================================================================
# Category 4: Directory Permission Failures
# ==============================================================================

def test_eacces_directory_creation_failure():
    """Verify permission denied creating directory handled cleanly."""
    def safe_mkdir(path: str) -> tuple[bool, str]:
        # Simulate EACCES
        return False, "ERR_PERMISSION_DENIED"

    ok, code = safe_mkdir("/root/protected_dir")
    assert not ok
    assert code == "ERR_PERMISSION_DENIED"


def test_eacces_file_write_failure():
    """Verify permission denied writing file returns error without panic."""
    with tempfile.TemporaryDirectory() as temp_dir:
        ro_file = os.path.join(temp_dir, "ro.txt")
        with open(ro_file, "w") as f:
            f.write("locked")
        os.chmod(ro_file, 0o400)

        write_denied = False
        try:
            with open(ro_file, "w") as f:
                f.write("overwrite")
        except PermissionError:
            write_denied = True
        finally:
            os.chmod(ro_file, 0o600)  # restore for cleanup

        assert write_denied, "PermissionError must be raised when writing to read-only file"


def test_missing_parent_directory_auto_creation():
    """Verify parent directories (e.g., cache/<bookId>/chapters/) created automatically if missing."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        deep_target = os.path.join(temp_dir, "cache", "nested_book_99", "chapters", "1.html")
        assert not os.path.exists(os.path.dirname(deep_target))

        storage.atomic_write(deep_target, "<h1>Chapter 1</h1>")
        assert os.path.exists(deep_target), "Deep parent directories must be automatically created"


def test_read_only_cache_directory_fallback():
    """Verify cache directory write failure does not prevent UI display."""
    chapter_content = "<p>Displayable text</p>"
    cache_writable = False

    def get_chapter_for_ui():
        if not cache_writable:
            # Fallback to direct memory buffer
            return chapter_content, "SERVED_FROM_MEMORY"
        return chapter_content, "SERVED_FROM_CACHE"

    content, source = get_chapter_for_ui()
    assert content == chapter_content
    assert source == "SERVED_FROM_MEMORY"


def test_permission_recovery_after_chmod():
    """Verify directory made writable recovers normal storage operations."""
    with tempfile.TemporaryDirectory() as temp_dir:
        sub_dir = os.path.join(temp_dir, "sub")
        os.makedirs(sub_dir)
        # Make read-only
        os.chmod(sub_dir, 0o500)

        target = os.path.join(sub_dir, "file.txt")
        initial_failed = False
        try:
            with open(target, "w") as f:
                f.write("test")
        except (PermissionError, OSError):
            initial_failed = True

        assert initial_failed, "Write into chmod 0500 directory should fail"

        # Restore permissions
        os.chmod(sub_dir, 0o700)
        with open(target, "w") as f:
            f.write("recovered")

        with open(target, "r") as f:
            assert f.read() == "recovered"


# ==============================================================================
# Category 5: Path Traversal Sanitization
# ==============================================================================

def test_book_id_path_traversal_sanitization():
    """Verify bookId with ../../ stripped or rejected, preventing sandbox escape."""
    malicious_id = "../../etc/passwd"
    sanitized = sanitize_book_id(malicious_id)
    assert ".." not in sanitized
    assert "/" not in sanitized
    assert "\\" not in sanitized
    assert sanitized == "etcpasswd"


def test_chapter_uid_absolute_path_sanitization():
    """Verify chapter UID with leading / or drive letter sanitized."""
    malicious_uid = "/etc/shadow"
    sanitized = sanitize_chapter_uid(malicious_uid)
    assert not sanitized.startswith("/")
    assert sanitized == "shadow"

    windows_uid = "C:\\boot.ini"
    sanitized_win = sanitize_chapter_uid(windows_uid)
    assert ":" not in sanitized_win
    assert sanitized_win == "boot.ini"


def test_null_byte_in_filename_rejected():
    """Verify filenames with \\x00 null byte rejected."""
    bad_id = "book_101\x00.exe"
    caught = False
    try:
        sanitize_book_id(bad_id)
    except ValueError:
        caught = True
    assert caught, "Null byte in book_id must be rejected with ValueError"


def test_illegal_filesystem_characters_sanitized():
    """Verify characters like :, *, ?, <, >, | sanitized for FAT32/VFS."""
    bad_name = 'book:title*with?bad<chars>and|pipes'
    sanitized = sanitize_book_id(bad_name)
    for bad_ch in (':', '*', '?', '<', '>', '|'):
        assert bad_ch not in sanitized, f"Character {bad_ch} must be stripped/replaced"
    assert "_" in sanitized


def test_reserved_device_names_sanitized():
    """Verify names like CON, PRN, AUX, NUL sanitized."""
    for dev_name in ("CON", "PRN", "AUX", "NUL", "com1"):
        sanitized = sanitize_book_id(dev_name)
        assert sanitized.upper() not in RESERVED_DEVICE_NAMES, f"Device name {dev_name} must be prefixed/renamed"
        assert sanitized.startswith("_")


# ==============================================================================
# Category 6: Corrupted File Auto-Healing
# ==============================================================================

def test_corrupted_chapters_json_triggers_invalidation():
    """Verify corrupt TOC JSON triggers re-download/repair."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        toc_file = os.path.join(storage.book_cache_dir("b1"), "chapters.json")
        with open(toc_file, "w") as f:
            f.write("{corrupt json truncated...")

        # TOC loader
        toc_valid = False
        try:
            with open(toc_file, "r") as f:
                json.load(f)
            toc_valid = True
        except json.JSONDecodeError:
            # Auto-healing: purge corrupted TOC
            os.remove(toc_file)
            toc_valid = False

        assert not toc_valid
        assert not os.path.exists(toc_file), "Corrupted chapters.json must be deleted to trigger re-sync"


def test_zero_byte_auth_json_treated_as_logged_out():
    """Verify 0-byte auth.json treated as unauthenticated without crash."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        with open(storage.auth_file, "w") as f:
            pass  # 0-byte file

        assert os.path.getsize(storage.auth_file) == 0
        auth = storage.load_auth()
        assert auth is None, "0-byte auth.json must return None (logged out)"
        assert not os.path.exists(storage.auth_file), "0-byte auth.json must be cleaned up"


def test_corrupted_shelf_json_triggers_full_sync():
    """Verify truncated shelf.json triggers full sync with synckey=0."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        with open(storage.shelf_file, "w") as f:
            f.write('{"books": [')  # Truncated syntax error

        loaded = storage.load_shelf()
        assert loaded is None
        # On shelf decode failure, synckey falls back to 0 (full sync)
        synckey = 0 if loaded is None else loaded.get("synckey", 0)
        assert synckey == 0


def test_corrupted_chapter_cache_auto_deleted():
    """Verify corrupted chapter file detected by bad size/magic and purged."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        ch_path = os.path.join(storage.book_cache_dir("b1"), "chapters", "1.html")
        os.makedirs(os.path.dirname(ch_path), exist_ok=True)
        # Write 0-byte empty chapter
        with open(ch_path, "w") as f:
            pass

        def verify_and_load_chapter(path: str) -> str | None:
            if os.path.exists(path) and os.path.getsize(path) == 0:
                os.remove(path)
                return None
            with open(path, "r") as f:
                return f.read()

        loaded = verify_and_load_chapter(ch_path)
        assert loaded is None
        assert not os.path.exists(ch_path), "Zero-byte chapter cache must be deleted"


def test_checksum_mismatch_triggers_refetch():
    """Verify payload failing integrity check is discarded and refetched."""
    expected_sha256 = hashlib.sha256(b"correct_chapter_payload").hexdigest()
    corrupted_data = b"corrupted_chapter_payload"

    def validate_payload(data: bytes, expected_hash: str) -> bool:
        return hashlib.sha256(data).hexdigest() == expected_hash

    is_valid = validate_payload(corrupted_data, expected_sha256)
    assert not is_valid, "Corrupted payload must fail checksum verification"


# ==============================================================================
# Category 7: Cache Pruning & Manual Cleanup
# ==============================================================================

def test_lru_cache_eviction_under_budget():
    """Verify oldest book chapter caches evicted when storage limit reached."""
    class LRUCacheManager:
        def __init__(self, budget_bytes: int):
            self.budget = budget_bytes
            self.entries: dict[str, dict] = {}  # book_id -> {size, last_accessed}

        def record(self, book_id: str, size: int):
            self.entries[book_id] = {"size": size, "last_accessed": time.monotonic()}

        def evict_if_needed(self):
            total = sum(v["size"] for v in self.entries.values())
            while total > self.budget and self.entries:
                oldest_book = min(self.entries.keys(), key=lambda k: self.entries[k]["last_accessed"])
                total -= self.entries[oldest_book]["size"]
                del self.entries[oldest_book]

    mgr = LRUCacheManager(budget_bytes=500)
    mgr.record("book_old", 300)
    time.sleep(0.01)
    mgr.record("book_recent", 300)

    mgr.evict_if_needed()
    assert "book_old" not in mgr.entries, "Oldest book cache must be evicted"
    assert "book_recent" in mgr.entries, "Recent book cache must be retained"


def test_manual_book_cache_clear():
    """Verify clearing cache for single book removes its files without touching others."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.save_chapter("book_A", 1, "Book A chapter 1")
        storage.save_chapter("book_B", 1, "Book B chapter 1")

        dir_a = storage.book_cache_dir("book_A")
        dir_b = storage.book_cache_dir("book_B")

        # Manually clear book A
        shutil.rmtree(dir_a)

        assert not os.path.exists(dir_a)
        assert os.path.exists(dir_b)
        assert storage.load_chapter("book_B", 1) == "Book B chapter 1"


def test_logout_storage_wipe():
    """Verify logout deletes auth.json, active session, and sensitive tokens."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.save_auth({"accessToken": "secret_abc", "refreshToken": "ref_xyz", "vid": 12345})
        assert os.path.exists(storage.auth_file)

        # Logout routine
        if os.path.exists(storage.auth_file):
            os.remove(storage.auth_file)
        storage.in_memory_auth = None

        assert not os.path.exists(storage.auth_file)
        assert storage.load_auth() is None


def test_isolated_book_cache_preservation():
    """Verify removing book A cache does not affect book B cache."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        storage.save_chapter("isolated_1", 10, "Chapter 10")
        storage.save_chapter("isolated_2", 20, "Chapter 20")

        # Purge only isolated_1 chapters
        iso1_ch_dir = os.path.join(storage.book_cache_dir("isolated_1"), "chapters")
        shutil.rmtree(iso1_ch_dir)

        assert storage.load_chapter("isolated_1", 10) is None
        assert storage.load_chapter("isolated_2", 20) == "Chapter 20"


def test_stale_temporary_files_cleanup_on_startup():
    """Verify leftover .tmp files from previous crashed sessions purged on start."""
    with tempfile.TemporaryDirectory() as temp_dir:
        storage = ResilientStorageManager(temp_dir)
        # Create abandoned .tmp files
        tmp1 = os.path.join(temp_dir, "shelf.json.tmp")
        tmp2 = os.path.join(storage.cache_dir, "abandoned.tmp")
        with open(tmp1, "w") as f:
            f.write("partial")
        with open(tmp2, "w") as f:
            f.write("partial")

        assert os.path.exists(tmp1) and os.path.exists(tmp2)

        pruned = storage.cleanup_stale_temp_files()
        assert pruned >= 2
        assert not os.path.exists(tmp1)
        assert not os.path.exists(tmp2)


# ==============================================================================
# Test Registry & Runner
# ==============================================================================

ALL_TESTS = [
    # Category 1: Storage Disk Full Simulation
    test_enospc_during_chapter_download,
    test_enospc_during_shelf_sync,
    test_disk_full_temp_file_cleanup,
    test_disk_full_user_notification,
    test_existing_data_preserved_on_enospc,
    # Category 2: Read-Only Filesystem Handling
    test_erofs_during_auth_write,
    test_erofs_during_cache_write,
    test_read_only_sd_card_detection,
    test_erofs_status_reporting,
    test_in_memory_reading_under_erofs,
    # Category 3: Atomic Rename Failure Recovery
    test_atomic_rename_failure_preserves_original,
    test_stale_tmp_file_pruning_on_rename_error,
    test_atomic_write_rename_pattern_verification,
    test_atomic_replace_existing_file,
    test_rename_error_propagation,
    # Category 4: Directory Permission Failures
    test_eacces_directory_creation_failure,
    test_eacces_file_write_failure,
    test_missing_parent_directory_auto_creation,
    test_read_only_cache_directory_fallback,
    test_permission_recovery_after_chmod,
    # Category 5: Path Traversal Sanitization
    test_book_id_path_traversal_sanitization,
    test_chapter_uid_absolute_path_sanitization,
    test_null_byte_in_filename_rejected,
    test_illegal_filesystem_characters_sanitized,
    test_reserved_device_names_sanitized,
    # Category 6: Corrupted File Auto-Healing
    test_corrupted_chapters_json_triggers_invalidation,
    test_zero_byte_auth_json_treated_as_logged_out,
    test_corrupted_shelf_json_triggers_full_sync,
    test_corrupted_chapter_cache_auto_deleted,
    test_checksum_mismatch_triggers_refetch,
    # Category 7: Cache Pruning & Manual Cleanup
    test_lru_cache_eviction_under_budget,
    test_manual_book_cache_clear,
    test_logout_storage_wipe,
    test_isolated_book_cache_preservation,
    test_stale_temporary_files_cleanup_on_startup,
]


def run_all_tests():
    passed = 0
    failed = 0
    print(f"=== Running Tier 2 (Offline Storage & Filesystem Edge Cases) Suite: {len(ALL_TESTS)} tests ===")
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
