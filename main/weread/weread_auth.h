/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead authentication, QR login state machine, and persistence.
 */
#pragma once

#include "weread_types.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the WeRead authentication subsystem and session storage.
 *
 * If storage_root is NULL or empty, inspects WEREAD_STORAGE_ROOT environment
 * variable, falling back to WEREAD_STORAGE_ROOT_DEFAULT ("/sdcard/.kiikoread/weread").
 * Ensures directories exist, loads any existing session from auth.json, and
 * generates/restores the device ID.
 *
 * @param storage_root Base storage directory path, or NULL for default.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t weread_auth_init(const char *storage_root);

/**
 * @brief Retrieve the active storage root directory path.
 *
 * @return const char* Pointer to storage root string.
 */
const char *weread_auth_get_storage_root(void);

/**
 * @brief Retrieve the persistent device ID (e.g. "eink550e8400-...").
 *
 * @return const char* Device ID string.
 */
const char *weread_auth_get_device_id(void);

/**
 * @brief Retrieve the device model/brand name (e.g. "KiikoReader").
 *
 * @return const char* Device name string.
 */
const char *weread_auth_get_device_name(void);

/**
 * @brief Start a new QR code login session.
 *
 * Calls GET /web/getuid?platform=desktop, parses session UID,
 * and builds the QR confirmation URL: https://weread.qq.com/web/confirm?pf=2&uid=<uid>.
 *
 * @param out_uid Buffer to receive temporary session UID.
 * @param uid_cap Capacity of out_uid buffer.
 * @param out_qr_url Buffer to receive full QR code URL for rendering.
 * @param qr_url_cap Capacity of out_qr_url buffer.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t weread_auth_start_qr_login(char *out_uid, size_t uid_cap, char *out_qr_url, size_t qr_url_cap);

/**
 * @brief Poll QR scan confirmation status.
 *
 * Calls POST /web/getlogininfo?platform=desktop with {"uid": "<uid>"}.
 * Decodes status into out_state. When CONFIRMED, populates out_code.
 *
 * @param uid The session UID obtained from start_qr_login.
 * @param out_state Pointer to receive decoded QR state.
 * @param out_code Buffer to receive authorization code upon confirmation.
 * @param code_cap Capacity of out_code buffer.
 * @return ESP_OK on communication success, or an error code.
 */
esp_err_t weread_auth_poll_qr_login(const char *uid, weread_qr_state_t *out_state, char *out_code, size_t code_cap);

/**
 * @brief Exchange QR confirmation code for persistent device tokens.
 *
 * Calls POST /weblogin?deviceType=4 with device login parameters.
 * Updates internal session state and atomically saves to auth.json.
 *
 * @param code Authorization code.
 * @param out_session Optional pointer to receive established session struct.
 * @return ESP_OK on success, or an error code.
 */
esp_err_t weread_auth_exchange_code(const char *code, weread_auth_session_t *out_session);

/**
 * @brief Automatically renew an expired session using the refresh token.
 *
 * Calls POST /weblogin?deviceType=7&isFromweb=0 with vid, refreshToken, deviceId, deviceName.
 * Updates internal session state and saves updated session to auth.json.
 *
 * @param session Pointer to session to renew (if NULL, renews current active session).
 * @return ESP_OK on successful renewal, or error code.
 */
esp_err_t weread_auth_renew_session(weread_auth_session_t *session);

/**
 * @brief Atomically save session credentials to disk (auth.json via auth.json.tmp).
 *
 * @param session Pointer to session to save.
 * @return ESP_OK on success, or error code.
 */
esp_err_t weread_auth_save_session(const weread_auth_session_t *session);

/**
 * @brief Load session credentials from disk (auth.json).
 *
 * @param out_session Pointer to struct to receive loaded session data.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file missing, or error code.
 */
esp_err_t weread_auth_load_session(weread_auth_session_t *out_session);

/**
 * @brief Log out and completely wipe all session credentials.
 *
 * Securely zeroes in-memory credentials using explicit zeroing and unlinks auth.json.
 *
 * @return ESP_OK on success, or error code.
 */
esp_err_t weread_auth_clear_session(void);
/// 拔卡只擦除RAM，不访问已失效的卡。/ Media loss wipes RAM without touching invalid storage.
void weread_auth_forget_session(void);

/**
 * @brief Check whether an authenticated session is currently loaded and active in memory.
 *
 * @return true if authenticated, false otherwise.
 */
bool weread_auth_is_authenticated(void);

/**
 * @brief Get a read-only pointer to the current active session in memory.
 *
 * @return const weread_auth_session_t* Pointer to session, or NULL if not authenticated.
 */
const weread_auth_session_t *weread_auth_get_current_session(void);

/**
 * @brief Securely zero memory buffer to eliminate sensitive credential residue.
 *
 * @param ptr Pointer to memory.
 * @param len Length in bytes.
 */
void weread_secure_zero(void *ptr, size_t len);

#ifdef __cplusplus
}
#endif
