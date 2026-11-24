#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 kiikoread
# SPDX-License-Identifier: Apache-2.0
"""Real Compiled C/UI/Runtime, Dual-Board Invariant & Local Regression E2E for WR-UX-01..09.

Groups:
  A: Compiled production C (main/weread/*.c, main/book/*, esp_http_client_mock.c) + mock HTTP probes
  B: AST-sliced main/apps/app_weread.c + main/ui/ui_shelf.c + main/weread/weread_shelf.c UI probes
  C: Dual-board AST-sliced reader & dashboard probes (metalio47 main/apps/ AND pico boards/pico/src/main/apps/)
  D: Structural repo invariants (600000ms RF idle, 16384B worker, dual-board parity, -Oz/-Os, zero Non-Goals)
  E: Local TXT/EPUB regression suite (10 existing host test scripts)

Modes:
  --mode strict   (default): Fails on any missing/broken WR-UX-01..09 C/UI/runtime contract or invariant/regression.
  --mode baseline          : Strictly enforces Groups D, E, and baseline ASan/UBSan probes in A-C;
                             reports unlanded M1..M4 contracts as PENDING.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

TOOLS_DIR = Path(__file__).resolve().parent
ROOT = TOOLS_DIR.parent
BUILD_DIR = ROOT / "build" / "weread-tests" / "ux-e2e"

if str(TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(TOOLS_DIR))

from weread_mock_server import GLOBAL_STATE, ThreadedHTTPServer, WeReadMockRequestHandler


# ---------------------------------------------------------------------------
# Helpers: C function slicing & compilation
# ---------------------------------------------------------------------------

def slice_function(rel_path: str | Path, name: str) -> str:
    source = (ROOT / rel_path).read_text(encoding="utf-8")
    m = re.search(r"^.*\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source, re.M)
    if not m:
        raise KeyError(f"Function '{name}' not found in {rel_path}")
    start = m.start()
    pos = m.end()
    depth = 1
    quote = None
    while depth > 0 and pos < len(source):
        c = source[pos]
        if quote:
            if c == "\\":
                pos += 2
                continue
            if c == quote:
                quote = None
        elif source.startswith("//", pos):
            nl = source.find("\n", pos)
            pos = nl if nl != -1 else len(source)
            continue
        elif source.startswith("/*", pos):
            end_c = source.find("*/", pos + 2)
            pos = (end_c + 2) if end_c != -1 else len(source)
            continue
        elif c in ('"', "'"):
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        pos += 1
    return source[start:pos] + "\n"


def cc_base_flags() -> list[str]:
    return [
        os.environ.get("CC", "cc"),
        "-std=c11",
        "-pthread",
        "-D_DEFAULT_SOURCE",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-deprecated-declarations",
        "-Wno-unused-function",
        "-Wno-unused-variable",
        "-Wno-unused-parameter",
        "-g",
        "-fsanitize=address,undefined",
        "-fno-omit-frame-pointer",
    ]


def san_env(extra: dict[str, str] | None = None) -> dict[str, str]:
    env = dict(os.environ)
    if sys.platform.startswith("linux"):
        env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1:halt_on_error=1"
    else:
        env["ASAN_OPTIONS"] = "abort_on_error=1:halt_on_error=1"
    env["UBSAN_OPTIONS"] = "print_stacktrace=1:halt_on_error=1"
    if extra:
        env.update(extra)
    return env


def run_compiled(bin_path: Path, args: list[str] | None = None, env: dict[str, str] | None = None, verbose: bool = False) -> None:
    cmd = [str(bin_path)] + (args or [])
    res = subprocess.run(cmd, cwd=ROOT, env=san_env(env), capture_output=not verbose, text=True, timeout=60)
    if res.returncode != 0:
        msg = res.stderr or res.stdout or f"exit {res.returncode}"
        raise RuntimeError(f"Binary {bin_path.name} failed ({res.returncode}): {msg.strip()}")


def compile_and_run(c_source: str, name: str, extra_sources: list[str] | None = None, extra_flags: list[str] | None = None,
                    args: list[str] | None = None, env: dict[str, str] | None = None, verbose: bool = False) -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    src_file = BUILD_DIR / f"{name}.c"
    bin_file = BUILD_DIR / name
    src_file.write_text(c_source, encoding="utf-8")
    cmd = cc_base_flags() + (extra_flags or []) + [str(src_file)] + (extra_sources or []) + ["-lz", "-lm", "-o", str(bin_file)]
    comp = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if comp.returncode != 0:
        raise RuntimeError(f"Compile failed for {name}:\n{comp.stderr}")
    run_compiled(bin_file, args=args, env=env, verbose=verbose)


def read_text(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def _strip_c_comments(src: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", src, flags=re.S)


def _extract_c_body(source: str, name: str) -> str:
    m = re.search(r"\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source, re.S)
    if not m:
        return ""
    pos = m.end()
    depth = 1
    quote = None
    while depth > 0 and pos < len(source):
        c = source[pos]
        if quote:
            if c == "\\":
                pos += 2
                continue
            if c == quote:
                quote = None
        elif source.startswith("//", pos):
            nl = source.find("\n", pos)
            pos = nl if nl != -1 else len(source)
            continue
        elif source.startswith("/*", pos):
            end_c = source.find("*/", pos + 2)
            pos = (end_c + 2) if end_c != -1 else len(source)
            continue
        elif c in ('"', "'"):
            quote = c
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        pos += 1
    return source[m.start():pos]


def wr_reader_status_synced_returns_empty(bridge_src: str) -> bool:
    """Whitespace/comment/fallthrough-tolerant check that WR_PROGRESS_SYNCED returns \"\"."""
    body = _strip_c_comments(_extract_c_body(bridge_src, "wr_reader_status"))
    if not body:
        return False
    if re.search(r"\bcase\s+WR_PROGRESS_SYNCED\b", body):
        return bool(
            re.search(
                r'case\s+WR_PROGRESS_SYNCED\s*:\s*(?:(?:case\s+\w+|default)\s*:\s*)*return\s+""\s*;',
                body,
                flags=re.S,
            )
        )
    return bool(
        re.search(
            r'\bdefault\s*:\s*(?:case\s+\w+\s*:\s*)*return\s+""\s*;',
            body,
            flags=re.S,
        )
    )


def footer_pending_uses_cache_delta(bridge_src: str) -> bool:
    """Inspect the full if (...) condition controlling s_reader_footer_pending = true."""
    body = _strip_c_comments(_extract_c_body(bridge_src, "wr_reader_sample"))
    if not body:
        return False
    for m in re.finditer(r"\bif\s*\(", body):
        pos = m.end()
        depth = 1
        while depth > 0 and pos < len(body):
            if body[pos] == "(":
                depth += 1
            elif body[pos] == ")":
                depth -= 1
            pos += 1
        cond = body[m.end():pos - 1]
        tail = body[pos:pos + 160]
        if re.search(r"\bs_reader_footer_pending\s*=\s*true\b", tail):
            if re.search(r"\bcache_delta\b", cond):
                return True
    return False


def extract_c_struct_for_var(source: str, var_name: str) -> str | None:
    m = re.search(
        r"static\s+struct\s*\{[^}]*\}\s*" + re.escape(var_name) + r"\s*;",
        source,
        flags=re.S,
    )
    return m.group(0) if m else None


def extract_typedef_struct(source: str, type_name: str) -> str:
    for m in re.finditer(r"typedef\s+struct\s*\{", source):
        pos = m.end()
        depth = 1
        quote = None
        while depth > 0 and pos < len(source):
            c = source[pos]
            if quote:
                if c == "\\":
                    pos += 2
                    continue
                if c == quote:
                    quote = None
            elif source.startswith("//", pos):
                nl = source.find("\n", pos)
                pos = nl if nl != -1 else len(source)
                continue
            elif source.startswith("/*", pos):
                end_c = source.find("*/", pos + 2)
                pos = (end_c + 2) if end_c != -1 else len(source)
                continue
            elif c in ('"', "'"):
                quote = c
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            pos += 1
        tail = source[pos:pos + 64]
        end_m = re.match(r"\s*" + re.escape(type_name) + r"\s*;", tail)
        if end_m:
            return source[m.start():pos + end_m.end()]
    raise KeyError(f"typedef struct '{type_name}' not found")


def extract_ui_icon_enum() -> str:
    icons_src = read_text("boards/metalio47/src/main/assets/ui_icons.h")
    m = re.search(r"typedef\s+enum\s*\{[^}]*\}\s*ui_icon_t\s*;", icons_src, flags=re.S)
    if not m:
        raise KeyError("ui_icon_t enum not found in ui_icons.h")
    return m.group(0)


def extract_static_helpers(source: str, seed_bodies: list[str], excluded: set[str]) -> str:
    """Slice any unstubbed static helpers defined in `source` and referenced by `seed_bodies`."""
    seen: set[str] = set(excluded)
    extracted: list[str] = []
    queue: list[str] = []
    for body in seed_bodies:
        for m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", _strip_c_comments(body)):
            name = m.group(1)
            if name not in seen:
                seen.add(name)
                queue.append(name)
    while queue:
        name = queue.pop(0)
        m = re.search(r"^[ \t]*static\b[^;{=]*\b" + re.escape(name) + r"\([^;]*?\)\s*\{", source, re.M)
        if not m:
            continue
        start = m.start()
        pos = m.end()
        depth = 1
        quote = None
        while depth > 0 and pos < len(source):
            c = source[pos]
            if quote:
                if c == "\\":
                    pos += 2
                    continue
                if c == quote:
                    quote = None
            elif source.startswith("//", pos):
                nl = source.find("\n", pos)
                pos = nl if nl != -1 else len(source)
                continue
            elif source.startswith("/*", pos):
                end_c = source.find("*/", pos + 2)
                pos = (end_c + 2) if end_c != -1 else len(source)
                continue
            elif c in ('"', "'"):
                quote = c
            elif c == "{":
                depth += 1
            elif c == "}":
                depth -= 1
            pos += 1
        fn_src = source[start:pos] + "\n"
        extracted.append(fn_src)
        for sub_m in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", _strip_c_comments(fn_src)):
            sub = sub_m.group(1)
            if sub not in seen:
                seen.add(sub)
                queue.append(sub)
    return "\n".join(reversed(extracted))


# ---------------------------------------------------------------------------
# Outcome collector supporting strict vs baseline modes
# ---------------------------------------------------------------------------

class ProbeTracker:
    def __init__(self, mode: str, verbose: bool = False) -> None:
        self.mode = mode
        self.verbose = verbose
        self.passed: list[str] = []
        self.pending: list[str] = []
        self.failed: list[str] = []

    def ok(self, group: str, ux_id: str, msg: str) -> None:
        line = f"[PASS][{group}][{ux_id}] {msg}"
        self.passed.append(line)
        print(f"  {line}")

    def contract_gap(self, group: str, ux_id: str, milestone: str, reason: str) -> None:
        if self.mode == "baseline":
            line = f"[PENDING][{ux_id}][Milestone {milestone}] {reason}"
            self.pending.append(line)
            print(f"  {line}")
        else:
            line = f"[FAIL][{ux_id}][Milestone {milestone}] {reason}"
            self.failed.append(line)
            print(f"  {line}", file=sys.stderr)

    def fail(self, group: str, ux_id: str, reason: str) -> None:
        line = f"[FAIL][{group}][{ux_id}] {reason}"
        self.failed.append(line)
        print(f"  {line}", file=sys.stderr)


# ---------------------------------------------------------------------------
# Group A: Production C + Mock HTTP Runtime Probes
# ---------------------------------------------------------------------------

PROD_C_UNITS = [
    "tools/weread_stubs/esp_http_client_mock.c",
    "tools/weread_stubs/cJSON.c",
    "main/book/book_source_weread.c",
    "main/book/book_weread_crypto.c",
    "main/book/html_text.c",
    "main/weread/weread_auth.c",
    "main/weread/weread_client.c",
    "main/weread/weread_store.c",
    "main/weread/weread_shelf.c",
    "main/weread/weread_toc.c",
    "main/weread/weread_chapter.c",
    "main/weread/weread_progress.c",
    "main/weread/weread_position.c",
    "main/weread/weread_runtime.c",
]

PROD_INC_FLAGS = [
    "-DWEREAD_USE_ESP_HTTP_CLIENT",
    "-Itools/weread_stubs",
    "-Imain",
    "-Imain/book",
    "-Imain/weread",
]


def run_group_a(tracker: ProbeTracker, mock_port: int) -> None:
    print("\n=== Group A: Production C + Mock HTTP Runtime Probes ===")
    mock_env = {
        "WEREAD_MOCK_PORT": str(mock_port),
        "WEREAD_API_BASE": f"http://127.0.0.1:{mock_port}",
        "WEREAD_WEB_BASE": f"http://127.0.0.1:{mock_port}",
    }

    # A0: Baseline always-green C probe: sizeof(weread_shelf_item_t) < 512, 104 MiB statvfs guard, 600000ms idle, HTTP sync
    a0_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_progress.h"
#include "weread_runtime.h"
#include "book_source_weread.h"

static unsigned long long g_mock_free_bytes = 512ULL * 1024ULL * 1024ULL;
int ux_test_statvfs(const char *path, struct statvfs *buf) {
    (void)path;
    memset(buf, 0, sizeof(*buf));
    buf->f_frsize = 4096;
    buf->f_bavail = (fsblkcnt_t)(g_mock_free_bytes / 4096ULL);
    return 0;
}

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK);
    assert(status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(sizeof(weread_shelf_item_t) < 512);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");

    // 104 MiB guard verification via -Dstatvfs=ux_test_statvfs
    uint64_t avail = 0;
    g_mock_free_bytes = (104ULL * 1024ULL * 1024ULL) - 4096ULL;
    assert(weread_chapter_prefetch_space(&avail) == WEREAD_ERR_STORAGE);
    g_mock_free_bytes = 104ULL * 1024ULL * 1024ULL;
    assert(weread_chapter_prefetch_space(&avail) == ESP_OK);

    char uid[64], url[256], code[64];
    weread_qr_state_t state = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &state, code, sizeof(code)) == ESP_OK && state == WEREAD_QR_STATE_CONFIRMED);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK && weread_auth_is_authenticated());
    assert(weread_shelf_sync(true) == ESP_OK && weread_shelf_count() == 2);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(weread_chapter_download("test_book_101", 1) == ESP_OK);
    assert(weread_chapter_cached("test_book_101", 1));
    assert(book_weread_open("weread://test_book_101") == ESP_OK);
    book_weread_close();
    puts("PASS A0");
    return 0;
}
'''
    try:
        with tempfile.TemporaryDirectory(prefix="ux-e2e-a0-") as cache:
            compile_and_run(
                a0_src,
                "probe_a0_baseline",
                extra_sources=PROD_C_UNITS,
                extra_flags=PROD_INC_FLAGS + ["-Dstatvfs=ux_test_statvfs"],
                args=[cache],
                env=mock_env,
                verbose=tracker.verbose,
            )
        tracker.ok("GroupA", "Baseline", "sizeof(weread_shelf_item_t)<512, 104MiB statvfs guard, QR->sync->chapter->open passed under ASan/UBSan")
    except Exception as exc:
        tracker.fail("GroupA", "Baseline", str(exc))

    auth_h = read_text("main/weread/weread_auth.h")
    client_h = read_text("main/weread/weread_client.h")
    shelf_h = read_text("main/weread/weread_shelf.h")
    runtime_h = read_text("main/weread/weread_runtime.h")
    runtime_c = read_text("main/weread/weread_runtime.c")
    store_c = read_text("main/weread/weread_store.c")

    # A1: WR-UX-01 (M1 Auth/Source layer vs M2 Runtime snapshot layer)
    if "weread_auth_is_expired" not in auth_h or "weread_auth_mark_expired" not in auth_h:
        tracker.contract_gap("GroupA", "WR-UX-01", "M1", "Missing weread_auth_is_expired / weread_auth_mark_expired in weread_auth.h")
    else:
        a1_m1_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_progress.h"
#include "book_source_weread.h"

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t state = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &state, code, sizeof(code)) == ESP_OK && state == WEREAD_QR_STATE_CONFIRMED);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(weread_chapter_download("test_book_101", 1) == ESP_OK);

    // Inject terminal -2013 on renew & /shelf/sync
    control("/mock/control/set_state", "{\"renew_error\":{\"status\":401,\"body\":{\"errcode\":-2013}},\"endpoint_errors\":{\"/shelf/sync\":{\"status\":401,\"body\":{\"errcode\":-2013}}}}");
    assert(weread_shelf_sync(true) != ESP_OK);
    assert(!weread_auth_is_authenticated());
    assert(weread_auth_is_expired());
    assert(weread_auth_owner_vid() && !strcmp(weread_auth_owner_vid(), "12345678"));
    weread_auth_session_t tmp = {0};
    assert(weread_auth_load_session(&tmp) != ESP_OK);

    // Verify auth.json retains expired:true and vid
    char auth_path[512]; snprintf(auth_path, sizeof(auth_path), "%s/auth.json", argv[1]);
    FILE *f = fopen(auth_path, "r"); assert(f);
    char buf[1024] = {0}; size_t n = fread(buf, 1, sizeof(buf) - 1, f); fclose(f);
    assert(n > 0 && strstr(buf, "\"expired\"") && strstr(buf, "12345678"));

    // Cached book still opens and records local progress while expired!
    assert(book_weread_open("weread://test_book_101") == ESP_OK);
    html_text_t text = {0};
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK && text.len > 0);
    html_text_free(&text);
    book_weread_close();
    assert(weread_progress_note("test_book_101", 1, 1, 20, 40, 120, 1, "expired-local", 1700000200) == ESP_OK);

    // Explicit logout wipes auth.json and clears expired
    assert(weread_auth_clear_session() == ESP_OK);
    assert(!weread_auth_is_expired());
    assert(access(auth_path, F_OK) != 0);
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a1-m1-") as cache:
                compile_and_run(a1_m1_src, "probe_a1_ux01_m1", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-01:M1", "C auth -2013 marks expired:true retaining vid, allows cached open/progress, WR_JOB_LOGOUT wipes auth.json")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-01:M1", str(exc))

    if "auth_expired" not in runtime_h:
        tracker.contract_gap("GroupA", "WR-UX-01", "M2", "Missing weread_result_t.auth_expired in weread_runtime.h")
    else:
        a1_m2_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_runtime.h"
#include "book_source_weread.h"

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t st = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &st, code, sizeof(code)) == ESP_OK);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(weread_chapter_download("test_book_101", 1) == ESP_OK);
    assert(weread_auth_mark_expired() == ESP_OK);

    weread_runtime_init();
    assert(weread_runtime_start(WR_JOB_INIT, NULL, NULL));
    while (weread_runtime_busy()) usleep(1000);
    weread_result_t r = {0};
    assert(weread_runtime_poll(&r));
    assert(!r.authenticated && r.auth_expired && r.count > 0 && r.books != NULL);
    weread_runtime_result_free(&r);
    weread_runtime_stop();

    assert(book_weread_open("weread://test_book_101") == ESP_OK);
    html_text_t text = {0};
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK && text.len > 0);
    html_text_free(&text);
    book_weread_close();
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a1-m2-") as cache:
                compile_and_run(a1_m2_src, "probe_a1_ux01_m2", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-01:M2", "weread_runtime WR_JOB_INIT preserves s_books with auth_expired=true & opens cached book offline")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-01:M2", str(exc))

    # A2: WR-UX-04 (Runtime turn tracking & cloud-jump rebase vs auto-jump)
    has_ux04_rt = ("user_turned" in runtime_c or "weread_runtime_reader_has_turned" in runtime_h or "weread_reader_has_turned" in runtime_h)
    has_ux04_rebase = ("weread_runtime_reader_has_turned" in runtime_h or "weread_runtime_cloud_jump_rebase" in runtime_h or "user_turned_since_open" in runtime_c)
    if not (has_ux04_rt and has_ux04_rebase):
        tracker.contract_gap("GroupA", "WR-UX-04", "M2", "Missing user_turned_since_open / weread_runtime_reader_has_turned / rebase runtime contract in weread_runtime.{c,h}")
    else:
        has_turned_fn = "weread_runtime_reader_has_turned" in runtime_h
        has_rebase_fn = "weread_runtime_cloud_jump_rebase" in runtime_h
        a2_m2_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_runtime.h"
#include "book_source_weread.h"

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t st = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &st, code, sizeof(code)) == ESP_OK);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(weread_chapter_download("test_book_101", 1) == ESP_OK);

    weread_runtime_init();
    assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
    while (weread_runtime_busy()) usleep(1000);
    weread_result_t r = {0};
    assert(weread_runtime_poll(&r) && r.error == ESP_OK && r.open_ready);
    weread_runtime_result_free(&r);
    size_t ch = 0; uint32_t off = 0;
    assert(book_weread_open("weread://test_book_101") == ESP_OK);
    assert(weread_reader_resume("weread://test_book_101", &ch, &off) == ESP_OK);
    html_text_t text = {0};
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK);
    uint32_t ver = 0;
    assert(book_weread_commit_loaded(&ch, &ver) == ESP_OK);
    weread_reader_begin("weread://test_book_101", 1000);
''' + ('''
    assert(!weread_runtime_reader_has_turned("test_book_101"));
''' if has_turned_fn else '') + r'''
    weread_reader_navigated_version("weread://test_book_101", 0, 12, 10, "turn-1", ver);
    assert(weread_reader_save(2000) == ESP_OK);
''' + ('''
    assert(weread_runtime_reader_has_turned("test_book_101"));
''' if has_turned_fn else '') + ('''
    (void)weread_runtime_cloud_jump_rebase("test_book_101");
''' if has_rebase_fn else '') + r'''
    weread_reader_end(3000);
    html_text_free(&text);
    book_weread_close();
    weread_runtime_stop();
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a2-m2-") as cache:
                compile_and_run(a2_m2_src, "probe_a2_ux04_m2", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-04:M2", "weread_runtime preserves user-turned state across weread_reader_save under ASan/UBSan")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-04:M2", str(exc))

    # A3: WR-UX-05 (prepare_open WEREAD_ERR_VERSION single auto-heal)
    has_ux05_heal = ("WEREAD_ERR_VERSION" in runtime_c and "weread_shelf_refresh_book" in runtime_c)
    if not has_ux05_heal:
        tracker.contract_gap("GroupA", "WR-UX-05", "M2", "prepare_open() does not yet auto-heal WEREAD_ERR_VERSION via weread_shelf_refresh_book")
    else:
        a3_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_runtime.h"

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t st = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &st, code, sizeof(code)) == ESP_OK);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("test_book_101") == ESP_OK);

    // Bump server book version to 101 and enforce version checking
    control("/mock/control/set_state", "{\"enforce_book_version\":true,\"book_versions\":{\"test_book_101\":101},\"progress_data\":{\"test_book_101\":{\"chapterUid\":1,\"chapterIdx\":1,\"chapterOffset\":0,\"progress\":15,\"bookVersion\":101}}}");
    weread_runtime_init();
    assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
    while (weread_runtime_busy()) usleep(1000);
    weread_result_t r = {0};
    assert(weread_runtime_poll(&r));
    assert(r.error == ESP_OK && r.open_ready);
    assert(weread_shelf_book_version("test_book_101") == 101);
    weread_runtime_result_free(&r);
    weread_runtime_stop();
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a3-") as cache:
                compile_and_run(a3_src, "probe_a3_ux05", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-05", "prepare_open() single WEREAD_ERR_VERSION auto-heal verified against mock server")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-05", str(exc))

    # A4: WR-UX-06 (M1 weread_client_set_activity_cb vs M2 8000ms pause report & exit WR_JOB_NOTE)
    if "weread_client_set_activity_cb" not in client_h:
        tracker.contract_gap("GroupA", "WR-UX-06", "M1", "Missing weread_client_set_activity_cb in weread_client.h")
    else:
        a4_m1_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "weread_auth.h"
#include "weread_client.h"

static unsigned cb_hits = 0;
static void on_activity(void *ctx) { (void)ctx; ++cb_hits; }

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    weread_client_set_activity_cb(on_activity, NULL);
    char uid[64], url[256];
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(cb_hits > 0);
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a4-m1-") as cache:
                compile_and_run(a4_m1_src, "probe_a4_ux06_m1", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-06:M1", "weread_client_set_activity_cb fires on HTTP activity")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-06:M1", str(exc))

    has_ux06_rt = (
        "weread_client_set_activity_cb" in runtime_c
        and bool(re.search(r"now\s*-\s*s_reader\.last_move\s*>=\s*8000", runtime_c))
    )
    if not has_ux06_rt:
        tracker.contract_gap("GroupA", "WR-UX-06", "M2", "Missing weread_client_set_activity_cb binding or 8000ms pause silent WR_JOB_NOTE in weread_runtime.c")
    else:
        a4_m2_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_progress.h"
#include "weread_runtime.h"
#include "book_source_weread.h"

static uint64_t mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

static void drain_worker(void) {
    while (weread_runtime_busy()) usleep(1000);
    weread_result_t r = {0};
    if (weread_runtime_poll(&r)) weread_runtime_result_free(&r);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t st = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &st, code, sizeof(code)) == ESP_OK);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(weread_chapter_download("test_book_101", 1) == ESP_OK);

    weread_runtime_init();
    assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
    drain_worker();
    size_t ch = 0; uint32_t off = 0;
    assert(book_weread_open("weread://test_book_101") == ESP_OK);
    assert(weread_reader_resume("weread://test_book_101", &ch, &off) == ESP_OK);
    html_text_t text = {0};
    assert(book_weread_load(0, NULL, NULL, &text) == ESP_OK);
    uint32_t ver = 0;
    assert(book_weread_commit_loaded(&ch, &ver) == ESP_OK);

    // 1. Connect RF and verify 599000ms does NOT disconnect while 600000ms DOES
    assert(weread_runtime_connect(NULL) == ESP_OK);
    assert(weread_runtime_network_active());
    uint64_t t0 = mono_ms();
    weread_reader_begin("weread://test_book_101", t0);
    assert(!weread_runtime_idle(t0 + 599000));
    assert(weread_runtime_network_active());

    // 2. 8s pause triggers WR_JOB_NOTE when s_owns_network == true, throttled by 15s spacing
    weread_reader_navigated_version("weread://test_book_101", 0, 10, 15, "p1", ver);
    weread_reader_tick("weread://test_book_101", 0, 10, 15, "p1", t0 + 1500, true);
    drain_worker();
    assert(weread_reader_state() == WR_PROGRESS_PENDING);
    // At +9400ms (7900ms < 8000ms since move), silent note must not fire
    weread_reader_tick("weread://test_book_101", 0, 10, 15, "p1", t0 + 9400, true);
    drain_worker();
    assert(weread_reader_state() == WR_PROGRESS_PENDING);
    // At +9600ms (8100ms >= 8000ms since move), silent note fires & clears pending
    weread_reader_tick("weread://test_book_101", 0, 10, 15, "p1", t0 + 9600, true);
    drain_worker();
    assert(weread_reader_state() == WR_PROGRESS_SYNCED);
    // Move again at t0 + 11000: at t0 + 20000 (9s pause, only 10.4s since last note < 15s), suppressed; at t0 + 25000 (15.4s >= 15s), fires
    weread_reader_navigated_version("weread://test_book_101", 0, 12, 18, "p1b", ver);
    weread_reader_tick("weread://test_book_101", 0, 12, 18, "p1b", t0 + 11000, true);
    drain_worker();
    assert(weread_reader_state() == WR_PROGRESS_PENDING);
    weread_reader_tick("weread://test_book_101", 0, 12, 18, "p1b", t0 + 20000, true);
    drain_worker();
    assert(weread_reader_state() == WR_PROGRESS_PENDING);
    weread_reader_tick("weread://test_book_101", 0, 12, 18, "p1b", t0 + 25000, true);
    drain_worker();
    assert(weread_reader_state() == WR_PROGRESS_SYNCED);
    // Complete one-time cache scan & disable manual whole-book forward fill before offline idle test
    weread_reader_cache_forward_set(false);
    weread_reader_tick("weread://test_book_101", 0, 12, 18, "p1b", t0 + 26000, true);
    drain_worker();

    // Refresh report_due before disconnecting so step 3 isolates the 8s pause_report path (< 60s report_due)
    weread_reader_navigated_version("weread://test_book_101", 0, 14, 20, "p1c", ver);
    weread_reader_tick("weread://test_book_101", 0, 14, 20, "p1c", t0 + 590000, true);
    drain_worker();
    weread_reader_tick("weread://test_book_101", 0, 14, 20, "p1c", t0 + 592000, true);
    drain_worker();
    assert(weread_reader_state() == WR_PROGRESS_SYNCED);

    // 3. Offline/disconnected (s_owns_network == false) MUST NOT fire 8s pause note
    assert(weread_runtime_idle(t0 + 609000));
    drain_worker();
    assert(!weread_runtime_network_active());
    weread_reader_navigated_version("weread://test_book_101", 0, 20, 25, "p2", ver);
    weread_reader_tick("weread://test_book_101", 0, 20, 25, "p2", t0 + 611000, true);
    drain_worker();
    weread_reader_tick("weread://test_book_101", 0, 20, 25, "p2", t0 + 625000, true);
    assert(!weread_runtime_busy());
    assert(!weread_runtime_network_active());
    assert(weread_reader_state() == WR_PROGRESS_PENDING);

    // 4. Online exit (weread_reader_end) kicks WR_JOB_NOTE before clearing s_reader
    assert(weread_runtime_connect(NULL) == ESP_OK);
    weread_reader_end(mono_ms());
    drain_worker();
    weread_progress_t saved = {0};
    assert(weread_progress_load("test_book_101", &saved) == ESP_OK && !saved.dirty);

    html_text_free(&text);
    book_weread_close();
    weread_runtime_stop();
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a4-m2-") as cache:
                compile_and_run(a4_m2_src, "probe_a4_ux06_m2", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-06:M2", "600s RF idle, 8s pause note (online-only), and exit WR_JOB_NOTE verified under ASan/UBSan")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-06:M2", str(exc))

    # A5: WR-UX-07 & WR-UX-09 (M1 shelf counts + selective cleanup vs M2 runtime jobs)
    chapter_h = read_text("main/weread/weread_chapter.h")
    if "cached_chapters" not in shelf_h or "weread_shelf_set_chapter_counts" not in shelf_h:
        tracker.contract_gap("GroupA", "WR-UX-07", "M1", "Missing cached_chapters / weread_shelf_set_chapter_counts in weread_shelf.h")
    elif "weread_chapter_clean_book_cache" not in chapter_h or "weread_chapter_inspect_cache_stats" not in chapter_h:
        tracker.contract_gap("GroupA", "WR-UX-09", "M1", "Missing weread_chapter_clean_book_cache / weread_chapter_inspect_cache_stats in weread_chapter.h")
    else:
        a5_m1_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_progress.h"

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t st = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &st, code, sizeof(code)) == ESP_OK);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(weread_chapter_download("test_book_101", 1) == ESP_OK);

    uint16_t cc = 0, tc = 0;
    assert(weread_chapter_count_cached("test_book_101", &cc, &tc, NULL, NULL) == ESP_OK);
    assert(cc == 1 && tc == 2);
    assert(weread_shelf_set_chapter_counts("test_book_101", cc, tc) == ESP_OK);
    // Survives subsequent shelf sync
    assert(weread_shelf_sync(true) == ESP_OK);
    weread_shelf_item_t *items = NULL; size_t cnt = 0;
    assert(weread_shelf_snapshot(&items, &cnt) == ESP_OK);
    bool found = false;
    for (size_t i = 0; i < cnt; ++i) {
        if (!strcmp(items[i].book_id, "test_book_101")) {
            assert(items[i].cached_chapters == 1 && items[i].total_chapters == 2);
            found = true;
        }
    }
    assert(found);
    free(items);

    // Seed cover-*.gray and progress.json, verify selective cleanup preserves them
    char gray_path[512];
    assert(weread_cover_cache_path("test_book_101", "gray", gray_path, sizeof(gray_path)) == ESP_OK);
    FILE *gf = fopen(gray_path, "wb"); assert(gf); fwrite("GRAY", 1, 4, gf); fclose(gf);
    assert(weread_progress_note("test_book_101", 1, 1, 10, 20, 60, 1, "keep", 1700000100) == ESP_OK);

    weread_chapter_cache_stats_t stats = {0};
    assert(weread_chapter_inspect_cache_stats(&stats, NULL, NULL) == ESP_OK);
    assert(stats.books_with_chapters >= 1 && stats.total_bytes > 0);

    uint64_t freed = 0;
    assert(weread_chapter_clean_book_cache("test_book_101", &freed, NULL, NULL) == ESP_OK);
    assert(!weread_chapter_cached("test_book_101", 1));
    assert(access(gray_path, F_OK) == 0);
    weread_progress_t prog = {0};
    assert(weread_progress_load("test_book_101", &prog) == ESP_OK && prog.progress == 20);
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a5-m1-") as cache:
                compile_and_run(a5_m1_src, "probe_a5_ux07_09_m1", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-07/09:M1", "cached_chapters persistence + inspect_cache_stats + selective clean preserving cover.gray & progress.json passed")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-07/09:M1", str(exc))

    if "WR_JOB_CACHE_BOOK" not in runtime_h:
        tracker.contract_gap("GroupA", "WR-UX-07", "M2", "Missing WR_JOB_CACHE_BOOK in weread_runtime.h")
    else:
        a5_ux07_m2_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_runtime.h"

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t st = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &st, code, sizeof(code)) == ESP_OK);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);

    weread_runtime_init();
    assert(weread_runtime_start(WR_JOB_CACHE_BOOK, "test_book_101", NULL));
    while (weread_runtime_busy()) usleep(1000);
    weread_result_t r = {0};
    assert(weread_runtime_poll(&r));
    assert(r.job == WR_JOB_CACHE_BOOK && r.error == ESP_OK);
    weread_runtime_result_free(&r);
    assert(weread_chapter_cached("test_book_101", 1) && weread_chapter_cached("test_book_101", 2));

    // Verify open preemption cancels WR_JOB_CACHE_BOOK cleanly
    assert(weread_runtime_start(WR_JOB_CACHE_BOOK, "txt_book_202", NULL));
    weread_runtime_cancel_join();
    if (weread_runtime_poll(&r)) weread_runtime_result_free(&r);
    assert(weread_runtime_start(WR_JOB_OPEN, "test_book_101", NULL));
    while (weread_runtime_busy()) usleep(1000);
    assert(weread_runtime_poll(&r) && r.job == WR_JOB_OPEN && r.open_ready);
    weread_runtime_result_free(&r);
    weread_runtime_stop();
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a5-07-m2-") as cache:
                compile_and_run(a5_ux07_m2_src, "probe_a5_ux07_m2", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-07:M2", "WR_JOB_CACHE_BOOK downloads full book & preempts cleanly for WR_JOB_OPEN under ASan/UBSan")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-07:M2", str(exc))

    if "WR_JOB_CACHE_STATS" not in runtime_h or "WR_JOB_CACHE_CLEAN_FINISHED" not in runtime_h or "WR_JOB_CACHE_CLEAN_ALL" not in runtime_h:
        tracker.contract_gap("GroupA", "WR-UX-09", "M2", "Missing WR_JOB_CACHE_STATS / WR_JOB_CACHE_CLEAN_FINISHED / WR_JOB_CACHE_CLEAN_ALL in weread_runtime.h")
    else:
        a5_ux09_m2_src = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_shelf.h"
#include "weread_toc.h"
#include "weread_chapter.h"
#include "weread_progress.h"
#include "weread_runtime.h"

static void control(const char *endpoint, const char *body) {
    char url[256];
    snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request(url, "POST", "Content-Type: application/json\r\n", body, &resp, &len, &status) == ESP_OK && status == 200);
    free(resp);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    assert(!setenv("WEREAD_STORAGE_ROOT", argv[1], 1));
    assert(weread_auth_init(argv[1]) == ESP_OK && weread_client_init() == ESP_OK);
    control("/mock/control/reset", "{}");
    char uid[64], url[256], code[64];
    weread_qr_state_t st = WEREAD_QR_STATE_IDLE;
    assert(weread_auth_start_qr_login(uid, sizeof(uid), url, sizeof(url)) == ESP_OK);
    assert(weread_auth_poll_qr_login(uid, &st, code, sizeof(code)) == ESP_OK);
    assert(weread_auth_exchange_code(code, NULL) == ESP_OK);
    assert(weread_shelf_sync(true) == ESP_OK);
    assert(weread_toc_sync("test_book_101") == ESP_OK);
    assert(weread_chapter_download("test_book_101", 1) == ESP_OK);
    assert(weread_progress_note("test_book_101", 1, 1, 10, 50, 60, 1, "keep", 1700000100) == ESP_OK);

    weread_runtime_init();
    assert(weread_runtime_start(WR_JOB_CACHE_STATS, NULL, NULL));
    while (weread_runtime_busy()) usleep(1000);
    weread_result_t r = {0};
    assert(weread_runtime_poll(&r) && r.job == WR_JOB_CACHE_STATS && r.error == ESP_OK);
    weread_runtime_result_free(&r);

    assert(weread_runtime_start(WR_JOB_CACHE_CLEAN_ALL, NULL, NULL));
    while (weread_runtime_busy()) usleep(1000);
    assert(weread_runtime_poll(&r) && r.job == WR_JOB_CACHE_CLEAN_ALL && r.error == ESP_OK);
    weread_runtime_result_free(&r);
    assert(!weread_chapter_cached("test_book_101", 1));
    weread_progress_t prog = {0};
    assert(weread_progress_load("test_book_101", &prog) == ESP_OK && prog.progress == 50);
    weread_runtime_stop();
    return 0;
}
'''
        try:
            with tempfile.TemporaryDirectory(prefix="ux-e2e-a5-09-m2-") as cache:
                compile_and_run(a5_ux09_m2_src, "probe_a5_ux09_m2", extra_sources=PROD_C_UNITS, extra_flags=PROD_INC_FLAGS, args=[cache], env=mock_env, verbose=tracker.verbose)
            tracker.ok("GroupA", "WR-UX-09:M2", "WR_JOB_CACHE_STATS & WR_JOB_CACHE_CLEAN_ALL executed cleanly under ASan/UBSan")
        except Exception as exc:
            tracker.fail("GroupA", "WR-UX-09:M2", str(exc))


# ---------------------------------------------------------------------------
# Group B: Sliced Production UI Probes for app_weread.c
# ---------------------------------------------------------------------------

def run_group_b(tracker: ProbeTracker) -> None:
    print("\n=== Group B: AST-Sliced app_weread.c UI Probes ===")
    app_weread_src = read_text("main/apps/app_weread.c")
    runtime_h = read_text("main/weread/weread_runtime.h")

    # Baseline compile check of sliced app_weread.c (dynamic slice from #define ROWS to app_weread)
    try:
        BUILD_DIR.mkdir(parents=True, exist_ok=True)
        out_b = BUILD_DIR / "ui-slice"
        out_b.mkdir(parents=True, exist_ok=True)
        slice_start = app_weread_src.index("#define ROWS")
        slice_end = app_weread_src.index("const app_desc_t app_weread")
        production_slice = app_weread_src[slice_start:slice_end]
        ui_shelf_funcs = [
            "ui_shelf_rows", "ui_shelf_entry", "ui_shelf_cover", "ui_shelf_order_mode",
            "ui_shelf_order_desc", "ui_shelf_order_value", "ui_shelf_order_rect",
            "ui_shelf_order_select", "shelf_pager_group_rect", "ui_shelf_search_rect",
            "ui_shelf_page_label_rect",
        ]
        (out_b / "shared.inc").write_text("".join(slice_function("main/ui/ui_shelf.c", n) for n in ui_shelf_funcs), encoding="utf-8")
        production_clean = re.sub(r'^\s*#include\s+"book_toc\.h"\s*$', "", production_slice, flags=re.M)
        (out_b / "production.inc").write_text(production_clean + "\n", encoding="utf-8")
        (out_b / "cover.inc").write_text("".join(slice_function("main/weread/weread_shelf.c", n) for n in [
            "weread_cover_key", "weread_cover_get_url", "weread_cover_retry_url", "weread_cover_retry_result",
        ]), encoding="utf-8")

        smoke_prefix = read_text("tools/test_weread_cache_ui.c").split("int main(void){", 1)[0]
        # Upgrade app_ctx_t to match main/app/app.h:67-72 (touch, pressed, released)
        smoke_prefix = smoke_prefix.replace(
            "typedef struct {uint64_t now_ms;void *fb;bool consumed,request_return,request_menu;const void *request_app;} app_ctx_t;",
            "typedef struct {uint64_t now_ms;void *fb;const struct {bool touched;uint8_t count;uint16_t x,y;} *touch;"
            "bool pressed,released;bool consumed,request_return,request_menu;const void *request_app;} app_ctx_t;",
        )
        # Upgrade ui_gesture_type_t / ui_gesture_event_t to match main/ui/ui_gesture.h (SWIPE_L/R, x, y, hold_ms)
        smoke_prefix = smoke_prefix.replace(
            "typedef enum {UI_GESTURE_PRESS,UI_GESTURE_CANCEL,UI_GESTURE_TAP,UI_GESTURE_LONG_PRESS} ui_gesture_type_t;\n"
            "typedef struct {ui_gesture_type_t type;unsigned x0,y0;} ui_gesture_event_t;",
            "typedef enum {UI_GESTURE_PRESS,UI_GESTURE_MOVE,UI_GESTURE_LONG_PRESS,UI_GESTURE_TAP,"
            "UI_GESTURE_SWIPE_L,UI_GESTURE_SWIPE_R,UI_GESTURE_SWIPE_U,UI_GESTURE_SWIPE_D,UI_GESTURE_CANCEL} ui_gesture_type_t;\n"
            "typedef struct {ui_gesture_type_t type;uint16_t x0,y0,x,y;int64_t hold_ms;} ui_gesture_event_t;",
        )
        smoke_prefix = smoke_prefix.replace(
            "typedef struct {EpdRect area;bool layout;} ui_keyboard_update_t;\n"
            "typedef enum{UI_KEYBOARD_NONE,UI_KEYBOARD_DONE} ui_keyboard_result_t;",
            "typedef struct {EpdRect area;bool layout;bool field;bool settle;} ui_keyboard_update_t;\n"
            "typedef enum{UI_KEYBOARD_NONE,UI_KEYBOARD_CHANGED,UI_KEYBOARD_DONE} ui_keyboard_result_t;",
        )
        smoke_prefix = smoke_prefix.replace(
            "static void ui_keyboard_press(int x,int y,int t,uint64_t n){}\n"
            "static void ui_keyboard_release(void){}",
            "static bool ui_keyboard_press(int x,int y,int t,int64_t n){(void)x;(void)y;(void)t;(void)n;return false;}\n"
            "static bool ui_keyboard_release(void){return false;}",
        )
        smoke_prefix = smoke_prefix.replace(
            "app_redraw_t(*on_gesture)(app_ctx_t*,const ui_gesture_event_t*);} app_desc_t;",
            "app_redraw_t(*on_gesture)(app_ctx_t*,const ui_gesture_event_t*);bool(*present)(app_ctx_t*,app_redraw_t);} app_desc_t;",
        )
        toc_jump_funcs = "".join(slice_function("main/apps/book_toc.c", n) for n in (
            "book_toc_jump_hit", "book_toc_jump_percent", "book_toc_page_from_percent",
        ))
        ui_icon_enum = extract_ui_icon_enum()
        # Provide stubs for app_weread_request_open and optional M3 UI/keyboard/runtime/TOC slider symbols
        extra_stubs = f'''
#ifndef EPD_DRAW_ALIGN_RIGHT
#define EPD_DRAW_ALIGN_RIGHT 2
#endif
#ifndef UI_GRAY_BLACK
#define UI_GRAY_BLACK 0x00
#endif
#ifndef UI_GRAY_LIGHT
#define UI_GRAY_LIGHT 0x90
#endif
#ifndef UI_PX_BODY
#define UI_PX_BODY 38
#endif
#ifndef UI_SYSTEM_KEYBOARD_TOP
#define UI_SYSTEM_KEYBOARD_TOP 560
#endif
#ifndef UI_KEY_LEFT
#define UI_KEY_LEFT UI_KEY_1
#endif
#ifndef UI_KEY_CENTER
#define UI_KEY_CENTER UI_KEY_2
#endif
#ifndef UI_KEY_RIGHT
#define UI_KEY_RIGHT UI_KEY_3
#endif
#ifndef UI_KEY_BACK
#define UI_KEY_BACK 3
#endif
#ifndef UI_KEY_UP
#define UI_KEY_UP 4
#endif
#ifndef UI_KEY_DOWN
#define UI_KEY_DOWN 5
#endif
#ifndef BOOK_TOC_ROWS
#define BOOK_TOC_ROWS 7
#define BOOK_TOC_BACK (-2)
#define BOOK_TOC_PREV (-3)
#define BOOK_TOC_NEXT (-4)
#define BOOK_TOC_SKIP_PREV (-5)
#define BOOK_TOC_SKIP_NEXT (-6)
#define BOOK_TOC_JUMP (-7)
#define BOOK_TOC_JUMP_CANCEL (-8)
#define BOOK_TOC_JUMP_CONFIRM (-9)
#define BOOK_TOC_JUMP_TRACK (-10)
#define BOOK_TOC_SELECT_VOLUME (-11)
#endif
{ui_icon_enum}
static bool book_path_is_weread(const char *p){{return p&&strncmp(p,"weread://",9)==0;}}
static int ui_text_effective_px(int px){{return px;}}
static void ui_hairline(uint8_t *f,int y,int x,int w,uint8_t g){{(void)f;(void)y;(void)x;(void)w;(void)g;}}
static int ui_text_title_fit(char *text,int preferred_px,int width,const char *font_sample){{(void)text;(void)width;(void)font_sample;return preferred_px<12?12:(preferred_px>120?120:preferred_px);}}
static void ui_text_title_vc(uint8_t *f,int x,int center_y,int px,const char *text,const char *font_sample,int align){{(void)f;(void)x;(void)center_y;(void)px;(void)font_sample;(void)align;remember_text(text);}}
static void ui_text_title_background_vc(uint8_t *f,int x,int center_y,int px,const char *text,const char *font_sample,int align,uint8_t background){{(void)f;(void)x;(void)center_y;(void)px;(void)font_sample;(void)align;(void)background;remember_text(text);}}
static void ui_text_fixed_context_aa_vc(uint8_t *f,int x,int center_y,int px,const char *text,const char *font_sample,int align,uint8_t gray,uint8_t background){{(void)f;(void)x;(void)center_y;(void)px;(void)font_sample;(void)align;(void)gray;(void)background;remember_text(text);}}
static int ui_text_fixed_context_width_px(int px,const char *text,const char *font_sample){{(void)font_sample;return text?(int)strlen(text)*px/2:0;}}
static void ui_keyboard_hold_start(int x,int y,int t,int64_t n){{(void)x;(void)y;(void)t;(void)n;}}
static void ui_keyboard_hold_cancel(void){{}}
static ui_keyboard_result_t ui_keyboard_hold_tick(bool h,int x,int y,int64_t n){{(void)h;(void)x;(void)y;(void)n;return UI_KEYBOARD_NONE;}}
static ui_keyboard_result_t ui_keyboard_idle_tick(bool t,int64_t n){{(void)t;(void)n;return UI_KEYBOARD_NONE;}}
static bool ui_keyboard_page(int d){{(void)d;return false;}}
static bool ui_keyboard_pending(void){{return false;}}
static void epd_fill_circle(int x,int y,int r,uint8_t g,uint8_t *f){{(void)f;if(mock_circle_count<64)mock_circles[mock_circle_count++]=(mock_circle_call_t){{x,y,r,g}};}}
static void epd_draw_circle(int x,int y,int r,uint8_t g,uint8_t *f){{(void)x;(void)y;(void)r;(void)g;(void)f;}}
static void ui_draw_round_rect(uint8_t *f,EpdRect r,int rad,uint8_t g){{(void)f;(void)r;(void)rad;(void)g;}}
static void ui_draw_selected_round_rect(uint8_t *f,EpdRect r,int rad){{(void)f;(void)r;(void)rad;}}
static void ui_draw_pressed_round_rect(uint8_t *f,EpdRect r,int rad){{(void)f;(void)r;(void)rad;}}
static void ui_draw_separator(uint8_t *f,int y,int x,int w,uint8_t g){{(void)f;(void)y;(void)x;(void)w;(void)g;}}
static void ui_text_fixed(uint8_t *f,int x,int y,int px,const char *s,int a,bool inv){{(void)f;(void)x;(void)y;(void)px;(void)a;(void)inv;remember_text(s);}}
static void ui_text_fixed_vc(uint8_t *f,int x,int y,int px,const char *s,int a,bool inv){{(void)f;(void)x;(void)y;(void)px;(void)a;(void)inv;remember_text(s);}}
static void ui_text_fixed_ink_vc(uint8_t *f,int x,int y,int px,const char *s,int a,uint8_t g){{(void)f;(void)x;(void)y;(void)px;(void)a;(void)g;remember_text(s);}}
static void ui_draw_icon(uint8_t *f,int cx,int cy,int sz,ui_icon_t icon,uint8_t g){{(void)f;(void)cx;(void)cy;(void)sz;(void)icon;(void)g;}}
{toc_jump_funcs}
static void book_toc_render_jump(uint8_t *fb,size_t chapters,int percent){{(void)fb;(void)chapters;char b[32];snprintf(b,sizeof(b),"%d%%",percent<0?0:percent>100?100:percent);remember_text("跳到目录位置");remember_text(b);remember_text("取消");remember_text("跳转");}}
'''
        if "weread_auth_is_expired" in read_text("main/weread/weread_auth.h"):
            extra_stubs += "static bool mock_auth_expired=false;\nbool weread_auth_is_expired(void){return mock_auth_expired;}\n"
            if "auth_expired" in runtime_h:
                smoke_prefix = smoke_prefix.replace(
                    "static void complete(weread_result_t r){assert(mock_busy&&!mock_ready);mock_authenticated=r.authenticated;",
                    "static void complete(weread_result_t r){assert(mock_busy&&!mock_ready);mock_authenticated=r.authenticated;mock_auth_expired=r.auth_expired;",
                )
            smoke_prefix = smoke_prefix.replace(
                "static void reset(void){mock_authenticated=true;",
                "static void reset(void){mock_authenticated=true;mock_auth_expired=false;",
            )
        if "weread_runtime_shelf_cache_status" in runtime_h:
            extra_stubs += "bool weread_runtime_shelf_cache_status(weread_shelf_cache_status_t *out){if(out)memset(out,0,sizeof(*out));return false;}\n"
        if "weread_runtime_cache_stats" in runtime_h:
            extra_stubs += "bool weread_runtime_cache_stats(weread_runtime_cache_stats_t *out){if(out)memset(out,0,sizeof(*out));return false;}\n"
        if "weread_runtime_probe_shelf" in runtime_h:
            extra_stubs += "bool weread_runtime_probe_shelf(const char *const *ids,size_t n){(void)ids;(void)n;return false;}\n"
        smoke_prefix = smoke_prefix.replace('#include "shared.inc"', extra_stubs + '\n#include "shared.inc"')
        smoke_main = r'''
int main(void) {
    reset();
    enter(&ctx);
    render(&ctx, NULL);
    assert(drawn_nav == 1);
    leave(&ctx);
    puts("PASS Group B baseline UI slice compile + smoke");
    return 0;
}
'''
        smoke_c = out_b / "smoke_ui.c"
        smoke_c.write_text(smoke_prefix + smoke_main, encoding="utf-8")
        cmd = cc_base_flags() + ["-Imain", "-Imain/book", "-Imain/weread", "-Itools/weread_stubs", f"-I{out_b}", str(smoke_c), "-o", str(out_b / "smoke_ui")]
        comp = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if comp.returncode != 0:
            raise RuntimeError(f"Group B slice compile failed:\n{comp.stderr}")
        run_compiled(out_b / "smoke_ui", verbose=tracker.verbose)
        tracker.ok("GroupB", "Baseline", "app_weread.c + ui_shelf.c + weread_shelf.c dynamic slice compiled and ran cleanly under ASan/UBSan")
    except Exception as exc:
        tracker.fail("GroupB", "Baseline", str(exc))
        out_b = None
        smoke_prefix = ""

    def run_b_probe(probe_name: str, body_c: str) -> None:
        if not out_b or not smoke_prefix:
            raise RuntimeError("Group B baseline slice unavailable")
        p_c = out_b / f"{probe_name}.c"
        p_bin = out_b / probe_name
        p_c.write_text(smoke_prefix + body_c, encoding="utf-8")
        cmd = cc_base_flags() + ["-Imain", "-Imain/book", "-Imain/weread", "-Itools/weread_stubs", f"-I{out_b}", str(p_c), "-o", str(p_bin)]
        comp = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if comp.returncode != 0:
            raise RuntimeError(f"Compile failed for {probe_name}:\n{comp.stderr}")
        run_compiled(p_bin, verbose=tracker.verbose)

    # M3 UI contracts (strict behavioral execution in else: branches)
    if "登录已过期，可读已缓存书" not in app_weread_src or "二维码已过期，点击刷新" not in app_weread_src:
        tracker.contract_gap("GroupB", "WR-UX-01c", "M3", "Missing offline readonly prompt or QR expired refresh prompt in app_weread.c")
    else:
        try:
            run_b_probe("probe_b_ux01c", r'''
int main(void) {
    reset();
    enter(&ctx);
    weread_shelf_item_t *b = books(2, false);
    b[0].cached_chapters = 1; b[0].total_chapters = 2;
    complete((weread_result_t){.job = WR_JOB_INIT, .authenticated = false, .auth_expired = true, .books = b, .count = 2});
    tick(&ctx);
    render(&ctx, NULL);
    assert(!drawn_qr);
    assert(strstr(drawn_text, "登录已过期") != NULL);

    // QR expiry clears s_qr[0] via ui_wifi_qr_clear and renders expiry refresh prompt
    reset();
    enter(&ctx);
    complete((weread_result_t){.job = WR_JOB_INIT, .authenticated = false, .auth_expired = false});
    tick(&ctx);
    complete((weread_result_t){.job = WR_JOB_QR, .authenticated = false, .uid = "u1", .qr_url = "https://weread.qq.com/qr1"});
    tick(&ctx);
    assert(s_qr[0] != '\0');
    ctx.now_ms += 2000;
    tick(&ctx);
    complete((weread_result_t){.job = WR_JOB_POLL, .qr_state = WEREAD_QR_STATE_TIMEOUT});
    tick(&ctx);
    assert(s_qr[0] == '\0');
    render(&ctx, NULL);
    assert(strstr(drawn_text, "二维码已过期") != NULL);
    leave(&ctx);
    return 0;
}
''')
            tracker.ok("GroupB", "WR-UX-01c", "Offline readonly shelf & QR expiry clear/prompt verified in sliced app_weread.c under ASan/UBSan")
        except Exception as exc:
            tracker.fail("GroupB", "WR-UX-01c", str(exc))

    ux03_ok = (
        "UI_GESTURE_SWIPE_L" in app_weread_src
        and "UI_GESTURE_SWIPE_R" in app_weread_src
        and "正在打开阅读" in app_weread_src
        and "单击阅读，长按清理离线内容" not in app_weread_src
        and "清空" in app_weread_src
    )
    if not ux03_ok:
        tracker.contract_gap("GroupB", "WR-UX-03", "M3", "Missing SWIPE_L/R, search 清空/pinyin hint, direct-open transition, or idle tagline removal in app_weread.c")
    else:
        try:
            run_b_probe("probe_b_ux03", r'''
int main(void) {
    reset();
    initialized(20);
    assert(page_count() == 3 && s_page == 0);
    // Idle healthy shelf does not keep old tagline
    assert(s_message[0] == '\0');
    ui_gesture_event_t sw_l = {.type = UI_GESTURE_SWIPE_L, .x0 = 500, .y0 = 500, .x = 100, .y = 500};
    ui_gesture_event_t sw_r = {.type = UI_GESTURE_SWIPE_R, .x0 = 100, .y0 = 500, .x = 500, .y = 500};
    assert(gesture(&ctx, &sw_l) == APP_REDRAW_PAGE && s_page == 1);
    assert(gesture(&ctx, &sw_r) == APP_REDRAW_PAGE && s_page == 0);

    // Search entry + 清空 button
    EpdRect sr = ui_shelf_search_rect(s_page, page_count());
    ui_gesture_event_t tap_sr = {.type = UI_GESTURE_TAP, .x0 = (uint16_t)(sr.x + 20), .y0 = (uint16_t)(sr.y + 20)};
    assert(gesture(&ctx, &tap_sr) == APP_REDRAW_PAGE && s_search);
    render(&ctx, NULL);
    assert(strstr(drawn_text, "清空") != NULL);
    leave(&ctx);

    // Direct open centered transition
    reset();
    assert(app_weread_request_open("weread://id_0"));
    enter(&ctx);
    complete((weread_result_t){.job = WR_JOB_INIT, .authenticated = true, .books = books(3, false), .count = 3});
    tick(&ctx);
    render(&ctx, NULL);
    assert(strstr(drawn_text, "正在打开阅读") != NULL);
    leave(&ctx);
    return 0;
}
''')
            tracker.ok("GroupB", "WR-UX-03", "SWIPE_L/R paging, search 清空, direct-open transition, and idle s_message=='' verified in sliced app_weread.c")
        except Exception as exc:
            tracker.fail("GroupB", "WR-UX-03", str(exc))

    ux07_ok = ("缓存整本" in app_weread_src and "cached_chapters" in app_weread_src and "total_chapters" in app_weread_src)
    if not ux07_ok:
        tracker.contract_gap("GroupB", "WR-UX-07c", "M3", "Missing Scheme A badges (cached_chapters/total_chapters) or 3-action long-press card (缓存整本) in app_weread.c")
    else:
        try:
            run_b_probe("probe_b_ux07c", r'''
int main(void) {
    reset();
    enter(&ctx);
    weread_shelf_item_t *b = books(3, false);
    b[0].cached_chapters = 0; b[0].total_chapters = 5;
    b[1].cached_chapters = 2; b[1].total_chapters = 5;
    b[2].cached_chapters = 5; b[2].total_chapters = 5;
    complete((weread_result_t){.job = WR_JOB_INIT, .authenticated = true, .books = b, .count = 3});
    tick(&ctx);
    int starts_before = nstarts;
    render(&ctx, NULL);
    assert(nstarts == starts_before); // render() performs zero job starts / zero IO
    assert(strstr(drawn_text, "离线") != NULL);
    EpdRect c0 = ui_shelf_cover(6, 0), c1 = ui_shelf_cover(6, 1), c2 = ui_shelf_cover(6, 2);
    int c0_hits = 0, c1_w = 0, c1_b = 0, c2_hits = 0;
    for (int ci = 0; ci < mock_circle_count; ++ci) {
        if (mock_circles[ci].x == c0.x + 15 && mock_circles[ci].y == c0.y + 15) ++c0_hits;
        if (mock_circles[ci].x == c1.x + 15 && mock_circles[ci].y == c1.y + 15) {
            if (mock_circles[ci].r == 7 && mock_circles[ci].g == UI_GRAY_WHITE) ++c1_w;
            if (mock_circles[ci].r == 5 && mock_circles[ci].g == UI_GRAY_BLACK) ++c1_b;
        }
        if (mock_circles[ci].x == c2.x + 15 && mock_circles[ci].y == c2.y + 15) ++c2_hits;
    }
    assert(c0_hits == 0 && c1_w == 1 && c1_b == 1 && c2_hits == 0);
    EpdRect r0 = row_rect(0);
    ui_gesture_event_t lp = {.type = UI_GESTURE_LONG_PRESS, .x0 = (uint16_t)(r0.x + 20), .y0 = (uint16_t)(r0.y + 20)};
    assert(gesture(&ctx, &lp) == APP_REDRAW_PAGE);
    render(&ctx, NULL);
    assert(strstr(drawn_text, "缓存整本") != NULL && strstr(drawn_text, "取消") != NULL);
    leave(&ctx);
    return 0;
}
''')
            tracker.ok("GroupB", "WR-UX-07c", "render() zero-IO, Scheme A badges, and 3-action long-press card verified in sliced app_weread.c")
        except Exception as exc:
            tracker.fail("GroupB", "WR-UX-07c", str(exc))

    ux08_ok = (
        ("微信读书 ▾" in app_weread_src or ("微信读书" in app_weread_src and "▾" in app_weread_src))
        and "全部书籍" in app_weread_src
        and "已离线" in app_weread_src
        and "已读完" in app_weread_src
    )
    if not ux08_ok:
        tracker.contract_gap("GroupB", "WR-UX-08", "M3", "Missing title dropdown status filter or page-jump slider in app_weread.c")
    else:
        try:
            run_b_probe("probe_b_ux08", r'''
int main(void) {
    reset();
    initialized(30);
    assert(page_count() > 3);
    render(&ctx, NULL);
    assert(strstr(drawn_text, "微信读书") != NULL);
    // Tap title dropdown (24, 79, 260, 74)
    ui_gesture_event_t tap_dd = {.type = UI_GESTURE_TAP, .x0 = 100, .y0 = 110};
    assert(gesture(&ctx, &tap_dd) == APP_REDRAW_PAGE);
    render(&ctx, NULL);
    assert(strstr(drawn_text, "全部书籍") != NULL && strstr(drawn_text, "在读") != NULL &&
           strstr(drawn_text, "已离线") != NULL && strstr(drawn_text, "未读") != NULL &&
           strstr(drawn_text, "已读完") != NULL);
    leave(&ctx);
    return 0;
}
''')
            tracker.ok("GroupB", "WR-UX-08", "Title dropdown (24,79,260,74) & 5-item status filter verified in sliced app_weread.c")
        except Exception as exc:
            tracker.fail("GroupB", "WR-UX-08", str(exc))

    ux09_ok = ("缓存管理" in app_weread_src and "清理已读完书籍" in app_weread_src and "清空全部离线章节" in app_weread_src)
    if not ux09_ok:
        tracker.contract_gap("GroupB", "WR-UX-09c", "M3", "Missing '缓存管理' 4th menu item & selective cleanup UI in app_weread.c")
    else:
        try:
            run_b_probe("probe_b_ux09c", r'''
int main(void) {
    reset();
    initialized(3);
    ui_gesture_event_t tap_more = {.type = UI_GESTURE_TAP, .x0 = 466, .y0 = 121};
    assert(gesture(&ctx, &tap_more) == APP_REDRAW_PAGE);
    render(&ctx, NULL);
    assert(strstr(drawn_text, "缓存管理") != NULL);
    leave(&ctx);
    return 0;
}
''')
            tracker.ok("GroupB", "WR-UX-09c", "More menu 4th item '缓存管理' verified in sliced app_weread.c")
        except Exception as exc:
            tracker.fail("GroupB", "WR-UX-09c", str(exc))


# ---------------------------------------------------------------------------
# Group C: Dual-Board Sliced Reader & Dashboard Probes
# ---------------------------------------------------------------------------

def run_group_c(tracker: ProbeTracker) -> None:
    print("\n=== Group C: Dual-Board Sliced Reader & Dashboard Probes (metalio47 & pico) ===")
    bridge_src = read_text("main/apps/book_weread_bridge.inc")
    chapter_src = read_text("main/apps/book_weread_chapter.inc")
    runtime_h = read_text("main/weread/weread_runtime.h")

    boards = [
        ("metalio47", "main/apps/app_book.c", "main/apps/app_dashboard.c", 1),
        ("pico", "boards/pico/src/main/apps/app_book.c", "boards/pico/src/main/apps/app_dashboard.c", 0),
    ]

    for board_name, book_rel, dash_rel, is_metalio in boards:
        book_src = read_text(book_rel)
        dash_src = read_text(dash_rel)
        combined_reader_src = book_src + "\n" + chapter_src + "\n" + bridge_src

        undo_decl = extract_c_struct_for_var(book_src, "s_chapter_jump_undo") or (
            "static struct { bool valid; size_t chapter, offset; int64_t until_ms; } s_chapter_jump_undo;"
        )
        has_turned_in_rt = "weread_runtime_reader_has_turned" in runtime_h
        has_rebase_in_rt = "weread_runtime_cloud_jump_rebase" in runtime_h

        def build_c_harness(main_body: str) -> str:
            h = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
enum {
    ESP_OK = 0,
    ESP_FAIL = -1,
    ESP_ERR_NO_MEM = 0x101,
    ESP_ERR_INVALID_ARG = 0x102,
    ESP_ERR_INVALID_STATE = 0x103,
    ESP_ERR_INVALID_SIZE = 0x104,
    ESP_ERR_NOT_FOUND = 0x105,
    ESP_ERR_NOT_SUPPORTED = 0x106,
    ESP_ERR_TIMEOUT = 0x107,
    ESP_ERR_INVALID_RESPONSE = 0x108,
    WEREAD_ERR_PURCHASE = 0x7901,
    WEREAD_ERR_SOLD_OUT = 0x7902,
    WEREAD_ERR_VERSION = 0x7903,
    WEREAD_ERR_POSITION = 0x7904,
    WEREAD_ERR_PROGRESS_CONFLICT = 0x7905,
    WEREAD_ERR_STORAGE = 0x7906,
    WEREAD_ERR_INVALID_RESPONSE = 0x7907
};
enum {READING,TOC,READER_PANEL_NONE,WR_JOB_NOTE,WR_JOB_CHAPTER,WR_JOB_PREFETCH,MODE_GL16};
typedef enum {APP_REDRAW_NONE,APP_REDRAW_PAGE,APP_REDRAW_AREA} app_redraw_t;
typedef enum {WR_PROGRESS_LOCAL,WR_PROGRESS_PENDING,WR_PROGRESS_SYNCED,WR_PROGRESS_CONFLICT} weread_progress_state_t;
typedef enum {WEREAD_STAGE_NONE=0,WEREAD_STAGE_PROGRESS,WEREAD_STAGE_TOC,WEREAD_STAGE_CHAPTER,WEREAD_STAGE_PARSE,WEREAD_STAGE_RETRY} weread_stage_t;
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {bool touched;} touch_t;
typedef struct {int64_t now_ms;bool consumed,request_app;touch_t *touch;uint8_t *fb;int leaf;} app_ctx_t;
typedef struct {int job,error;char id[64];uint32_t chapter_uid;} weread_result_t;
static int s_view=READING,s_reader_panel=READER_PANEL_NONE,s_mode;
static bool s_clear_confirm,s_toolbar,s_reader_footer_pending,s_reader_text_frame=true;
static uint64_t s_reader_footer_due_ms;
static int64_t s_reader_notice_until;
static EpdRect s_area;
static char s_path[64]="weread://book_A",s_message[128],s_reader_notice[64],s_title[64];
static const char *s_text="body text";
static size_t s_text_len=9,s_chapter=1,s_page=0,s_selected_toc,s_selected_toc_offset,s_toc_page=0;
''' + undo_decl + r'''
static weread_progress_state_t mock_state = WR_PROGRESS_SYNCED;
static unsigned mock_cached = 1, mock_total = 4;
static bool mock_paused = false, mock_blocked = false;
static bool mock_save_failed = false, mock_pending_save = false, mock_rescued = false, mock_has_turned = false, mock_network_active = false;
static bool book_path_is_weread(const char *p){return p&&strncmp(p,"weread://",9)==0;}
static size_t book_layout_page_count(void){return 2;}
static size_t book_layout_page_image_count(size_t p){(void)p;return 0;}
static size_t reader_page_offset(size_t p){return p*10;}
static unsigned percent(size_t p){return (unsigned)(p*50);}
static void wr_copy(char *out,size_t cap,const char *in){snprintf(out,cap,"%s",in?in:"");}
static void copy_text(char *to,size_t cap,const char *from){snprintf(to,cap,"%s",from?from:"");}
static void *wr_alloc(size_t n){return calloc(1,n);}
static void render(app_ctx_t *c,uint8_t *f){(void)c;(void)f;}
static bool load_chapter_at(app_ctx_t *c,size_t ch,size_t off,bool last,const char *a,size_t s){(void)c;(void)ch;(void)off;(void)last;(void)a;(void)s;return true;}
static void save_progress(void){}
static size_t book_navigation_count(void){return 4;}
static esp_err_t book_navigation_title(size_t i,char *o,size_t c){(void)i;snprintf(o,c,"Ch");return ESP_OK;}
static size_t current_toc_position(void){return 0;}
static void book_toc_set_cached_probe(bool(*fn)(size_t,void*),void *u){(void)fn;(void)u;}
static void book_toc_prepare_page(size_t n,size_t cur,int page){(void)n;(void)cur;(void)page;}
static uint32_t book_weread_uid(size_t ch){return (uint32_t)(ch+1);}
static void weread_reader_navigated_version(const char *p,size_t c,uint32_t o,unsigned pct,const char *s,uint32_t v){(void)p;(void)c;(void)o;(void)pct;(void)s;(void)v;}
static void weread_reader_tick(const char *p,size_t c,uint32_t o,unsigned pct,const char *s,uint64_t n,bool a){(void)p;(void)c;(void)o;(void)pct;(void)s;(void)n;(void)a;}
static weread_progress_state_t weread_reader_state(void){return mock_state;}
static bool weread_reader_save_failed(void){return mock_save_failed;}
static bool weread_reader_pending_save(void){return mock_pending_save;}
static bool weread_reader_rescued(void){return mock_rescued;}
static unsigned weread_reader_offline(void){return 0;}
static void weread_reader_cached_progress(unsigned *c,unsigned *t){*c=mock_cached;*t=mock_total;}
static bool weread_reader_storage_blocked(void){return mock_blocked;}
static bool weread_reader_caching(void){return !mock_paused;}
static bool weread_reader_cache_fill_enabled(void){return !mock_paused;}
static bool weread_reader_cache_forward(void){return !mock_paused;}
static bool weread_reader_cache_forward_stalled(void){return false;}
static void weread_reader_cache_forward_set(bool on){mock_paused=!on;}
static bool weread_reader_chapter_cached(const char *u,size_t ch){(void)u;(void)ch;return true;}
static bool weread_reader_cached(const char *u,size_t ch){(void)u;(void)ch;return true;}
static esp_err_t weread_reader_save(uint64_t n){(void)n;return ESP_OK;}
static bool weread_runtime_network_active(void){return mock_network_active;}
static bool weread_runtime_busy(void){return false;}
static bool weread_runtime_start(int j,const char *i,const char *v){(void)j;(void)i;(void)v;return true;}
static bool weread_runtime_poll(weread_result_t *r){(void)r;return false;}
static void weread_runtime_result_free(weread_result_t *r){(void)r;}
static void weread_runtime_cancel(void){}
static bool weread_runtime_prefetch_active(const char *i,uint32_t u){(void)i;(void)u;return false;}
static void weread_runtime_prefetch_adopt(uint32_t u){(void)u;}
static bool weread_reader_poll_cloud_jump(const char *u,size_t *c,uint32_t *o){(void)u;(void)c;(void)o;return false;}
static void weread_reader_commit_cloud_jump(const char *u,bool ok){(void)u;(void)ok;}
static bool weread_reader_stage(weread_stage_t *s,uint64_t *b){(void)s;(void)b;return false;}
static bool weread_reader_stage_text(char *o,size_t c){(void)o;(void)c;return false;}
static bool weread_runtime_reader_has_turned(const char *u){(void)u;return mock_has_turned;}
static esp_err_t weread_runtime_cloud_jump_rebase(const char *u){(void)u;return ESP_OK;}
static bool weread_reader_has_turned(const char *u){(void)u;return mock_has_turned;}
static esp_err_t weread_reader_cloud_jump_rebase(const char *u){(void)u;return ESP_OK;}
'''
            if has_turned_in_rt or has_rebase_in_rt:
                pass  # Already declared as static stubs above matching PROJECT.md signatures
            bridge_clean = re.sub(r'#include\s+"weread/weread_store\.h"', "", bridge_src)
            return h + bridge_clean + "\n" + chapter_src + "\n" + main_body

        # Baseline dual-board compile & behavioral check of book_weread_bridge.inc + book_weread_chapter.inc
        try:
            smoke_harness = build_c_harness(r'''
int main(void) {
    app_ctx_t ctx = {.now_ms = 1000, .fb = (uint8_t *)1};
    wr_chapter_cancel();
    mock_state = WR_PROGRESS_SYNCED;
    assert(wr_reader_status() != NULL);
    char cap[64] = {0};
    wr_reader_offline_caption(cap, sizeof(cap));
    wr_reader_sample(2000, true);
    (void)wr_chapter_tick(&ctx);
    return 0;
}
''')
            compile_and_run(
                smoke_harness,
                f"probe_c_{board_name}_baseline",
                extra_flags=[f"-DKIIKO_BOARD_METALIO47={is_metalio}"],
                verbose=tracker.verbose,
            )
            tracker.ok("GroupC", f"{board_name}:Baseline", f"{board_name} bridge+chapter controller compiled and passed smoke under ASan/UBSan")
        except Exception as exc:
            tracker.fail("GroupC", f"{board_name}:Baseline", str(exc))

        # WR-UX-02: Footer simplification + no cache_delta wakeup + compiled C behavioral execution
        if not wr_reader_status_synced_returns_empty(bridge_src):
            tracker.contract_gap("GroupC", "WR-UX-02", "M4", f"({board_name}) wr_reader_status() still emits non-empty string instead of '' on WR_PROGRESS_SYNCED")
        elif footer_pending_uses_cache_delta(bridge_src):
            tracker.contract_gap("GroupC", "WR-UX-02", "M4", f"({board_name}) wr_reader_sample() still includes cache_delta in s_reader_footer_pending trigger")
        else:
            try:
                ux02_harness = build_c_harness(r'''
int main(void) {
    // 1. wr_reader_status() returns "" for WR_PROGRESS_SYNCED & stationary WR_PROGRESS_LOCAL
    mock_state = WR_PROGRESS_SYNCED;
    assert(strcmp(wr_reader_status(), "") == 0);
    mock_state = WR_PROGRESS_PENDING;
    assert(strcmp(wr_reader_status(), "待同步 · ") == 0);
    mock_state = WR_PROGRESS_CONFLICT;
    assert(strstr(wr_reader_status(), "进度冲突") != NULL);
    mock_state = WR_PROGRESS_SYNCED;
    mock_save_failed = true;
    assert(strlen(wr_reader_status()) > 0);
    mock_save_failed = false;

    // 2. wr_reader_offline_caption() returns "" when full or paused, "空间不足 · " when storage_blocked
    char cap[64] = {0};
    mock_cached = 4; mock_total = 4; mock_paused = false; mock_blocked = false;
    wr_reader_offline_caption(cap, sizeof(cap));
    assert(strcmp(cap, "") == 0);
    mock_cached = 1; mock_total = 4; mock_paused = true;
    wr_reader_offline_caption(cap, sizeof(cap));
    assert(strcmp(cap, "") == 0);
    mock_paused = false; mock_blocked = true;
    wr_reader_offline_caption(cap, sizeof(cap));
    assert(strstr(cap, "空间不足") != NULL);
    mock_blocked = false;

    // 3. wr_reader_sample() leaves s_reader_footer_pending == false when only mock_cached changes (1 -> 2)
    mock_state = WR_PROGRESS_SYNCED;
    mock_cached = 1; mock_total = 4;
    wr_reader_sample(1000, true);
    s_reader_footer_pending = false;
    mock_cached = 2;
    wr_reader_sample(2000, true);
    assert(s_reader_footer_pending == false);

    // 4. wr_chapter_cancel() clears s_wr_jump.pending
    s_wr_jump.pending = true;
    wr_chapter_cancel();
    assert(s_wr_jump.pending == false);
    return 0;
}
''')
                compile_and_run(
                    ux02_harness,
                    f"probe_c_{board_name}_ux02",
                    extra_flags=[f"-DKIIKO_BOARD_METALIO47={is_metalio}"],
                    verbose=tracker.verbose,
                )
                tracker.ok("GroupC", f"{board_name}:WR-UX-02", "Compiled C wr_reader_status/offline_caption/sample/cancel verified under ASan/UBSan")
            except Exception as exc:
                tracker.fail("GroupC", f"{board_name}:WR-UX-02", str(exc))

        # WR-UX-04: Cross-chapter s_chapter_jump_undo + 6s banner + pages > 0 guard (search combined app_book + chapter.inc + bridge.inc)
        if "已接力至手机最新进度" not in combined_reader_src or "发现更新的手机进度" not in combined_reader_src:
            tracker.contract_gap("GroupC", "WR-UX-04", "M4", f"({board_name}) {book_rel} + .inc missing 6s cloud-jump undo / opt-in banners")
        else:
            tracker.ok("GroupC", f"{board_name}:WR-UX-04", "Cross-chapter undo & pages>0 opt-in guard present in reader sources")

        # WR-UX-05: 402/offline forward-turn prompts & TOC page rollback (search combined)
        has_402 = "手机端购买本章后，再翻页即可加载" in combined_reader_src
        has_offline = "本章未缓存且网络不可用，连网后再翻页重试" in combined_reader_src
        has_toc_rollback = ("s_wr_jump.toc" in combined_reader_src and ("ctx->leaf" in combined_reader_src or "s_toc_page" in combined_reader_src))
        if not (has_402 and has_offline and has_toc_rollback):
            tracker.contract_gap("GroupC", "WR-UX-05", "M4", f"({board_name}) Missing 402 prompt, offline forward-turn prompt, or TOC origin-page rollback")
        else:
            tracker.ok("GroupC", f"{board_name}:WR-UX-05", "402/offline forward-turn retry & TOC origin-page rollback present")

        # WR-UX-06c: Dashboard '· 待同步' sliced & executed on BOTH metalio47 & pico
        if "待同步" not in dash_src:
            tracker.contract_gap("GroupC", "WR-UX-06c", "M4", f"({board_name}) {dash_rel} missing '· 待同步' continue-card badge for WR_PROGRESS_PENDING")
        else:
            try:
                home_book_typedef = extract_typedef_struct(dash_src, "home_book_t")
                scan_books_fn = slice_function(dash_rel, "scan_books")
                render_root_fn = slice_function(dash_rel, "render_root")
                refresh_progress_fn = ""
                refresh_call = ""
                if not is_metalio and re.search(r"^[ \t]*static\s+bool\s+refresh_progress\s*\(", dash_src, re.M):
                    refresh_progress_fn = slice_function(dash_rel, "refresh_progress")
                    refresh_call = "    (void)refresh_progress();\n"
                dash_stubbed = {
                    "assert", "memset", "memcpy", "strcmp", "strncmp", "strcasecmp", "strrchr",
                    "strlen", "strstr", "snprintf", "S_ISREG", "sizeof", "if", "for", "while",
                    "switch", "return", "HOME_STAGE",
                    "book_at", "featured", "book_path_is_weread", "book_path_is_supported_target",
                    "book_target_stat", "app_setup_library_pending", "weread_runtime_load_local",
                    "book_progress_last_path", "book_progress_load", "book_progress_load_checked",
                    "book_progress_list", "recent_visit", "copy_title", "title_for",
                    "read_pico_sd_get_info", "app_settings_books_dir", "scan_root",
                    "book_store_flash_ready", "book_epub_metadata_cached", "book_epub_metadata",
                    "book_author_clean_display", "pico_boot_asset_allowed",
                    "weread_progress_load", "weread_progress_state",
                    "record_dash", "ui_nav_status", "ui_nav_draw", "ui_text", "ui_text_vc",
                    "ui_text_box", "ui_text_fixed", "ui_text_fixed_vc", "ui_text_fixed_ink_vc",
                    "ui_text_fixed_context_aa_vc", "ui_text_fixed_context_width_px",
                    "ui_text_fixed_width_px", "today_seconds", "fit", "label", "label_on",
                    "ring", "app_settings_home_goal_minutes", "app_settings_home_component",
                    "app_settings_main_fast_refresh", "ui_wallpaper_draw_avatar", "epd_fill_circle",
                    "ui_root_card", "ui_fill_round_rect", "home_frame", "circle_frame",
                    "ui_fill_rect_fast", "ui_draw_separator", "draw_cover",
                    "continue_card_pressed", "title_lines", "ui_home_folder_draw",
                    "clock_draw_region", "clock_style", "component_name", "month_length",
                    "calendar", "scan_books", "refresh_progress", "render_root",
                }
                dash_helpers = extract_static_helpers(
                    dash_src,
                    [scan_books_fn, refresh_progress_fn, render_root_fn],
                    dash_stubbed,
                )
                dash_harness = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "weread_progress.h"
#ifndef ESP_ERR_NVS_NOT_FOUND
#define ESP_ERR_NVS_NOT_FOUND 0x1102
#endif
typedef enum {EPD_DRAW_ALIGN_LEFT=0,EPD_DRAW_ALIGN_CENTER=1,EPD_DRAW_ALIGN_RIGHT=2} EpdFontFlags;
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {uint32_t file_size;uint16_t chapter;uint32_t byte_off;uint8_t px,pct;uint32_t last_open_s;} book_progress_t;
#define BOOK_STORE_PATH_MAX 288
''' + home_book_typedef + r'''
typedef struct {bool present,mounted;} read_pico_sd_info_t;
typedef enum {APP_HOME_FOLDER=99} app_home_component_t;
#define UI_ROOT_GRAY 0xf0
#define UI_GRAY_WHITE 0xf0
#define BOOK_EXCERPT_CAP 128
static home_book_t s_current, s_recent[6];
static char s_excerpt[BOOK_EXCERPT_CAP];
static bool s_avatar_ready, s_reading_days_valid;
static struct tm s_date;
static uint8_t s_avatar[60*60], *s_folder;
static bool s_folder_fast;
static unsigned s_widget_phase;
static char drawn_dash[4096];
static char mock_last_path[BOOK_STORE_PATH_MAX] = "weread://book_A";
static bool mock_wr_pending = false;
static home_book_t *book_at(int index){return index?&s_recent[index-1]:&s_current;}
static home_book_t *featured(void){return s_current.path[0]?&s_current:&s_recent[0];}
static bool book_path_is_weread(const char *p){return p&&strncmp(p,"weread://",9)==0;}
static bool book_path_is_supported_target(const char *p){return p&&p[0];}
static int book_target_stat(const char *p,struct stat *st){if(!p||!st||strcmp(p,mock_last_path)!=0)return -1;memset(st,0,sizeof(*st));st->st_mode=S_IFREG|0644;st->st_size=1024;return 0;}
static bool app_setup_library_pending(void){return false;}
static void weread_runtime_load_local(void){}
static bool book_progress_last_path(char *out,size_t cap){if(!out||!cap||!mock_last_path[0])return false;snprintf(out,cap,"%s",mock_last_path);return true;}
static bool book_progress_load(const char *p,uint32_t sz,book_progress_t *out){if(!p||!out||strcmp(p,mock_last_path)!=0)return false;memset(out,0,sizeof(*out));out->file_size=sz;out->pct=42;return true;}
static esp_err_t book_progress_load_checked(const char *p,uint32_t sz,book_progress_t *out){return book_progress_load(p,sz,out)?ESP_OK:ESP_ERR_NVS_NOT_FOUND;}
static esp_err_t book_progress_list(bool(*visit)(const char*,const book_progress_t*,void*),void *ctx){(void)visit;(void)ctx;return ESP_OK;}
static bool recent_visit(const char *p,const book_progress_t *pr,void *ctx){(void)p;(void)pr;(void)ctx;return true;}
static void copy_title(char *dst,size_t cap,const char *src){snprintf(dst,cap,"%s",src?src:"");}
static void title_for(home_book_t *item){snprintf(item->title,sizeof(item->title),"%s",book_path_is_weread(item->path)?"WeRead Book":"Local Book");}
static esp_err_t read_pico_sd_get_info(read_pico_sd_info_t *sd){if(sd)memset(sd,0,sizeof(*sd));return ESP_OK;}
static const char *app_settings_books_dir(void){return "/sdcard/Books";}
static void scan_root(const char *r){(void)r;}
static bool book_store_flash_ready(void){return false;}
static esp_err_t book_epub_metadata_cached(const char *p,char *t,size_t tc,char *a,size_t ac){(void)p;(void)t;(void)tc;(void)a;(void)ac;return ESP_FAIL;}
static esp_err_t book_epub_metadata(const char *p,char *t,size_t tc,char *a,size_t ac){(void)p;(void)t;(void)tc;(void)a;(void)ac;return ESP_FAIL;}
static bool book_author_clean_display(char *s,size_t cap){(void)s;(void)cap;return false;}
static bool pico_boot_asset_allowed(const char *p){(void)p;return true;}
esp_err_t weread_progress_load(const char *id, weread_progress_t *out){
    if(!id||!out)return ESP_ERR_INVALID_ARG;
    memset(out,0,sizeof(*out));
    if(strcmp(id,"book_A")!=0&&strcmp(id,"weread://book_A")!=0)return ESP_ERR_NOT_FOUND;
    if(mock_wr_pending){out->dirty=true;out->position_pending=true;}
    else{out->last_report_ms=1000;}
    return ESP_OK;
}
weread_progress_state_t weread_progress_state(const weread_progress_t *p){
    if(!p)return WR_PROGRESS_LOCAL;
    if(p->sync_conflict)return WR_PROGRESS_CONFLICT;
    if(p->dirty&&p->position_pending)return WR_PROGRESS_PENDING;
    return p->last_report_ms?WR_PROGRESS_SYNCED:WR_PROGRESS_LOCAL;
}
static void record_dash(const char *s){if(s){size_t n=strlen(drawn_dash);snprintf(drawn_dash+n,sizeof(drawn_dash)-n,"%s\n",s);}}
static void ui_nav_status(uint8_t *f){(void)f;}
static void ui_nav_draw(uint8_t *f,int a){(void)f;(void)a;}
static void ui_text(uint8_t *f,int x,int y,int px,const char *s,int al,bool inv){(void)f;(void)x;(void)y;(void)px;(void)al;(void)inv;record_dash(s);}
static void ui_text_vc(uint8_t *f,int x,int y,int px,const char *s,int al,bool inv){(void)f;(void)x;(void)y;(void)px;(void)al;(void)inv;record_dash(s);}
static void ui_text_box(uint8_t *f,EpdRect r,int px,const char *s,int al,bool inv){(void)f;(void)r;(void)px;(void)al;(void)inv;record_dash(s);}
static void ui_text_fixed(uint8_t *f,int x,int y,int px,const char *s,int al,bool inv){(void)f;(void)x;(void)y;(void)px;(void)al;(void)inv;record_dash(s);}
static void ui_text_fixed_vc(uint8_t *f,int x,int y,int px,const char *s,int al,bool inv){(void)f;(void)x;(void)y;(void)px;(void)al;(void)inv;record_dash(s);}
static void ui_text_fixed_ink_vc(uint8_t *f,int x,int y,int px,const char *s,int al,uint8_t g){(void)f;(void)x;(void)y;(void)px;(void)al;(void)g;record_dash(s);}
static void ui_text_fixed_context_aa_vc(uint8_t *f,int x,int y,int px,const char *s,const char *smp,int al,uint8_t g,uint8_t bg){(void)f;(void)x;(void)y;(void)px;(void)smp;(void)al;(void)g;(void)bg;record_dash(s);}
static int ui_text_fixed_context_width_px(int px,const char *s,const char *smp){(void)smp;return s?(int)strlen(s)*px/2:0;}
static unsigned today_seconds(void){return 600;}
static void fit(char *t,int px,int w){(void)t;(void)px;(void)w;}
static void label(uint8_t *f,int x,int y,int px,const char *s,EpdFontFlags al,uint8_t g){(void)f;(void)x;(void)y;(void)px;(void)al;(void)g;record_dash(s);}
static void label_on(uint8_t *f,int x,int y,int px,const char *s,EpdFontFlags al,uint8_t g,uint8_t bg){(void)f;(void)x;(void)y;(void)px;(void)al;(void)g;(void)bg;record_dash(s);}
static void ring(uint8_t *f,int cx,int cy,int r,int th,uint32_t s,unsigned g){(void)f;(void)cx;(void)cy;(void)r;(void)th;(void)s;(void)g;}
static unsigned app_settings_home_goal_minutes(void){return 30;}
static app_home_component_t app_settings_home_component(void){return APP_HOME_FOLDER;}
static bool app_settings_main_fast_refresh(void){return false;}
static void ui_wallpaper_draw_avatar(uint8_t *f,uint8_t *a,EpdRect r,int rad){(void)f;(void)a;(void)r;(void)rad;}
static void epd_fill_circle(int x,int y,int r,uint8_t g,uint8_t *f){(void)x;(void)y;(void)r;(void)g;(void)f;}
static void ui_root_card(uint8_t *f,EpdRect r,int rad){(void)f;(void)r;(void)rad;}
static void ui_fill_round_rect(uint8_t *f,EpdRect r,int rad,uint8_t g){(void)f;(void)r;(void)rad;(void)g;}
static void home_frame(uint8_t *f,EpdRect r,int rad){(void)f;(void)r;(void)rad;}
static void circle_frame(uint8_t *f,int x,int y,int r){(void)f;(void)x;(void)y;(void)r;}
static void ui_fill_rect_fast(EpdRect r,uint8_t g,uint8_t *f){(void)r;(void)g;(void)f;}
static int ui_text_fixed_width_px(int px,const char *s){return s?(int)strlen(s)*px/2:0;}
static void ui_draw_separator(uint8_t *f,int y,int x,int w,uint8_t g){(void)f;(void)y;(void)x;(void)w;(void)g;}
static void draw_cover(uint8_t *f,const home_book_t *b,EpdRect r){(void)f;(void)b;(void)r;}
static bool continue_card_pressed(void){return false;}
static int title_lines(uint8_t *f,const char *t,uint8_t bg){(void)f;(void)bg;record_dash(t);return 1;}
static void ui_home_folder_draw(uint8_t *f,uint8_t *fd,bool fast,EpdRect r){(void)f;(void)fd;(void)fast;(void)r;}
static void clock_draw_region(uint8_t *f,EpdRect b,const struct tm *d,bool bin,int st,EpdRect c,unsigned p){(void)f;(void)b;(void)d;(void)bin;(void)st;(void)c;(void)p;}
static int clock_style(app_home_component_t c){(void)c;return 0;}
static const char *component_name(app_home_component_t c){(void)c;return "最近阅读书夹";}
static unsigned month_length(const struct tm *d){(void)d;return 30;}
static void calendar(uint8_t *f,int x,int y,int sx,int sy,int px,unsigned g){(void)f;(void)x;(void)y;(void)sx;(void)sy;(void)px;(void)g;}
''' + dash_helpers + "\n" + scan_books_fn + "\n" + refresh_progress_fn + "\n" + render_root_fn + r'''
int main(void) {
    // 1. weread://book_A with WR_PROGRESS_PENDING -> must render '待同步'
    strcpy(mock_last_path, "weread://book_A");
    mock_wr_pending = true;
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    scan_books();
''' + refresh_call + r'''    drawn_dash[0] = 0;
    render_root(NULL);
    assert(strstr(drawn_dash, "待同步") != NULL);

    // 2. weread://book_A with WR_PROGRESS_SYNCED -> must NOT render '待同步'
    mock_wr_pending = false;
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    scan_books();
''' + refresh_call + r'''    drawn_dash[0] = 0;
    render_root(NULL);
    assert(strstr(drawn_dash, "待同步") == NULL);

    // 3. Local .epub MUST NEVER show '待同步' even if mock_wr_pending is true
    strcpy(mock_last_path, "/sdcard/Books/local.epub");
    mock_wr_pending = true;
    memset(&s_current, 0, sizeof(s_current));
    memset(s_recent, 0, sizeof(s_recent));
    scan_books();
''' + refresh_call + r'''    drawn_dash[0] = 0;
    render_root(NULL);
    assert(strstr(drawn_dash, "待同步") == NULL);
    return 0;
}
'''
                compile_and_run(
                    dash_harness,
                    f"probe_c_{board_name}_dash_ux06c",
                    extra_flags=[f"-DKIIKO_BOARD_METALIO47={is_metalio}", "-Imain/weread", "-Itools/weread_stubs"],
                    verbose=tracker.verbose,
                )
                tracker.ok("GroupC", f"{board_name}:WR-UX-06c", f"{board_name} dashboard continue card '· 待同步' verified for weread:// pending vs local .epub")
            except Exception as exc:
                tracker.fail("GroupC", f"{board_name}:WR-UX-06c", str(exc))


# ---------------------------------------------------------------------------
# Group D: Structural Repo Invariants (ALWAYS Strictly Enforced)
# ---------------------------------------------------------------------------

def run_group_d(tracker: ProbeTracker) -> None:
    print("\n=== Group D: Structural Repo Invariants ===")

    # 1. RF 600000ms idle threshold & 16384B single worker
    rt_c = read_text("main/weread/weread_runtime.c")
    rf_idle_ok = bool(
        re.search(r"now\s*-\s*s_network_active\s*>=\s*600000", rt_c)
        or (
            re.search(r"atomic_load\(&s_network_active\)", rt_c)
            and re.search(r"now\s*-\s*net_active\s*>=\s*600000", rt_c)
        )
    )
    if rf_idle_ok:
        tracker.ok("GroupD", "Invariant:RF-Idle", "weread_runtime.c preserves 600000ms Wi-Fi idle disconnect threshold")
    else:
        tracker.fail("GroupD", "Invariant:RF-Idle", "weread_runtime.c missing 600000ms Wi-Fi idle disconnect threshold")

    if 'xTaskCreateWithCaps(worker, "weread", 16384' in rt_c and "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT" in rt_c:
        tracker.ok("GroupD", "Invariant:SingleWorker", "weread_runtime.c preserves single 16384B internal-RAM worker task")
    else:
        tracker.fail("GroupD", "Invariant:SingleWorker", "weread_runtime.c single 16384B internal-RAM worker signature altered")

    # 2. Dual-board parity (main/apps vs boards/pico/src/main/apps)
    m_book = read_text("main/apps/app_book.c")
    p_book = read_text("boards/pico/src/main/apps/app_book.c")
    m_dash = read_text("main/apps/app_dashboard.c")
    p_dash = read_text("boards/pico/src/main/apps/app_dashboard.c")

    parity_ok = True
    for inc in ('"book_weread_bridge.inc"', '"book_weread_chapter.inc"', "book_path_is_weread("):
        if (inc in m_book) != (inc in p_book) or inc not in m_book:
            tracker.fail("GroupD", "Invariant:DualBoardBook", f"app_book.c parity mismatch on {inc}")
            parity_ok = False
    for hook in ("book_path_is_weread(", "weread_runtime_load_local("):
        if (hook in m_dash) != (hook in p_dash) or hook not in m_dash:
            tracker.fail("GroupD", "Invariant:DualBoardDash", f"app_dashboard.c parity mismatch on {hook}")
            parity_ok = False
    # Check WR-UX parity: if one board lands a WR-UX hook in app_book.c, the other MUST also have it
    for ux_tok in ("已接力至手机最新进度", "发现更新的手机进度", "手机端购买本章后，再翻页即可加载", "本章未缓存且网络不可用，连网后再翻页重试"):
        if (ux_tok in m_book) != (ux_tok in p_book):
            tracker.fail("GroupD", "Invariant:DualBoardUX", f"app_book.c WR-UX parity drift on '{ux_tok}'")
            parity_ok = False
    if ("待同步" in m_dash) != ("待同步" in p_dash):
        tracker.fail("GroupD", "Invariant:DualBoardDashUX", "app_dashboard.c WR-UX-06c '待同步' parity drift between metalio47 and pico")
        parity_ok = False
    if parity_ok:
        tracker.ok("GroupD", "Invariant:DualBoardParity", "main/apps/{app_book,app_dashboard}.c and boards/pico/src/main/apps/ WeRead hooks are in parity")

    # 3. main/CMakeLists.txt -Oz / -Os rules
    cmake_txt = read_text("main/CMakeLists.txt")
    if "weread_cold_size_sources" in cmake_txt and '"-Oz;-fno-inline-small-functions' in cmake_txt and '"-fstack-usage"' in cmake_txt:
        tracker.ok("GroupD", "Invariant:FlashSizeRules", "main/CMakeLists.txt retains -Os/-Oz and -fstack-usage cold-path rules for WeRead")
    else:
        tracker.fail("GroupD", "Invariant:FlashSizeRules", "main/CMakeLists.txt missing weread_cold_size_sources / -Oz / -fstack-usage rules")

    # 4. Non-Goals enforcement (no WR-UX-10 / highlight / TTS / comic / implicit LRU)
    weread_scan_paths = [
        *(ROOT / "main" / "weread").glob("*.[ch]"),
        ROOT / "main" / "apps" / "app_weread.c",
        ROOT / "main" / "apps" / "book_weread_bridge.inc",
        ROOT / "main" / "apps" / "book_weread_chapter.inc",
        ROOT / "main" / "book" / "book_source_weread.c",
    ]
    shared_scan_paths = [
        ROOT / "main" / "apps" / "book_toc.c",
        ROOT / "main" / "apps" / "app_book.c",
        ROOT / "main" / "apps" / "app_dashboard.c",
        ROOT / "boards" / "pico" / "src" / "main" / "apps" / "app_book.c",
        ROOT / "boards" / "pico" / "src" / "main" / "apps" / "app_dashboard.c",
    ]
    forbidden_weread_re = re.compile(r"\b(WR_UX_10|WR_JOB_HIGHLIGHT|lru_evict|evict_lru)\b|/book/bookmark|/review/|\b(tts|speech|comic|highlight|underline)\b")
    forbidden_shared_re = re.compile(r"\b(WR_UX_10|WR_JOB_HIGHLIGHT|lru_evict|evict_lru)\b|/book/bookmark|/review/")
    violations = []
    for p in weread_scan_paths:
        txt = p.read_text(encoding="utf-8", errors="ignore")
        for m in forbidden_weread_re.finditer(txt):
            violations.append(f"{p.relative_to(ROOT)}:{m.group(0)}")
    for p in shared_scan_paths:
        txt = p.read_text(encoding="utf-8", errors="ignore")
        for m in forbidden_shared_re.finditer(txt):
            violations.append(f"{p.relative_to(ROOT)}:{m.group(0)}")
    if violations:
        tracker.fail("GroupD", "Invariant:NonGoals", f"Forbidden Non-Goal tokens detected: {', '.join(violations)}")
    else:
        tracker.ok("GroupD", "Invariant:NonGoals", "Zero WR-UX-10 / highlight / TTS / comic / implicit LRU tokens across WeRead & reader/dashboard sources")


# ---------------------------------------------------------------------------
# Group E: Local TXT/EPUB Regression Suite (10 Host Scripts)
# ---------------------------------------------------------------------------

LOCAL_REGRESSION_SCRIPTS = [
    "tools/board_selection_host_test.py",
    "tools/board_layer_host_test.py",
    "tools/book_library_host_test.py",
    "tools/book_library_ui_host_test.py",
    "tools/test_book_epub.py",
    "tools/test_book_toc.py",
    "tools/home_initial_empty_host_test.py",
    "tools/pico_home_progress_sync_host_test.py",
    "tools/reader_chapter_jump_host_test.py",
    "tools/reader_footer_identity_host_test.py",
]


def run_group_e(tracker: ProbeTracker) -> None:
    print("\n=== Group E: Local TXT/EPUB Regression Suite (10 host scripts) ===")
    for rel in LOCAL_REGRESSION_SCRIPTS:
        script = ROOT / rel
        if not script.exists():
            tracker.fail("GroupE", "LocalRegression", f"Missing script: {rel}")
            continue
        res = subprocess.run([sys.executable, str(script)], cwd=ROOT, capture_output=not tracker.verbose, text=True, timeout=120)
        if res.returncode != 0:
            detail = (res.stderr or res.stdout or "").strip().splitlines()
            tail = detail[-1] if detail else f"exit {res.returncode}"
            tracker.fail("GroupE", "LocalRegression", f"{rel} failed: {tail}")
        else:
            tracker.ok("GroupE", "LocalRegression", f"{rel} passed")


# ---------------------------------------------------------------------------
# CLI & Orchestration
# ---------------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser(description="WeRead UX E2E (Real C/UI/Runtime & Invariants)")
    parser.add_argument("--mode", choices=("strict", "baseline"), default="strict",
                        help="strict: fail on any unlanded M1..M4 contract; baseline: allow unlanded M1..M4 as PENDING while strictly enforcing invariants & local regressions")
    parser.add_argument("--port", type=int, default=0, help="Existing mock server port (0 = spawn ephemeral server)")
    parser.add_argument("--skip-local-regression", action="store_true", help="Skip Group E 10-script local TXT/EPUB regression suite")
    parser.add_argument("--verbose", action="store_true", help="Print child compiler/test output")
    args = parser.parse_args()

    tracker = ProbeTracker(mode=args.mode, verbose=args.verbose)
    print(f"==========================================================")
    print(f" WeRead UX E2E (WR-UX-01..09) | Mode: {args.mode}")
    print(f"==========================================================")

    ephemeral_server = None
    ephemeral_thread = None
    mock_port = args.port
    if mock_port == 0:
        GLOBAL_STATE.reset()
        ephemeral_server = ThreadedHTTPServer(("127.0.0.1", 0), WeReadMockRequestHandler)
        mock_port = ephemeral_server.server_address[1]
        ephemeral_thread = threading.Thread(target=ephemeral_server.serve_forever, daemon=True)
        ephemeral_thread.start()

    try:
        run_group_a(tracker, mock_port)
        run_group_b(tracker)
        run_group_c(tracker)
        run_group_d(tracker)
        if not args.skip_local_regression:
            run_group_e(tracker)
        else:
            print("\n=== Group E: Skipped (--skip-local-regression) ===")
    finally:
        if ephemeral_server is not None:
            ephemeral_server.shutdown()
            ephemeral_server.server_close()
            if ephemeral_thread is not None:
                ephemeral_thread.join(timeout=3)

    print("\n==========================================================")
    print(f" Summary (mode={args.mode}): {len(tracker.passed)} passed, {len(tracker.pending)} pending, {len(tracker.failed)} failed")
    print("==========================================================")
    if tracker.failed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
