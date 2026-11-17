/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead host test ESP-IDF esp_http_client mock declarations.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
    HTTP_METHOD_PUT,
    HTTP_METHOD_PATCH,
    HTTP_METHOD_DELETE,
    HTTP_METHOD_HEAD,
} esp_http_client_method_t;

typedef enum {
    HTTP_EVENT_ERROR = 0,
    HTTP_EVENT_ON_CONNECTED,
    HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_HEADER_SENT = HTTP_EVENT_HEADERS_SENT,
    HTTP_EVENT_ON_HEADER,
    HTTP_EVENT_ON_DATA,
    HTTP_EVENT_ON_FINISH,
    HTTP_EVENT_DISCONNECTED,
} esp_http_client_event_id_t;

struct esp_http_client;
typedef struct esp_http_client *esp_http_client_handle_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    esp_http_client_handle_t client;
    void *data;
    int data_len;
    void *user_data;
    char *header_key;
    char *header_value;
} esp_http_client_event_t;

typedef struct {
    const char *url;
    const char *host;
    int port;
    const char *path;
    const char *query;
    esp_http_client_method_t method;
    int timeout_ms;
    int buffer_size;
    int buffer_size_tx;
    int (*crt_bundle_attach)(void *conf);
    bool disable_auto_redirect;
    bool keep_alive_enable;
    bool is_async,skip_cert_common_name_check;
    const char *common_name;
    int transport_type;
    int (*event_handler)(esp_http_client_event_t *evt);
    void *user_data;
} esp_http_client_config_t;
#define HTTP_TRANSPORT_OVER_SSL 2

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_get_header(esp_http_client_handle_t client, const char *key, char **value);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif

int esp_http_client_get_socket(esp_http_client_handle_t client);
