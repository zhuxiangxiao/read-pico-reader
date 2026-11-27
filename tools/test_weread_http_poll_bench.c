/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
/* Benchmark for weread_http_poll.inc over real non-blocking POSIX socketpair with simulated RTT. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#include "weread_client.h"
#include "weread_auth.h"
#include "esp_http_client.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct esp_http_client {
    int fd;
    int server_fd;
    int open_phase;
    bool headers_done;
    int status;
    int64_t content_length;
    int64_t bytes_read;
    char hdr_buf[4096];
    size_t hdr_len;
    char body_peek[4096];
    size_t body_peek_len;
    size_t body_peek_pos;
    bool eof;
    char *headers_k[16];
    char *headers_v[16];
    size_t hcount;
};

static atomic_bool g_cancel;
static atomic_bool g_resp_stop;
static atomic_uint g_rtt_us = 2000;
static atomic_uint g_chunks = 20;
static atomic_uint g_chunk_bytes = 4096;

static uint64_t now_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ULL + (uint64_t)t.tv_nsec / 1000ULL;
}

static bool bench_cancel(void *ctx) { (void)ctx; return atomic_load(&g_cancel); }
unsigned weread_test_http_idle_ms(const char *url) { (void)url; return 15000; }
esp_err_t weread_test_http_dns(const char *host, char *out, size_t cap, weread_cancel_fn cancel, void *ctx, uint64_t deadline) {
    (void)host; (void)cancel; (void)ctx; (void)deadline;
    if (cap < 10) return ESP_ERR_NO_MEM;
    strcpy(out, "127.0.0.1");
    return ESP_OK;
}

static void *responder_thread(void *arg) {
    struct esp_http_client *c = (struct esp_http_client *)arg;
    unsigned rtt = atomic_load(&g_rtt_us);
    unsigned chunks = atomic_load(&g_chunks);
    unsigned chunk_sz = atomic_load(&g_chunk_bytes);

    /* Simulate TLS handshake 2 flights with RTT */
    for (unsigned waited = 0; waited < rtt && !atomic_load(&g_resp_stop); waited += 2000) usleep(2000);
    if (atomic_load(&g_resp_stop)) return NULL;
    (void)send(c->server_fd, "H", 1, 0);
    for (unsigned waited = 0; waited < rtt && !atomic_load(&g_resp_stop); waited += 2000) usleep(2000);
    if (atomic_load(&g_resp_stop)) return NULL;

    /* Send HTTP response headers after another RTT */
    size_t total = (size_t)chunks * (size_t)chunk_sz;
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: application/json\r\n\r\n", total);
    (void)send(c->server_fd, hdr, (size_t)hn, 0);

    char *payload = malloc(chunk_sz);
    assert(payload);
    memset(payload, 'A', chunk_sz);
    /* Write chunked stream separated by 1ms micro-gaps */
    for (unsigned i = 0; i < chunks && !atomic_load(&g_resp_stop); ++i) {
        if (i == 0) usleep(rtt);
        else usleep(1000);
        size_t off = 0;
        while (off < chunk_sz && !atomic_load(&g_resp_stop)) {
            ssize_t w = send(c->server_fd, payload + off, chunk_sz - off, 0);
            if (w <= 0) break;
            off += (size_t)w;
        }
    }
    free(payload);
    shutdown(c->server_fd, SHUT_WR);
    return NULL;
}

static pthread_t g_resp_tid;
static bool g_resp_started = false;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg) {
    (void)cfg;
    struct esp_http_client *c = calloc(1, sizeof(*c));
    assert(c);
    c->fd = c->server_fd = -1;
    c->content_length = -1;
    return c;
}
esp_err_t esp_http_client_set_url(esp_http_client_handle_t c, const char *u) { (void)c; (void)u; return ESP_OK; }
esp_err_t esp_http_client_set_method(esp_http_client_handle_t c, esp_http_client_method_t m) { (void)c; (void)m; return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v) {
    if (!c || !k || !v) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < c->hcount; ++i) {
        if (!strcasecmp(c->headers_k[i], k)) { free(c->headers_v[i]); c->headers_v[i] = strdup(v); return ESP_OK; }
    }
    if (c->hcount >= 16) return ESP_ERR_NO_MEM;
    c->headers_k[c->hcount] = strdup(k); c->headers_v[c->hcount] = strdup(v); c->hcount++;
    return ESP_OK;
}
esp_err_t esp_http_client_get_header(esp_http_client_handle_t c, const char *k, char **v) {
    *v = NULL;
    for (size_t i = 0; i < c->hcount; ++i) {
        if (!strcasecmp(c->headers_k[i], k)) { *v = c->headers_v[i]; return ESP_OK; }
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t c, const char *d, int l) { (void)c; (void)d; (void)l; return ESP_OK; }
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms) { (void)c; (void)ms; return ESP_OK; }
int esp_http_client_get_socket(esp_http_client_handle_t c) { return c ? c->fd : -1; }

esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len) {
    (void)write_len;
    if (c->fd < 0) {
        int sv[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
        int flags = fcntl(sv[0], F_GETFL, 0);
        fcntl(sv[0], F_SETFL, flags | O_NONBLOCK);
        c->fd = sv[0];
        c->server_fd = sv[1];
        c->open_phase = 0;
        atomic_store(&g_resp_stop, false);
        g_resp_started = true;
        assert(pthread_create(&g_resp_tid, NULL, responder_thread, c) == 0);
        errno = EAGAIN;
        return ESP_ERR_HTTP_EAGAIN;
    }
    char ack;
    ssize_t n = recv(c->fd, &ack, 1, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        errno = EAGAIN;
        return ESP_ERR_HTTP_EAGAIN;
    }
    c->headers_done = false;
    c->hdr_len = 0;
    c->body_peek_len = c->body_peek_pos = 0;
    c->bytes_read = 0;
    c->content_length = -1;
    c->eof = false;
    return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *b, int l) { (void)c; (void)b; return l; }
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    if (c->headers_done) return c->content_length;
    while (c->hdr_len < sizeof(c->hdr_buf) - 1) {
        ssize_t n = recv(c->fd, c->hdr_buf + c->hdr_len, sizeof(c->hdr_buf) - 1 - c->hdr_len, 0);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { errno = EAGAIN; return -ESP_ERR_HTTP_EAGAIN; }
            return -1;
        }
        if (n == 0) return -1;
        c->hdr_len += (size_t)n;
        c->hdr_buf[c->hdr_len] = '\0';
        char *end = strstr(c->hdr_buf, "\r\n\r\n");
        if (end) {
            *end = '\0';
            c->status = 200;
            const char *cl = strstr(c->hdr_buf, "Content-Length: ");
            c->content_length = cl ? atoll(cl + 16) : -1;
            size_t tail = c->hdr_len - (size_t)((end + 4) - c->hdr_buf);
            if (tail > 0) {
                assert(tail <= sizeof(c->body_peek));
                memcpy(c->body_peek, end + 4, tail);
                c->body_peek_len = tail;
            }
            c->headers_done = true;
            return c->content_length;
        }
    }
    return -1;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c ? c->status : 0; }
int64_t esp_http_client_get_content_length(esp_http_client_handle_t c) { return c ? c->content_length : -1; }
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len) {
    if (c->body_peek_pos < c->body_peek_len) {
        size_t avail = c->body_peek_len - c->body_peek_pos;
        size_t take = avail < (size_t)len ? avail : (size_t)len;
        memcpy(buf, c->body_peek + c->body_peek_pos, take);
        c->body_peek_pos += take;
        c->bytes_read += (int64_t)take;
        return (int)take;
    }
    if (c->eof) return 0;
    ssize_t n = recv(c->fd, buf, (size_t)len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) { errno = EAGAIN; return -ESP_ERR_HTTP_EAGAIN; }
        return -1;
    }
    if (n == 0) { c->eof = true; return 0; }
    c->bytes_read += n;
    return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    if (!c) return false;
    return c->content_length >= 0 ? c->bytes_read >= c->content_length : c->eof;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    if (!c) return ESP_ERR_INVALID_ARG;
    atomic_store(&g_resp_stop, true);
    if (c->fd >= 0) { close(c->fd); c->fd = -1; }
    if (c->server_fd >= 0) { shutdown(c->server_fd, SHUT_RDWR); }
    if (g_resp_started) { pthread_join(g_resp_tid, NULL); g_resp_started = false; }
    if (c->server_fd >= 0) { close(c->server_fd); c->server_fd = -1; }
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    if (!c) return ESP_OK;
    esp_http_client_close(c);
    for (size_t i = 0; i < c->hcount; ++i) { free(c->headers_k[i]); free(c->headers_v[i]); }
    free(c);
    return ESP_OK;
}
esp_err_t esp_http_client_perform(esp_http_client_handle_t c) { (void)c; return ESP_OK; }

static void *cancel_trigger_thread(void *arg) {
    (void)arg;
    usleep(5000);
    atomic_store(&g_cancel, true);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    assert(argc >= 2);
    assert(weread_auth_init(argv[1]) == ESP_OK);
    weread_auth_session_t s = {0};
    strcpy(s.vid, "123"); strcpy(s.access_token, "tok"); strcpy(s.refresh_token, "ref");
    assert(weread_auth_save_session(&s) == ESP_OK && weread_auth_load_session(&s) == ESP_OK);
    weread_client_set_cancel(bench_cancel, NULL);
    weread_client_set_polling(true);
    weread_client_set_background(true);

    const int runs = 5;
    uint64_t total_us = 0;
    for (int i = 0; i < runs; ++i) {
        char *out = NULL; size_t len = 0; int status = 0;
        uint64_t t0 = now_us();
        esp_err_t err = weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &out, &len, &status);
        total_us += now_us() - t0;
        assert(err == ESP_OK && status == 200 && len == 20u * 4096u);
        free(out);
    }
    /* Cancel responsiveness check under slow RTT */
    atomic_store(&g_cancel, false);
    atomic_store(&g_rtt_us, 80000);
    pthread_t ctid;
    assert(pthread_create(&ctid, NULL, cancel_trigger_thread, NULL) == 0);
    char *cout = NULL; size_t clen = 0; int cstatus = 0;
    uint64_t ct0 = now_us();
    esp_err_t cerr = weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &cout, &clen, &cstatus);
    uint64_t cancel_us = now_us() - ct0;
    pthread_join(ctid, NULL);
    assert(cerr == ESP_ERR_INVALID_STATE);
    free(cout);
    assert(cancel_us < 50000);

    printf("METRIC name=http_poll_rtt_20x4k_5x us=%llu cancel_us=%llu\n",
           (unsigned long long)total_us, (unsigned long long)cancel_us);
    weread_client_set_radio_awake(false);
    weread_auth_clear_session();
    return 0;
}
