/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：有界读取与原子缓存提交；失败保留旧文件。
 * English: Bounded reads and atomic cache commits, retaining old files on failure.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Os")
#endif
#include "weread_store.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "esp_log.h"
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
#include "read_pico_sd_access.h"
#endif
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static _Thread_local unsigned s_memory_depth;
static _Thread_local bool s_memory_failed;
#if defined(ESP_PLATFORM) || defined(WEREAD_TEST_MEMORY)
#include "esp_heap_caps.h"
static void *json_allocate(size_t bytes) {
    if(!s_memory_depth)return malloc(bytes);
    void *p=wr_alloc(bytes);if(!p)s_memory_failed=true;return p;
}
#endif
void wr_memory_init(void) {
#if defined(ESP_PLATFORM) || defined(WEREAD_TEST_MEMORY)
    // 只在开机、其他JSON调用之前安装，运行中不切换全局hooks。/ Install before other JSON calls at boot; never swap global hooks during requests.
    static bool initialized;if(initialized)return;
    cJSON_Hooks hooks={.malloc_fn=json_allocate,.free_fn=free};cJSON_InitHooks(&hooks);initialized=true;
#endif
}
bool wr_memory_failed(void){return s_memory_failed;}
cJSON *wr_parse_json(const char *text,size_t bytes){
    if(!text||!bytes||bytes>WEREAD_JSON_LIMIT+1u)return NULL;
    unsigned depth=0;bool string=false,escaped=false;
    for(size_t i=0;i<bytes;++i){unsigned char c=(unsigned char)text[i];
        if(string){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')string=false;}
        else if(c=='"')string=true;
        else if(c=='{'||c=='['){if(++depth>32)return NULL;}
        else if(c=='}'||c==']'){if(!depth)return NULL;--depth;}
    }
    return cJSON_ParseWithLengthOpts(text,bytes,NULL,true);
}
void wr_memory_enter(void) {
    if (!s_memory_depth++) s_memory_failed = false;
}
void wr_memory_leave(int *guard) {
    (void)guard;
    --s_memory_depth;
}
#if defined(ESP_PLATFORM)
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
static StaticSemaphore_t s_service_mutex_buffer;
static SemaphoreHandle_t s_service_mutex;
static portMUX_TYPE s_service_init = portMUX_INITIALIZER_UNLOCKED;
void wr_lock(void) {
    portENTER_CRITICAL(&s_service_init);
    if (!s_service_mutex) s_service_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_service_mutex_buffer);
    portEXIT_CRITICAL(&s_service_init);
    xSemaphoreTakeRecursive(s_service_mutex, portMAX_DELAY);
    wr_memory_enter();
}
void wr_unlock(int *guard) { (void)guard; wr_memory_leave(NULL); xSemaphoreGiveRecursive(s_service_mutex); }
#else
#include <pthread.h>
static pthread_mutex_t s_service_mutex;
static pthread_once_t s_service_once = PTHREAD_ONCE_INIT;
static void init_service_mutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr); pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_service_mutex, &attr); pthread_mutexattr_destroy(&attr);
}
void wr_lock(void) { pthread_once(&s_service_once, init_service_mutex); pthread_mutex_lock(&s_service_mutex); wr_memory_enter(); }
void wr_unlock(int *guard) { (void)guard; wr_memory_leave(NULL); pthread_mutex_unlock(&s_service_mutex); }
#endif

void *wr_alloc(size_t bytes) {
#if defined(ESP_PLATFORM) || defined(WEREAD_TEST_MEMORY)
    size_t available=heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(available<512u*1024u||bytes>available-512u*1024u)return NULL;
    return heap_caps_calloc(1,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return calloc(1,bytes);
#endif
}
void *wr_realloc(void *pointer,size_t bytes) {
#if defined(ESP_PLATFORM) || defined(WEREAD_TEST_MEMORY)
    return heap_caps_realloc(pointer,bytes,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
#else
    return realloc(pointer,bytes);
#endif
}
void wr_release(void *slot) {
    void *p=NULL;memcpy(&p,slot,sizeof(p));free(p);p=NULL;memcpy(slot,&p,sizeof(p));
}

bool wr_valid_id(const char *id) {
    if (!id || !*id || strlen(id) >= WEREAD_BOOK_ID_CAP) return false;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return true;
}

const char *wr_string(const cJSON *o, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

bool wr_uint(const cJSON *o, const char *key, uint64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsNumber(v) || !isfinite(v->valuedouble) || v->valuedouble < 0 ||
        v->valuedouble > 9007199254740991.0 || floor(v->valuedouble) != v->valuedouble) return false;
    if (out) *out = (uint64_t)v->valuedouble;
    return true;
}

bool wr_copy(char *out, size_t cap, const char *text) {
    if (!out || !cap || !text || strlen(text) >= cap) return false;
    memcpy(out, text, strlen(text) + 1);
    return true;
}

void wr_display_copy(char *out, size_t cap, const char *text) {
    if (!out || !cap) return;
    if (!text) { out[0] = 0; return; }
    size_t n = strlen(text);
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
    }
    memcpy(out, text, n); out[n] = 0;
}

esp_err_t wr_mkdirs(const char *path) {
    if (!path || !*path) return ESP_ERR_INVALID_ARG;
    size_t len = strlen(path);
    if (len >= 768) return ESP_ERR_INVALID_SIZE;
    char *buf __attribute__((cleanup(wr_release))) = wr_alloc(len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; ; ++p) {
        if (*p != '/' && *p) continue;
        char c = *p; *p = 0;
        struct stat st;
        // FatFS的挂载根目录无法mkdir；先接受已经存在的目录。/ FatFS cannot mkdir its mount root; accept existing directories first.
        if (stat(buf, &st)) {
            if (errno != ENOENT || (mkdir(buf, 0700) && errno != EEXIST) || stat(buf, &st)) {
                ESP_LOGW("weread", "cache directory failed errno=%d",errno);return ESP_FAIL;
            }
        }
        if (!S_ISDIR(st.st_mode)) return ESP_FAIL;
        *p = c;
        if (!c) return ESP_OK;
    }
}

esp_err_t wr_cache_path(const char *id, const char *leaf, char *out, size_t cap) {
    if (!out || !cap || !wr_valid_id(id) || !leaf || !*leaf || strstr(leaf, "..") || *leaf == '/' ||
        strchr(leaf, '\\')) return ESP_ERR_INVALID_ARG;
    int n = snprintf(out, cap, "%s/cache/%s/%s", weread_auth_get_storage_root(), id, leaf);
    return n >= 0 && (size_t)n < cap ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t read_json(const char *path, cJSON **out) {
    *out = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    struct stat st;
    if(fstat(fileno(f),&st)){fclose(f);return ESP_FAIL;}
    if (st.st_size <= 0 || st.st_size > WEREAD_JSON_LIMIT) {
        fclose(f); return ESP_ERR_INVALID_SIZE;
    }
    size_t len = (size_t)st.st_size;
    char *buf = wr_alloc(len + 1);
    if (!buf) { fclose(f); return ESP_ERR_NO_MEM; }
    bool ok = fread(buf, 1, len, f) == len;
    if (fclose(f)) ok = false;
    buf[len] = 0;
    if (ok) *out = wr_parse_json(buf, len + 1);
    free(buf);
    if(!ok)return ESP_FAIL;
    return *out ? ESP_OK : wr_memory_failed()?ESP_ERR_NO_MEM:ESP_ERR_INVALID_RESPONSE;
}

esp_err_t wr_read_json_primary(const char *path,cJSON **out){return read_json(path,out);}

esp_err_t wr_read_json(const char *path, cJSON **out) {
    esp_err_t err = read_json(path, out);
    if (err == ESP_OK || err == ESP_ERR_NO_MEM) return err;
    // FAT 不支持覆盖 rename；掉电窗口可从上一份完整缓存恢复。/ FAT cannot replace by rename; recover the last complete cache after an interrupted commit.
    size_t len = path ? strlen(path) : 0;
    if (!path || len + sizeof(".previous") > 800) return err;
    char *previous __attribute__((cleanup(wr_release))) = wr_alloc(len + sizeof(".previous"));
    if (!previous) return ESP_ERR_NO_MEM;
    memcpy(previous, path, len);
    memcpy(previous + len, ".previous", sizeof(".previous"));
    esp_err_t recovery = read_json(previous, out);
    return recovery == ESP_OK ? ESP_OK : err;
}

esp_err_t wr_write_bytes(const char *path, const void *data, size_t len) {
    if (!path) return ESP_ERR_INVALID_ARG;
    size_t path_len = strlen(path);
    if (path_len >= 768 || path_len + sizeof(".previous") > 800) return ESP_ERR_INVALID_SIZE;
#if defined(ESP_PLATFORM) && defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    read_pico_sd_access_scope_t sd_access __attribute__((cleanup(read_pico_sd_access_scope_end))) =
        read_pico_sd_access_scope(path);
    if (!sd_access.allowed) return ESP_ERR_INVALID_STATE;
#endif
    size_t cap = path_len + sizeof(".previous");
    char *buf __attribute__((cleanup(wr_release))) = wr_alloc(cap * 2);
    if (!buf) return ESP_ERR_NO_MEM;
    char *tmp = buf, *scratch = buf + cap;
    memcpy(scratch, path, path_len + 1);
    char *slash = strrchr(scratch, '/');
    if (!slash) return ESP_ERR_INVALID_ARG;
    *slash = 0;
    esp_err_t err = wr_mkdirs(scratch);
    if (err != ESP_OK) return err;
    memcpy(tmp, path, path_len);
    memcpy(tmp + path_len, ".tmp", sizeof(".tmp"));
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(tmp, flags, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "wb") : NULL;
    if (!f && fd >= 0) close(fd);
    if (!f) {ESP_LOGW("weread","cache open failed errno=%d",errno);return ESP_FAIL;}
    bool ok = fwrite(data, 1, len, f) == len && !fflush(f);
    if (ok && fsync(fileno(f))) ok = false;
    if (fclose(f)) ok = false;
    if (ok && !rename(tmp, path)) return ESP_OK;
    if (ok && errno == EEXIST) {
        memcpy(scratch, path, path_len);
        memcpy(scratch + path_len, ".previous", sizeof(".previous"));
        if ((!unlink(scratch) || errno == ENOENT) && !rename(path, scratch)) {
            if (!rename(tmp, path)) return ESP_OK;
            (void)rename(scratch, path);
        }
    }
    ESP_LOGW("weread","cache commit failed errno=%d",errno);
    unlink(tmp);
    return ESP_FAIL;
}

esp_err_t wr_write_json(const char *path, const cJSON *json) {
    char *text = cJSON_PrintUnformatted(json);
    if (!text) return ESP_ERR_NO_MEM;
    size_t len = strlen(text);
    esp_err_t err = len <= WEREAD_JSON_LIMIT ? wr_write_bytes(path, text, len) : ESP_ERR_INVALID_SIZE;
    cJSON_free(text);
    return err;
}

esp_err_t wr_request_json(const char *endpoint, const cJSON *body, cJSON **out) { WR_MEMORY_SCOPE;
    *out = NULL;
    char url[768];
    int n = snprintf(url, sizeof(url), "%s%s", weread_client_get_base_url(), endpoint);
    if (n < 0 || (size_t)n >= sizeof(url)) return ESP_ERR_INVALID_SIZE;
    char *payload = body ? cJSON_PrintUnformatted(body) : NULL;
    if (body && !payload) return ESP_ERR_NO_MEM;
    char *response = NULL;
    size_t len = 0;
    int status = 0;
    esp_err_t err = weread_client_request(url, body ? "POST" : "GET",
        body ? "Content-Type: application/json\r\n" : NULL, payload, &response, &len, &status);
    cJSON_free(payload);
    if (err == ESP_OK && (status < 200 || status >= 300)) err = ESP_ERR_INVALID_RESPONSE;
    if (err == ESP_OK && (!response || !len || len > WEREAD_JSON_LIMIT)) err = ESP_ERR_INVALID_SIZE;
    if (err == ESP_OK) {
        *out = wr_parse_json(response, len + 1);
        if (!cJSON_IsObject(*out)) err = wr_memory_failed()?ESP_ERR_NO_MEM:ESP_ERR_INVALID_RESPONSE;
        const cJSON *code = cJSON_GetObjectItem(*out, "errcode");
        if (!code) code = cJSON_GetObjectItem(*out, "errCode");
        if (!code) code = cJSON_GetObjectItem(*out, "ret");
        if (cJSON_IsNumber(code) && code->valuedouble != 0) err = ESP_ERR_INVALID_RESPONSE;
    }
    free(response);
    if (err != ESP_OK) { cJSON_Delete(*out); *out = NULL; }
    return err;
}

bool wr_owner_matches(const cJSON *json) {
    const weread_auth_session_t *s = weread_auth_get_current_session();
    const char *vid = wr_string(json, "vid");
    return s && vid && !strcmp(s->vid, vid);
}

bool wr_set_owner(cJSON *json) {
    const weread_auth_session_t *s = weread_auth_get_current_session();
    if (!s) return false;
    cJSON_DeleteItemFromObject(json, "vid");
    return cJSON_AddStringToObject(json, "vid", s->vid) != NULL;
}

bool wr_set_copy(cJSON *o, const char *key, const cJSON *value) {
    cJSON *copy = cJSON_Duplicate(value, true);
    if (!copy) return false;
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    if (cJSON_AddItemToObject(o, key, copy)) return true;
    cJSON_Delete(copy);
    return false;
}
