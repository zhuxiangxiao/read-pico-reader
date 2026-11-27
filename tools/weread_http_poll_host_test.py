#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise production polling against real HTTP and IDF's unchanged HTTP parser.

The socket shim injects the documented SDK EAGAIN/partial-write contract; this is
not a real TLS handshake test. Firmware builds separately compile the real SDK.
"""
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import os
import json
import shutil
import subprocess
import tempfile
import threading
import time

root=Path(__file__).resolve().parents[1]
def find_sdk_parser():
    """Use the configured firmware SDK; never replace its parser with a mock."""
    configured=os.environ.get("IDF_PATH") or os.environ.get("KIIKO_IDF_PICO")
    executable=shutil.which("idf.py")
    candidates=[Path(configured).expanduser()] if configured else [
        *([Path(executable).resolve().parents[1]] if executable else []),
        Path.home()/"esp/esp-idf",
    ]
    for sdk in candidates:
        parser=sdk.resolve()/"components/http_parser"
        if all((parser/name).is_file() for name in ("http_parser.c","http_parser.h")):
            print(f"Testing production HTTP poller with SDK parser: {parser}",flush=True)
            return parser
    checked=", ".join(str(path.resolve()) for path in candidates)
    raise SystemExit(
        "WeRead HTTP polling tests require ESP-IDF's real components/http_parser/http_parser.c and .h. "
        "Set IDF_PATH (or KIIKO_IDF_PICO) to the SDK used for firmware, or activate its export.sh. "
        f"Checked: {checked}. This required test has not run."
    )
parser=find_sdk_parser()
out=Path(os.environ.get("WEREAD_TEST_BUILD_DIR",root/"build/weread-tests")).resolve()/"http-poll"
out.mkdir(parents=True,exist_ok=True)
flags=[os.environ.get("CC","cc"),"-std=c11","-D_DEFAULT_SOURCE","-DWEREAD_USE_ESP_HTTP_CLIENT","-DWEREAD_TEST_ASYNC_HTTP",
       "-pthread","-Wall","-Wextra","-Werror","-Wno-deprecated-declarations","-g","-fsanitize=address,undefined","-fno-omit-frame-pointer",
       "-Imain/weread","-Itools/weread_stubs","-I"+str(parser)]
names=["init","open","write","fetch_headers","read","get_status_code","get_content_length","is_complete_data_received"]
rename=[f"-Desp_http_client_{name}=actual_{name}" for name in names]
subprocess.run(flags+rename+["-c","tools/weread_stubs/esp_http_client_mock.c","-o",str(out/"mock.o")],cwd=root,check=True)
subprocess.run(flags+["tools/test_weread_http_poll.c",str(out/"mock.o"),str(parser/"http_parser.c"),
    *[f"main/weread/weread_{name}.c" for name in ("auth","client","store")],"tools/weread_stubs/cJSON.c","-lm","-o",str(out/"probe")],cwd=root,check=True)

expected=b'{"bookId":"fixture","chapterOffset":123,"summary":"partial transport"}'
errors=[]
requests=[]
auth_requests=[]
current_mode=0
qr_polls=0
class Handler(BaseHTTPRequestHandler):
    protocol_version="HTTP/1.1"
    def auth_json(self,data,status=200,cookies=()):
        raw=json.dumps(data,separators=(",",":")).encode()
        self.send_response(status)
        for cookie in cookies:self.send_header("Set-Cookie",cookie)
        self.send_header("Transfer-Encoding","chunked")
        self.send_header("Connection","close")
        self.end_headers()
        for i in range(0,len(raw),9):
            part=raw[i:i+9];self.wfile.write(f"{len(part):x}\r\n".encode()+part+b"\r\n")
        self.wfile.write(b"0\r\n\r\n");self.close_connection=True
    def auth_post(self,body):
        global qr_polls
        assert self.headers.get_all("Host")==["i.weread.qq.com"]
        payload=json.loads(body)
        auth_requests.append(self.path)
        if self.path.startswith("/weblogin"):
            if current_mode==23:
                self.auth_json({"errcode":-2013},401);return
            if current_mode==24:
                assert payload["code"]=="confirmed-code"
                self.auth_json({},cookies=("wr_vid=456; Path=/; Secure","wr_skey=cookie-token; Path=/; Secure","wr_rt=cookie-refresh; Path=/; Secure"));return
            assert payload["refreshToken"]=="fixture-refresh"
            self.auth_json({},cookies=("wr_vid=123; Path=/; Secure","wr_skey=fresh-token; Path=/; Secure","wr_rt=fresh-refresh; Path=/; Secure"));return
        if self.path.startswith("/web/getlogininfo"):
            assert payload=={"uid":"poll-uid"}
            qr_polls+=1
            if qr_polls==1:
                time.sleep(16)
                self.auth_json({"scan":0})
            else:self.auth_json({"succeed":True,"code":"confirmed-code","vid":"456","skey":"scan-token"})
            return
        assert self.path==f"/foreground{current_mode}"
        assert body==expected
        cookie=self.headers.get("Cookie","")
        fresh="fresh-token" in cookie
        if fresh and current_mode==21:self.auth_json({"succ":True})
        else:self.auth_json({"errcode":-2013},401)
    def do_POST(self):
        try:
            body=self.rfile.read(int(self.headers["Content-Length"]))
            if len(body)<int(self.headers["Content-Length"]):
                if current_mode<21:assert expected.startswith(body),body
                return
            if current_mode>=21:
                self.auth_post(body);return
            assert body==expected,(self.path,body)
            assert self.path.split("?")[1]=="utf8=%E4%B8%80&n=1",self.path
            assert self.headers.get_all("Host")==["i.weread.qq.com:443"],self.headers.get_all("Host")
            assert "fixture-token" in self.headers.get("Cookie","")
            requests.append(self.path)
            if self.path.startswith("/slow"):time.sleep(.3)
            if self.path.startswith("/fragmented"):
                self.wfile.write(b"HTTP/1.1 200 OK\r\nTransfer-Encod")
                self.wfile.flush()
                time.sleep(.07)
                self.wfile.write(b"ing: chunked\r\nConnection: close\r\n\r\n")
                self.wfile.flush()
            else:
                self.send_response(401 if self.path.startswith("/expired") else 200)
                self.send_header("Transfer-Encoding","chunked")
                self.send_header("Connection","close")
                self.end_headers()
            if current_mode==15:
                chunks=[b'{"succ":',b'true,"data":"']+[b'A'*1500]*6+[b'A'*2000]
            else:
                chunks=[b'{"errcode":',b'-2013}'] if self.path.startswith("/expired") else [b'{"succ":',b'true,"data":"',b'A'*11000,b'"}']
            for chunk in chunks:
                if current_mode==13:time.sleep(0.008)
                elif current_mode==15:time.sleep(0.012)
                self.wfile.write(f"{len(chunk):x}\r\n".encode()+chunk+b"\r\n")
                self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n")
            self.close_connection=True
        except (BrokenPipeError,ConnectionResetError):pass
        except Exception as exc:errors.append(repr(exc))
    def do_GET(self):
        try:
            if current_mode==24:
                assert self.path=="/web/getuid?platform=desktop"
                assert self.headers.get_all("Host")==["i.weread.qq.com"]
                auth_requests.append(self.path);self.auth_json({"uid":"poll-uid"});return
            hosts=self.headers.get_all("Host")
            assert len(hosts)==1 and hosts[0] in ("res.weread.qq.com","outside.example"),hosts
            if hosts[0]=="outside.example":assert not self.headers.get("Cookie") and not self.headers.get("accessToken")
            if self.path=="/redirect":
                self.send_response(302)
                self.send_header("Location","https://outside.example/file")
                data=b"intermediate response must not reach output"
                self.send_header("Content-Length",str(len(data)))
                self.end_headers();self.wfile.write(data);return
            assert self.path=="/file"
            self.send_response(200);self.send_header("Transfer-Encoding","chunked");self.send_header("Connection","close");self.end_headers()
            data=bytes(ord('A')+i%26 for i in range(256))
            for i in range(0,len(data),11):
                if current_mode==16 and i<120:time.sleep(0.003)
                part=data[i:i+11];self.wfile.write(f"{len(part):x}\r\n".encode()+part+b"\r\n");self.wfile.flush()
            self.wfile.write(b"0\r\n\r\n");self.close_connection=True
        except (BrokenPipeError,ConnectionResetError):pass
        except Exception as exc:errors.append(repr(exc))
    def log_message(self,*args):pass

server=ThreadingHTTPServer(("127.0.0.1",0),Handler)
thread=threading.Thread(target=server.serve_forever,daemon=True);thread.start()
try:
    for mode in range(25):
        current_mode=mode
        auth_requests.clear();qr_polls=0
        before=len(requests)
        with tempfile.TemporaryDirectory(prefix="weread-http-poll-") as cache:
            subprocess.run([str(out/"probe"),cache,str(mode)],check=True,timeout=25,
                           env=dict(os.environ,WEREAD_MOCK_PORT=str(server.server_port),ASAN_OPTIONS="abort_on_error=1:halt_on_error=1",UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
        if mode in (0,7,12,13,17,19):assert len(requests)==before+1,"partial body must never replay a POST"
        if mode in (21,22):assert len(auth_requests)==3 and sum("weblogin" in p for p in auth_requests)==1,auth_requests
        if mode==23:assert len(auth_requests)==2 and sum("weblogin" in p for p in auth_requests)==1,auth_requests
        if mode==24:assert len(auth_requests)==4 and qr_polls==2,auth_requests
    assert not errors,errors
finally:
    server.shutdown();server.server_close();thread.join()
print("PASS: production poller partial writes, EAGAIN at every stage, IDF-parser chunked reads, bound cancellation, URL/SNI/Host identity, redirects, limits, foreground single renewal/replay, revoked logout, cookie-only QR exchange, 16s long poll and background401 retention")
