#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 kiikoread
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""生产 C 实现的 HTTP 端到端验收。/ HTTP E2E for the production C implementation."""
import os
import json
import runpy
from pathlib import Path
import subprocess
import tempfile
import threading
import time

from weread_mock_server import GLOBAL_STATE, ThreadedHTTPServer, WeReadMockRequestHandler, encrypt_encrypt_key


class NativeHandler(WeReadMockRequestHandler):
    cookie_only=False
    chapter_two=b""
    def _handle_chapter_download(self,query):
        if query.get("chapters",[""])[0].split(",")[-1]=="2":
            self.send_response(200)
            self.send_header("Content-Length",str(len(self.chapter_two)))
            self.send_header("encryptkey",encrypt_encrypt_key("test_password_123",str(GLOBAL_STATE.current_vid)))
            self.end_headers();self.wfile.write(self.chapter_two);return
        return super()._handle_chapter_download(query)
    def _send_json(self,status,data,extra_headers=None):
        # 超过旧3秒窗口的正常登录仍应成功；强制上报须服从更短截止时间。
        # Normal login outlasts the old3s window; forced reporting still obeys its shorter deadline.
        if self.cookie_only and self.path.startswith("/web/getuid"):time.sleep(3.3)
        # 扫码接口是长轮询，超过普通API的15秒仍须接收授权结果。
        # QR long polls must receive confirmation beyond the ordinary15s API limit.
        if self.cookie_only and self.path.startswith("/web/getlogininfo"):time.sleep(16.2)
        delay=GLOBAL_STATE.endpoint_errors.get("/book/read",{}).get("delay",0)
        if self.path.startswith("/book/read") and delay:time.sleep(delay)
        if self.cookie_only and self.path.startswith("/weblogin") and extra_headers:
            data={"data":{}}
        if self.cookie_only and self.path.startswith("/web/getlogininfo") and "vid" in data:
            data={"data":{"user_vid":data["vid"],"wr_skey":data["skey"],"code":data["code"]}}
        try:return super()._send_json(status,data,extra_headers)
        except (BrokenPipeError,ConnectionResetError):return


def main():
    root = Path(__file__).resolve().parents[1]
    build = root / "build/weread-tests"
    build.mkdir(parents=True, exist_ok=True)
    binary = build / "native-e2e"
    sources = [
        "tools/test_weread_native_e2e.c", "tools/weread_stubs/esp_http_client_mock.c",
        "tools/weread_stubs/cJSON.c", "main/book/book_source_weread.c",
        "main/book/book_weread_crypto.c", "main/book/html_text.c",
        *[f"main/weread/weread_{name}.c" for name in ("auth", "client", "store", "shelf", "toc", "chapter", "progress", "position", "runtime")],
    ]
    memory_flags=[]
    if os.environ.get("WEREAD_NATIVE_MEMORY")=="1":
        sources.append("tools/weread_stubs/heap_caps_memory_mock.c")
        memory_flags=["-DWEREAD_TEST_MEMORY","-Dmalloc=weread_test_malloc","-Dcalloc=weread_test_calloc",
                      "-Drealloc=weread_test_realloc","-Dfree=weread_test_free"]
    subprocess.run([
        os.environ.get("CC", "/usr/bin/cc"), "-std=c11", "-pthread", "-D_DEFAULT_SOURCE", "-DWEREAD_USE_ESP_HTTP_CLIENT",
        "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-g",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-Itools/weread_stubs", "-Imain", "-Imain/book", "-Imain/weread",
        *memory_flags,*sources, "-lz", "-lm", "-o", str(binary),
    ], cwd=root, check=True)
    fixture=runpy.run_path(str(root/"tools/fixtures/weread/generate_fixtures.py"))
    NativeHandler.chapter_two=fixture["_make_zipcrypto_zip"]({
        "info.txt":json.dumps({"bookId":"test_book_101","bookVersion":100,"chapters":[{"chapterUid":2,"files":["Text/second.xhtml"]}]}).encode(),
        "Text/second.xhtml":"<html><body><p>第二章😀预读成功</p></body></html>".encode(),
    },b"test_password_123")
    GLOBAL_STATE.reset()
    server = ThreadedHTTPServer(("127.0.0.1", 0), NativeHandler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        for cookie_only in (False,True):
            GLOBAL_STATE.reset();NativeHandler.cookie_only=cookie_only
            with tempfile.TemporaryDirectory(prefix="weread-native-e2e-") as cache:
                port = str(server.server_address[1])
                env = dict(os.environ, WEREAD_MOCK_PORT=port, WEREAD_API_BASE=f"http://127.0.0.1:{port}",
                           WEREAD_WEB_BASE=f"http://127.0.0.1:{port}", ASAN_OPTIONS="abort_on_error=1:halt_on_error=1",
                           UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
                subprocess.run([str(binary), cache], cwd=root, env=env, check=True, timeout=90)
            history = GLOBAL_STATE.get_history()
            paths = [r["path"] for r in history]
            assert any(p.startswith("/book/chapterdownload") for p in paths)
            assert not GLOBAL_STATE.security_violations
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=3)


if __name__ == "__main__":
    main()
