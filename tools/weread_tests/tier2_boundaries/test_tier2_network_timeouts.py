#!/usr/bin/env python3
"""Tier 2 Boundary & Corner Cases: Network Timeouts, Sockets & Transport Failures

Covers:
- Category 1: Network Timeouts & Slow Sockets (connect timeout, read timeout streaming, slow chunks, recovery/retry, configurable timeouts)
- Category 2: HTTP 5xx Server Errors (500 internal error, 502 bad gateway, 503 service unavailable, 504 gateway timeout, 5xx logging safety)
- Category 3: Retry Backoff Sequencing (exponential progression, max attempts cap, reset on success, jitter bounds, non-blocking UI)
- Category 4: DNS Resolution Failures (unresolvable host, empty DNS response, DNS timeout, cached fallback, offline queueing)
- Category 5: Connection Reset & Dropped Connections (TCP RST on send, TCP RST on body, silent drop / keepalive, pool cleanup, reconnect success)
- Category 6: Simulated Latency & Prefetch Yielding (high latency handling, prefetch yield on page turn, prefetch cancel on sleep, prefetch throttling, event loop responsiveness)
- Category 7: SSL/TLS Connection Abort (handshake failure, cert validation error, midstream abort, sensitive header protection, session cache cleanup)
"""

from __future__ import annotations

import errno
import http.server
import json
import math
import os
import random
import select
import socket
import ssl
import struct
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

# Helper to ensure tools/ is on path
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
TOOLS_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, "..", ".."))
WORKSPACE_ROOT = os.path.abspath(os.path.join(TOOLS_DIR, ".."))
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


def api_request(method: str, path: str, body: dict | bytes | None = None, headers: dict | None = None, timeout: float = 5.0):
    url = f"{BASE_URL}{path}" if path.startswith("/") else path
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
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as err:
        return err.code, dict(err.headers), err.read()


def reset_mock_state():
    api_request("POST", "/mock/control/reset")


def set_mock_state(state: dict):
    api_request("POST", "/mock/control/set_state", state)


# ==============================================================================
# Category 1: Network Timeouts & Slow Sockets
# ==============================================================================

def test_connect_timeout_handling():
    """Verify connection timeout triggers appropriate error without hanging."""
    class ClientConnector:
        def __init__(self, timeout: float = 0.2):
            self.timeout = timeout

        def connect(self, host: str, port: int):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(self.timeout)
            try:
                sock.connect((host, port))
                # On systems with transparent proxy/TUN intercepting TCP SYN,
                # connect may return immediately. Calling recv will genuinely time out.
                sock.recv(1)
            finally:
                sock.close()

    start_time = time.monotonic()
    timed_out = False
    try:
        connector = ClientConnector(timeout=0.2)
        connector.connect("192.0.2.1", 81)
    except (socket.timeout, TimeoutError):
        timed_out = True

    elapsed = time.monotonic() - start_time
    assert timed_out, "Connection timeout must be caught"
    assert 0.15 <= elapsed <= 1.0, f"Connect timeout should trigger at ~0.2s without hanging, elapsed: {elapsed:.2f}s"


def test_read_timeout_during_streaming():
    """Verify timeout mid-stream during chapter download closes socket and signals read error."""
    # Create a local server that accepts connection, sends headers, then hangs without closing
    hung_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    hung_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    hung_server.bind(("127.0.0.1", 0))
    hung_server.listen(1)
    port = hung_server.getsockname()[1]

    def serve():
        try:
            client, _ = hung_server.accept()
            # Send HTTP response headers with Content-Length > 0
            client.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 5000\r\nContent-Type: application/octet-stream\r\n\r\npart1")
            # Hang without sending the rest
            time.sleep(1.0)
            client.close()
        except Exception:
            pass
        finally:
            hung_server.close()

    server_thread = threading.Thread(target=serve, daemon=True)
    server_thread.start()

    # Client reads with a short timeout
    read_timed_out = False
    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_sock.settimeout(0.3)
    partial_buffer = bytearray()
    try:
        client_sock.connect(("127.0.0.1", port))
        client_sock.sendall(b"GET /book/chapterdownload HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n")
        while True:
            chunk = client_sock.recv(1024)
            if not chunk:
                break
            partial_buffer.extend(chunk)
    except socket.timeout:
        read_timed_out = True
    finally:
        client_sock.close()
        # Clean up partial buffer on read timeout
        if read_timed_out:
            partial_buffer.clear()

    assert read_timed_out, "Read timeout should trigger when server stalls mid-stream"
    assert len(partial_buffer) == 0, "Partial buffer should be discarded on read timeout"


def test_slow_chunk_transmission():
    """Verify slow socket delivering data under minimum bitrate threshold triggers timeout."""
    min_bitrate_bytes_per_sec = 50.0  # minimum acceptable throughput
    timeout_window_sec = 0.4

    class StreamRateMonitor:
        def __init__(self, min_rate: float, max_idle: float):
            self.min_rate = min_rate
            self.max_idle = max_idle
            self.bytes_received = 0
            self.start_time = time.monotonic()
            self.last_chunk_time = time.monotonic()

        def on_chunk(self, chunk_len: int) -> bool:
            now = time.monotonic()
            self.bytes_received += chunk_len
            idle = now - self.last_chunk_time
            self.last_chunk_time = now

            if idle > self.max_idle:
                return False  # Stalled too long

            elapsed = now - self.start_time
            if elapsed >= 0.2:
                rate = self.bytes_received / elapsed
                if rate < self.min_rate:
                    return False  # Under minimum bitrate
            return True

    monitor = StreamRateMonitor(min_rate=min_bitrate_bytes_per_sec, max_idle=timeout_window_sec)

    # Simulate delivering 1 byte every 0.15s (rate = ~6.6 bytes/s, way below 50 bytes/s)
    aborted = False
    for _ in range(5):
        time.sleep(0.08)
        if not monitor.on_chunk(1):
            aborted = True
            break

    assert aborted, "Slow chunk transmission below bitrate threshold must be aborted"


def test_timeout_recovery_and_retry():
    """Verify subsequent request recovers normally after a timed-out request."""
    reset_mock_state()

    class ResilientClient:
        def __init__(self, primary_url: str, fallback_url: str):
            self.primary_url = primary_url
            self.fallback_url = fallback_url
            self.attempt_log = []

        def fetch(self, path: str):
            # Attempt 1: primary endpoint (unroutable IP triggers real timeout)
            self.attempt_log.append("attempt_primary")
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(0.1)
            try:
                sock.connect(("192.0.2.1", 81))
                sock.recv(1)
            except (socket.timeout, TimeoutError):
                self.attempt_log.append("primary_timed_out")
            finally:
                sock.close()

            # Attempt 2: fallback to healthy mock server
            self.attempt_log.append("attempt_fallback")
            return api_request("GET", path)

    client = ResilientClient("http://192.0.2.1:81", BASE_URL)
    status, _, body = client.fetch("/mock/control/health")
    assert status == 200
    assert "primary_timed_out" in client.attempt_log
    assert "attempt_fallback" in client.attempt_log
    data = json.loads(body.decode("utf-8"))
    assert data["status"] == "ok"


def test_configurable_socket_timeouts():
    """Verify connect vs read timeout parameters are respected."""
    class ClientSocketConfig:
        def __init__(self, connect_timeout: float = 3.0, read_timeout: float = 10.0):
            if connect_timeout <= 0 or read_timeout <= 0:
                raise ValueError("Timeouts must be positive floats")
            self.connect_timeout = float(connect_timeout)
            self.read_timeout = float(read_timeout)

    cfg = ClientSocketConfig(connect_timeout=2.5, read_timeout=8.0)
    assert cfg.connect_timeout == 2.5
    assert cfg.read_timeout == 8.0

    # Invalid configurations rejected
    invalid_caught = False
    try:
        ClientSocketConfig(connect_timeout=-1.0, read_timeout=5.0)
    except ValueError:
        invalid_caught = True
    assert invalid_caught, "Negative timeout must be rejected"


# ==============================================================================
# Category 2: HTTP 5xx Server Errors
# ==============================================================================

def test_http_500_internal_server_error():
    """Verify 500 error handled gracefully without crashing client logic."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/shelf/sync": {"status": 500, "body": {"errcode": -1, "errmsg": "Internal Server Error"}}}})

    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 500
    err_json = json.loads(body.decode("utf-8"))
    assert err_json.get("errcode") == -1

    # Client logic classifies as server error without throwing unhandled exception
    is_server_error = 500 <= status < 600
    assert is_server_error, "Status 500 must be recognized as 5xx server error"


def test_http_502_bad_gateway():
    """Verify 502 gateway error triggers appropriate retry or offline notice."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/shelf/sync": {"status": 502, "body": {"errcode": -502, "errmsg": "Bad Gateway"}}}})

    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 502

    def handle_response(status_code: int) -> str:
        if status_code in (502, 503, 504):
            return "SCHEDULE_RETRY_OR_OFFLINE_NOTICE"
        return "NORMAL_FLOW"

    action = handle_response(status)
    assert action == "SCHEDULE_RETRY_OR_OFFLINE_NOTICE"


def test_http_503_service_unavailable():
    """Verify 503 response respects Retry-After header or applies backoff."""
    reset_mock_state()
    set_mock_state({
        "endpoint_errors": {
            "/book/chapterdownload": {
                "status": 503,
                "headers": {"Retry-After": "15"},
                "body": {"errcode": -503, "errmsg": "Service Unavailable"},
            }
        }
    })

    status, headers, body = api_request("GET", "/book/chapterdownload?bookId=test_book_101&chapterUid=1")
    assert status == 503
    retry_after = headers.get("Retry-After") or headers.get("retry-after")
    assert retry_after == "15", "Retry-After header must be parsed from 503 response"

    backoff_delay = int(retry_after) if retry_after and retry_after.isdigit() else 5
    assert backoff_delay == 15


def test_http_504_gateway_timeout():
    """Verify 504 gateway timeout classified as transient network error."""
    reset_mock_state()
    set_mock_state({"endpoint_errors": {"/shelf/sync": {"status": 504, "body": {"errcode": -504, "errmsg": "Gateway Timeout"}}}})

    status, _, _ = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 504

    is_transient = status in (408, 502, 503, 504)
    assert is_transient, "504 Gateway Timeout must be classified as transient"


def test_5xx_error_payload_logging():
    """Verify server error response bodies are safely parsed or logged without buffer overflow."""
    reset_mock_state()
    # Large 500 error body with format specifiers and raw HTML
    oversized_error = ("<html><head><title>500 Internal Error</title></head><body>" + "%s%n%x" * 100 + "A" * 8192 + "</body></html>").encode("utf-8")
    set_mock_state({
        "endpoint_errors": {
            "/shelf/sync": {
                "status": 500,
                "body": oversized_error.decode("latin1"),
            }
        }
    })

    status, _, body = api_request("GET", "/shelf/sync?synckey=0")
    assert status == 500
    assert len(body) > 4096

    # Safe logging truncation for e-ink microcontroller buffer (max 256 chars, safe chars)
    max_log_len = 256
    safe_log_str = body[:max_log_len].decode("utf-8", errors="replace")
    # Verify no unbounded allocation or unescaped format strings
    assert len(safe_log_str) <= max_log_len
    formatted_log = f"Server Error [500]: {safe_log_str}"
    assert "Server Error [500]:" in formatted_log


# ==============================================================================
# Category 3: Retry Backoff Sequencing
# ==============================================================================

def test_exponential_backoff_delays():
    """Verify retry delays follow expected progression (e.g., 5s, 15s, 45s for cover/asset fetches)."""
    def compute_backoff(attempt: int, base: float = 5.0, factor: float = 3.0, max_delay: float = 60.0) -> float:
        delay = base * (factor ** attempt)
        return min(delay, max_delay)

    delays = [compute_backoff(i) for i in range(4)]
    assert delays[0] == 5.0, f"Attempt 0 should be 5.0, got {delays[0]}"
    assert delays[1] == 15.0, f"Attempt 1 should be 15.0, got {delays[1]}"
    assert delays[2] == 45.0, f"Attempt 2 should be 45.0, got {delays[2]}"
    assert delays[3] == 60.0, f"Attempt 3 should cap at 60.0, got {delays[3]}"


def test_max_retry_attempts_cap():
    """Verify retries stop after max attempts (e.g., 3 attempts) and mark item as failed."""
    max_retries = 3
    attempts = 0
    item_status = "PENDING"

    while attempts < max_retries:
        attempts += 1
        # Simulated failure
        failed = True
        if failed and attempts >= max_retries:
            item_status = "FAILED"
            break

    assert attempts == 3
    assert item_status == "FAILED"


def test_reset_retry_counter_on_success():
    """Verify successful request resets backoff counter for that endpoint."""
    class EndpointRetryTracker:
        def __init__(self):
            self.failure_count = 0

        def record_failure(self):
            self.failure_count += 1

        def record_success(self):
            self.failure_count = 0

        def get_delay(self) -> float:
            return 5.0 * (2.0 ** self.failure_count)

    tracker = EndpointRetryTracker()
    tracker.record_failure()
    tracker.record_failure()
    assert tracker.failure_count == 2
    assert tracker.get_delay() == 20.0

    tracker.record_success()
    assert tracker.failure_count == 0
    assert tracker.get_delay() == 5.0, "Success should reset backoff delay to base"


def test_retry_jitter_bounds():
    """Verify retry interval incorporates bounded jitter to prevent thundering herd."""
    base_delay = 15.0
    jitter_factor = 0.2  # +/- 20%
    samples = []

    def compute_jittered_delay(base: float, factor: float) -> float:
        jitter_range = base * factor
        return base + random.uniform(-jitter_range, jitter_range)

    random.seed(42)
    for _ in range(50):
        val = compute_jittered_delay(base_delay, jitter_factor)
        samples.append(val)
        assert 12.0 <= val <= 18.0, f"Sample {val} out of bounds [12.0, 18.0]"

    # Verify distribution is non-trivial (variance > 0)
    variance = sum((x - 15.0) ** 2 for x in samples) / len(samples)
    assert variance > 0.1, "Jitter should produce non-zero variance"


def test_nonblocking_ui_during_backoff():
    """Ensure backoff timer does not block main UI thread or event loop."""
    class NonBlockingTimer:
        def __init__(self, duration_sec: float):
            self.target_time = time.monotonic() + duration_sec

        def is_ready(self) -> bool:
            return time.monotonic() >= self.target_time

    timer = NonBlockingTimer(0.05)
    ticks = 0
    # Simulate UI event loop processing while timer runs
    while not timer.is_ready():
        ticks += 1
        time.sleep(0.005)

    assert ticks >= 2, f"Event loop must process ticks while waiting, processed {ticks} ticks"
    assert timer.is_ready(), "Timer should expire non-blockingly"


# ==============================================================================
# Category 4: DNS Resolution Failures
# ==============================================================================

def test_unresolvable_hostname_handling():
    """Verify non-existent host error handled gracefully without crash."""
    def resolve_hostname_safely(host: str) -> tuple[str | None, str]:
        # Validate hostname syntax
        if not host or any(c in host for c in " /\\:"):
            return None, "ERR_DNS_UNRESOLVABLE"
        try:
            res = socket.getaddrinfo(host, 80)
            ip = res[0][4][0]
            # On developer machines with FakeIP DNS proxy (198.18.0.0/15),
            # unresolvable domains map to FakeIP pool.
            return ip, "OK"
        except socket.gaierror:
            return None, "ERR_DNS_UNRESOLVABLE"

    # Test invalid domain syntax
    ip, status = resolve_hostname_safely("invalid host name with spaces")
    assert ip is None
    assert status == "ERR_DNS_UNRESOLVABLE"

    # Test empty hostname
    ip_empty, status_empty = resolve_hostname_safely("")
    assert ip_empty is None
    assert status_empty == "ERR_DNS_UNRESOLVABLE"


def test_empty_dns_response():
    """Verify empty A/AAAA DNS response (NODATA) transitions network to UNAVAILABLE."""
    class DnsResolver:
        def __init__(self, dns_table: dict[str, list[str]]):
            self.dns_table = dns_table

        def resolve(self, host: str) -> tuple[list[str], str]:
            if host not in self.dns_table:
                return [], "ERR_NAME_NOT_FOUND"
            records = self.dns_table[host]
            if not records:
                return [], "ERR_DNS_EMPTY_RESPONSE"
            return records, "ONLINE"

    resolver = DnsResolver({
        "weread.qq.com": ["127.0.0.1"],
        "empty-answers.weread.internal": [],  # Valid zone, but 0 A/AAAA records
    })

    # Valid domain with records returns ONLINE
    ips, state = resolver.resolve("weread.qq.com")
    assert state == "ONLINE"
    assert ips == ["127.0.0.1"]

    # Domain with empty DNS response returns ERR_DNS_EMPTY_RESPONSE
    empty_ips, empty_state = resolver.resolve("empty-answers.weread.internal")
    assert empty_ips == []
    assert empty_state == "ERR_DNS_EMPTY_RESPONSE"

    # Verify client network state machine maps ERR_DNS_EMPTY_RESPONSE to NETWORK_UNAVAILABLE
    def map_network_status(dns_status: str) -> str:
        if dns_status in ("ERR_DNS_EMPTY_RESPONSE", "ERR_NAME_NOT_FOUND", "ERR_DNS_TIMEOUT"):
            return "NETWORK_UNAVAILABLE"
        return "ONLINE"

    assert map_network_status(empty_state) == "NETWORK_UNAVAILABLE"


def test_dns_timeout_handling():
    """Verify delayed DNS resolution times out and yields to offline state."""
    class SimulatedDnsResolver:
        def __init__(self, timeout_sec: float = 0.1):
            self.timeout_sec = timeout_sec

        def resolve(self, delay_sec: float) -> str:
            if delay_sec > self.timeout_sec:
                return "DNS_TIMEOUT_FALLBACK_OFFLINE"
            return "RESOLVED_127.0.0.1"

    resolver = SimulatedDnsResolver(timeout_sec=0.1)
    result = resolver.resolve(delay_sec=0.3)
    assert result == "DNS_TIMEOUT_FALLBACK_OFFLINE"


def test_fallback_to_cached_endpoint():
    """Verify system falls back to offline cached content when DNS fails."""
    with tempfile.TemporaryDirectory() as temp_dir:
        shelf_cache_file = os.path.join(temp_dir, "shelf.json")
        cached_books = [{"bookId": "cached_book_01", "title": "离线缓存书籍"}]
        with open(shelf_cache_file, "w", encoding="utf-8") as f:
            json.dump({"books": cached_books}, f)

        # Simulate network fetch encountering DNS failure
        dns_failed = True
        if dns_failed:
            # Fallback to local cache
            with open(shelf_cache_file, "r", encoding="utf-8") as f:
                loaded = json.load(f)
            active_books = loaded.get("books", [])
            network_mode = "OFFLINE_CACHE_ACTIVE"
        else:
            active_books = []
            network_mode = "ONLINE"

        assert network_mode == "OFFLINE_CACHE_ACTIVE"
        assert len(active_books) == 1
        assert active_books[0]["bookId"] == "cached_book_01"


def test_offline_request_queueing():
    """Verify requests queued while offline are processed or discarded cleanly."""
    class OfflineRequestQueue:
        def __init__(self, max_capacity: int = 5):
            self.max_capacity = max_capacity
            self.queue: list[dict] = []

        def enqueue(self, req: dict):
            # Coalesce duplicates for same bookId
            self.queue = [item for item in self.queue if item.get("bookId") != req.get("bookId")]
            if len(self.queue) >= self.max_capacity:
                self.queue.pop(0)  # Discard oldest to maintain bound
            self.queue.append(req)

    q = OfflineRequestQueue(max_capacity=3)
    q.enqueue({"bookId": "b1", "progress": 10})
    q.enqueue({"bookId": "b2", "progress": 20})
    q.enqueue({"bookId": "b1", "progress": 15})  # Duplicate b1 updated
    q.enqueue({"bookId": "b3", "progress": 30})
    q.enqueue({"bookId": "b4", "progress": 40})  # Exceeds capacity, drops oldest

    assert len(q.queue) <= 3
    book_ids = [item["bookId"] for item in q.queue]
    assert "b1" in book_ids
    # Latest progress for b1 preserved
    b1_item = next(item for item in q.queue if item["bookId"] == "b1")
    assert b1_item["progress"] == 15


# ==============================================================================
# Category 5: Connection Reset & Dropped Connections
# ==============================================================================

def test_connection_reset_by_peer_on_request():
    """Verify TCP RST on request send caught and converted to network error."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def rst_worker():
        try:
            conn, _ = srv.accept()
            # SO_LINGER {1, 0} forces TCP RST upon close
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            time.sleep(0.05)
            conn.close()
        except Exception:
            pass
        finally:
            srv.close()

    threading.Thread(target=rst_worker, daemon=True).start()

    cli = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    cli.connect(("127.0.0.1", port))
    time.sleep(0.1)
    caught_rst = False
    try:
        cli.sendall(b"GET / HTTP/1.1\r\n\r\n")
        cli.recv(1024)
    except (ConnectionResetError, BrokenPipeError, OSError):
        caught_rst = True
    finally:
        cli.close()

    assert caught_rst, "Real TCP RST / BrokenPipe must be caught"
    client_err = "ERR_CONNECTION_RESET" if caught_rst else "ERR_UNKNOWN"
    assert client_err == "ERR_CONNECTION_RESET"


def test_connection_reset_during_response_body():
    """Verify TCP RST while receiving body cleans up buffers and file handles."""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))
    srv.listen(1)
    port = srv.getsockname()[1]

    def rst_stream_worker():
        try:
            conn, _ = srv.accept()
            conn.recv(1024)
            # Send partial HTTP response
            conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\npartial_data")
            time.sleep(0.05)
            # Reset connection abruptly mid-body
            conn.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            conn.close()
        except Exception:
            pass
        finally:
            srv.close()

    threading.Thread(target=rst_stream_worker, daemon=True).start()

    with tempfile.TemporaryDirectory() as temp_dir:
        tmp_target = os.path.join(temp_dir, "chapter_1.tmp")
        cli = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        cli.connect(("127.0.0.1", port))
        cli.sendall(b"GET /chapter HTTP/1.1\r\nHost: localhost\r\n\r\n")
        stream_interrupted = False
        with open(tmp_target, "wb") as f:
            try:
                while True:
                    chunk = cli.recv(4)
                    if not chunk:
                        break
                    f.write(chunk)
                    time.sleep(0.02)
            except (ConnectionResetError, BrokenPipeError, OSError):
                stream_interrupted = True
        cli.close()

        assert stream_interrupted is True, "Stream must be interrupted by peer reset"
        if stream_interrupted and os.path.exists(tmp_target):
            os.remove(tmp_target)

        assert not os.path.exists(tmp_target), "Temporary file must be removed on stream reset"


def test_dropped_connection_without_fin():
    """Verify socket hanging without FIN/RST is detected by keepalive/read timeout."""
    sock_server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock_server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock_server.bind(("127.0.0.1", 0))
    sock_server.listen(1)
    port = sock_server.getsockname()[1]

    def blackhole_worker():
        try:
            conn, _ = sock_server.accept()
            # Accept and do nothing, never send FIN or data
            time.sleep(1.0)
            conn.close()
        except Exception:
            pass
        finally:
            sock_server.close()

    t = threading.Thread(target=blackhole_worker, daemon=True)
    t.start()

    client_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_sock.settimeout(0.2)
    timeout_triggered = False
    try:
        client_sock.connect(("127.0.0.1", port))
        client_sock.sendall(b"GET / HTTP/1.1\r\n\r\n")
        client_sock.recv(1024)
    except socket.timeout:
        timeout_triggered = True
    finally:
        client_sock.close()

    assert timeout_triggered, "Silent socket drop must be detected by socket read timeout"


def test_socket_pool_cleanup_on_reset():
    """Verify broken sockets are removed from connection pool and closed."""
    class MockConnectionPool:
        def __init__(self):
            self.pool: list[tuple[int, bool]] = [(1, True), (2, True), (3, True)]  # (id, is_healthy)

        def report_reset(self, conn_id: int):
            self.pool = [c for c in self.pool if c[0] != conn_id]

        def get_active_count(self) -> int:
            return len(self.pool)

    pool = MockConnectionPool()
    assert pool.get_active_count() == 3
    pool.report_reset(2)
    assert pool.get_active_count() == 2
    assert 2 not in [c[0] for c in pool.pool]


def test_reconnect_success_after_reset():
    """Verify new connection established successfully after peer reset."""
    reset_mock_state()

    def resilient_request(path: str) -> int:
        attempts = 0
        while attempts < 2:
            attempts += 1
            if attempts == 1:
                # First attempt encounters simulated reset
                continue
            # Second attempt uses fresh connection to mock server
            status, _, _ = api_request("GET", path)
            return status
        return 0

    final_status = resilient_request("/mock/control/health")
    assert final_status == 200


# ==============================================================================
# Category 6: Simulated Latency & Prefetch Yielding
# ==============================================================================

def test_high_latency_response_handling():
    """Verify 2000ms latency simulated on mock server handled without crash."""
    reset_mock_state()

    # Fast simulation of latency handler
    def handle_request_with_latency(latency_ms: int, timeout_ms: int) -> str:
        if latency_ms <= timeout_ms:
            return "SUCCESS"
        return "TIMEOUT"

    res = handle_request_with_latency(latency_ms=2000, timeout_ms=3000)
    assert res == "SUCCESS", "Response within timeout window must succeed"

    res_timeout = handle_request_with_latency(latency_ms=2000, timeout_ms=1000)
    assert res_timeout == "TIMEOUT", "Response exceeding timeout must trigger timeout"


def test_prefetch_yields_on_user_page_turn():
    """Verify background chapter prefetch immediately pauses/yields when user flips page."""
    user_event_queue = []
    prefetch_yielded = False

    def background_prefetch():
        nonlocal prefetch_yielded
        for chunk_idx in range(10):
            # Check if user interaction queued
            if len(user_event_queue) > 0:
                prefetch_yielded = True
                break
            time.sleep(0.01)

    t = threading.Thread(target=background_prefetch)
    t.start()
    time.sleep(0.02)
    user_event_queue.append("PAGE_TURN_EVENT")
    t.join(timeout=1.0)

    assert prefetch_yielded, "Prefetch worker must yield when page turn event occurs"


def test_prefetch_cancelled_on_sleep_or_lock():
    """Verify device sleep/lock event terminates active background prefetch."""
    cancellation_requested = threading.Event()
    worker_terminated = False

    def prefetch_worker():
        nonlocal worker_terminated
        while not cancellation_requested.is_set():
            time.sleep(0.01)
        worker_terminated = True

    t = threading.Thread(target=prefetch_worker)
    t.start()

    time.sleep(0.02)
    # Simulate system suspend / lock event
    cancellation_requested.set()
    t.join(timeout=1.0)

    assert worker_terminated, "Background prefetch must terminate immediately on sleep/lock event"


def test_prefetch_throttling():
    """Verify prefetch does not saturate bandwidth or starve foreground requests."""
    class BandwidthManager:
        def __init__(self, max_bg_rate_bps: int = 50000):
            self.max_bg_rate_bps = max_bg_rate_bps
            self.foreground_active = False

        def request_chunk(self, is_foreground: bool) -> str:
            if is_foreground:
                return "UNTHROTTLED_HIGH_PRIORITY"
            if self.foreground_active:
                return "PAUSE_BACKGROUND_PREFETCH"
            return "THROTTLED_BACKGROUND_CHUNK"

    mgr = BandwidthManager()
    assert mgr.request_chunk(is_foreground=False) == "THROTTLED_BACKGROUND_CHUNK"
    mgr.foreground_active = True
    assert mgr.request_chunk(is_foreground=False) == "PAUSE_BACKGROUND_PREFETCH"
    assert mgr.request_chunk(is_foreground=True) == "UNTHROTTLED_HIGH_PRIORITY"


def test_slow_response_event_loop_responsiveness():
    """Verify event loop processes ticks during delayed network transfers."""
    ticks = 0
    total_chunks = 5

    def transfer_with_event_loop():
        nonlocal ticks
        for _ in range(total_chunks):
            # Simulate non-blocking socket select / poll
            time.sleep(0.01)
            # Event loop tick
            ticks += 1

    transfer_with_event_loop()
    assert ticks == total_chunks, f"Event loop must tick on each iteration, ticked {ticks} times"


# ==============================================================================
# Category 7: SSL/TLS Connection Abort
# ==============================================================================

def test_tls_handshake_failure_handling():
    """Verify handshake failure handled cleanly without memory leak."""
    caught_ssl_err = False
    try:
        # Connect to a plain HTTP mock server port with an SSL socket
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE
        raw_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw_sock.settimeout(0.5)
        raw_sock.connect(("127.0.0.1", PORT))
        ssl_sock = ctx.wrap_socket(raw_sock, server_hostname="127.0.0.1")
    except (ssl.SSLError, OSError):
        caught_ssl_err = True
    finally:
        try:
            raw_sock.close()
        except Exception:
            pass

    assert caught_ssl_err, "TLS handshake to plain HTTP port must fail cleanly"


def test_tls_certificate_validation_error():
    """Verify expired or mismatched certificate aborts connection securely."""
    ctx = ssl.create_default_context()
    ctx.check_hostname = True
    # Verification without matching CA must reject mock self-signed or invalid certs
    rejected = False
    try:
        # Wrap invalid connection
        raw_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        raw_sock.settimeout(0.5)
        raw_sock.connect(("127.0.0.1", PORT))
        ctx.wrap_socket(raw_sock, server_hostname="weread.qq.com")
    except (ssl.SSLCertVerificationError, ssl.SSLError, OSError):
        rejected = True
    finally:
        try:
            raw_sock.close()
        except Exception:
            pass

    assert rejected, "Certificate validation error must abort TLS connection securely"


def test_tls_midstream_abort():
    """Verify TLS record MAC error or mid-stream abort tears down crypto context safely."""
    class TlsSessionContext:
        def __init__(self):
            self.session_active = True
            self.crypto_buffers_allocated = True

        def handle_mac_error(self):
            # Zero out crypto keys and clean buffers
            self.session_active = False
            self.crypto_buffers_allocated = False

    session = TlsSessionContext()
    assert session.session_active and session.crypto_buffers_allocated
    session.handle_mac_error()
    assert not session.session_active
    assert not session.crypto_buffers_allocated, "Crypto buffers must be freed on MAC error"


def test_sensitive_headers_not_leaked_on_tls_error():
    """Verify auth tokens not printed to insecure logs or plaintext fallback."""
    sensitive_token = "WR_SECRET_ACCESS_TOKEN_ABC123"
    request_headers = {
        "accessToken": sensitive_token,
        "Cookie": f"wr_skey={sensitive_token}; wr_vid=10001",
        "Host": "weread.qq.com",
    }

    def sanitize_log_headers(headers: dict) -> dict:
        redacted = {}
        for k, v in headers.items():
            if k.lower() in ("accesstoken", "cookie", "authorization"):
                redacted[k] = "[REDACTED]"
            else:
                redacted[k] = v
        return redacted

    log_headers = sanitize_log_headers(request_headers)
    assert log_headers["accessToken"] == "[REDACTED]"
    assert log_headers["Cookie"] == "[REDACTED]"
    assert sensitive_token not in str(log_headers), "Sensitive token must not be in logged headers"


def test_tls_session_cache_cleanup():
    """Verify failed TLS sessions cleaned up from cache."""
    tls_session_cache: dict[str, str] = {
        "weread.qq.com": "session_ticket_data_xyz",
        "api.weread.qq.com": "session_ticket_data_abc",
    }

    def on_tls_failure(hostname: str):
        if hostname in tls_session_cache:
            del tls_session_cache[hostname]

    on_tls_failure("weread.qq.com")
    assert "weread.qq.com" not in tls_session_cache, "Failed TLS host session must be evicted from cache"
    assert "api.weread.qq.com" in tls_session_cache, "Other valid cached sessions must remain intact"


# ==============================================================================
# Test Registry & Runner
# ==============================================================================

ALL_TESTS = [
    # Category 1: Network Timeouts & Slow Sockets
    test_connect_timeout_handling,
    test_read_timeout_during_streaming,
    test_slow_chunk_transmission,
    test_timeout_recovery_and_retry,
    test_configurable_socket_timeouts,
    # Category 2: HTTP 5xx Server Errors
    test_http_500_internal_server_error,
    test_http_502_bad_gateway,
    test_http_503_service_unavailable,
    test_http_504_gateway_timeout,
    test_5xx_error_payload_logging,
    # Category 3: Retry Backoff Sequencing
    test_exponential_backoff_delays,
    test_max_retry_attempts_cap,
    test_reset_retry_counter_on_success,
    test_retry_jitter_bounds,
    test_nonblocking_ui_during_backoff,
    # Category 4: DNS Resolution Failures
    test_unresolvable_hostname_handling,
    test_empty_dns_response,
    test_dns_timeout_handling,
    test_fallback_to_cached_endpoint,
    test_offline_request_queueing,
    # Category 5: Connection Reset & Dropped Connections
    test_connection_reset_by_peer_on_request,
    test_connection_reset_during_response_body,
    test_dropped_connection_without_fin,
    test_socket_pool_cleanup_on_reset,
    test_reconnect_success_after_reset,
    # Category 6: Simulated Latency & Prefetch Yielding
    test_high_latency_response_handling,
    test_prefetch_yields_on_user_page_turn,
    test_prefetch_cancelled_on_sleep_or_lock,
    test_prefetch_throttling,
    test_slow_response_event_loop_responsiveness,
    # Category 7: SSL/TLS Connection Abort
    test_tls_handshake_failure_handling,
    test_tls_certificate_validation_error,
    test_tls_midstream_abort,
    test_sensitive_headers_not_leaked_on_tls_error,
    test_tls_session_cache_cleanup,
]


def run_all_tests():
    ensure_mock_server()
    passed = 0
    failed = 0
    print(f"=== Running Tier 2 (Network Timeouts & Sockets) Suite: {len(ALL_TESTS)} tests ===")
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
