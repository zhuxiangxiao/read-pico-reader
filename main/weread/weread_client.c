/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead secure HTTPS client with sensitive header stripping and renewal replay.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Os")
#endif
#include "weread_client.h"
#include "weread_store.h"
#include "weread_auth.h"
#include "cJSON.h"
#include "esp_log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>
#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
__attribute__((weak)) bool epd_display_scan_active(void) { return false; }
typedef struct { wifi_ps_type_t previous; bool restore; } radio_ps_guard_t;
static radio_ps_guard_t radio_ps_enter(void) {
    radio_ps_guard_t g = { .previous = WIFI_PS_MIN_MODEM, .restore = false };
    g.restore = esp_wifi_get_ps(&g.previous) == ESP_OK &&
        g.previous != WIFI_PS_NONE && esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK;
    return g;
}
static void radio_ps_leave(radio_ps_guard_t *g) {
    if (g && g->restore) { (void)esp_wifi_set_ps(g->previous); g->restore = false; }
}
static void http_pool_close(void);
static bool s_global_ps_restore;
static wifi_ps_type_t s_global_ps_prev = WIFI_PS_MIN_MODEM;
void weread_client_set_radio_awake(bool awake) {
    if (awake) {
        wifi_ps_type_t cur = WIFI_PS_MIN_MODEM;
        if (esp_wifi_get_ps(&cur) == ESP_OK && cur != WIFI_PS_NONE &&
            esp_wifi_set_ps(WIFI_PS_NONE) == ESP_OK && !s_global_ps_restore) {
            s_global_ps_prev = cur;
            s_global_ps_restore = true;
        }
    } else {
        http_pool_close();
        if (s_global_ps_restore) {
            (void)esp_wifi_set_ps(s_global_ps_prev);
            s_global_ps_restore = false;
        }
    }
}
#else
void weread_client_set_radio_awake(bool awake) { (void)awake; }
#endif
void weread_client_yield_for_display(weread_cancel_fn cancel, void *ctx) {
#if defined(ESP_PLATFORM)
    for (unsigned waited = 0; waited < 80 && epd_display_scan_active(); ++waited) {
        if (weread_client_cancelled() || (cancel && cancel(ctx))) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
#else
    (void)cancel; (void)ctx;
#endif
}

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <pthread.h>
#endif

#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <sys/socket.h>
#endif

#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
static esp_http_client_handle_t s_active_http;
#if defined(ESP_PLATFORM)
static StaticSemaphore_t s_http_mutex_buffer;
static SemaphoreHandle_t s_http_mutex;
static portMUX_TYPE s_http_init = portMUX_INITIALIZER_UNLOCKED;
static void http_lock(void) {
    portENTER_CRITICAL(&s_http_init);
    if (!s_http_mutex) s_http_mutex = xSemaphoreCreateMutexStatic(&s_http_mutex_buffer);
    portEXIT_CRITICAL(&s_http_init);
    xSemaphoreTake(s_http_mutex, portMAX_DELAY);
}
static void http_unlock(void) { xSemaphoreGive(s_http_mutex); }
static bool http_try_lock(void) {
    portENTER_CRITICAL(&s_http_init);
    if (!s_http_mutex) s_http_mutex = xSemaphoreCreateMutexStatic(&s_http_mutex_buffer);
    portEXIT_CRITICAL(&s_http_init);
    return xSemaphoreTake(s_http_mutex, 0) == pdTRUE;
}
#else
static pthread_mutex_t s_http_mutex = PTHREAD_MUTEX_INITIALIZER;
static void http_lock(void) { pthread_mutex_lock(&s_http_mutex); }
static void http_unlock(void) { pthread_mutex_unlock(&s_http_mutex); }
static bool http_try_lock(void) { return pthread_mutex_trylock(&s_http_mutex) == 0; }
#endif
static void http_active(esp_http_client_handle_t client) { http_lock(); s_active_http = client; http_unlock(); }
static void http_cleanup(esp_http_client_handle_t client) {
    http_lock(); if (s_active_http == client) s_active_http = NULL;
    esp_http_client_close(client); esp_http_client_cleanup(client); http_unlock();
}
#endif
void weread_client_abort_active(void) {
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
    // 清理已持锁时没有仍需shutdown的句柄，不能让UI等待TLS释放。
    // Cleanup already owns the closing handle; UI cancellation must not wait for TLS teardown.
    if (!http_try_lock()) return;
    int fd = s_active_http ? esp_http_client_get_socket(s_active_http) : -1;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    http_unlock();
#endif
}
static weread_timeout_fn s_timeout_budget;
static void *s_timeout_ctx;
void weread_client_set_timeout_budget(weread_timeout_fn fn, void *ctx) { s_timeout_budget=fn;s_timeout_ctx=ctx; }
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
static int request_default_timeout_ms(const char *url) {
    const char *authority=strstr(url,"://");
    const char *path=authority?strchr(authority+3,'/'):NULL;
    // 实际扫码接口长轮询约55秒；普通API仍使用15秒。/ The real QR endpoint long-polls for about55s; ordinary APIs retain15s.
    return path&&!strncmp(path,"/web/getlogininfo",17)&&(path[17]==0||path[17]=='?')?65000:15000;
}
static int request_timeout_ms(const char *url) {
    int ms=request_default_timeout_ms(url);
    int budget=s_timeout_budget?s_timeout_budget(s_timeout_ctx):0;
    return budget>0&&budget<ms?budget:ms;
}
#endif
static weread_network_fn s_network;
static void *s_network_ctx;
static bool s_background,s_polling;
void weread_client_set_background(bool enabled) { s_background=enabled; }
void weread_client_set_polling(bool enabled) { s_polling=enabled; }
void weread_client_set_network(weread_network_fn fn, void *ctx) { s_network = fn; s_network_ctx = ctx; }

static weread_http_transport_fn s_transport = NULL;
static void *s_transport_user_ctx = NULL;
static weread_download_transport_fn s_download_transport;
static void *s_download_ctx;
static weread_cancel_fn s_cancel_query;
static void *s_cancel_context;
void weread_client_set_cancel(weread_cancel_fn fn, void *ctx) { s_cancel_query = fn; s_cancel_context = ctx; }
bool weread_client_cancelled(void) { return s_cancel_query && s_cancel_query(s_cancel_context); }

static weread_download_progress_fn s_download_progress;
static void *s_download_progress_ctx;
void weread_client_set_download_progress(weread_download_progress_fn fn, void *ctx) {
    s_download_progress = fn; s_download_progress_ctx = ctx;
}
void weread_client_report_download_progress(uint64_t bytes, bool done) {
    if (s_download_progress) s_download_progress(bytes, done, s_download_progress_ctx);
}

static weread_client_activity_fn_t s_activity_cb;
static void *s_activity_ctx;
#if defined(ESP_PLATFORM)
static portMUX_TYPE s_activity_lock = portMUX_INITIALIZER_UNLOCKED;
#else
static pthread_mutex_t s_activity_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
void weread_client_set_activity_cb(weread_client_activity_fn_t cb, void *ctx) {
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_activity_lock);
    s_activity_cb = cb;
    s_activity_ctx = ctx;
    portEXIT_CRITICAL(&s_activity_lock);
#else
    pthread_mutex_lock(&s_activity_lock);
    s_activity_cb = cb;
    s_activity_ctx = ctx;
    pthread_mutex_unlock(&s_activity_lock);
#endif
}
static void client_notify_activity(void) {
    weread_client_activity_fn_t cb;
    void *ctx;
#if defined(ESP_PLATFORM)
    portENTER_CRITICAL(&s_activity_lock);
    cb = s_activity_cb;
    ctx = s_activity_ctx;
    portEXIT_CRITICAL(&s_activity_lock);
#else
    pthread_mutex_lock(&s_activity_lock);
    cb = s_activity_cb;
    ctx = s_activity_ctx;
    pthread_mutex_unlock(&s_activity_lock);
#endif
    if (cb) cb(ctx);
}

esp_err_t weread_client_init(void) {
    return ESP_OK;
}

void weread_client_set_transport(weread_http_transport_fn transport, void *user_ctx) {
    s_transport = transport;
    s_transport_user_ctx = user_ctx;
}

const char *weread_client_get_base_url(void) {
    const char *env_base = getenv("WEREAD_API_BASE");
    if (env_base && env_base[0] != '\0') {
        return env_base;
    }
    return WEREAD_API_BASE_DEFAULT;
}

bool weread_client_url_encode(const char *text, char *out, size_t cap) {
    if (!text || !out || !cap) return false;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || strchr("-._~", *p);
        size_t width = plain ? 1 : 3;
        if (width >= cap - n) { out[0] = 0; return false; }
        if (plain) out[n++] = (char)*p;
        else {
            const char *hex = "0123456789ABCDEF";
            out[n++] = '%'; out[n++] = hex[*p >> 4]; out[n++] = hex[*p & 15];
        }
    }
    out[n] = 0; return true;
}

static bool contains_case_insensitive(const char *haystack, const char *needle) {
    if (!haystack || !needle) {
        return false;
    }
    size_t needle_len = strlen(needle);
    if (needle_len == 0) {
        return true;
    }
    for (const char *h = haystack; *h != '\0'; h++) {
        if (strncasecmp(h, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_sensitive_header_name(const char *key, size_t len) {
    if ((len == 6 && strncasecmp(key, "Cookie", 6) == 0) ||
        (len == 13 && strncasecmp(key, "Authorization", 13) == 0) ||
        (len == 11 && strncasecmp(key, "accessToken", 11) == 0) ||
        (len == 3 && strncasecmp(key, "vid", 3) == 0)) {
        return true;
    }
    return false;
}

bool weread_client_is_trusted_url(const char *url) {
    if (!url || url[0] == '\0' || strpbrk(url, "\r\n\\")) {
        return false;
    }

    // Reject untrusted path markers case-insensitively
    if (contains_case_insensitive(url, "/untrusted") ||
        contains_case_insensitive(url, "/thirdparty")) {
        return false;
    }

    const char *scheme_end = strstr(url, "://");
    if (!scheme_end) {
        return false;
    }

    size_t scheme_len = scheme_end - url;
    bool is_https = (scheme_len == 5 && strncasecmp(url, "https", 5) == 0);
    bool is_http  = (scheme_len == 4 && strncasecmp(url, "http", 4) == 0);

    // Isolate RFC 3986 authority component: between "://" and first '/', '?', '#', '\\', or '\0'
    const char *auth_start = scheme_end + 3;
    const char *auth_end = auth_start;
    while (*auth_end && *auth_end != '/' && *auth_end != '?' &&
           *auth_end != '#' && *auth_end != '\\') {
        auth_end++;
    }

    if (auth_end == auth_start) {
        return false;
    }

    // If userinfo (@) is present, host starts after the last '@'
    const char *host_start = auth_start;
    for (const char *p = auth_start; p < auth_end; p++) {
        if (*p == '@') {
            host_start = p + 1;
        }
    }

    if (host_start >= auth_end) {
        return false;
    }

    // Isolate host: scan until port delimiter ':' or end of authority
    const char *host_end = host_start;
    if (*host_start == '[') {
        // IPv6 literal
        while (host_end < auth_end && *host_end != ']') {
            host_end++;
        }
        if (host_end < auth_end && *host_end == ']') {
            host_end++;
        }
    } else {
        while (host_end < auth_end && *host_end != ':') {
            host_end++;
        }
    }

    size_t host_len = host_end - host_start;
    if (host_len == 0 || host_len > 255) {
        return false;
    }

    // 严格检查端口和主机字符，防止 HTTP 实现使用不同的权限边界。/ Validate ports/host bytes so HTTP parsers cannot disagree about the authority.
    if (host_end != auth_end) {
        if (*host_end != ':' || host_end + 1 == auth_end || auth_end - host_end > 6) return false;
        unsigned port = 0;
        for (const char *p = host_end + 1; p < auth_end; ++p) {
            if (*p < '0' || *p > '9') return false;
            port = port * 10 + (unsigned)(*p - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    for (const char *p = auth_start; p < auth_end; ++p)
        if ((unsigned char)*p <= 32 || (unsigned char)*p >= 127 || *p == '%') return false;
    if (*host_start != '[') for (const char *p = host_start; p < host_end; ++p) {
        unsigned char c = (unsigned char)*p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
    }

    char host[256];
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

#if !defined(ESP_PLATFORM)
    // Loopback test exemption
    if (strcasecmp(host, "127.0.0.1") == 0 ||
        strcasecmp(host, "localhost") == 0 ||
        strcasecmp(host, "[::1]") == 0) {
        return is_https || is_http;
    }

#endif

    // All external targets MUST be HTTPS
    if (!is_https) {
        return false;
    }

    // Exact match for "qq.com"
    if (strcasecmp(host, "qq.com") == 0) {
        return true;
    }

    // Suffix match for "*.qq.com"
    if (host_len > 7 && strcasecmp(host + host_len - 7, ".qq.com") == 0) {
        return true;
    }

    return false;
}

void weread_client_strip_sensitive_headers(const char *url, const char *in_headers, char *out_headers, size_t out_cap) {
    if (!out_headers || out_cap == 0) {
        return;
    }
    out_headers[0] = '\0';
    if (!in_headers || in_headers[0] == '\0') {
        return;
    }

    // If trusted URL, keep all headers intact
    if (weread_client_is_trusted_url(url)) {
        size_t len = strlen(in_headers);
        if (len >= out_cap) {
            len = out_cap - 1;
        }
        memcpy(out_headers, in_headers, len);
        out_headers[len] = '\0';
        return;
    }

    // Untrusted URL: parse line by line and strip Cookie, Authorization, accessToken, vid
    // Also strip continuation lines (obs-fold) for sensitive headers, and handle leading whitespace.
    const char *p = in_headers;
    size_t out_len = 0;
    bool last_was_sensitive = false;

    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') {
            p++;
        }
        const char *line_end = p;
        if (*p == '\n') {
            p++;
        }

        // Trim leading whitespace (spaces and tabs)
        const char *trimmed = line_start;
        while (trimmed < line_end && (*trimmed == ' ' || *trimmed == '\t')) {
            trimmed++;
        }

        bool is_blank = (trimmed >= line_end || *trimmed == '\r');
        bool starts_with_ws = (trimmed > line_start);
        bool is_sensitive = false;

        if (is_blank) {
            // Blank line terminates previous header continuation
            is_sensitive = false;
            last_was_sensitive = false;
        } else if (starts_with_ws && last_was_sensitive) {
            // Continuation line of a sensitive header (RFC 7230 § 3.2.4 obs-fold)
            is_sensitive = true;
            // last_was_sensitive remains true
        } else {
            // New header line or leading-whitespace header line
            const char *colon = trimmed;
            while (colon < line_end && *colon != ':') {
                colon++;
            }
            if (colon < line_end) {
                const char *key_end = colon;
                while (key_end > trimmed && (*(key_end - 1) == ' ' || *(key_end - 1) == '\t')) {
                    key_end--;
                }
                size_t key_len = key_end - trimmed;
                if (is_sensitive_header_name(trimmed, key_len)) {
                    is_sensitive = true;
                    last_was_sensitive = true;
                } else {
                    is_sensitive = false;
                    last_was_sensitive = false;
                }
            } else {
                // Continuation line of non-sensitive header or unrecognized line without colon
                is_sensitive = false;
                last_was_sensitive = false;
            }
        }

        if (!is_sensitive) {
            size_t line_total = p - line_start;
            if (out_len + line_total < out_cap) {
                memcpy(out_headers + out_len, line_start, line_total);
                out_len += line_total;
                out_headers[out_len] = '\0';
            }
        }
    }
}

static WR_EXT_BSS struct { char vid[WEREAD_VID_MAX_LEN], token[WEREAD_TOKEN_MAX_LEN], refresh[WEREAD_TOKEN_MAX_LEN]; } s_login_cookies;
void weread_client_forget_login_cookies(void) { weread_secure_zero(&s_login_cookies,sizeof(s_login_cookies)); }
bool weread_client_login_cookie(const char *key, char *out, size_t cap) {
    const char *v = !strcmp(key,"wr_vid") ? s_login_cookies.vid : !strcmp(key,"wr_skey") ? s_login_cookies.token : !strcmp(key,"wr_rt") ? s_login_cookies.refresh : "";
    if (!*v || strlen(v) >= cap) return false;
    memcpy(out,v,strlen(v)+1);return true;
}
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
static int hex_digit(unsigned char c) {
    return c >= '0' && c <= '9' ? c-'0' : c >= 'a' && c <= 'f' ? c-'a'+10 : c >= 'A' && c <= 'F' ? c-'A'+10 : -1;
}
static esp_err_t login_header(esp_http_client_event_t *event) {
    if (!*(bool *)event->user_data || event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key ||
        !event->header_value || strcasecmp(event->header_key,"Set-Cookie")) return ESP_OK;
    const char *p=event->header_value;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == ',') ++p;
        const char *start=p;while (*p && *p != '=' && *p != ';' && *p != ',') ++p;
        size_t keylen=(size_t)(p-start); if (*p != '=') { if (*p) ++p;continue; } ++p;
        char *dst=NULL;size_t cap=0;
        if (keylen==6 && !memcmp(start,"wr_vid",6)) {dst=s_login_cookies.vid;cap=sizeof(s_login_cookies.vid);}
        if (keylen==7 && !memcmp(start,"wr_skey",7)) {dst=s_login_cookies.token;cap=sizeof(s_login_cookies.token);}
        if (keylen==5 && !memcmp(start,"wr_rt",5)) {dst=s_login_cookies.refresh;cap=sizeof(s_login_cookies.refresh);}
        size_t used=0;bool valid=true;
        while (*p && *p != ';' && *p != ',') {
            unsigned char c=(unsigned char)*p++;
            if(c=='%' && p[0] && p[1] && hex_digit(p[0])>=0 && hex_digit(p[1])>=0){c=(unsigned char)(hex_digit(p[0])*16+hex_digit(p[1]));p+=2;}
            if (c <= 32 || c >= 127 || c == ';' || c == ',') valid=false;
            if(dst && used+1<cap)dst[used++]=(char)c;else if(dst)valid=false;
        }
        if(dst)dst[valid?used:0]=0;
    }
    return ESP_OK;
}
#endif
static uint64_t client_now_ms(void) {
#if defined(ESP_PLATFORM)
    return (uint64_t)esp_timer_get_time()/1000;
#else
    struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
    return (uint64_t)now.tv_sec*1000+(uint64_t)now.tv_nsec/1000000;
#endif
}
#if defined(ESP_PLATFORM)
#define HTTP_POOL_SLOTS 2u
#define HTTP_POOL_IDLE_MS 45000u
typedef struct {
    esp_http_client_handle_t client;
    char host[128];
    char peer[48];
    int port;
    uint64_t last_used_ms;
} http_pool_slot_t;
static WR_EXT_BSS http_pool_slot_t s_pool[HTTP_POOL_SLOTS];
static void http_close_handles(esp_http_client_handle_t *list, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (list[i]) { esp_http_client_close(list[i]); esp_http_client_cleanup(list[i]); }
    }
}
static void http_pool_prune_locked(uint64_t now, esp_http_client_handle_t stale[HTTP_POOL_SLOTS]) {
    for (size_t i = 0; i < HTTP_POOL_SLOTS; ++i) {
        if (s_pool[i].client && (now - s_pool[i].last_used_ms >= HTTP_POOL_IDLE_MS ||
            esp_http_client_get_state(s_pool[i].client) < HTTP_STATE_CONNECTED)) {
            stale[i] = s_pool[i].client;
            memset(&s_pool[i], 0, sizeof(s_pool[i]));
        }
    }
}
static int http_pool_match_locked(const char *host, int port) {
    for (size_t i = 0; i < HTTP_POOL_SLOTS; ++i) {
        if (s_pool[i].client && s_pool[i].port == port && !strcasecmp(s_pool[i].host, host)) return (int)i;
    }
    return -1;
}
static void http_pool_close(void) {
    esp_http_client_handle_t doomed[HTTP_POOL_SLOTS] = {0};
    http_lock();
    for (size_t i = 0; i < HTTP_POOL_SLOTS; ++i) doomed[i] = s_pool[i].client;
    memset(&s_pool, 0, sizeof(s_pool));
    http_unlock();
    http_close_handles(doomed, HTTP_POOL_SLOTS);
}
static bool http_pool_peer(const char *host, int port, char peer[48]) {
    esp_http_client_handle_t stale[HTTP_POOL_SLOTS] = {0};
    http_lock();
    http_pool_prune_locked(client_now_ms(), stale);
    int idx = http_pool_match_locked(host, port);
    if (idx >= 0) memcpy(peer, s_pool[idx].peer, sizeof(s_pool[idx].peer));
    http_unlock();
    http_close_handles(stale, HTTP_POOL_SLOTS);
    return idx >= 0;
}
#endif
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
#include "weread_http_poll.inc"
#endif
#if defined(ESP_PLATFORM)
static bool http_pool_eligible(const char *url) {
    if (!weread_client_is_trusted_url(url)) return false;
    const char *auth = strstr(url, "://");
    const char *path = auth ? strchr(auth + 3, '/') : NULL;
    if (!path) return true;
    if (!strncmp(path, "/weblogin", 9) && (path[9] == 0 || path[9] == '?')) return false;
    if (!strncmp(path, "/web/getlogininfo", 17) && (path[17] == 0 || path[17] == '?')) return false;
    return true;
}
static esp_http_client_handle_t http_pool_take(const char *url, http_poll_t *poll, http_event_handle_cb cb, void *cb_ctx) {
    if (!poll || !http_pool_eligible(url)) return NULL;
    esp_http_client_handle_t client = NULL;
    esp_http_client_handle_t stale[HTTP_POOL_SLOTS] = {0};
    http_lock();
    http_pool_prune_locked(client_now_ms(), stale);
    int idx = http_pool_match_locked(poll->host, poll->port);
    if (idx >= 0) {
        client = s_pool[idx].client;
        memcpy(poll->peer, s_pool[idx].peer, sizeof(poll->peer));
        memset(&s_pool[idx], 0, sizeof(s_pool[idx]));
    }
    http_unlock();
    http_close_handles(stale, HTTP_POOL_SLOTS);
    if (!client) return NULL;
    size_t cap = strlen(poll->path) + (poll->query ? strlen(poll->query) : 0) + 96;
    char *pool_url = wr_alloc(cap);
    if (!pool_url) { http_cleanup(client); return NULL; }
    bool v6 = strchr(poll->peer, ':') != NULL;
    int n = poll->query
        ? snprintf(pool_url, cap, "https://%s%s%s:%d%s?%s", v6 ? "[" : "", poll->peer, v6 ? "]" : "", poll->port, poll->path, poll->query)
        : snprintf(pool_url, cap, "https://%s%s%s:%d%s", v6 ? "[" : "", poll->peer, v6 ? "]" : "", poll->port, poll->path);
    esp_err_t err = (n > 0 && (size_t)n < cap) ? esp_http_client_set_url(client, pool_url) : ESP_ERR_INVALID_SIZE;
    free(pool_url);
    if (err != ESP_OK) { http_cleanup(client); return NULL; }
    static const char *const k_clear_hdrs[] = {
        "Content-Type", "Content-Length", "Transfer-Encoding", "Accept",
        "Cookie", "Authorization", "vid", "accessToken", "Origin",
        "Referer", "User-Agent", "CLIENT_TYPE", "X-Is-Serverside", "fingerprint"
    };
    for (size_t i = 0; i < sizeof(k_clear_hdrs) / sizeof(*k_clear_hdrs); ++i)
        (void)esp_http_client_delete_header(client, k_clear_hdrs[i]);
    poll->connections = 0;
    poll->header_restarts = 0;
    poll->connected = true;
    poll->error = ESP_OK;
    poll->event_handler = cb;
    poll->event_ctx = cb_ctx;
    esp_http_client_set_event_handler(client, http_poll_event);
    esp_http_client_set_user_data(client, poll);
    return client;
}
static void http_pool_put(const char *url, esp_http_client_handle_t client, http_poll_t *poll, bool ok) {
    if (!client) return;
    size_t hlen = poll ? strlen(poll->host) : 0;
    if (!ok || !poll || !hlen || hlen >= sizeof(s_pool[0].host) || !http_pool_eligible(url) ||
        weread_client_cancelled() || esp_http_client_get_state(client) < HTTP_STATE_CONNECTED ||
        !esp_http_client_is_persistent_connection(client) ||
        !esp_http_client_is_complete_data_received(client)) {
        http_cleanup(client);
        return;
    }
    esp_http_client_set_event_handler(client, NULL);
    esp_http_client_set_user_data(client, NULL);
    http_lock();
    if (s_active_http == client) s_active_http = NULL;
    int hit = http_pool_match_locked(poll->host, poll->port);
    size_t pick = 0;
    if (hit >= 0) {
        pick = (size_t)hit;
    } else {
        for (size_t i = 0; i < HTTP_POOL_SLOTS; ++i) {
            if (!s_pool[i].client) { pick = i; break; }
            if (s_pool[i].last_used_ms < s_pool[pick].last_used_ms) pick = i;
        }
    }
    esp_http_client_handle_t evicted = s_pool[pick].client;
    memset(&s_pool[pick], 0, sizeof(s_pool[pick]));
    s_pool[pick].client = client;
    memcpy(s_pool[pick].host, poll->host, hlen + 1);
    memcpy(s_pool[pick].peer, poll->peer, sizeof(s_pool[pick].peer));
    s_pool[pick].port = poll->port;
    s_pool[pick].last_used_ms = client_now_ms();
    http_unlock();
    if (evicted && evicted != client) { esp_http_client_close(evicted); esp_http_client_cleanup(evicted); }
}
#endif
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
__attribute__((noinline)) static esp_err_t apply_headers(esp_http_client_handle_t client, const char *headers, bool strict_size) {
    if (!headers) return ESP_OK;
    const char *p = headers;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        const char *line_end = p;
        if (*p == '\n') p++;

        const char *k_start = line_start;
        while (k_start < line_end && (*k_start == ' ' || *k_start == '\t')) k_start++;
        const char *colon = k_start;
        while (colon < line_end && *colon != ':') colon++;
        if (colon < line_end) {
            char k[128], v[1024];
            const char *key_end = colon;
            while (key_end > k_start && (*(key_end - 1) == ' ' || *(key_end - 1) == '\t')) key_end--;
            size_t klen = (size_t)(key_end - k_start);

            const char *val_start = colon + 1;
            while (val_start < line_end && (*val_start == ' ' || *val_start == '\t')) val_start++;
            const char *val_end = line_end;
            while (val_end > val_start && (*(val_end - 1) == '\r' || *(val_end - 1) == ' ' || *(val_end - 1) == '\t')) val_end--;
            size_t vlen = (size_t)(val_end - val_start);

            if (strict_size && (klen >= sizeof(k) || vlen >= sizeof(v))) return ESP_ERR_INVALID_SIZE;
            if (klen >= sizeof(k)) klen = sizeof(k) - 1;
            if (vlen >= sizeof(v)) vlen = sizeof(v) - 1;
            memcpy(k, k_start, klen); k[klen] = '\0';
            memcpy(v, val_start, vlen); v[vlen] = '\0';
            esp_http_client_set_header(client, k, v);
        }
    }
    return ESP_OK;
}
static void request_timing(const char *url,const uint64_t phase[7],esp_err_t result,int status,size_t bytes,bool reused) {
    static unsigned sequence,native,web,auth;
    const char *base=weread_client_get_base_url();
    unsigned route=strstr(url,"/weblogin")?3:!strncmp(url,base,strlen(base))?1:2;
    if(route==1)++native;else if(route==3)++auth;else ++web;
    // connect包含DNS/TCP/TLS；不要把SDK一次连接事件解释成三个独立指标。
    // Connect includes DNS/TCP/TLS; one SDK event cannot separate those stages.
    ESP_LOGI("weread","HTTP timing n=%u route=%u native=%u web=%u auth=%u reused=%u network=%u dns=%u connect=%u response=%u body=%u cleanup=%u total=%u bytes=%u status=%d result=%d",
        ++sequence,route,native,web,auth,(unsigned)reused,(unsigned)(phase[1]-phase[0]),phase[2]?(unsigned)(phase[2]-phase[1]):0,(unsigned)(phase[3]-(phase[2]?phase[2]:phase[1])),
        (unsigned)(phase[4]-phase[3]),(unsigned)(phase[5]-phase[4]),(unsigned)(phase[6]-phase[5]),
        (unsigned)(phase[6]-phase[0]),(unsigned)bytes,status,(int)result);
}
#endif
static esp_err_t weread_client_dispatch_raw(
    const char *url,
    const char *method,
    const char *headers,
    const char *body,
    char **out_resp,
    size_t *out_len,
    int *out_status,
    bool background
) {
    uint64_t phase[7]={client_now_ms(),0,0,0,0,0,0};
    weread_secure_zero(&s_login_cookies,sizeof(s_login_cookies));
    if (weread_client_cancelled()) return ESP_ERR_INVALID_STATE;
    if (s_network) { esp_err_t ready = s_network(s_network_ctx); if (ready != ESP_OK) {
        ESP_LOGW("weread", "network preparation failed result=%d", (int)ready); return ready;
    } }
    phase[1]=client_now_ms();
    if (s_transport) {
        int local_status = 0;
        if (out_status) *out_status = 0;
        int *status_ptr = out_status ? out_status : &local_status;
        esp_err_t err = s_transport(url, method, headers, body, out_resp, out_len, status_ptr, s_transport_user_ctx);
        if (err == ESP_OK || *status_ptr > 0) client_notify_activity();
        return err;
    }

#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
    http_poll_t *poll __attribute__((cleanup(wr_release)))=NULL;
    esp_err_t prepared=http_poll_prepare(url,background,NULL,NULL,&poll);
    if(prepared!=ESP_OK)return prepared;
    if(poll)phase[2]=client_now_ms();
    const char *authority_end=strstr(url,"://");
    const char *url_path=authority_end?strchr(authority_end+3,'/'):NULL;
    bool login = weread_client_is_trusted_url(url) && url_path && !strncmp(url_path,"/weblogin",9) &&
        (url_path[9] == 0 || url_path[9] == '?');
#if !defined(ESP_PLATFORM)
    if (url_path && !strncmp(url_path,"/weblogin",9) && !strncmp(url,"http://127.0.0.1:",17)) login=true;
#endif
    // 轮询传输仅由worker关闭，避免SDK内部重连与跨线程shutdown的fd复用竞态。
    // The polling worker owns close/reconnect; cancellation polls avoid cross-thread fd reuse races.
#if defined(ESP_PLATFORM)
    radio_ps_guard_t ps_guard __attribute__((cleanup(radio_ps_leave))) = radio_ps_enter();
    (void)ps_guard;
#endif
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_OK;
    bool reused = false;
    size_t body_len = body ? strlen(body) : 0;
    for (unsigned reuse_try = 0; reuse_try < 2; ++reuse_try) {
#if defined(ESP_PLATFORM)
        if (reuse_try == 0) {
            client = http_pool_take(url, poll, login_header, &login);
            reused = (client != NULL);
        }
#endif
        if (!client) {
            if (reuse_try > 0 && poll) {
                http_poll_reset(poll);
                weread_secure_zero(&s_login_cookies, sizeof(s_login_cookies));
            }
            esp_http_client_config_t config = {
                .url = url,
                .timeout_ms = request_timeout_ms(url),
                .crt_bundle_attach = esp_crt_bundle_attach,
                .disable_auto_redirect = true,
                .keep_alive_enable = true,
                .buffer_size = 4096,
                .buffer_size_tx = 2048,
                .event_handler = login_header, .user_data = &login,
            };
            http_poll_config(poll,&config);
            client = esp_http_client_init(&config);
            if (!client) {
                return ESP_FAIL;
            }
        }
        if (strcasecmp(method, "POST") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_POST);
        } else if (strcasecmp(method, "PUT") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_PUT);
        } else if (strcasecmp(method, "DELETE") == 0) {
            esp_http_client_set_method(client, HTTP_METHOD_DELETE);
        } else {
            esp_http_client_set_method(client, HTTP_METHOD_GET);
        }
        (void)apply_headers(client, headers, false);
        if(poll)esp_http_client_set_header(client,"Host",http_poll_host(poll));
        http_active(poll?NULL:client);
        err = weread_client_cancelled() ? ESP_ERR_INVALID_STATE : http_poll_open(client, (int)body_len,poll);
        if (err == ESP_OK && !reused) client_notify_activity();
        phase[3]=client_now_ms();
        size_t sent = 0;
        while (err == ESP_OK && sent < body_len) {
            int n = http_poll_write(client, body + sent, (int)(body_len - sent),poll);
            if (n <= 0) { err = http_poll_error(poll,ESP_FAIL); break; }
            sent += (size_t)n;
            client_notify_activity();
        }
        if (err == ESP_OK) {
            if (http_poll_headers(client,poll) < 0) err = http_poll_error(poll,ESP_FAIL);
            else client_notify_activity();
        }
        phase[4]=client_now_ms();
#if defined(ESP_PLATFORM)
        if (reused && err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_TIMEOUT && !weread_client_cancelled()) {
            http_cleanup(client); client = NULL; reused = false; continue;
        }
#endif
        break;
    }
    if (out_status) *out_status = esp_http_client_get_status_code(client);
    const size_t limit = 2u * 1024u * 1024u;
    int64_t content_len = err == ESP_OK ? esp_http_client_get_content_length(client) : -1;
    if (err == ESP_OK && content_len > (int64_t)limit) err = ESP_ERR_INVALID_SIZE;
    size_t used = 0;
    size_t capacity = (err == ESP_OK && content_len > 0) ? (size_t)content_len : 4096u;
    char *buffer = err == ESP_OK ? wr_alloc_raw(capacity + 1u) : NULL;
    if (err == ESP_OK && !buffer) err = ESP_ERR_NO_MEM;
    unsigned body_chunks = 0;
    while (err == ESP_OK) {
        if (weread_client_cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        weread_client_yield_for_display(NULL, NULL);
        if (used == capacity) {
            if (capacity >= limit) { err = ESP_ERR_INVALID_SIZE; break; }
            size_t next = capacity * 2u;
            if (next > limit) next = limit;
            char *larger = wr_realloc(buffer, next + 1u);
            if (!larger) { err = ESP_ERR_NO_MEM; break; }
            buffer = larger; capacity = next;
        }
        size_t avail = capacity - used;
        int want = avail > 8192u ? 8192 : (int)avail;
        int n = http_poll_read(client, buffer + used, want, poll);
        if (n < 0) { err = http_poll_error(poll,ESP_FAIL); break; }
        if (!n) {
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        used += (size_t)n;
        if (!(body_chunks++ & 7u)) client_notify_activity();
    }
    if (err == ESP_OK) {
        client_notify_activity();
        buffer[used] = 0;
        if (out_resp && capacity > used + 4096u) {
            char *shrunk = wr_realloc(buffer, used + 1);
            if (shrunk) buffer = shrunk;
        }
        if (out_resp) *out_resp = buffer; else free(buffer);
        if (out_len) *out_len = used;
    } else {
        free(buffer);
        if (out_resp) *out_resp = NULL;
        if (out_len) *out_len = 0;
    }
#if defined(ESP_PLATFORM)
    if (err != ESP_OK) {
        int tls=0,flags=0;(void)esp_http_client_get_and_clear_last_tls_error(client,&tls,&flags);
        ESP_LOGW("weread", "HTTP failed result=%d status=%d bytes=%u tls=%d flags=%x",
            (int)err,esp_http_client_get_status_code(client),(unsigned)used,tls,flags);
    }
#endif
    phase[5]=client_now_ms();
#if defined(ESP_PLATFORM)
    http_pool_put(url,client,poll,err==ESP_OK);
#else
    http_cleanup(client);
#endif
    phase[6]=client_now_ms();
    request_timing(url,phase,err, out_status?*out_status:0,used,reused);
    return err;
#else
    (void)phase;(void)background;return ESP_ERR_INVALID_STATE;
#endif
}

static void release_headers(char **p) {
    if(*p)weread_secure_zero(*p,4096);
    wr_release(p);
}
static bool request_headers(const char *url, const char *custom_headers, char final_headers[4096]) {
    char *combined __attribute__((cleanup(release_headers)))=wr_alloc(4096);
    if(!combined)return false;
    combined[0] = '\0';

    // If authenticated, inject session credentials if not already in custom_headers
    {
        WR_GUARD;
        const weread_auth_session_t *sess = weread_auth_get_current_session();
        if (sess && weread_auth_is_authenticated()) {
            if (!custom_headers || strstr(custom_headers, "vid:") == NULL) {
                size_t cur = strlen(combined);
                snprintf(combined + cur, 4096 - cur, "vid: %s\r\n", sess->vid);
            }
            if (!custom_headers || strstr(custom_headers, "accessToken:") == NULL) {
                size_t cur = strlen(combined);
                snprintf(combined + cur, 4096 - cur, "accessToken: %s\r\n", sess->access_token);
            }
            if (!custom_headers || strstr(custom_headers, "Cookie:") == NULL) {
                char refresh[WEREAD_TOKEN_MAX_LEN * 3];
                weread_client_url_encode(sess->refresh_token, refresh, sizeof(refresh));
                size_t cur = strlen(combined);
                snprintf(combined + cur, 4096 - cur,
                         "Cookie: wr_vid=%s; wr_skey=%s; wr_rt=%s\r\n",
                         sess->vid, sess->access_token, refresh);
            }
        }
    }

    // Append custom headers if present
    if (custom_headers && custom_headers[0] != '\0') {
        size_t cur = strlen(combined);
        snprintf(combined + cur, 4096 - cur, "%s", custom_headers);
    }

    // Standard WeRead headers
    if (strstr(combined, "User-Agent:") == NULL) {
        size_t cur = strlen(combined);
        snprintf(combined + cur, 4096 - cur, "User-Agent: %s\r\n", WEREAD_DEFAULT_USER_AGENT);
    }
    if (strstr(combined, "Accept:") == NULL) {
        size_t cur = strlen(combined);
        snprintf(combined + cur, 4096 - cur, "Accept: application/json, text/plain, */*\r\n");
    }
    if (strstr(combined, "Origin:") == NULL) {
        size_t cur = strlen(combined);
        snprintf(combined + cur, 4096 - cur, "Origin: https://weread.qq.com\r\n");
    }
    if (strstr(combined, "Referer:") == NULL) {
        size_t cur = strlen(combined);
        snprintf(combined + cur, 4096 - cur, "Referer: https://weread.qq.com/\r\n");
    }

    weread_client_strip_sensitive_headers(url, combined, final_headers, 4096);
    return true;

}

static int extract_error_code(const char *resp, size_t resp_len) {
    if (!resp || !resp_len) return 0;
    cJSON *chk = wr_parse_json(resp, resp_len + 1);
    int out = 0;
    if (chk) {
        cJSON *code = cJSON_GetObjectItem(chk, "errcode");
        if (!code) code = cJSON_GetObjectItem(chk, "errCode");
        if (!code) code = cJSON_GetObjectItem(chk, "ret");
        if (cJSON_IsNumber(code)) out = (int)code->valuedouble;
        cJSON_Delete(chk);
    }
    return out;
}

static int classify_session_code(const char *resp, size_t resp_len) {
    if (!resp || !resp_len) return 0;
    if (strstr(resp, "-2013") == NULL && strstr(resp, "-2012") == NULL) return 0;
    int c = extract_error_code(resp, resp_len);
    return (c == WEREAD_AUTH_ERRCODE_EXPIRED || c == WEREAD_AUTH_ERRCODE_TIMEOUT) ? c : 0;
}

static esp_err_t weread_client_request_internal(
    const char *url,
    const char *method,
    const char *custom_headers,
    const char *body,
    char **out_resp,
    size_t *out_len,
    int *out_status,
    bool allow_replay
) {
    if (weread_client_cancelled()) return ESP_ERR_INVALID_STATE;
    if (!url || !method) {
        return ESP_ERR_INVALID_ARG;
    }

    char *final_headers __attribute__((cleanup(release_headers)))=wr_alloc(4096);
    if(!final_headers || !request_headers(url,custom_headers,final_headers))return ESP_ERR_NO_MEM;

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;

    esp_err_t err = weread_client_dispatch_raw(url, method, final_headers, body, &resp, &resp_len, &status,s_polling);
    release_headers(&final_headers);

    if (out_status) {
        *out_status = status;
    }

    // 区分超时与会话撤销，超时保留本地凭据以供 Web 进度回退。/ Distinguish timeout from revocation; preserve credentials for web-progress fallback.
    int session_code = classify_session_code(resp, resp_len);
    bool is_expired = (status == 401) || (session_code == WEREAD_AUTH_ERRCODE_EXPIRED || session_code == WEREAD_AUTH_ERRCODE_TIMEOUT);

    bool is_weblogin = (strstr(url, "/weblogin") != NULL);

    bool allow_bg_timeout = s_background && session_code == WEREAD_AUTH_ERRCODE_TIMEOUT;
    bool authed = false;
    if ((!s_background || allow_bg_timeout) && !weread_client_cancelled() && err == ESP_OK && is_expired && !is_weblogin && weread_client_is_trusted_url(url)) {
        WR_GUARD;
        authed = weread_auth_is_authenticated();
    }
    if (authed) {
        if (allow_replay) {
            // Renew session
            esp_err_t renew_err = weread_auth_renew_session(NULL);
            if (renew_err == ESP_OK) {
                free(resp);
                // Replay request ONCE
                return weread_client_request_internal(
                    url, method, custom_headers, body, out_resp, out_len, out_status, false
                );
            } else {
                // Renewal failed
                if (!s_background && session_code == WEREAD_AUTH_ERRCODE_EXPIRED && renew_err == ESP_ERR_INVALID_STATE) {
                    {
                        WR_GUARD;
                        (void)weread_auth_mark_expired();
                    }
#if defined(ESP_PLATFORM)
                    http_pool_close();
#endif
                }
                if (out_resp) *out_resp = resp; else free(resp);
                if (out_len) *out_len = resp_len;
                return renew_err;
            }
        } else {
            // Already replayed once and still expired -> abort loop, mark expired
            if (!s_background && session_code == WEREAD_AUTH_ERRCODE_EXPIRED) {
                {
                    WR_GUARD;
                    (void)weread_auth_mark_expired();
                }
#if defined(ESP_PLATFORM)
                http_pool_close();
#endif
            }
            if (out_resp) *out_resp = resp;
            else if (resp) free(resp);
            if (out_len) *out_len = resp_len;
            return ESP_FAIL;
        }
    }

    if (out_resp) {
        *out_resp = resp;
    } else if (resp) {
        free(resp);
    }
    if (out_len) {
        *out_len = resp_len;
    }

    return err;
}

esp_err_t weread_client_request(
    const char *url,
    const char *method,
    const char *custom_headers,
    const char *body,
    char **out_resp,
    size_t *out_len,
    int *out_status
) { WR_MEMORY_SCOPE;
    return weread_client_request_internal(
        url, method, custom_headers, body, out_resp, out_len, out_status, true
    );
}

typedef struct { char headers[4096];weread_client_identity_t identity; } request_snapshot_t;
static bool identity_matches(const weread_client_identity_t *identity) {
    const weread_auth_session_t *session=weread_auth_get_current_session();
    return weread_auth_is_authenticated()&&session&&!strcmp(identity->root,weread_auth_get_storage_root())&&!strcmp(identity->vid,session->vid)&&
        !strcmp(identity->token,session->access_token)&&!strcmp(identity->refresh,session->refresh_token)&&!strcmp(identity->device,session->device_id);
}
bool weread_client_identity_get(weread_client_identity_t *out) { WR_GUARD;
    if(!out)return false;
    memset(out,0,sizeof(*out));
    const weread_auth_session_t *s=weread_auth_get_current_session();
    return weread_auth_is_authenticated()&&s&&wr_copy(out->root,sizeof(out->root),weread_auth_get_storage_root())&&wr_copy(out->vid,sizeof(out->vid),s->vid)&&
        wr_copy(out->token,sizeof(out->token),s->access_token)&&wr_copy(out->refresh,sizeof(out->refresh),s->refresh_token)&&
        wr_copy(out->device,sizeof(out->device),s->device_id);
}
static void release_snapshot(request_snapshot_t **p) {
    if (*p) { weread_secure_zero(*p, sizeof(**p)); free(*p); *p = NULL; }
}
static esp_err_t request_captured(const char *url, const char *method,
    const char *custom_headers, const char *body, char **out_resp, size_t *out_len, int *out_status,
    weread_client_identity_t *expected, bool allow_timeout_renew) {
    if (out_resp) *out_resp = NULL;
    if (out_len) *out_len = 0;
    if (out_status) *out_status = 0;
    if (!url || !method || !weread_client_is_trusted_url(url)) return ESP_ERR_INVALID_ARG;
    request_snapshot_t *snapshot __attribute__((cleanup(release_snapshot))) = wr_alloc(sizeof(*snapshot));
    if (!snapshot) return ESP_ERR_NO_MEM;
    char *response = NULL; size_t bytes = 0; int status = 0;
    esp_err_t err = ESP_OK;
    bool is_weblogin = strstr(url, "/weblogin") != NULL;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        free(response); response = NULL; bytes = 0; status = 0;
        {
            WR_GUARD;
            if (weread_client_cancelled() || (expected && !identity_matches(expected)) ||
                !weread_client_identity_get(&snapshot->identity)) return ESP_ERR_INVALID_STATE;
            if (!request_headers(url, custom_headers, snapshot->headers)) return ESP_ERR_NO_MEM;
        }
        // 网络只使用拥有的凭据；本地读取和保存不等待后台HTTP。/ Transport owns its credentials so local reads/saves do not wait for background HTTP.
        err = weread_client_dispatch_raw(url, method, snapshot->headers, body, &response, &bytes, &status, true);
        int session_code = err == ESP_OK ? classify_session_code(response, bytes) : 0;
        bool fg_bound = !s_background && expected != NULL;
        bool renew_timeout = allow_timeout_renew && session_code == WEREAD_AUTH_ERRCODE_TIMEOUT;
        bool renew_fg_auth = fg_bound && (session_code == WEREAD_AUTH_ERRCODE_EXPIRED ||
            (allow_timeout_renew && status == 401));
        if (err == ESP_OK && !weread_client_cancelled() && !is_weblogin &&
            (renew_timeout || renew_fg_auth)) {
            {
                WR_GUARD;
                if (!identity_matches(&snapshot->identity)) { free(response); return ESP_ERR_INVALID_STATE; }
            }
            if (!attempt) {
                esp_err_t renew_err = weread_auth_renew_session(NULL);
                if (renew_err != ESP_OK) {
                    if (fg_bound && session_code == WEREAD_AUTH_ERRCODE_EXPIRED &&
                        renew_err == ESP_ERR_INVALID_STATE) {
                        {
                            WR_GUARD;
                            (void)weread_auth_mark_expired();
                        }
#if defined(ESP_PLATFORM)
                        http_pool_close();
#endif
                    }
                    free(response);
                    return renew_err;
                }
                {
                    WR_GUARD;
                    const weread_auth_session_t *live = weread_auth_get_current_session();
                    if (!live || strcmp(snapshot->identity.root, weread_auth_get_storage_root()) ||
                        strcmp(snapshot->identity.vid, live->vid) || strcmp(snapshot->identity.device, live->device_id) ||
                        !weread_client_identity_get(&snapshot->identity)) {
                        free(response); return ESP_ERR_INVALID_STATE;
                    }
                    if (expected) *expected = snapshot->identity;
                }
                continue;
            }
            if (fg_bound) {
                if (session_code == WEREAD_AUTH_ERRCODE_EXPIRED) {
                    {
                        WR_GUARD;
                        (void)weread_auth_mark_expired();
                    }
#if defined(ESP_PLATFORM)
                    http_pool_close();
#endif
                }
                free(response);
                return ESP_FAIL;
            }
        }
        break;
    }
    {
        WR_GUARD;
        if (weread_client_cancelled() || !identity_matches(&snapshot->identity)) {
            free(response); return ESP_ERR_INVALID_STATE;
        }
    }
    if (out_resp) *out_resp = response; else free(response);
    if (out_len) *out_len = bytes;
    if (out_status) *out_status = status;
    return err;
}
esp_err_t weread_client_request_snapshot(const char *url,const char *method,const char *headers,
    const char *body,char **out,size_t *bytes,int *status) {
    return request_captured(url,method,headers,body,out,bytes,status,NULL,true);
}
esp_err_t weread_client_request_bound(const char *url,const char *method,const char *headers,
    const char *body,char **out,size_t *bytes,int *status,const weread_client_identity_t *expected) {
    if(!expected){if(out)*out=NULL;if(bytes)*bytes=0;if(status)*status=0;return ESP_ERR_INVALID_ARG;}
    return request_captured(url,method,headers,body,out,bytes,status,(weread_client_identity_t *)expected,false);
}
esp_err_t weread_client_request_bound_renew(const char *url,const char *method,const char *headers,
    const char *body,char **out,size_t *bytes,int *status,weread_client_identity_t *expected) {
    if(!expected){if(out)*out=NULL;if(bytes)*bytes=0;if(status)*status=0;return ESP_ERR_INVALID_ARG;}
    return request_captured(url,method,headers,body,out,bytes,status,expected,true);
}

void weread_client_set_download_transport(weread_download_transport_fn fn, void *ctx) {
    s_download_transport = fn; s_download_ctx = ctx;
}

#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
typedef struct { weread_download_info_t *info; char **redirect; bool invalid; } download_event_t;
static esp_err_t download_event(esp_http_client_event_t *event) {
    download_event_t *ctx = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_HEADER || !event->header_key || !event->header_value) return ESP_OK;
    if (!strcasecmp(event->header_key, "Location")) {
        size_t n = strlen(event->header_value);
        free(*ctx->redirect); *ctx->redirect = n < 2048 ? wr_alloc(n + 1) : NULL;
        if (!*ctx->redirect) ctx->invalid = true;
        else memcpy(*ctx->redirect, event->header_value, n + 1);
    }
    char *out = NULL; size_t cap = 0;
    if (!strcasecmp(event->header_key, "encryptkey")) { out = ctx->info->encrypt_key; cap = sizeof(ctx->info->encrypt_key); }
    if (!strcasecmp(event->header_key, "Content-Type")) { out = ctx->info->content_type; cap = sizeof(ctx->info->content_type); }
    if (out) {
        if (strlen(event->header_value) >= cap) ctx->invalid = true;
        else memcpy(out, event->header_value, strlen(event->header_value) + 1);
    }
    return ESP_OK;
}
static esp_err_t download_raw(const char *url, const char *headers, FILE *file, size_t limit,
    weread_cancel_fn cancel, void *ctx, weread_download_info_t *info, char **redirect, bool *may_be_json) {
    *may_be_json = false;
    http_poll_t *poll __attribute__((cleanup(wr_release)))=NULL;
    esp_err_t prepared=http_poll_prepare(url,s_polling,cancel,ctx,&poll);
    if(prepared!=ESP_OK)return prepared;
#if defined(ESP_PLATFORM)
    radio_ps_guard_t ps_guard __attribute__((cleanup(radio_ps_leave))) = radio_ps_enter();
    (void)ps_guard;
#endif
    download_event_t event = {.info = info, .redirect = redirect};
    esp_http_client_handle_t client = NULL;
    esp_err_t err = ESP_OK;
    bool reused = false; (void)reused;
    for (unsigned reuse_try = 0; reuse_try < 2; ++reuse_try) {
        if (reuse_try > 0) {
            free(*redirect); *redirect = NULL;
            memset(info, 0, sizeof(*info));
            event.invalid = false;
            http_poll_reset(poll);
        }
#if defined(ESP_PLATFORM)
        if (reuse_try == 0) {
            client = http_pool_take(url, poll, download_event, &event);
            reused = (client != NULL);
        }
#endif
        if (!client) {
            esp_http_client_config_t cfg = {.url = url, .timeout_ms = request_timeout_ms(url),
                .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
                .keep_alive_enable = true,
                .buffer_size = 4096, .buffer_size_tx = 2048,
                .event_handler = download_event, .user_data = &event};
            http_poll_config(poll,&cfg);
            client = esp_http_client_init(&cfg);
            if (!client) return ESP_ERR_NO_MEM;
        }
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        if (apply_headers(client, headers, true) != ESP_OK) { http_cleanup(client); return ESP_ERR_INVALID_SIZE; }
        if(poll)esp_http_client_set_header(client,"Host",http_poll_host(poll));
        http_active(poll?NULL:client);
        err = cancel && cancel(ctx) ? ESP_ERR_INVALID_STATE : http_poll_open(client,0,poll);
        if (err == ESP_OK && !reused) client_notify_activity();
        if (err == ESP_OK) {
            if (http_poll_headers(client,poll) < 0) err = http_poll_error(poll,ESP_FAIL);
            else client_notify_activity();
        }
#if defined(ESP_PLATFORM)
        if (reused && err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_TIMEOUT && !(cancel && cancel(ctx))) {
            http_cleanup(client); client = NULL; reused = false; continue;
        }
#endif
        break;
    }
    info->status = esp_http_client_get_status_code(client);
    if (event.invalid) err = ESP_ERR_INVALID_RESPONSE;
    // 跳转体不属于最终文件；URL和凭据仍由外层逐跳校验。/ Redirect bodies are not file data; the outer loop still validates every URL and its credentials.
    if(err==ESP_OK&&*redirect&&(info->status==301||info->status==302||info->status==303||info->status==307||info->status==308)){
        http_cleanup(client);return ESP_OK;
    }
    if (esp_http_client_get_content_length(client) > (int64_t)limit) err = ESP_ERR_INVALID_SIZE;
    const size_t chunk_cap = 8192;
    char *buffer __attribute__((cleanup(wr_release))) = err == ESP_OK ? wr_alloc(chunk_cap) : NULL;
    if (err == ESP_OK && !buffer) err = ESP_ERR_NO_MEM;
    unsigned chunks = 0;
    while (err == ESP_OK) {
        if (cancel && cancel(ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        weread_client_yield_for_display(cancel, ctx);
        int n = http_poll_read(client, buffer, (int)chunk_cap, poll);
        if (n < 0) { err = http_poll_error(poll,ESP_FAIL); break; }
        if (!n) {
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if ((size_t)n > limit - info->bytes) { err = ESP_ERR_INVALID_SIZE; break; }
        if (!info->bytes && n > 0) {
            int j = 0;
            while (j < n && isspace((unsigned char)buffer[j])) ++j;
            *may_be_json = (j == n || buffer[j] == '{' || buffer[j] == '[');
        }
        weread_client_yield_for_display(cancel, ctx);
        if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) { err = ESP_FAIL; break; }
        info->bytes += (size_t)n;
        // 每4块（32KiB）推进一次跨线程进度，节流避免逐块同步。/ Advance cross-thread progress every4 chunks (32KiB), keeping per-chunk sync cheap.
        if (!(++chunks & 3u)) {
            weread_client_report_download_progress(info->bytes, false);
            client_notify_activity();
        }
    }
    if (err == ESP_OK) client_notify_activity();
#if defined(ESP_PLATFORM)
    if (err != ESP_OK) {
        int tls=0,flags=0;(void)esp_http_client_get_and_clear_last_tls_error(client,&tls,&flags);
        ESP_LOGW("weread", "HTTP download failed result=%d status=%d bytes=%u tls=%d flags=%x",
            (int)err,info->status,(unsigned)info->bytes,tls,flags);
    }
#endif
#if defined(ESP_PLATFORM)
    http_pool_put(url, client, poll, err == ESP_OK && !(cancel && cancel(ctx)));
#else
    http_cleanup(client);
#endif
    return err;
}
#endif

esp_err_t weread_client_download(const char *url, FILE *file, size_t limit,
    weread_cancel_fn cancel, void *ctx, weread_download_info_t *info) { WR_MEMORY_SCOPE;
    if (!url || !file || !limit || !info) return ESP_ERR_INVALID_ARG;
    if (s_network) { esp_err_t ready = s_network(s_network_ctx); if (ready != ESP_OK) return ready; }
    char *redirected __attribute__((cleanup(wr_release))) = NULL;
    const char *current = url;
    unsigned redirects = 0;
    for (unsigned attempt = 0; attempt < 2;) {
        memset(info, 0, sizeof(*info));
        char *headers __attribute__((cleanup(release_headers)))=wr_alloc(4096);
        const char *accept=strstr(current,"/book/chapterdownload?")?NULL:"Accept: image/png,image/jpeg,image/webp,*/*\r\n";
        bool hdr_ok = false;
        if (headers) { WR_GUARD; hdr_ok = request_headers(current, accept, headers); }
        if (!headers || !hdr_ok) return ESP_ERR_NO_MEM;
        char *next __attribute__((cleanup(wr_release))) = NULL;
        esp_err_t err;
        bool may_be_json = true;
        if (s_download_transport) {
            err = s_download_transport(current, headers, file, limit, cancel, ctx, info, s_download_ctx);
            if (err == ESP_OK || info->status > 0 || info->bytes > 0) client_notify_activity();
        }
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
        else err = download_raw(current, headers, file, limit, cancel, ctx, info, &next, &may_be_json);
#else
        else err = ESP_ERR_INVALID_STATE;
#endif
        release_headers(&headers);
        if (err != ESP_OK || info->bytes > limit) return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
        if (fflush(file)) return ESP_FAIL;
        if (next && (info->status == 301 || info->status == 302 || info->status == 303 || info->status == 307 || info->status == 308)) {
            // 原生封面会跳到腾讯云；每跳重建请求头，外域绝不继承会话凭据。
            // Native covers redirect to Tencent storage; rebuild headers on every hop to strip credentials outside qq.com.
            if (++redirects > 3 || strncmp(next, "https://", 8) || strpbrk(next, "\r\n\\") ||
                !strcspn(next + 8, "/?#") || memchr(next + 8, '@', strcspn(next + 8, "/?#"))) return ESP_ERR_INVALID_RESPONSE;
            if (ftruncate(fileno(file), 0) || fseek(file, 0, SEEK_SET)) return ESP_FAIL;
            free(redirected); redirected = next; next = NULL; current = redirected;
            continue;
        }
        int code = 0;
        if (may_be_json && info->bytes <= 4096 && !fseek(file, 0, SEEK_SET)) {
            char *text __attribute__((cleanup(wr_release)))=wr_alloc(4097);
            if(!text)return ESP_ERR_NO_MEM;
            size_t n = fread(text, 1, 4096, file); text[n] = 0;
            code = extract_error_code(text, n);
        }
        if (!weread_client_is_trusted_url(current) ||
            (info->status != 401 && code != -2013 && code != -2012)) {
            // 正文传输结束且不再续签时才进入解包/校验；401/续签重下不会先跳到“校验”再倒回下载。
            // Only a finished, non-renewing body moves on to unpack/verify; a 401 renewal replay cannot jump to verify first.
            if (info->status == 200) weread_client_report_download_progress(info->bytes, true);
            return ESP_OK;
        }
        if (s_background && code != WEREAD_AUTH_ERRCODE_TIMEOUT) return ESP_ERR_INVALID_RESPONSE;
        if (attempt || (cancel && cancel(ctx))) {
            if (attempt && code == -2013 && !s_background) {
                { WR_GUARD; (void)weread_auth_mark_expired(); }
#if defined(ESP_PLATFORM)
                http_pool_close();
#endif
            }
            return ESP_FAIL;
        }
        err = weread_auth_renew_session(NULL);
        if (err != ESP_OK) {
            if (code == WEREAD_AUTH_ERRCODE_EXPIRED && !s_background && err == ESP_ERR_INVALID_STATE) {
#if defined(ESP_PLATFORM)
                http_pool_close();
#endif
            }
            return err;
        }
        if (ftruncate(fileno(file), 0) || fseek(file, 0, SEEK_SET)) return ESP_FAIL;
        ++attempt;
    }
    return ESP_FAIL;
}
