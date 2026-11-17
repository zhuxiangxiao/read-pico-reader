/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead secure HTTPS client with sensitive header stripping and renewal replay.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "weread_client.h"
#include "weread_store.h"
#include "weread_auth.h"
#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include <sys/socket.h>
#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#else
#include <pthread.h>
#endif
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
#else
static pthread_mutex_t s_http_mutex = PTHREAD_MUTEX_INITIALIZER;
static void http_lock(void) { pthread_mutex_lock(&s_http_mutex); }
static void http_unlock(void) { pthread_mutex_unlock(&s_http_mutex); }
#endif
static void http_active(esp_http_client_handle_t client) { http_lock(); s_active_http = client; http_unlock(); }
static void http_cleanup(esp_http_client_handle_t client) {
    http_lock(); if (s_active_http == client) s_active_http = NULL;
    esp_http_client_close(client); esp_http_client_cleanup(client); http_unlock();
}
#endif
void weread_client_abort_active(void) {
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
    http_lock();
    int fd = s_active_http ? esp_http_client_get_socket(s_active_http) : -1;
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    http_unlock();
#endif
}
static weread_timeout_fn s_timeout_budget;
static void *s_timeout_ctx;
void weread_client_set_timeout_budget(weread_timeout_fn fn, void *ctx) { s_timeout_budget=fn;s_timeout_ctx=ctx; }
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
static int request_timeout_ms(const char *url) {
    const char *authority=strstr(url,"://");
    const char *path=authority?strchr(authority+3,'/'):NULL;
    // 实际扫码接口长轮询约55秒；普通API仍使用15秒。/ The real QR endpoint long-polls for about55s; ordinary APIs retain15s.
    int ms=path&&!strncmp(path,"/web/getlogininfo",17)&&(path[17]==0||path[17]=='?')?65000:15000;
    int budget=s_timeout_budget?s_timeout_budget(s_timeout_ctx):0;
    return budget>0&&budget<ms?budget:ms;
}
#endif
static weread_network_fn s_network;
static void *s_network_ctx;
void weread_client_set_network(weread_network_fn fn, void *ctx) { s_network = fn; s_network_ctx = ctx; }

static weread_http_transport_fn s_transport = NULL;
static void *s_transport_user_ctx = NULL;
static weread_download_transport_fn s_download_transport;
static void *s_download_ctx;
static weread_cancel_fn s_cancel_query;
static void *s_cancel_context;
void weread_client_set_cancel(weread_cancel_fn fn, void *ctx) { s_cancel_query = fn; s_cancel_context = ctx; }
bool weread_client_cancelled(void) { return s_cancel_query && s_cancel_query(s_cancel_context); }

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

static struct { char vid[WEREAD_VID_MAX_LEN], token[WEREAD_TOKEN_MAX_LEN], refresh[WEREAD_TOKEN_MAX_LEN]; } s_login_cookies;
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
static esp_err_t weread_client_dispatch_raw(
    const char *url,
    const char *method,
    const char *headers,
    const char *body,
    char **out_resp,
    size_t *out_len,
    int *out_status
) {
    weread_secure_zero(&s_login_cookies,sizeof(s_login_cookies));
    if (weread_client_cancelled()) return ESP_ERR_INVALID_STATE;
    if (s_network) { esp_err_t ready = s_network(s_network_ctx); if (ready != ESP_OK) {
        ESP_LOGW("weread", "network preparation failed result=%d", (int)ready); return ready;
    } }
    if (s_transport) {
        return s_transport(url, method, headers, body, out_resp, out_len, out_status, s_transport_user_ctx);
    }

#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
    const char *authority_end=strstr(url,"://");
    const char *url_path=authority_end?strchr(authority_end+3,'/'):NULL;
    bool login = weread_client_is_trusted_url(url) && url_path && !strncmp(url_path,"/weblogin",9) &&
        (url_path[9] == 0 || url_path[9] == '?');
#if !defined(ESP_PLATFORM)
    if (url_path && !strncmp(url_path,"/weblogin",9) && !strncmp(url,"http://127.0.0.1:",17)) login=true;
#endif
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = request_timeout_ms(url),
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = login_header, .user_data = &login,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
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

    if (headers) {
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
                char k[128] = {0};
                char v[512] = {0};
                const char *key_end = colon;
                while (key_end > k_start && (*(key_end - 1) == ' ' || *(key_end - 1) == '\t')) key_end--;
                size_t klen = key_end - k_start;
                if (klen >= sizeof(k)) klen = sizeof(k) - 1;
                memcpy(k, k_start, klen);

                const char *val_start = colon + 1;
                while (val_start < line_end && (*val_start == ' ' || *val_start == '\t')) val_start++;
                const char *val_end = line_end;
                while (val_end > val_start && (*(val_end - 1) == '\r' || *(val_end - 1) == ' ' || *(val_end - 1) == '\t')) val_end--;
                size_t vlen = val_end - val_start;
                if (vlen >= sizeof(v)) vlen = sizeof(v) - 1;
                memcpy(v, val_start, vlen);

                esp_http_client_set_header(client, k, v);
            }
        }
    }

    // 按流读取含 chunked 的响应；禁止跨域自动重定向凭据。/ Read streamed/chunked bodies; never redirect credentials automatically.
    http_active(client);
    size_t body_len = body ? strlen(body) : 0;
    esp_err_t err = weread_client_cancelled() ? ESP_ERR_INVALID_STATE : esp_http_client_open(client, (int)body_len);
    size_t sent = 0;
    while (err == ESP_OK && sent < body_len) {
        int n = esp_http_client_write(client, body + sent, (int)(body_len - sent));
        if (n <= 0) { err = ESP_FAIL; break; }
        sent += (size_t)n;
    }
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (out_status) *out_status = esp_http_client_get_status_code(client);
    const size_t limit = 2u * 1024u * 1024u;
    size_t used = 0, capacity = 4096;
    char *buffer = err == ESP_OK ? wr_alloc(capacity + 1) : NULL;
    if (err == ESP_OK && !buffer) err = ESP_ERR_NO_MEM;
    if (err == ESP_OK && esp_http_client_get_content_length(client) > (int64_t)limit) err = ESP_ERR_INVALID_SIZE;
    while (err == ESP_OK) {
        if (weread_client_cancelled()) { err = ESP_ERR_INVALID_STATE; break; }
        char chunk[2048];
        int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n < 0) { err = ESP_FAIL; break; }
        if (!n) {
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if ((size_t)n > limit - used) { err = ESP_ERR_INVALID_SIZE; break; }
        if (used + (size_t)n > capacity) {
            size_t next = capacity * 2;
            if (next > limit) next = limit;
            char *larger = wr_realloc(buffer, next + 1);
            if (!larger) { err = ESP_ERR_NO_MEM; break; }
            buffer = larger; capacity = next;
        }
        memcpy(buffer + used, chunk, (size_t)n); used += (size_t)n;
    }
    if (err == ESP_OK) {
        buffer[used] = 0;
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
    http_cleanup(client);
    return err;
#else
    return ESP_ERR_INVALID_STATE;
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

    esp_err_t err = weread_client_dispatch_raw(url, method, final_headers, body, &resp, &resp_len, &status);
    release_headers(&final_headers);

    if (out_status) {
        *out_status = status;
    }

    // 区分超时与会话撤销，超时保留本地凭据以供 Web 进度回退。/ Distinguish timeout from revocation; preserve credentials for web-progress fallback.
    int session_code = 0;
    bool is_expired = (status == 401);
    // HTTP状态与业务码分别判断；401也可能明确撤销会话。/ Classify revocation even in HTTP401 bodies.
    if (resp && resp_len > 0) {
        if (strstr(resp, "-2013") != NULL || strstr(resp, "-2012") != NULL) {
            cJSON *chk = wr_parse_json(resp,resp_len + 1);
            if (chk) {
                cJSON *code = cJSON_GetObjectItem(chk, "errcode");
                if (!code) code = cJSON_GetObjectItem(chk, "errCode");
                if (!code) code = cJSON_GetObjectItem(chk, "ret");
                if (code && cJSON_IsNumber(code)) {
                    int c = (int)code->valuedouble;
                    if (c == WEREAD_AUTH_ERRCODE_EXPIRED || c == WEREAD_AUTH_ERRCODE_TIMEOUT) {
                        session_code = c;
                        is_expired = true;
                    }
                }
                cJSON_Delete(chk);
            }
        }
    }

    bool is_weblogin = (strstr(url, "/weblogin") != NULL);

    if (!weread_client_cancelled() && err == ESP_OK && is_expired && !is_weblogin && weread_client_is_trusted_url(url) &&
        weread_auth_is_authenticated()) {
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
                if (session_code == WEREAD_AUTH_ERRCODE_EXPIRED && renew_err == ESP_ERR_INVALID_STATE)
                    weread_auth_clear_session();
                if (out_resp) *out_resp = resp; else free(resp);
                if (out_len) *out_len = resp_len;
                return renew_err;
            }
        } else {
            // Already replayed once and still expired -> abort loop, clear session
            if (session_code == WEREAD_AUTH_ERRCODE_EXPIRED) weread_auth_clear_session();
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
) { WR_GUARD;
    return weread_client_request_internal(
        url, method, custom_headers, body, out_resp, out_len, out_status, true
    );
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
    weread_cancel_fn cancel, void *ctx, weread_download_info_t *info, char **redirect) {
    download_event_t event = {.info = info, .redirect = redirect};
    esp_http_client_config_t cfg = {.url = url, .timeout_ms = request_timeout_ms(url),
        .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
        .event_handler = download_event, .user_data = &event};
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_ERR_NO_MEM;
    const char *p = headers;
    while (*p) {
        const char *end = strchr(p, '\n'); if (!end) end = p + strlen(p);
        const char *colon = memchr(p, ':', (size_t)(end - p));
        if (colon) {
            char key[128], value[1024];
            size_t k = (size_t)(colon - p);
            const char *v = colon + 1; while (v < end && (*v == ' ' || *v == '\t')) ++v;
            size_t n = (size_t)(end - v); if (n && v[n - 1] == '\r') --n;
            if (k >= sizeof(key) || n >= sizeof(value)) { esp_http_client_cleanup(client); return ESP_ERR_INVALID_SIZE; }
            memcpy(key, p, k); key[k] = 0; memcpy(value, v, n); value[n] = 0;
            esp_http_client_set_header(client, key, value);
        }
        p = *end ? end + 1 : end;
    }
    http_active(client);
    esp_err_t err = cancel && cancel(ctx) ? ESP_ERR_INVALID_STATE : esp_http_client_open(client, 0);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    info->status = esp_http_client_get_status_code(client);
    if (event.invalid) err = ESP_ERR_INVALID_RESPONSE;
    if (esp_http_client_get_content_length(client) > (int64_t)limit) err = ESP_ERR_INVALID_SIZE;
    char buffer[2048];
    while (err == ESP_OK) {
        if (cancel && cancel(ctx)) { err = ESP_ERR_INVALID_STATE; break; }
        int n = esp_http_client_read(client, buffer, sizeof(buffer));
        if (n < 0) { err = ESP_FAIL; break; }
        if (!n) {
            if (!esp_http_client_is_complete_data_received(client)) err = ESP_ERR_INVALID_RESPONSE;
            break;
        }
        if ((size_t)n > limit - info->bytes) { err = ESP_ERR_INVALID_SIZE; break; }
        if (fwrite(buffer, 1, (size_t)n, file) != (size_t)n) { err = ESP_FAIL; break; }
        info->bytes += (size_t)n;
    }
    http_cleanup(client);
    return err;
}
#endif

esp_err_t weread_client_download(const char *url, FILE *file, size_t limit,
    weread_cancel_fn cancel, void *ctx, weread_download_info_t *info) { WR_GUARD;
    if (!url || !file || !limit || !info) return ESP_ERR_INVALID_ARG;
    if (s_network) { esp_err_t ready = s_network(s_network_ctx); if (ready != ESP_OK) return ready; }
    char *redirected __attribute__((cleanup(wr_release))) = NULL;
    const char *current = url;
    unsigned redirects = 0;
    for (unsigned attempt = 0; attempt < 2;) {
        memset(info, 0, sizeof(*info));
        char *headers __attribute__((cleanup(release_headers)))=wr_alloc(4096);
        const char *accept=strstr(current,"/book/chapterdownload?")?NULL:"Accept: image/png,image/jpeg,image/webp,*/*\r\n";
        if(!headers || !request_headers(current,accept,headers))return ESP_ERR_NO_MEM;
        char *next __attribute__((cleanup(wr_release))) = NULL;
        esp_err_t err;
        if (s_download_transport) err = s_download_transport(current, headers, file, limit, cancel, ctx, info, s_download_ctx);
#if defined(ESP_PLATFORM) || defined(WEREAD_USE_ESP_HTTP_CLIENT)
        else err = download_raw(current, headers, file, limit, cancel, ctx, info, &next);
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
        if (info->bytes <= 4096 && !fseek(file, 0, SEEK_SET)) {
            char *text __attribute__((cleanup(wr_release)))=wr_alloc(4097);
            if(!text)return ESP_ERR_NO_MEM;
            size_t n = fread(text, 1, 4096, file); text[n] = 0;
            cJSON *r = wr_parse_json(text,strlen(text)+1), *v = cJSON_GetObjectItem(r, "errcode");
            if (!v) v = cJSON_GetObjectItem(r, "ret");
            if (cJSON_IsNumber(v)) code = v->valueint;
            cJSON_Delete(r);
        }
        if (!weread_client_is_trusted_url(current) ||
            (info->status != 401 && code != -2013 && code != -2012)) return ESP_OK;
        if (attempt || (cancel && cancel(ctx))) {
            if (attempt && code == -2013) weread_auth_clear_session();
            return ESP_FAIL;
        }
        err = weread_auth_renew_session(NULL);
        if (err != ESP_OK) return err;
        if (ftruncate(fileno(file), 0) || fseek(file, 0, SEEK_SET)) return ESP_FAIL;
        ++attempt;
    }
    return ESP_FAIL;
}
