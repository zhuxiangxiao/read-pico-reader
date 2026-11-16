/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead host test esp_http_client loopback mock implementation.
 */

#include "esp_http_client.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define MAX_HEADERS 64
#define RECV_BUF_SIZE 4096

typedef struct {
    char *key;
    char *value;
} http_header_entry_t;

struct esp_http_client {
    char *url;
    char *host;
    char *path;
    esp_http_client_method_t method;
    int timeout_ms;

    http_header_entry_t req_headers[MAX_HEADERS];
    size_t req_header_count;

    char *post_data;
    size_t post_len;

    int (*event_handler)(esp_http_client_event_t *evt);
    void *user_data;

    // Connection & response state
    int sock_fd;
    int status_code;
    int64_t content_length;
    int64_t bytes_read;

    http_header_entry_t resp_headers[MAX_HEADERS];
    size_t resp_header_count;

    char resp_buf[RECV_BUF_SIZE];
    size_t resp_buf_len;
    size_t resp_buf_pos;
    bool headers_fetched;
    bool stream_eof;
};

static void str_to_lower(char *str)
{
    while (*str) {
        *str = (char)tolower((unsigned char)*str);
        str++;
    }
}

static void free_headers(http_header_entry_t *headers, size_t *count)
{
    for (size_t i = 0; i < *count; ++i) {
        free(headers[i].key);
        free(headers[i].value);
        headers[i].key = NULL;
        headers[i].value = NULL;
    }
    *count = 0;
}

static void parse_url(const char *url, char **out_host, char **out_path)
{
    if (out_host) *out_host = NULL;
    if (out_path) *out_path = NULL;
    if (!url) return;

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    }

    const char *slash = strchr(p, '/');
    if (slash) {
        size_t host_len = (size_t)(slash - p);
        char *h = (char *)malloc(host_len + 1);
        if (h) {
            memcpy(h, p, host_len);
            h[host_len] = '\0';
            // Strip port from host if present
            char *colon = strchr(h, ':');
            if (colon) *colon = '\0';
            if (out_host) *out_host = h;
        }
        if (out_path) *out_path = strdup(slash);
    } else {
        if (out_host) *out_host = strdup(p);
        if (out_path) *out_path = strdup("/");
    }
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct esp_http_client *client = (struct esp_http_client *)calloc(1, sizeof(struct esp_http_client));
    if (!client) return NULL;

    client->sock_fd = -1;
    client->timeout_ms = config && config->timeout_ms > 0 ? config->timeout_ms : 5000;
    client->content_length = -1;

    if (config) {
        if (config->url) {
            esp_http_client_set_url(client, config->url);
        }
        client->method = config->method;
        client->event_handler = config->event_handler;
        client->user_data = config->user_data;
    }

    return client;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url)
{
    if (!client || !url) return ESP_ERR_INVALID_ARG;
    free(client->url);
    free(client->host);
    free(client->path);
    client->url = strdup(url);
    parse_url(url, &client->host, &client->path);
    return ESP_OK;
}

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    client->method = method;
    return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value)
{
    if (!client || !key || !value) return ESP_ERR_INVALID_ARG;

    for (size_t i = 0; i < client->req_header_count; ++i) {
        if (strcasecmp(client->req_headers[i].key, key) == 0) {
            free(client->req_headers[i].value);
            client->req_headers[i].value = strdup(value);
            return ESP_OK;
        }
    }

    if (client->req_header_count >= MAX_HEADERS) return ESP_ERR_NO_MEM;
    client->req_headers[client->req_header_count].key = strdup(key);
    client->req_headers[client->req_header_count].value = strdup(value);
    client->req_header_count++;
    return ESP_OK;
}

esp_err_t esp_http_client_get_header(esp_http_client_handle_t client, const char *key, char **value)
{
    if (!client || !key || !value) return ESP_ERR_INVALID_ARG;
    *value = NULL;
    for (size_t i = 0; i < client->resp_header_count; ++i) {
        if (strcasecmp(client->resp_headers[i].key, key) == 0) {
            *value = client->resp_headers[i].value;
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    free(client->post_data);
    client->post_data = NULL;
    client->post_len = 0;

    if (data && len > 0) {
        client->post_data = (char *)malloc((size_t)len + 1);
        if (!client->post_data) return ESP_ERR_NO_MEM;
        memcpy(client->post_data, data, (size_t)len);
        client->post_data[len] = '\0';
        client->post_len = (size_t)len;
    }
    return ESP_OK;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    client->timeout_ms = timeout_ms;
    return ESP_OK;
}

static int get_mock_server_port(void)
{
    const char *p_env = getenv("WEREAD_MOCK_PORT");
    if (p_env && p_env[0] != '\0') {
        int p = atoi(p_env);
        if (p > 0) return p;
    }
    return 8989;
}

static const char *method_str(esp_http_client_method_t m)
{
    switch (m) {
        case HTTP_METHOD_POST: return "POST";
        case HTTP_METHOD_PUT: return "PUT";
        case HTTP_METHOD_PATCH: return "PATCH";
        case HTTP_METHOD_DELETE: return "DELETE";
        case HTTP_METHOD_HEAD: return "HEAD";
        case HTTP_METHOD_GET:
        default: return "GET";
    }
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
    }

    int port = get_mock_server_port();
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return ESP_FAIL;

    struct timeval tv;
    tv.tv_sec = client->timeout_ms / 1000;
    tv.tv_usec = (client->timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in serv_addr;
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        close(sock);
        return ESP_FAIL;
    }
    client->sock_fd = sock;

    // Send HTTP Request header
    char req[8192];
    size_t offset = 0;
    const char *m = method_str(client->method);
    const char *path = client->path ? client->path : "/";
    const char *host = client->host ? client->host : "127.0.0.1";

    offset += (size_t)snprintf(req + offset, sizeof(req) - offset,
                              "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n",
                              m, path, host);

    // Set Content-Length if body is provided
    size_t body_len = (write_len > 0) ? (size_t)write_len : client->post_len;
    if (body_len > 0 || client->method == HTTP_METHOD_POST) {
        offset += (size_t)snprintf(req + offset, sizeof(req) - offset,
                                  "Content-Length: %zu\r\n", body_len);
    }

    // Append custom request headers
    for (size_t i = 0; i < client->req_header_count; ++i) {
        offset += (size_t)snprintf(req + offset, sizeof(req) - offset,
                                  "%s: %s\r\n",
                                  client->req_headers[i].key,
                                  client->req_headers[i].value);
    }
    offset += (size_t)snprintf(req + offset, sizeof(req) - offset, "\r\n");

    ssize_t sent = send(sock, req, offset, 0);
    if (sent < (ssize_t)offset) {
        close(sock);
        client->sock_fd = -1;
        return ESP_FAIL;
    }

    // If post data was already set and write_len <= 0, write body immediately
    if (client->post_data && client->post_len > 0 && write_len <= 0) {
        ssize_t b_sent = send(sock, client->post_data, client->post_len, 0);
        if (b_sent < (ssize_t)client->post_len) {
            close(sock);
            client->sock_fd = -1;
            return ESP_FAIL;
        }
    }

    client->headers_fetched = false;
    client->bytes_read = 0;
    client->resp_buf_len = 0;
    client->resp_buf_pos = 0;
    client->stream_eof = false;
    free_headers(client->resp_headers, &client->resp_header_count);

    return ESP_OK;
}

int esp_http_client_write(esp_http_client_handle_t client, const char *buffer, int len)
{
    if (!client || client->sock_fd < 0 || !buffer || len <= 0) return -1;
    ssize_t sent = send(client->sock_fd, buffer, (size_t)len, 0);
    return (int)sent;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client)
{
    if (!client || client->sock_fd < 0) return -1;
    if (client->headers_fetched) return client->content_length;

    char header_accumulator[16384];
    size_t header_len = 0;
    char *end_of_headers = NULL;

    while (header_len < sizeof(header_accumulator) - 1) {
        ssize_t n = recv(client->sock_fd, header_accumulator + header_len, 1, 0);
        if (n <= 0) break;
        header_len++;
        header_accumulator[header_len] = '\0';

        if (header_len >= 4 &&
            strcmp(header_accumulator + header_len - 4, "\r\n\r\n") == 0) {
            end_of_headers = header_accumulator + header_len - 4;
            break;
        }
    }

    if (!end_of_headers) {
        return -1;
    }

    *end_of_headers = '\0';

    // Parse status line: HTTP/1.1 200 OK
    char *line_save = NULL;
    char *line = strtok_r(header_accumulator, "\r\n", &line_save);
    if (!line) return -1;

    int status = 0;
    if (sscanf(line, "HTTP/%*s %d", &status) == 1) {
        client->status_code = status;
    } else {
        client->status_code = 0;
    }

    // Parse subsequent header lines
    while ((line = strtok_r(NULL, "\r\n", &line_save)) != NULL) {
        char *colon = strchr(line, ':');
        if (!colon) continue;
        *colon = '\0';
        char *val = colon + 1;
        while (*val == ' ' || *val == '\t') val++;

        if (client->resp_header_count < MAX_HEADERS) {
            client->resp_headers[client->resp_header_count].key = strdup(line);
            client->resp_headers[client->resp_header_count].value = strdup(val);
            client->resp_header_count++;
        }

        char key_lower[128];
        snprintf(key_lower, sizeof(key_lower), "%s", line);
        str_to_lower(key_lower);

        if (strcmp(key_lower, "content-length") == 0) {
            client->content_length = atoll(val);
        }

        if (client->event_handler) {
            esp_http_client_event_t evt;
            memset(&evt, 0, sizeof(evt));
            evt.event_id = HTTP_EVENT_ON_HEADER;
            evt.client = client;
            evt.user_data = client->user_data;
            evt.header_key = line;
            evt.header_value = val;
            client->event_handler(&evt);
        }
    }

    client->headers_fetched = true;
    return client->content_length;
}

int esp_http_client_get_status_code(esp_http_client_handle_t client)
{
    return client ? client->status_code : 0;
}

int64_t esp_http_client_get_content_length(esp_http_client_handle_t client)
{
    return client ? client->content_length : -1;
}

int esp_http_client_read(esp_http_client_handle_t client, char *buffer, int len)
{
    if (!client || client->sock_fd < 0 || !buffer || len <= 0) return -1;
    if (!client->headers_fetched) {
        if (esp_http_client_fetch_headers(client) < 0 && client->status_code == 0) {
            return -1;
        }
    }

    if (client->stream_eof) return 0;

    ssize_t n = recv(client->sock_fd, buffer, (size_t)len, 0);
    if (n <= 0) {
        client->stream_eof = true;
        return 0;
    }
    client->bytes_read += n;
    return (int)n;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client)
{
    if (!client) return false;
    if (client->content_length >= 0) {
        return client->bytes_read >= client->content_length;
    }
    return client->stream_eof;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) return err;

    if (esp_http_client_fetch_headers(client) < 0 && client->status_code == 0) {
        return ESP_FAIL;
    }

    // Read full response
    char tmp[1024];
    while (true) {
        int r = esp_http_client_read(client, tmp, sizeof(tmp));
        if (r <= 0) break;
    }

    return ESP_OK;
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (client->sock_fd >= 0) close(client->sock_fd);
    client->sock_fd = -1;
    return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client)
{
    if (!client) return ESP_ERR_INVALID_ARG;
    if (client->sock_fd >= 0) {
        close(client->sock_fd);
        client->sock_fd = -1;
    }
    free(client->url);
    free(client->host);
    free(client->path);
    free(client->post_data);
    free_headers(client->req_headers, &client->req_header_count);
    free_headers(client->resp_headers, &client->resp_header_count);
    free(client);
    return ESP_OK;
}

int esp_http_client_get_socket(esp_http_client_handle_t client) { return client ? client->sock_fd : -1; }
