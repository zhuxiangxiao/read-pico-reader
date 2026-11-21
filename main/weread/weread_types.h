/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead shared types, constants, and data structures.
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define WR_EXT_BSS EXT_RAM_BSS_ATTR
#else
#define WR_EXT_BSS
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define WEREAD_API_BASE_DEFAULT        "https://i.weread.qq.com"
#define WEREAD_WEB_CONFIRM_BASE        "https://weread.qq.com/web/confirm"
#define WEREAD_DEFAULT_USER_AGENT      "WeRead/1.0.0 WRBrand/remarkable wr_eink"
#define WEREAD_STORAGE_ROOT_DEFAULT    "/sdcard/.kiikoread/weread"
#define WEREAD_AUTH_FILE_NAME          "auth.json"
#define WEREAD_AUTH_TMP_NAME           "auth.json.tmp"

#define WEREAD_VID_MAX_LEN             32
#define WEREAD_SKEY_MAX_LEN            64
#define WEREAD_TOKEN_MAX_LEN           128
#define WEREAD_DEVICE_ID_MAX_LEN       64
#define WEREAD_DEVICE_NAME_MAX_LEN     64
#define WEREAD_NICKNAME_MAX_LEN        64
#define WEREAD_AVATAR_URL_MAX_LEN      256
#define WEREAD_UID_MAX_LEN             64
#define WEREAD_QR_URL_MAX_LEN          256
#define WEREAD_CODE_MAX_LEN            64

#define WEREAD_DEVICE_TYPE_INITIAL     4
#define WEREAD_DEVICE_TYPE_RENEW       7

/* Protocol error codes */
#define WEREAD_AUTH_ERRCODE_EXPIRED    -2013
#define WEREAD_AUTH_ERRCODE_TIMEOUT    -2012
#define WEREAD_ERR_CODE_SESSION_EXPIRED -2013
#define WEREAD_ERR_CODE_INVALID_TOKEN   -2012
#define WEREAD_ERR_CODE_VERSION_EXPIRED -2063
#define WEREAD_ERR_CODE_PAYMENT_REQ     402
#define WEREAD_ERR_CODE_DELISTED        499

typedef enum {
    WEREAD_QR_STATE_IDLE = 0,
    WEREAD_QR_STATE_WAITING_SCAN = 1,
    WEREAD_QR_STATE_WAITING_CONFIRM = 2,
    WEREAD_QR_STATE_CONFIRMED = 3,
    WEREAD_QR_STATE_NEED_OTP = 4,
    WEREAD_QR_STATE_TIMEOUT = 5,
    WEREAD_QR_STATE_CANCELLED = 6,
    WEREAD_QR_STATE_ERROR = 7
} weread_qr_state_t;

typedef struct {
    char vid[WEREAD_VID_MAX_LEN];
    char skey[WEREAD_SKEY_MAX_LEN];
    char access_token[WEREAD_TOKEN_MAX_LEN];
    char refresh_token[WEREAD_TOKEN_MAX_LEN];
    char device_id[WEREAD_DEVICE_ID_MAX_LEN];
    char device_name[WEREAD_DEVICE_NAME_MAX_LEN];
    char user_name[WEREAD_NICKNAME_MAX_LEN];
    char avatar_url[WEREAD_AVATAR_URL_MAX_LEN];
    int64_t updated_at;
} weread_auth_session_t;

typedef esp_err_t (*weread_http_transport_fn)(
    const char *url,
    const char *method,
    const char *headers,
    const char *body,
    char **out_resp,
    size_t *out_len,
    int *out_status,
    void *user_ctx
);

#ifdef __cplusplus
}
#endif
