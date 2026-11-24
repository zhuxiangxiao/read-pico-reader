/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead secure HTTPS client with sensitive header stripping and renewal replay.
 */
#pragma once

#include "weread_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the WeRead HTTP client subsystem.
 *
 * @return ESP_OK on success.
 */
esp_err_t weread_client_init(void);

/**
 * @brief Set or replace the underlying HTTP transport function.
 *
 * Used for host testing or injecting mock HTTP transports.
 * If transport is NULL, resets to default esp_http_client transport.
 *
 * @param transport Custom transport callback function, or NULL for default.
 * @param user_ctx User context pointer passed to transport.
 */
void weread_client_set_transport(weread_http_transport_fn transport, void *user_ctx);

typedef bool (*weread_cancel_fn)(void *ctx);
/// 在服务空闲时安装长期有效的取消查询；回调不得获取服务锁。/ Install a lifetime-stable cancellation query while idle; it must not acquire the service lock.
void weread_client_set_cancel(weread_cancel_fn fn, void *ctx);
bool weread_client_cancelled(void);
/// 非阻塞中断自有socket；调用方先设置取消查询，正在清理时不等待句柄锁。/ Abort the owned socket without blocking; set cancellation first and do not wait on cleanup.
void weread_client_abort_active(void);
/// 仅最近一次受信任登录响应的指定Cookie，下一次请求即擦除。/ Selected cookies from the last trusted login response; erased at the next request.
void weread_client_forget_login_cookies(void);
bool weread_client_login_cookie(const char *key, char *out, size_t cap);
typedef esp_err_t (*weread_network_fn)(void *ctx);
void weread_client_set_network(weread_network_fn fn, void *ctx);
/// 返回0沿用请求默认等待；正数缩短截止时间，扫码长轮询仍可即时取消。/ Zero retains the request default; positive values cap its deadline. QR long polls remain cancellable.
typedef int (*weread_timeout_fn)(void *ctx);
void weread_client_set_timeout_budget(weread_timeout_fn fn, void *ctx);
/// 低优先级任务遇401保留凭据，不自动续期或清登录；不选择传输方式。
/// Background jobs retain credentials on401 without renewal/logout; transport selection is independent.
void weread_client_set_background(bool enabled);
/// 仅选择WeRead可取消传输，不改变前台续期/撤销策略。/ Select cancellable WeRead transport without changing foreground renewal.
void weread_client_set_polling(bool enabled);
typedef struct {
    int status;
    size_t bytes;
    char encrypt_key[512], content_type[96];
} weread_download_info_t;
/// 测试传输须遵守 limit/cancel，并写入给定暂存文件。/ Test transports obey limit/cancel and write the supplied staging file.
typedef esp_err_t (*weread_download_transport_fn)(const char *url, const char *headers,
    FILE *file, size_t limit, weread_cancel_fn cancel, void *cancel_ctx,
    weread_download_info_t *info, void *ctx);
void weread_client_set_download_transport(weread_download_transport_fn fn, void *ctx);
void weread_client_set_radio_awake(bool awake);
/// 下载正文进度回调：bytes 为本次HTTP尝试的已写字节，done 表示正文传输完成、进入解包校验。
/// Download body progress: bytes are written for this HTTP attempt; done marks body completion before unpacking.
typedef void (*weread_download_progress_fn)(uint64_t bytes, bool done, void *ctx);
/// 服务空闲时安装长期有效的下载进度回调；NULL清除。回调由worker同步执行。
/// Install a lifetime-stable download progress callback while idle; NULL clears it. It runs synchronously on the worker.
void weread_client_set_download_progress(weread_download_progress_fn fn, void *ctx);
typedef void (*weread_client_activity_fn_t)(void *ctx);
/// 服务空闲或切换回调时安装线程安全的HTTP活动通知；在真实HTTP收发时于锁外触发，不得在回调内假设持有服务锁。
/// Install a thread-safe HTTP activity callback; invoked unlocked on real HTTP I/O without holding the service lock.
void weread_client_set_activity_cb(weread_client_activity_fn_t cb, void *ctx);
/// 传输实现上报写入字节数；原生ESP路径每4块调用，宿主替身可在写入后显式调用。
/// Transport implementations report written bytes; the native ESP path calls it every4 chunks.
void weread_client_report_download_progress(uint64_t bytes, bool done);
/// 下载到调用方的空暂存文件；返回前不关闭文件。/ Download into caller-owned empty staging; leave it open on return.
esp_err_t weread_client_download(const char *url, FILE *file, size_t limit,
    weread_cancel_fn cancel, void *ctx, weread_download_info_t *info);

/**
 * @brief Check whether an outbound URL is a trusted WeRead endpoint.
 *
 * Returns true if scheme is HTTPS and host matches qq.com or ends with .qq.com,
 * and does not contain untrusted paths or keywords. Also allows loopback addresses
 * (127.0.0.1 / localhost) without untrusted paths during host testing.
 *
 * @param url Full target URL.
 * @return true if trusted, false if untrusted.
 */
bool weread_client_is_trusted_url(const char *url);

/**
 * @brief Strip sensitive authentication headers from a header block for untrusted targets.
 *
 * If the URL is untrusted, strips Cookie, Authorization, accessToken, and vid.
 * If the URL is trusted, preserves all headers.
 *
 * @param url Target URL.
 * @param in_headers Input header block (\r\n or \n delimited).
 * @param out_headers Output buffer for stripped headers.
 * @param out_cap Capacity of out_headers buffer.
 */
void weread_client_strip_sensitive_headers(const char *url, const char *in_headers, char *out_headers, size_t out_cap);

/**
 * @brief Retrieve the base API URL (default: https://i.weread.qq.com, or WEREAD_API_BASE env override).
 *
 * @return const char* Pointer to base URL string.
 */
const char *weread_client_get_base_url(void);
/// RFC 3986 查询值与 Cookie 百分号编码。/ RFC 3986 percent encoding for query/cookie values.
bool weread_client_url_encode(const char *text, char *out, size_t cap);

/**
 * @brief Dispatch an HTTP request with automatic credential injection, security stripping,
 *        and automatic -2013 renewal & single-request replay.
 *
 * 1. Checks URL trust: if untrusted, sensitive headers are stripped.
 * 2. If authenticated, injects vid, accessToken, and Cookie headers.
 * 3. Appends standard WeRead headers (User-Agent, Accept, Origin, Referer).
 * 4. Dispatches request via active transport.
 * 5. If response indicates session expired (-2013 or -2012):
 *    - Automatically calls weread_auth_renew_session().
 *    - If renewal succeeds, re-issues the request ONCE (single replay).
 *    - If renewal or replay fails, clears session credentials.
 * 6. Populates out_resp with dynamic heap-allocated buffer (caller must free with free()),
 *    out_len with response length, and out_status with HTTP status code.
 *
 * @param url Target URL.
 * @param method HTTP method string ("GET", "POST", etc.).
 * @param custom_headers Additional custom headers (\r\n separated), or NULL.
 * @param body Request body string/data, or NULL.
 * @param out_resp Pointer to char* receiving newly allocated response body (caller frees), or NULL.
 * @param out_len Pointer to size_t receiving response body byte count, or NULL.
 * @param out_status Pointer to int receiving HTTP status code, or NULL.
 * @return ESP_OK on successful HTTP completion, or error code.
 */
esp_err_t weread_client_request(
    const char *url,
    const char *method,
    const char *custom_headers,
    const char *body,
    char **out_resp,
    size_t *out_len,
    int *out_status
);

/// 后台补报使用独立凭据快照，传输不持服务锁；过时会话拒收，401不续期或清登录。
/// Background reports own their credentials and release the service lock during transport; stale sessions are rejected and401 never renews or clears login.
esp_err_t weread_client_request_snapshot(const char *url, const char *method,
    const char *custom_headers, const char *body, char **out_resp, size_t *out_len, int *out_status);
typedef struct {
    char root[256],vid[WEREAD_VID_MAX_LEN],token[WEREAD_TOKEN_MAX_LEN],refresh[WEREAD_TOKEN_MAX_LEN],device[WEREAD_DEVICE_ID_MAX_LEN];
} weread_client_identity_t;
/// 跨多个请求绑定同一身份；捕获与请求构造均在短服务锁内完成。/ Bind multiple requests to one identity using short locked capture/construction.
bool weread_client_identity_get(weread_client_identity_t *out);
esp_err_t weread_client_request_bound(const char *url,const char *method,const char *custom_headers,
    const char *body,char **out_resp,size_t *out_len,int *out_status,const weread_client_identity_t *expected);
/// 允许在短令牌超时(-2012)时原位续期并同步更新绑定身份中的凭据。/ Allow in-place -2012 short-token renewal and refresh the bound identity credentials.
esp_err_t weread_client_request_bound_renew(const char *url,const char *method,const char *custom_headers,
    const char *body,char **out_resp,size_t *out_len,int *out_status,weread_client_identity_t *expected);

#ifdef __cplusplus
}
#endif
