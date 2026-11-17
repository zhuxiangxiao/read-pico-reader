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
static char *response;
static size_t response_bytes;
static int status;
static unsigned requests;
static bool cancel_query(void *ctx) { (void)ctx; return atomic_load(&cancelled); }
static uint64_t milliseconds(void) {
    struct timespec now; assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
static esp_err_t transport(const char *url, const char *method, const char *headers,
    const char *body, char **out, size_t *bytes, int *http_status, void *ctx) {
    (void)body; (void)ctx;
    assert(strstr(url, "/book/read") && !strcmp(method, "POST"));
    assert(strstr(headers, "fixture-old-token"));
    ++requests;
    assert(!pthread_mutex_lock(&gate)); entered = true; assert(!pthread_cond_broadcast(&changed));
    while (!released) assert(!pthread_cond_wait(&changed, &gate));
    assert(!pthread_mutex_unlock(&gate));
    *http_status = response_status;
    *out = strdup(response_status == 401 ? "{\"errcode\":-2013}" : "{\"succ\":1}");
    assert(*out); *bytes = strlen(*out); return ESP_OK;
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
    if(mode>=6){
        weread_client_identity_t expected;assert(weread_client_identity_get(&expected));
        if(mode==8)assert(weread_auth_init(other)==ESP_OK);
        else {
            strcpy(current.access_token,"fixture-new-token");if(mode==7)strcpy(current.vid,"456");
            assert(weread_auth_save_session(&current)==ESP_OK&&weread_auth_load_session(&current)==ESP_OK);
        }
        result=weread_client_request_bound("https://i.weread.qq.com/book/read","POST",NULL,"{}",&response,&response_bytes,&status,&expected);
        assert(result==ESP_ERR_INVALID_STATE&&!requests&&!response&&!response_bytes&&!status);
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
