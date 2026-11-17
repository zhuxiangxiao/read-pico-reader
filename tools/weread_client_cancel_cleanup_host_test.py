#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Cancellation must not wait on an HTTP handle already being cleaned up."""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import subprocess
import threading

root=Path(__file__).resolve().parents[1]
out=root/"build/night-optimization/cancel-cleanup"
out.mkdir(parents=True,exist_ok=True)
flags=[os.environ.get("CC","/usr/bin/cc"),"-std=c11","-D_DEFAULT_SOURCE","-DWEREAD_USE_ESP_HTTP_CLIENT",
       "-pthread","-Wall","-Wextra","-Werror","-Wno-deprecated-declarations","-g",
       "-fsanitize=address,undefined","-fno-omit-frame-pointer","-Imain/weread","-Itools/weread_stubs"]
subprocess.run(flags+["-Desp_http_client_cleanup=actual_http_cleanup","-c","tools/weread_stubs/esp_http_client_mock.c","-o",str(out/"mock.o")],cwd=root,check=True)
subprocess.run(flags+["tools/test_weread_client_cancel_cleanup.c",str(out/"mock.o"),
    *[f"main/weread/weread_{name}.c" for name in ("auth","client","store")],
    "tools/weread_stubs/cJSON.c","-lm","-o",str(out/"probe")],cwd=root,check=True)

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body=b'{"succ":1}'
        self.send_response(200)
        self.send_header("Content-Length",str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def log_message(self,*args):pass

server=ThreadingHTTPServer(("127.0.0.1",0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True)
thread.start()
try:
    subprocess.run([str(out/"probe"),f"http://127.0.0.1:{server.server_port}/ping"],check=True,timeout=8,
                   env=dict(os.environ,WEREAD_MOCK_PORT=str(server.server_port),ASAN_OPTIONS="abort_on_error=1:halt_on_error=1",UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
finally:
    server.shutdown();server.server_close();thread.join()
