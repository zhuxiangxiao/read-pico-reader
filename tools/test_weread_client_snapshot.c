/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
#include "weread_client.h"
#include "weread_auth.h"
#include "weread_store.h"
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool entered, released;
static atomic_bool cancelled;
static int response_status, result;
static int mode10_http_status;
static bool mode10_weblogin_ok, mode10_replay_ok;
static char *response;
static size_t response_bytes;
static int status;
static unsigned requests;
static atomic_uint activity_calls;
static void on_activity(void *ctx) {
    (void)ctx;
    WR_GUARD;
    atomic_fetch_add(&activity_calls, 1);
}
static bool cancel_query(void *ctx) { (void)ctx; return atomic_load(&cancelled); }
static uint64_t milliseconds(void) {
    struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static esp_err_t transport(const char *url, const char *method, const char *headers,
    const char *body, char **out, size_t *bytes, int *http_status, void *ctx) {
    (void)body; (void)ctx;
    ++requests;
    if (response_status == 2013) {
        if (strstr(url, "/weblogin")) {
            if (mode10_weblogin_ok) {
                *http_status = 200;
                *out = strdup("{\"accessToken\":\"fixture-renewed-token\",\"refreshToken\":\"fixture-renewed-refresh\"}");
            } else {
                *http_status = 401;
                *out = strdup("{\"errcode\":-2013}");
            }
            assert(*out); *bytes = strlen(*out); return ESP_OK;
        }
        assert(strstr(url, "/book/read") && !strcmp(method, "POST"));
        if (strstr(headers, "fixture-renewed-token") && mode10_replay_ok) {
            *http_status = 200;
            *out = strdup("{\"succ\":1}");
        } else {
            *http_status = mode10_http_status;
            *out = strdup("{\"errcode\":-2013}");
        }
        assert(*out); *bytes = strlen(*out); return ESP_OK;
    }
    if (response_status == 2012 || response_status == 4010) {
        if (strstr(url, "/weblogin")) {
            *http_status = 200;
            *out = strdup("{\"accessToken\":\"fixture-renewed-token\",\"refreshToken\":\"fixture-renewed-refresh\"}");
            assert(*out); *bytes = strlen(*out); return ESP_OK;
        }
        assert(strstr(url, "/book/read") && !strcmp(method, "POST"));
        if (strstr(headers, "fixture-old-token")) {
            *http_status = 401;
            *out = strdup(response_status == 4010 ? "{\"errcode\":401}" : "{\"errCode\":-2012,\"errMsg\":\"timeout\"}");
            assert(*out); *bytes = strlen(*out); return ESP_OK;
        }
        assert(strstr(headers, "fixture-renewed-token"));
        *http_status = 200;
        *out = strdup("{\"succ\":1}");
        assert(*out); *bytes = strlen(*out); return ESP_OK;
    }
    assert(strstr(url, "/book/read") && !strcmp(method, "POST"));
    assert(strstr(headers, "fixture-old-token"));
    assert(!pthread_mutex_lock(&gate)); entered = true; assert(!pthread_cond_broadcast(&changed));
    while (!released) assert(!pthread_cond_wait(&changed, &gate));
    assert(!pthread_mutex_unlock(&gate));
    *http_status = response_status;
    *out = strdup(response_status == 401 ? "{\"errcode\":-2013}" : "{\"succ\":1}");
    assert(*out); *bytes = strlen(*out); return ESP_OK;
}
static esp_err_t dl_401_timeout(const char *url, const char *headers, FILE *file, size_t limit,
    weread_cancel_fn cancel, void *ctx, weread_download_info_t *info, void *user) {
    (void)url; (void)limit; (void)cancel; (void)ctx; (void)user;
    ++requests;
    const char *body = strstr(headers, "fixture-renewed-token") ? "CHAPTER_BYTES_OK" : "{\"errCode\":-2012}";
    info->status = strstr(headers, "fixture-renewed-token") ? 200 : 401;
    info->bytes = strlen(body);
    assert(fwrite(body, 1, info->bytes, file) == info->bytes);
    return ESP_OK;
}
static void *dispatch(void *ctx) {
    (void)ctx;
    result = weread_client_request_snapshot("https://i.weread.qq.com/book/read", "POST",
        "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status);
    return NULL;
}
static weread_auth_session_t session(void) {
    weread_auth_session_t s = {0};
    strcpy(s.vid, "123"); strcpy(s.access_token, "fixture-old-token");
    strcpy(s.refresh_token, "fixture-refresh"); strcpy(s.device_id, "fixture-device");
    return s;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    char other[512]; assert(snprintf(other, sizeof(other), "%s/other-card", argv[1]) > 0);
    unsigned mode = (unsigned)strtoul(argv[2], NULL, 10);
    assert(weread_auth_init(argv[1]) == ESP_OK);
    weread_auth_session_t original = session(), current;
    assert(weread_auth_save_session(&original) == ESP_OK && weread_auth_load_session(&current) == ESP_OK);
    weread_client_set_transport(transport, NULL); weread_client_set_cancel(cancel_query, NULL);
    weread_client_set_activity_cb(on_activity, NULL);
    if (mode == 10) {
        response_status = 2013;
        weread_client_identity_t bound;
        // 10a: Background -2013 (200 & 401) must not renew or mark expired.
        weread_client_set_background(true);
        assert(weread_client_identity_get(&bound));
        mode10_http_status = 200; requests = 0;
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        assert(result == ESP_OK && status == 200 && requests == 1 && strstr(response, "-2013") &&
               weread_auth_is_authenticated() && !weread_auth_is_expired() && !strcmp(bound.token, "fixture-old-token"));
        free(response); response = NULL;
        mode10_http_status = 401; requests = 0;
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        assert(result == ESP_OK && status == 401 && requests == 1 && strstr(response, "-2013") &&
               weread_auth_is_authenticated() && !weread_auth_is_expired() && !strcmp(bound.token, "fixture-old-token"));
        free(response); response = NULL;
        weread_client_set_background(false);
        // 10b: Foreground 200 -2013 + /weblogin 401 -2013 marks expired with status == 0.
        mode10_http_status = 200; mode10_weblogin_ok = false; mode10_replay_ok = false; requests = 0;
        assert(weread_client_identity_get(&bound));
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        assert(result == ESP_ERR_INVALID_STATE && status == 0 && !response && !response_bytes &&
               requests == 2 && !weread_auth_is_authenticated() && weread_auth_is_expired() &&
               !strcmp(weread_auth_owner_vid(), "123"));
        // 10c: Foreground 401 -2013 + /weblogin 401 -2013 marks expired with status == 0.
        assert(weread_auth_save_session(&original) == ESP_OK && weread_auth_load_session(&current) == ESP_OK);
        mode10_http_status = 401; mode10_weblogin_ok = false; requests = 0;
        assert(weread_client_identity_get(&bound));
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        assert(result == ESP_ERR_INVALID_STATE && status == 0 && !response && !response_bytes &&
               requests == 2 && !weread_auth_is_authenticated() && weread_auth_is_expired());
        // 10d: Foreground 401 -2013 -> /weblogin 200 renew -> replay 200 ok.
        assert(weread_auth_save_session(&original) == ESP_OK && weread_auth_load_session(&current) == ESP_OK);
        mode10_http_status = 401; mode10_weblogin_ok = true; mode10_replay_ok = true; requests = 0;
        assert(weread_client_identity_get(&bound));
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        assert(result == ESP_OK && status == 200 && response && requests == 3 &&
               !strcmp(bound.token, "fixture-renewed-token") && weread_auth_is_authenticated() && !weread_auth_is_expired());
        free(response); response = NULL;
        // 10e: Foreground 401 -2013 -> /weblogin 200 renew -> replay 401 -2013 marks expired with status == 0.
        assert(weread_auth_save_session(&original) == ESP_OK && weread_auth_load_session(&current) == ESP_OK);
        mode10_http_status = 401; mode10_weblogin_ok = true; mode10_replay_ok = false; requests = 0;
        assert(weread_client_identity_get(&bound));
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        assert(result == ESP_FAIL && status == 0 && !response && !response_bytes &&
               requests == 3 && !weread_auth_is_authenticated() && weread_auth_is_expired());
        puts("mode10: foreground vs background -2013/401 bound-renew and expiry handling passed");
        return 0;
    }
    if (mode == 9) {
        response_status = 2012;
        weread_client_identity_t bound; assert(weread_client_identity_get(&bound));
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        assert(result == ESP_OK && status == 200 && requests == 3 && !strcmp(bound.token, "fixture-renewed-token"));
        free(response); response = NULL;
        weread_auth_session_t reset_tok = session();
        assert(weread_auth_save_session(&reset_tok) == ESP_OK && weread_auth_load_session(&reset_tok) == ESP_OK);
        requests = 0;
        weread_client_set_background(true);
        weread_client_set_download_transport(dl_401_timeout, NULL);
        FILE *tmp = tmpfile(); assert(tmp);
        weread_download_info_t dinfo = {0};
        result = weread_client_download("https://i.weread.qq.com/book/chapterdownload?preload=1",
            tmp, 4096, NULL, NULL, &dinfo);
        weread_client_set_background(false);
        assert(result == ESP_OK && dinfo.status == 200 && requests == 3);
        assert(atomic_load(&activity_calls) >= 6);
        fclose(tmp);
        assert(weread_auth_save_session(&reset_tok) == ESP_OK && weread_auth_load_session(&reset_tok) == ESP_OK);
        requests = 0; response_status = 4010;
        assert(weread_client_identity_get(&bound));
        weread_client_set_background(true);
        result = weread_client_request_bound_renew("https://i.weread.qq.com/book/read", "POST",
            "Content-Type: application/json\r\n", "{}", &response, &response_bytes, &status, &bound);
        weread_client_set_background(false);
        assert(result == ESP_OK && status == 200 && requests == 3 && !strcmp(bound.token, "fixture-renewed-token"));
        free(response); response = NULL;
        assert(weread_auth_mark_expired() == ESP_OK);
        assert(!weread_client_identity_get(&bound));
        atomic_store(&activity_calls, 0);
        assert(weread_client_request_snapshot("https://i.weread.qq.com/book/read", "POST",
            NULL, "{}", &response, &response_bytes, &status) == ESP_ERR_INVALID_STATE);
        assert(atomic_load(&activity_calls) == 0);
        puts("mode9: unified background -2012 HTTP 401 renewal passed for bound and download paths");
        return 0;
    }
    if(mode>=6){
        weread_client_identity_t expected;assert(weread_client_identity_get(&expected));
        if(mode==8)assert(weread_auth_init(other)==ESP_OK);
        else {
            strcpy(current.access_token,"fixture-new-token");if(mode==7)strcpy(current.vid,"456");
            assert(weread_auth_save_session(&current)==ESP_OK&&weread_auth_load_session(&current)==ESP_OK);
        }
        result=weread_client_request_bound("https://i.weread.qq.com/book/read","POST",NULL,"{}",&response,&response_bytes,&status,&expected);
        assert(result==ESP_ERR_INVALID_STATE&&!requests&&!response&&!response_bytes&&!status&&!atomic_load(&activity_calls));
        printf("bound mode%u: stale token/account/media identity rejected before any HTTP\n",mode);return 0;
    }
    response_status = mode == 1 ? 401 : 200;
    pthread_t thread; assert(!pthread_create(&thread, NULL, dispatch, NULL));
    assert(!pthread_mutex_lock(&gate));
    while (!entered) assert(!pthread_cond_wait(&changed, &gate));
    assert(!pthread_mutex_unlock(&gate));

    // HTTP is deliberately blocked until this real service-lock transaction completes.
    uint64_t started = milliseconds();
    {
        WR_GUARD;
        char path[512]; assert(snprintf(path, sizeof(path), "%s/local-save.json", argv[1]) > 0);
        cJSON *saved = cJSON_CreateObject(); assert(saved && cJSON_AddNumberToObject(saved, "offset", 123));
        assert(wr_write_json(path, saved) == ESP_OK); cJSON_Delete(saved);
        if (mode == 2 || mode == 3) {
            strcpy(current.access_token, "fixture-new-token");
            if (mode == 3) strcpy(current.vid, "456");
            assert(weread_auth_save_session(&current) == ESP_OK && weread_auth_load_session(&current) == ESP_OK);
        } else if (mode == 4) assert(weread_auth_init(other) == ESP_OK);
        else if (mode == 5) atomic_store(&cancelled, true);
    }
    uint64_t local_ms = milliseconds() - started;
    assert(local_ms < 100);
    assert(!pthread_mutex_lock(&gate)); released = true; assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate)); assert(!pthread_join(thread, NULL));
    assert(requests == 1);
    assert(atomic_load(&activity_calls) >= 1);
    if (mode >= 2) assert(result == ESP_ERR_INVALID_STATE && !response && !response_bytes && !status);
    else assert(result == ESP_OK && response && response_bytes && status == response_status);
    if (mode == 2 || mode == 3) {
        const weread_auth_session_t *active = weread_auth_get_current_session();
        assert(active && !strcmp(active->access_token, "fixture-new-token"));
        assert(!strcmp(active->vid, mode == 3 ? "456" : "123"));
    } else if (mode != 4) assert(weread_auth_is_authenticated());
    free(response);
    printf("snapshot mode%u: local durable transaction %llums while HTTP waits; requests=%u result=%d\n",
        mode, (unsigned long long)local_ms, requests, result);
    return 0;
}
