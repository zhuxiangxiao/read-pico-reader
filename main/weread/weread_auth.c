/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead authentication, QR login state machine, and persistence.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Os")
#endif
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"
#include "cJSON.h"
#include "esp_log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#if defined(ESP_PLATFORM)
#include "esp_random.h"
#endif

static char s_storage_root[256] = WEREAD_STORAGE_ROOT_DEFAULT;
static WR_EXT_BSS char s_device_id[WEREAD_DEVICE_ID_MAX_LEN];
static char s_device_name[WEREAD_DEVICE_NAME_MAX_LEN] = "KiikoReader";

static WR_EXT_BSS weread_auth_session_t s_current_session;
static bool s_authenticated = false;

typedef struct {
    char vid[WEREAD_VID_MAX_LEN];
    char skey[WEREAD_SKEY_MAX_LEN];
} pending_auth_t;

static WR_EXT_BSS pending_auth_t s_pending_auth;
static bool credential(const char *text, size_t cap, bool required);

static bool login_url(const cJSON *body, char *url, size_t cap) {
    int n = snprintf(url, cap, "%s/weblogin?", weread_client_get_base_url());
    if (n < 0 || (size_t)n >= cap) return false;
    size_t used = (size_t)n;
    const cJSON *field;
    cJSON_ArrayForEach(field, body) {
        char encoded[512], number[32];
        const char *value = cJSON_IsString(field) ? field->valuestring : number;
        if (cJSON_IsNumber(field)) snprintf(number, sizeof(number), "%.0f", field->valuedouble);
        else if (!cJSON_IsString(field)) return false;
        if (!weread_client_url_encode(value, encoded, sizeof(encoded))) return false;
        n = snprintf(url + used, cap - used, "%s%s=%s", field == body->child ? "" : "&", field->string, encoded);
        if (n < 0 || (size_t)n >= cap - used) return false;
        used += (size_t)n;
    }
    return weread_client_is_trusted_url(url);
}

void weread_secure_zero(void *ptr, size_t len) {
    if (!ptr || len == 0) {
        return;
    }
    volatile unsigned char *p = (volatile unsigned char *)ptr;
    while (len--) {
        *p++ = 0;
    }
}

static void ensure_dir_recursive(const char *dir) {
    if (!dir || dir[0] == '\0') {
        return;
    }
    char tmp[256];
    size_t len = strlen(dir);
    if (len >= sizeof(tmp)) {
        return;
    }
    memcpy(tmp, dir, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                // Ignore failure if intermediate directory exists
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        // Ignore failure if directory exists
    }
}

static void generate_device_id(char *out_id, size_t cap) {
    unsigned char b[16];
#if defined(ESP_PLATFORM)
    esp_fill_random(b, sizeof(b));
#else
    for (int i = 0; i < 16; i++) b[i] = (unsigned char)(rand() & 0xFF);
#endif
    b[6] = (b[6] & 0x0F) | 0x40; // Version 4
    b[8] = (b[8] & 0x3F) | 0x80; // Variant 1
    snprintf(out_id, cap, "eink%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
             b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

static void load_or_generate_device_id(void) {
    char dev_path[512];
    snprintf(dev_path, sizeof(dev_path), "%s/device_id", s_storage_root);

    FILE *f = fopen(dev_path, "rb");
    if (f) {
        char buf[128];
        if (fgets(buf, sizeof(buf), f)) {
            char *nl = strpbrk(buf, "\r\n");
            if (nl) *nl = '\0';
            if (credential(buf, sizeof(s_device_id), true)) {
                memcpy(s_device_id, buf, strlen(buf) + 1);
            }
        }
        fclose(f);
    }

    if (s_device_id[0] == '\0') {
        generate_device_id(s_device_id, sizeof(s_device_id));
        int fd = open(dev_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            write(fd, s_device_id, strlen(s_device_id));
#if !defined(ESP_PLATFORM)
            fchmod(fd, 0600);
#endif
            close(fd);
        }
    }
}

esp_err_t weread_auth_init(const char *storage_root) { WR_GUARD;
    if (storage_root && storage_root[0] != '\0') {
        snprintf(s_storage_root, sizeof(s_storage_root), "%s", storage_root);
    } else {
        const char *env_root = getenv("WEREAD_STORAGE_ROOT");
        if (env_root && env_root[0] != '\0') {
            snprintf(s_storage_root, sizeof(s_storage_root), "%s", env_root);
        } else {
            snprintf(s_storage_root, sizeof(s_storage_root), "%s", WEREAD_STORAGE_ROOT_DEFAULT);
        }
    }

    s_device_id[0] = 0;
    ensure_dir_recursive(s_storage_root);
    load_or_generate_device_id();

    // Reset memory state
    weread_secure_zero(&s_current_session, sizeof(s_current_session));
    weread_secure_zero(&s_pending_auth, sizeof(s_pending_auth));
    s_authenticated = false;

    // Attempt to load existing credentials
    weread_auth_session_t loaded;
    memset(&loaded, 0, sizeof(loaded));
    if (weread_auth_load_session(&loaded) == ESP_OK) {
        memcpy(&s_current_session, &loaded, sizeof(s_current_session));
        s_authenticated = true;
    }

    return ESP_OK;
}

const char *weread_auth_get_storage_root(void) {
    return s_storage_root;
}

const char *weread_auth_get_device_id(void) {
    if (s_device_id[0] == '\0') {
        load_or_generate_device_id();
    }
    return s_device_id;
}

const char *weread_auth_get_device_name(void) {
    return s_device_name;
}

esp_err_t weread_auth_start_qr_login(char *out_uid, size_t uid_cap, char *out_qr_url, size_t qr_url_cap) { WR_GUARD;
    if (!out_uid || uid_cap == 0 || !out_qr_url || qr_url_cap == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    weread_secure_zero(&s_pending_auth, sizeof(s_pending_auth));

    char url[512];
    snprintf(url, sizeof(url), "%s/web/getuid?platform=desktop", weread_client_get_base_url());

    const char *headers = "CLIENT_TYPE: 1\r\nX-Is-Serverside: 1\r\n";
    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;

    esp_err_t err = weread_client_request(url, "GET", headers, NULL, &resp, &resp_len, &status);
    if (err != ESP_OK || !resp || status != 200) {
        if (resp) free(resp);
        return ESP_FAIL;
    }

    cJSON *root = wr_parse_json(resp,resp_len + 1);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }

    cJSON *uid_item = NULL;
    cJSON *data_obj = cJSON_GetObjectItem(root, "data");
    if (data_obj && cJSON_IsObject(data_obj)) {
        uid_item = cJSON_GetObjectItem(data_obj, "uid");
    }
    if (!uid_item) {
        uid_item = cJSON_GetObjectItem(root, "uid");
    }

    if (!uid_item || !cJSON_IsString(uid_item) || !credential(uid_item->valuestring, WEREAD_UID_MAX_LEN, true)) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    char encoded[WEREAD_UID_MAX_LEN * 3];
    if (!wr_copy(out_uid, uid_cap, uid_item->valuestring) ||
        !weread_client_url_encode(uid_item->valuestring, encoded, sizeof(encoded))) {
        cJSON_Delete(root); return ESP_ERR_INVALID_SIZE;
    }
    int n = snprintf(out_qr_url, qr_url_cap, "%s?pf=2&uid=%s", WEREAD_WEB_CONFIRM_BASE, encoded);
    if (n < 0 || (size_t)n >= qr_url_cap) { cJSON_Delete(root); return ESP_ERR_INVALID_SIZE; }

    cJSON_Delete(root);
    return ESP_OK;
}

static cJSON *auth_alias(cJSON *root,const char *const *keys) {
    cJSON *data=cJSON_GetObjectItemCaseSensitive(root,"data");
    for (unsigned i=0;keys[i];++i) {
        cJSON *v=cJSON_GetObjectItemCaseSensitive(data,keys[i]);
        if(!v)v=cJSON_GetObjectItemCaseSensitive(root,keys[i]);
        if(v)return v;
    }
    return NULL;
}
static const char *const vid_keys[]={"webLoginVid","vid","userVid","user_vid","openid","wr_vid",NULL};
static const char *const token_keys[]={"accessToken","skey","wr_skey",NULL};
static const char *const refresh_keys[]={"refreshToken","wr_rt",NULL};
static void auth_cookie_fields(cJSON *root) {
    const char *cookies[]={"wr_vid","wr_skey","wr_rt"};
    const char *const *aliases[]={vid_keys,token_keys,refresh_keys};
    const char *names[]={"vid","accessToken","refreshToken"};
    for(unsigned i=0;i<3;++i) {
        char value[WEREAD_TOKEN_MAX_LEN];
        if(!auth_alias(root,aliases[i]) && weread_client_login_cookie(cookies[i],value,sizeof(value)))
            cJSON_AddStringToObject(root,names[i],value);
        weread_secure_zero(value,sizeof(value));
    }
    weread_client_forget_login_cookies();
}
esp_err_t weread_auth_poll_qr_login(const char *uid, weread_qr_state_t *out_state, char *out_code, size_t code_cap) { WR_GUARD;
    if (!uid || !out_state || !credential(uid, WEREAD_UID_MAX_LEN, true)) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_state = WEREAD_QR_STATE_IDLE;
    if (out_code && code_cap > 0) {
        out_code[0] = '\0';
    }

    char url[512];
    snprintf(url, sizeof(url), "%s/web/getlogininfo?platform=desktop", weread_client_get_base_url());

    const char *headers = "CLIENT_TYPE: 1\r\nX-Is-Serverside: 1\r\nContent-Type: application/json\r\n";
    cJSON *request = cJSON_CreateObject();
    if (!request || !cJSON_AddStringToObject(request, "uid", uid)) { cJSON_Delete(request); return ESP_ERR_NO_MEM; }
    char *body = cJSON_PrintUnformatted(request); cJSON_Delete(request);
    if (!body) return ESP_ERR_NO_MEM;

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;

    esp_err_t err = weread_client_request(url, "POST", headers, body, &resp, &resp_len, &status);
    cJSON_free(body);
    if (err != ESP_OK) {
        *out_state = WEREAD_QR_STATE_ERROR;
        if (resp) free(resp);
        return err;
    }
    if (!resp || status != 200) {
        *out_state = WEREAD_QR_STATE_ERROR;
        if (resp) free(resp);
        return ESP_FAIL;
    }

    cJSON *root = wr_parse_json(resp,resp_len + 1);
    free(resp);
    if (!root) {
        *out_state = WEREAD_QR_STATE_ERROR;
        return ESP_FAIL;
    }

    cJSON *data_obj = cJSON_GetObjectItem(root, "data");

    // Look for logicCode
    cJSON *logic_item = data_obj ? cJSON_GetObjectItem(data_obj, "logicCode") : NULL;
    if (!logic_item) {
        logic_item = cJSON_GetObjectItem(root, "logicCode");
    }
    const char *logic_code = (logic_item && cJSON_IsString(logic_item)) ? logic_item->valuestring : "";

    // Look for code
    cJSON *code_item = data_obj ? cJSON_GetObjectItem(data_obj, "code") : NULL;
    if (!code_item) {
        code_item = cJSON_GetObjectItem(root, "code");
    }

    // 接受顶层及data中的协议别名。/ Accept protocol aliases at root and under data.
    cJSON *vid_item=auth_alias(root,vid_keys);
    cJSON *skey_item=auth_alias(root,token_keys);

    // Look for succeed flag
    cJSON *succeed_item = data_obj ? cJSON_GetObjectItem(data_obj, "succeed") : NULL;
    if (!succeed_item) {
        succeed_item = cJSON_GetObjectItem(root, "succeed");
    }

    bool is_confirmed = false;
    if (strcmp(logic_code, "LOGIN_SUCCESS") == 0) {
        is_confirmed = true;
    } else if (succeed_item && cJSON_IsTrue(succeed_item)) {
        is_confirmed = true;
    } else if (code_item && cJSON_IsString(code_item) && strlen(code_item->valuestring) > 0) {
        is_confirmed = true;
    }

    if (is_confirmed) {
        *out_state = WEREAD_QR_STATE_CONFIRMED;
        if (code_item && cJSON_IsString(code_item) && out_code && code_cap > 0) {
            snprintf(out_code, code_cap, "%s", code_item->valuestring);
        }
        if (vid_item) {
            if (cJSON_IsNumber(vid_item)) {
                snprintf(s_pending_auth.vid, sizeof(s_pending_auth.vid), "%.0f", vid_item->valuedouble);
            } else if (cJSON_IsString(vid_item)) {
                snprintf(s_pending_auth.vid, sizeof(s_pending_auth.vid), "%s", vid_item->valuestring);
            }
        }
        if (skey_item && cJSON_IsString(skey_item)) {
            snprintf(s_pending_auth.skey, sizeof(s_pending_auth.skey), "%s", skey_item->valuestring);
        }
    } else if (strcmp(logic_code, "NEED_OTP") == 0) {
        *out_state = WEREAD_QR_STATE_NEED_OTP;
    } else if (strcmp(logic_code, "WAITING_SCAN") == 0) {
        *out_state = WEREAD_QR_STATE_WAITING_SCAN;
    } else if (strcmp(logic_code, "WAITING_CONFIRM") == 0) {
        *out_state = WEREAD_QR_STATE_WAITING_CONFIRM;
    } else if (strcmp(logic_code, "LOGIN_TIMEOUT") == 0 ||
               strcmp(logic_code, "EXPIRED") == 0 ||
               strcmp(logic_code, "OTP_EXPIRED") == 0) {
        *out_state = WEREAD_QR_STATE_TIMEOUT;
    } else if (strcmp(logic_code, "LOGIN_CANCELLED") == 0 ||
               strcmp(logic_code, "LOGIN_CANCELED") == 0) {
        *out_state = WEREAD_QR_STATE_CANCELLED;
    } else if (strcmp(logic_code, "LOGIN_FAILED") == 0 ||
               strcmp(logic_code, "LOGIN_FAIL") == 0 ||
               strcmp(logic_code, "OTP_NOT_MATCH") == 0) {
        *out_state = WEREAD_QR_STATE_ERROR;
    } else if (succeed_item && cJSON_IsFalse(succeed_item)) {
        *out_state = WEREAD_QR_STATE_WAITING_CONFIRM;
    } else {
        // 长轮询尚未扫码时实测返回scan:0，而非logicCode。/ Unscanned real long polls return scan:0 instead of logicCode.
        cJSON *scan=auth_alias(root,(const char *const[]){"scan",NULL});
        if(cJSON_IsNumber(scan)&&scan->valuedouble==0) *out_state=WEREAD_QR_STATE_WAITING_SCAN;
        else if(cJSON_IsNumber(scan)&&scan->valuedouble==1) *out_state=WEREAD_QR_STATE_WAITING_CONFIRM;
        else *out_state=WEREAD_QR_STATE_ERROR;
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t weread_auth_exchange_code(const char *code, weread_auth_session_t *out_session) { WR_GUARD;
    if (!code || code[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *dev_id = weread_auth_get_device_id();
    const char *dev_name = weread_auth_get_device_name();
    const char *vid = s_pending_auth.vid;
    const char *skey = s_pending_auth.skey;

    char url[2048];

    char headers[512];
    snprintf(headers, sizeof(headers),
             "CLIENT_TYPE: 1\r\nX-Is-Serverside: 1\r\nfingerprint: %s\r\nContent-Type: application/json\r\n",
             dev_id);

    cJSON *body_json = cJSON_CreateObject();
    cJSON_AddStringToObject(body_json, "platform", "desktop");
    cJSON_AddStringToObject(body_json, "vid", vid);
    cJSON_AddStringToObject(body_json, "skey", skey);
    cJSON_AddStringToObject(body_json, "code", code);
    cJSON_AddNumberToObject(body_json, "isAutoLogout", 0);
    cJSON_AddStringToObject(body_json, "deviceId", dev_id);
    cJSON_AddNumberToObject(body_json, "deviceType", WEREAD_DEVICE_TYPE_INITIAL);
    cJSON_AddStringToObject(body_json, "deviceName", dev_name);

    if (!body_json || !login_url(body_json, url, sizeof(url))) { cJSON_Delete(body_json); return ESP_ERR_INVALID_SIZE; }
    char *body_str = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);
    if (!body_str) {
        return ESP_ERR_NO_MEM;
    }

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;

    esp_err_t err = weread_client_request(url, "POST", headers, body_str, &resp, &resp_len, &status);
    free(body_str);

    if (err != ESP_OK || !resp || status != 200) {
        if (resp) free(resp);
        return ESP_FAIL;
    }

    cJSON *root = wr_parse_json(resp,resp_len + 1);
    free(resp);
    if (!root) {
        return ESP_FAIL;
    }

    auth_cookie_fields(root);
    cJSON *vid_item = auth_alias(root,vid_keys);
    cJSON *token_item = auth_alias(root,token_keys);
    cJSON *refresh_item = auth_alias(root,refresh_keys);
    cJSON *name_item = auth_alias(root,(const char *const[]){"name","wxNickName","nickName",NULL});

    if (!token_item || !cJSON_IsString(token_item) ||
        !credential(token_item->valuestring, WEREAD_TOKEN_MAX_LEN, true) ||
        (refresh_item && (!cJSON_IsString(refresh_item) || !credential(refresh_item->valuestring, WEREAD_TOKEN_MAX_LEN, true)))) {
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    weread_auth_session_t candidate = {0};

    if (vid_item) {
        if (cJSON_IsString(vid_item)) {
            snprintf(candidate.vid, sizeof(candidate.vid), "%s", vid_item->valuestring);
        } else if (cJSON_IsNumber(vid_item)) {
            snprintf(candidate.vid, sizeof(candidate.vid), "%.0f", vid_item->valuedouble);
        }
    } else if (vid[0] != '\0') {
        snprintf(candidate.vid, sizeof(candidate.vid), "%s", vid);
    }

    snprintf(candidate.access_token, sizeof(candidate.access_token), "%s", token_item->valuestring);
    snprintf(candidate.skey, sizeof(candidate.skey), "%s", token_item->valuestring);

    if (refresh_item && cJSON_IsString(refresh_item)) {
        snprintf(candidate.refresh_token, sizeof(candidate.refresh_token), "%s", refresh_item->valuestring);
    }

    if (name_item && cJSON_IsString(name_item)) {
        snprintf(candidate.user_name, sizeof(candidate.user_name), "%s", name_item->valuestring);
    }

    snprintf(candidate.device_id, sizeof(candidate.device_id), "%s", dev_id);
    snprintf(candidate.device_name, sizeof(candidate.device_name), "%s", dev_name);
    candidate.updated_at = (int64_t)time(NULL);

    esp_err_t saved = weread_auth_save_session(&candidate);
    if (saved != ESP_OK) {
        weread_secure_zero(&candidate, sizeof(candidate)); cJSON_Delete(root); return saved;
    }
    memcpy(&s_current_session, &candidate, sizeof(candidate));
    s_authenticated = true;

    if (out_session) {
        memcpy(out_session, &candidate, sizeof(weread_auth_session_t));
    }

    weread_secure_zero(&candidate, sizeof(candidate));
    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t weread_auth_renew_session(weread_auth_session_t *session) { WR_MEMORY_SCOPE;
    weread_auth_session_t snap;
    char dev_id[WEREAD_DEVICE_ID_MAX_LEN], dev_name[WEREAD_DEVICE_NAME_MAX_LEN];
    {
        WR_GUARD;
        weread_auth_session_t *target = session ? session : &s_current_session;
        if (target->refresh_token[0] == '\0') return ESP_ERR_INVALID_STATE;
        snap = *target;
        snprintf(dev_id, sizeof(dev_id), "%s", target->device_id[0] ? target->device_id : weread_auth_get_device_id());
        snprintf(dev_name, sizeof(dev_name), "%s", target->device_name[0] ? target->device_name : weread_auth_get_device_name());
    }

    char *url = wr_alloc(2048);
    char *headers = wr_alloc(512);
    if (!url || !headers) {
        weread_secure_zero(&snap, sizeof(snap));
        free(url); free(headers); return ESP_ERR_NO_MEM;
    }
    snprintf(headers, 512, "CLIENT_TYPE: 1\r\nX-Is-Serverside: 1\r\nfingerprint: %s\r\nContent-Type: application/json\r\n", dev_id);

    cJSON *body_json = cJSON_CreateObject();
    if (body_json) {
        cJSON_AddStringToObject(body_json, "vid", snap.vid);
        cJSON_AddStringToObject(body_json, "refreshToken", snap.refresh_token);
        cJSON_AddStringToObject(body_json, "deviceId", dev_id);
        cJSON_AddNumberToObject(body_json, "deviceType", WEREAD_DEVICE_TYPE_RENEW);
        cJSON_AddStringToObject(body_json, "deviceName", dev_name);
        cJSON_AddNumberToObject(body_json, "isFromweb", 0);
    }

    if (!body_json || !login_url(body_json, url, 2048)) {
        weread_secure_zero(&snap, sizeof(snap));
        cJSON_Delete(body_json); free(url); free(headers); return ESP_ERR_INVALID_SIZE;
    }
    char *body_str = cJSON_PrintUnformatted(body_json);
    cJSON_Delete(body_json);
    if (!body_str) {
        weread_secure_zero(&snap, sizeof(snap));
        free(url); free(headers); return ESP_ERR_NO_MEM;
    }

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;

    // Use transport callback directly to avoid recursive renewal loops in client
    esp_err_t err = weread_client_request(url, "POST", headers, body_str, &resp, &resp_len, &status);
    weread_secure_zero(body_str, strlen(body_str));
    free(body_str); free(url); free(headers);

    if (err != ESP_OK || !resp || (status != 200 && status != 401)) {
        weread_secure_zero(&snap, sizeof(snap));
        if (resp) free(resp);
        return ESP_FAIL;
    }

    cJSON *root = wr_parse_json(resp,resp_len + 1);
    free(resp);
    if (!root) {
        weread_secure_zero(&snap, sizeof(snap));
        return ESP_FAIL;
    }

    auth_cookie_fields(root);
    cJSON *tok_item = auth_alias(root,token_keys);

    if (!tok_item) tok_item = cJSON_GetObjectItem(root, "skey");

    if (!tok_item || !cJSON_IsString(tok_item) || !credential(tok_item->valuestring, WEREAD_TOKEN_MAX_LEN, true)) {
        const cJSON *code = auth_alias(root,(const char *const[]){"errCode","errcode","ret",NULL});
        bool revoked = cJSON_IsNumber(code) && code->valuedouble == WEREAD_AUTH_ERRCODE_EXPIRED;
        cJSON_Delete(root);
        weread_secure_zero(&snap, sizeof(snap));
        if (revoked) { WR_GUARD; weread_auth_clear_session(); return ESP_ERR_INVALID_STATE; }
        return ESP_FAIL;
    }

    cJSON *rt_item = auth_alias(root,refresh_keys);
    if (rt_item && (!cJSON_IsString(rt_item) || !credential(rt_item->valuestring, WEREAD_TOKEN_MAX_LEN, true))) {
        weread_secure_zero(&snap, sizeof(snap));
        cJSON_Delete(root); return ESP_ERR_INVALID_RESPONSE;
    }
    snprintf(snap.access_token, sizeof(snap.access_token), "%s", tok_item->valuestring);
    snprintf(snap.skey, sizeof(snap.skey), "%.*s", (int)sizeof(snap.skey) - 1, tok_item->valuestring);
    if (rt_item) snprintf(snap.refresh_token, sizeof(snap.refresh_token), "%s", rt_item->valuestring);
    snap.updated_at = (int64_t)time(NULL);
    WR_GUARD;
    weread_auth_session_t *target = session ? session : &s_current_session;
    esp_err_t saved = weread_auth_save_session(&snap);
    if (saved == ESP_OK) {
        memcpy(target, &snap, sizeof(snap));
        memcpy(&s_current_session, &snap, sizeof(snap));
        s_authenticated = true;
    }
    weread_secure_zero(&snap, sizeof(snap));
    cJSON_Delete(root); return saved;
}

static bool credential(const char *text, size_t cap, bool required) {
    size_t n = strnlen(text, cap);
    if (n == cap || (required && !n)) return false;
    for (size_t i = 0; i < n; ++i)
        if ((unsigned char)text[i] <= 32 || (unsigned char)text[i] >= 127 || text[i] == ';') return false;
    return true;
}

esp_err_t weread_auth_save_session(const weread_auth_session_t *session) { WR_GUARD;
    if (!session || !credential(session->vid, sizeof(session->vid), true) ||
        !credential(session->access_token, sizeof(session->access_token), true) ||
        !credential(session->refresh_token, sizeof(session->refresh_token), true) ||
        !credential(session->device_id, sizeof(session->device_id), false) ||
        !memchr(session->device_name, 0, sizeof(session->device_name)) ||
        !memchr(session->user_name, 0, sizeof(session->user_name)) ||
        !memchr(session->avatar_url, 0, sizeof(session->avatar_url))) return ESP_ERR_INVALID_ARG;
    cJSON *root = cJSON_CreateObject();
    if (!root) return ESP_ERR_NO_MEM;
    bool ok = cJSON_AddStringToObject(root, "vid", session->vid) &&
        cJSON_AddStringToObject(root, "skey", session->access_token) &&
        cJSON_AddStringToObject(root, "accessToken", session->access_token) &&
        cJSON_AddStringToObject(root, "refreshToken", session->refresh_token) &&
        cJSON_AddStringToObject(root, "deviceId", session->device_id) &&
        cJSON_AddStringToObject(root, "deviceName", session->device_name) &&
        cJSON_AddStringToObject(root, "wxNickName", session->user_name) &&
        cJSON_AddStringToObject(root, "wxHeadImg", session->avatar_url) &&
        cJSON_AddNumberToObject(root, "updatedAt", (double)session->updated_at);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", s_storage_root, WEREAD_AUTH_FILE_NAME);
    esp_err_t err = ok ? wr_write_json(path, root) : ESP_ERR_NO_MEM;
#if !defined(ESP_PLATFORM)
    if (err == ESP_OK && chmod(path, 0600)) err = ESP_FAIL;
#endif
    cJSON_Delete(root);
    return err;
}

esp_err_t weread_auth_load_session(weread_auth_session_t *out_session) { WR_GUARD;
    if (!out_session) return ESP_ERR_INVALID_ARG;
    char path[512]; snprintf(path, sizeof(path), "%s/%s", s_storage_root, WEREAD_AUTH_FILE_NAME);
    cJSON *root = NULL;
    esp_err_t err = wr_read_json(path, &root);
    weread_auth_session_t candidate = {0};
    const char *vid = wr_string(root, "vid"), *token = wr_string(root, "accessToken");
    if (!token) token = wr_string(root, "skey");
    const char *refresh = wr_string(root, "refreshToken");
    if (err == ESP_OK && (!wr_copy(candidate.vid, sizeof(candidate.vid), vid) ||
        !wr_copy(candidate.access_token, sizeof(candidate.access_token), token) ||
        !wr_copy(candidate.refresh_token, sizeof(candidate.refresh_token), refresh) ||
        !credential(candidate.vid, sizeof(candidate.vid), true) ||
        !credential(candidate.access_token, sizeof(candidate.access_token), true) ||
        !credential(candidate.refresh_token, sizeof(candidate.refresh_token), true))) err = ESP_ERR_INVALID_RESPONSE;
    const char *keys[] = {"deviceId", "deviceName", "wxNickName", "wxHeadImg"};
    char *dest[] = {candidate.device_id, candidate.device_name, candidate.user_name, candidate.avatar_url};
    size_t caps[] = {sizeof(candidate.device_id), sizeof(candidate.device_name), sizeof(candidate.user_name), sizeof(candidate.avatar_url)};
    for (size_t i = 0; i < 4 && err == ESP_OK; ++i) {
        const char *v = wr_string(root, keys[i]);
        if (v && !wr_copy(dest[i], caps[i], v)) err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && !credential(candidate.device_id, sizeof(candidate.device_id), false)) err = ESP_ERR_INVALID_RESPONSE;
    uint64_t time = 0; wr_uint(root, "updatedAt", &time); candidate.updated_at = (int64_t)time;
    if (err == ESP_OK) {
        snprintf(candidate.skey, sizeof(candidate.skey), "%.*s", (int)sizeof(candidate.skey) - 1, candidate.access_token);
        memcpy(out_session, &candidate, sizeof(candidate));
        memcpy(&s_current_session, &candidate, sizeof(candidate));
        s_authenticated = true;
    } else {
        weread_secure_zero(&s_current_session, sizeof(s_current_session)); s_authenticated = false;
        weread_secure_zero(out_session, sizeof(*out_session));
    }
    weread_secure_zero(&candidate, sizeof(candidate));
    cJSON_Delete(root); return err;
}

void weread_auth_forget_session(void) { WR_GUARD;
    weread_secure_zero(&s_current_session,sizeof(s_current_session));
    weread_secure_zero(&s_pending_auth,sizeof(s_pending_auth));
    weread_client_forget_login_cookies();s_authenticated=false;
}
esp_err_t weread_auth_clear_session(void) { WR_GUARD;
    char auth_path[512];
    char tmp_path[512];
    snprintf(auth_path, sizeof(auth_path), "%s/%s", s_storage_root, WEREAD_AUTH_FILE_NAME);
    snprintf(tmp_path, sizeof(tmp_path), "%s/%s", s_storage_root, WEREAD_AUTH_TMP_NAME);

    bool removed = true;
    if (unlink(auth_path) && errno != ENOENT) removed = false;
    if (unlink(tmp_path) && errno != ENOENT) removed = false;
    char previous[512]; snprintf(previous, sizeof(previous), "%s/%s.previous", s_storage_root, WEREAD_AUTH_FILE_NAME);
    if (unlink(previous) && errno != ENOENT) removed = false;

    weread_auth_forget_session();

    return removed ? ESP_OK : ESP_FAIL;
}

bool weread_auth_is_authenticated(void) {
    return s_authenticated && (s_current_session.access_token[0] != '\0');
}

const weread_auth_session_t *weread_auth_get_current_session(void) {
    if (!s_authenticated || s_current_session.access_token[0] == '\0') {
        return NULL;
    }
    return &s_current_session;
}
