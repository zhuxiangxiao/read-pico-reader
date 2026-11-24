/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead Milestone 2 Auth & Security Client Host Verification Suite.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>

#include "weread_types.h"
#include "weread_auth.h"
#include "weread_client.h"
#include "weread_store.h"

#define MAX_MOCK_RULES 32
#define MOCK_BODY_CAP  4096
#define MOCK_HDR_CAP   2048
#define MOCK_URL_CAP   512

typedef struct {
    char match_url_substr[MOCK_URL_CAP];
    char match_method[16];
    int resp_status;
    char resp_body[MOCK_BODY_CAP];
    esp_err_t resp_err;
    bool executed;
    int call_count;
    char recorded_url[MOCK_URL_CAP];
    char recorded_method[16];
    char recorded_headers[MOCK_HDR_CAP];
    char recorded_body[MOCK_BODY_CAP];
} mock_http_rule_t;

typedef struct {
    mock_http_rule_t rules[MAX_MOCK_RULES];
    size_t rule_count;
    int total_dispatched_requests;
} mock_http_context_t;

static mock_http_context_t s_mock_ctx;

static void mock_http_reset(void) {
    memset(&s_mock_ctx, 0, sizeof(s_mock_ctx));
}

static mock_http_rule_t *mock_http_add_rule(const char *url_substr, const char *method, int status, const char *body, esp_err_t err) {
    assert(s_mock_ctx.rule_count < MAX_MOCK_RULES);
    mock_http_rule_t *r = &s_mock_ctx.rules[s_mock_ctx.rule_count++];
    if (url_substr) snprintf(r->match_url_substr, sizeof(r->match_url_substr), "%s", url_substr);
    if (method) snprintf(r->match_method, sizeof(r->match_method), "%s", method);
    r->resp_status = status;
    if (body) snprintf(r->resp_body, sizeof(r->resp_body), "%s", body);
    r->resp_err = err;
    return r;
}

static esp_err_t mock_http_transport_callback(
    const char *url,
    const char *method,
    const char *headers,
    const char *body,
    char **out_resp,
    size_t *out_len,
    int *out_status,
    void *user_ctx
) {
    (void)user_ctx;
    s_mock_ctx.total_dispatched_requests++;

    mock_http_rule_t *matched = NULL;
    for (size_t i = 0; i < s_mock_ctx.rule_count; ++i) {
        mock_http_rule_t *r = &s_mock_ctx.rules[i];
        if (r->executed && s_mock_ctx.rule_count > 1) {
            continue;
        }
        if (r->match_url_substr[0] != '\0' && strstr(url, r->match_url_substr) == NULL) {
            continue;
        }
        if (r->match_method[0] != '\0' && strcmp(method, r->match_method) != 0) {
            continue;
        }
        matched = r;
        break;
    }

    assert(matched != NULL && "No mock HTTP rule matched the dispatched request!");
    matched->executed = true;
    matched->call_count++;

    if (url) snprintf(matched->recorded_url, sizeof(matched->recorded_url), "%s", url);
    if (method) snprintf(matched->recorded_method, sizeof(matched->recorded_method), "%s", method);
    if (headers) snprintf(matched->recorded_headers, sizeof(matched->recorded_headers), "%s", headers);
    if (body) snprintf(matched->recorded_body, sizeof(matched->recorded_body), "%s", body);

    if (out_status) *out_status = matched->resp_status;

    if (matched->resp_err != ESP_OK) {
        if (out_resp) *out_resp = NULL;
        if (out_len) *out_len = 0;
        return matched->resp_err;
    }

    if (out_resp) {
        *out_resp = strdup(matched->resp_body);
        assert(*out_resp != NULL);
    }
    if (out_len) {
        *out_len = strlen(matched->resp_body);
    }

    return ESP_OK;
}

/* ===========================================================================
 * Group 1: Outbound Security & Sensitive Header Stripping
 * =========================================================================== */
static void test_security_header_stripping_and_url_trust(void) {
    printf("[1/8] Testing outbound security & sensitive header stripping...\n");

    // 1.1 URL Trust Verification
    assert(weread_client_is_trusted_url("https://weread.qq.com/web/getuid"));
    assert(weread_client_is_trusted_url("https://i.weread.qq.com/shelf/sync"));
    assert(weread_client_is_trusted_url("https://qq.com/path"));
    assert(weread_client_is_trusted_url("https://sub.domain.qq.com/resource"));
    assert(weread_client_is_trusted_url("https://weread.qq.com:8443/custom_port"));

    // Reject non-HTTPS
    assert(!weread_client_is_trusted_url("http://weread.qq.com/insecure"));
    assert(!weread_client_is_trusted_url("http://i.weread.qq.com/insecure"));
    assert(!weread_client_is_trusted_url("ftp://weread.qq.com/file"));

    // Reject domain spoofing and suffix trickery
    assert(!weread_client_is_trusted_url("https://attacker-qq.com/steal"));
    assert(!weread_client_is_trusted_url("https://qq.com.attacker.com/steal"));
    assert(!weread_client_is_trusted_url("https://fakeqq.com/steal"));
    assert(!weread_client_is_trusted_url("https://evil.com/leak"));
    assert(!weread_client_is_trusted_url("https://notqq.com/"));

    // Remediation: Userinfo host confusion and fragment/backslash injection
    assert(weread_client_is_trusted_url("https://qq.com:password@attacker.com/") == false);
    assert(weread_client_is_trusted_url("https://user:pass@weread.qq.com/") == true);
    assert(weread_client_is_trusted_url("https://attacker.com#weread.qq.com") == false);
    assert(weread_client_is_trusted_url("https://attacker.com\\weread.qq.com") == false);

    // Remediation: Case-insensitive untrusted path markers
    assert(weread_client_is_trusted_url("https://weread.qq.com/Untrusted/data") == false);
    assert(weread_client_is_trusted_url("https://weread.qq.com/ThirdParty/data") == false);

    // Reject invalid inputs gracefully
    assert(!weread_client_is_trusted_url(NULL));
    assert(!weread_client_is_trusted_url(""));

    // 1.2 Header Stripping on Untrusted Targets
    const char *raw_headers =
        "User-Agent: WeRead/1.0.0\r\n"
        "Accept: application/json\r\n"
        "Cookie: wr_vid=12345; wr_skey=skey_secret\r\n"
        "Authorization: Bearer top_secret_token\r\n"
        "accessToken: skey_secret\r\n"
        "vid: 12345\r\n"
        "X-Custom-Client: KiikoReader\r\n";

    char stripped[1024];
    memset(stripped, 0, sizeof(stripped));

    // Untrusted URL -> All sensitive headers stripped
    weread_client_strip_sensitive_headers("https://evil.com/image.png", raw_headers, stripped, sizeof(stripped));
    assert(strstr(stripped, "User-Agent: WeRead/1.0.0") != NULL);
    assert(strstr(stripped, "Accept: application/json") != NULL);
    assert(strstr(stripped, "X-Custom-Client: KiikoReader") != NULL);
    assert(strstr(stripped, "Cookie:") == NULL);
    assert(strstr(stripped, "Authorization:") == NULL);
    assert(strstr(stripped, "accessToken:") == NULL);
    assert(strstr(stripped, "vid:") == NULL);

    // Case-insensitivity check (e.g. cookie:, authorization:)
    const char *lower_headers = "cookie: wr_vid=1\r\naccesstoken: secret\r\nvid: 123\r\nauthorization: tok\r\n";
    weread_client_strip_sensitive_headers("http://i.weread.qq.com/test", lower_headers, stripped, sizeof(stripped));
    assert(strlen(stripped) == 0 || (strstr(stripped, "cookie") == NULL && strstr(stripped, "vid") == NULL));

    // Remediation: Leading whitespace header stripping bypass check
    const char *leading_ws_headers =
        " Cookie: secret\r\n"
        "  Authorization: Bearer top\r\n"
        "\tvid: 12345\r\n"
        "X-Normal: safe\r\n";
    memset(stripped, 0, sizeof(stripped));
    weread_client_strip_sensitive_headers("https://evil.com/test", leading_ws_headers, stripped, sizeof(stripped));
    assert(strstr(stripped, "X-Normal: safe") != NULL);
    assert(strstr(stripped, "Cookie:") == NULL);
    assert(strstr(stripped, "Authorization:") == NULL);
    assert(strstr(stripped, "vid:") == NULL);
    assert(strstr(stripped, "secret") == NULL);
    assert(strstr(stripped, "Bearer top") == NULL);
    assert(strstr(stripped, "12345") == NULL);

    // Remediation: Multiline header folding continuation leak check
    const char *folded_headers =
        "Cookie:\r\n"
        "  wr_vid=folded\r\n"
        "X-Normal: preserved\r\n";
    memset(stripped, 0, sizeof(stripped));
    weread_client_strip_sensitive_headers("https://evil.com/test", folded_headers, stripped, sizeof(stripped));
    assert(strstr(stripped, "X-Normal: preserved") != NULL);
    assert(strstr(stripped, "Cookie:") == NULL);
    assert(strstr(stripped, "folded") == NULL);

    // Trusted URL -> All headers preserved intact
    memset(stripped, 0, sizeof(stripped));
    weread_client_strip_sensitive_headers("https://i.weread.qq.com/shelf/sync", raw_headers, stripped, sizeof(stripped));
    assert(strstr(stripped, "Cookie:") != NULL);
    assert(strstr(stripped, "Authorization:") != NULL);
    assert(strstr(stripped, "accessToken:") != NULL);
    assert(strstr(stripped, "vid:") != NULL);

    printf("  -> Security stripping tests passed.\n");
}

/* ===========================================================================
 * Group 2: QR Code URL Generation & UID Fetching
 * =========================================================================== */
static void test_qr_code_url_generation(void) {
    printf("[2/8] Testing QR code URL generation and /web/getuid...\n");
    mock_http_reset();

    mock_http_rule_t *rule = mock_http_add_rule(
        "/web/getuid", "GET", 200,
        "{\"data\": {\"uid\": \"mock_session_uid_9988\"}}", ESP_OK
    );

    char uid[WEREAD_UID_MAX_LEN] = {0};
    char qr_url[WEREAD_QR_URL_MAX_LEN] = {0};

    esp_err_t err = weread_auth_start_qr_login(uid, sizeof(uid), qr_url, sizeof(qr_url));
    assert(err == ESP_OK);
    assert(strcmp(uid, "mock_session_uid_9988") == 0);
    assert(strcmp(qr_url, "https://weread.qq.com/web/confirm?pf=2&uid=mock_session_uid_9988") == 0);

    // Verify outbound request headers
    assert(strstr(rule->recorded_headers, "CLIENT_TYPE: 1") != NULL);
    assert(strstr(rule->recorded_headers, "X-Is-Serverside: 1") != NULL);
    assert(strstr(rule->recorded_url, "platform=desktop") != NULL);

    // Fallback: top-level uid without "data" object
    mock_http_reset();
    mock_http_add_rule("/web/getuid", "GET", 200, "{\"uid\": \"toplevel_uid_7766\"}", ESP_OK);
    err = weread_auth_start_qr_login(uid, sizeof(uid), qr_url, sizeof(qr_url));
    assert(err == ESP_OK);
    assert(strcmp(uid, "toplevel_uid_7766") == 0);
    assert(strcmp(qr_url, "https://weread.qq.com/web/confirm?pf=2&uid=toplevel_uid_7766") == 0);

    // Error: empty UID response
    mock_http_reset();
    mock_http_add_rule("/web/getuid", "GET", 200, "{\"data\": {\"uid\": \"\"}}", ESP_OK);
    err = weread_auth_start_qr_login(uid, sizeof(uid), qr_url, sizeof(qr_url));
    assert(err != ESP_OK);

    printf("  -> QR code URL generation tests passed.\n");
}

/* ===========================================================================
 * Group 3: QR Polling State Machine
 * =========================================================================== */
static void test_qr_polling_state_machine(void) {
    printf("[3/8] Testing QR polling state machine across all states...\n");

    const struct {
        const char *response_json;
        weread_qr_state_t expected_state;
        const char *expected_code;
    } cases[] = {
        {"{\"scan\":0}", WEREAD_QR_STATE_WAITING_SCAN, ""},
        {"{\"data\":{\"scan\":1}}", WEREAD_QR_STATE_WAITING_CONFIRM, ""},
        {"{\"scan\":2}", WEREAD_QR_STATE_ERROR, ""},
        {"{\"data\": {\"logicCode\": \"WAITING_SCAN\"}}", WEREAD_QR_STATE_WAITING_SCAN, ""},
        {"{\"data\": {\"logicCode\": \"WAITING_CONFIRM\"}}", WEREAD_QR_STATE_WAITING_CONFIRM, ""},
        {"{\"data\": {\"logicCode\": \"NEED_OTP\"}}", WEREAD_QR_STATE_NEED_OTP, ""},
        // Confirmed state (real-world WeRead response format)
        {"{\"skey\": \"sk_123\", \"vid\": 998877, \"code\": \"AUTH_CODE_ALPHA_1\", \"pf\": \"2\"}",
         WEREAD_QR_STATE_CONFIRMED, "AUTH_CODE_ALPHA_1"},
        // Confirmed state (alternative logicCode payload)
        {"{\"data\": {\"logicCode\": \"LOGIN_SUCCESS\", \"code\": \"AUTH_CODE_BETA_2\"}}",
         WEREAD_QR_STATE_CONFIRMED, "AUTH_CODE_BETA_2"},
        // Timeouts and Cancellations
        {"{\"data\": {\"logicCode\": \"LOGIN_TIMEOUT\"}}", WEREAD_QR_STATE_TIMEOUT, ""},
        {"{\"data\": {\"logicCode\": \"OTP_EXPIRED\"}}", WEREAD_QR_STATE_TIMEOUT, ""},
        {"{\"data\": {\"logicCode\": \"EXPIRED\"}}", WEREAD_QR_STATE_TIMEOUT, ""},
        {"{\"data\": {\"logicCode\": \"LOGIN_CANCELLED\"}}", WEREAD_QR_STATE_CANCELLED, ""},
        {"{\"data\": {\"logicCode\": \"LOGIN_CANCELED\"}}", WEREAD_QR_STATE_CANCELLED, ""},
        {"{\"data\": {\"logicCode\": \"LOGIN_FAILED\"}}", WEREAD_QR_STATE_ERROR, ""},
        {"{\"data\": {\"logicCode\": \"LOGIN_FAIL\"}}", WEREAD_QR_STATE_ERROR, ""},
    };

    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
        mock_http_reset();
        mock_http_rule_t *rule = mock_http_add_rule(
            "/web/getlogininfo", "POST", 200, cases[i].response_json, ESP_OK
        );

        weread_qr_state_t state = WEREAD_QR_STATE_IDLE;
        char code[WEREAD_CODE_MAX_LEN] = {0};
        esp_err_t err = weread_auth_poll_qr_login("mock_uid", &state, code, sizeof(code));

        assert(err == ESP_OK);
        assert(state == cases[i].expected_state);
        if (cases[i].expected_code[0] != '\0') {
            assert(strcmp(code, cases[i].expected_code) == 0);
        }
        assert(strstr(rule->recorded_body, "\"uid\": \"mock_uid\"") != NULL ||
               strstr(rule->recorded_body, "\"uid\":\"mock_uid\"") != NULL);
    }

    // Transport error injection (e.g. timeout)
    mock_http_reset();
    mock_http_add_rule("/web/getlogininfo", "POST", 0, NULL, ESP_ERR_TIMEOUT);
    weread_qr_state_t state = WEREAD_QR_STATE_IDLE;
    char code[WEREAD_CODE_MAX_LEN] = {0};
    esp_err_t err = weread_auth_poll_qr_login("mock_uid", &state, code, sizeof(code));
    assert(err != ESP_OK);
    assert(state == WEREAD_QR_STATE_ERROR);

    printf("  -> QR polling state machine tests passed.\n");
}

/* ===========================================================================
 * Group 4: Initial Device Login Token Exchange (POST /weblogin deviceType=4)
 * =========================================================================== */
static void test_initial_device_login_token_exchange(void) {
    printf("[4/8] Testing initial device login token exchange (deviceType=4)...\n");
    mock_http_reset();

    const char *weblogin_resp =
        "{\n"
        "  \"webLoginVid\": \"88776655\",\n"
        "  \"accessToken\": \"acc_token_initial_xyz\",\n"
        "  \"refreshToken\": \"ref_token_initial_123\",\n"
        "  \"name\": \"TestKiikoUser\"\n"
        "}";

    mock_http_rule_t *rule = mock_http_add_rule("/weblogin", "POST", 200, weblogin_resp, ESP_OK);

    weread_auth_session_t session;
    memset(&session, 0, sizeof(session));

    esp_err_t err = weread_auth_exchange_code("AUTH_CODE_ALPHA_1", &session);
    assert(err == ESP_OK);
    assert(strcmp(session.vid, "88776655") == 0);
    assert(strcmp(session.access_token, "acc_token_initial_xyz") == 0);
    assert(strcmp(session.refresh_token, "ref_token_initial_123") == 0);
    assert(weread_auth_is_authenticated());

    // Verify request payload fields
    assert(strstr(rule->recorded_headers, "CLIENT_TYPE: 1") != NULL);
    assert(strstr(rule->recorded_headers, "X-Is-Serverside: 1") != NULL);
    assert(strstr(rule->recorded_headers, "fingerprint:") != NULL);
    assert(strstr(rule->recorded_url, "deviceType=4") != NULL);
    assert(strstr(rule->recorded_body, "\"deviceType\": 4") != NULL ||
           strstr(rule->recorded_body, "\"deviceType\":4") != NULL);
    assert(strstr(rule->recorded_body, "AUTH_CODE_ALPHA_1") != NULL);

    printf("  -> Initial token exchange tests passed.\n");
}

/* ===========================================================================
 * Group 5: Atomic Persistence (auth.json), Boot Load & Logout Wipe
 * =========================================================================== */
static void test_atomic_persistence_boot_load_and_wipe(const char *temp_dir) {
    printf("[5/8] Testing atomic persistence to auth.json, boot load, and logout wipe...\n");

    weread_auth_session_t save_sess;
    memset(&save_sess, 0, sizeof(save_sess));
    snprintf(save_sess.vid, sizeof(save_sess.vid), "88776655");
    snprintf(save_sess.access_token, sizeof(save_sess.access_token), "acc_token_saved");
    snprintf(save_sess.refresh_token, sizeof(save_sess.refresh_token), "ref_token_saved");
    snprintf(save_sess.device_id, sizeof(save_sess.device_id), "eink-test-uuid");
    save_sess.updated_at = 1700000000;

    esp_err_t err = weread_auth_save_session(&save_sess);
    assert(err == ESP_OK);

    // Verify auth.json exists and auth.json.tmp was cleanly renamed
    char auth_path[512], tmp_path[512];
    snprintf(auth_path, sizeof(auth_path), "%s/auth.json", temp_dir);
    snprintf(tmp_path, sizeof(tmp_path), "%s/auth.json.tmp", temp_dir);
    assert(access(auth_path, F_OK) == 0);
    assert(access(tmp_path, F_OK) != 0);

    // Verify permissions: 0600
    struct stat st;
    assert(stat(auth_path, &st) == 0);
    assert((st.st_mode & 0777) == 0600);

    // Boot Load verification
    weread_auth_session_t loaded_sess;
    memset(&loaded_sess, 0, sizeof(loaded_sess));
    err = weread_auth_load_session(&loaded_sess);
    assert(err == ESP_OK);
    assert(strcmp(loaded_sess.vid, "88776655") == 0);
    assert(strcmp(loaded_sess.access_token, "acc_token_saved") == 0);
    assert(strcmp(loaded_sess.refresh_token, "ref_token_saved") == 0);

    // Logout Wipe verification
    err = weread_auth_clear_session();
    assert(err == ESP_OK);
    assert(access(auth_path, F_OK) != 0); // File deleted
    assert(!weread_auth_is_authenticated()); // In-memory cleared
    assert(weread_auth_get_current_session() == NULL);

    printf("  -> Atomic persistence and wipe tests passed.\n");
}

/* ===========================================================================
 * Group 6: Corrupted / Partial auth.json Resilience
 * =========================================================================== */
static void test_corrupted_auth_json_resilience(const char *temp_dir) {
    printf("[6/8] Testing corrupted and partial auth.json resilience...\n");

    char auth_path[512];
    snprintf(auth_path, sizeof(auth_path), "%s/auth.json", temp_dir);

    const char *corrupted_inputs[] = {
        "",                                                  // Empty file
        "Not a JSON string at all!",                        // Non-JSON
        "{\"vid\": \"12345\", \"accessToken\": \"incomplete", // Truncated
        "{\"foo\": \"bar\"}",                               // Missing required fields
        "{\"vid\": 12345, \"accessToken\": true}",          // Type mismatches
        "{\"vid\": \"\", \"accessToken\": \"\"}",           // Empty strings
        "{\"vid\": \"12345\", \"expired\": \"yes\"}",       // Non-boolean expired flag
        "{\"vid\": \"\", \"expired\": true}",               // Expired true with empty vid
    };

    for (size_t i = 0; i < sizeof(corrupted_inputs)/sizeof(corrupted_inputs[0]); ++i) {
        FILE *f = fopen(auth_path, "wb");
        assert(f != NULL);
        if (strlen(corrupted_inputs[i]) > 0) {
            size_t w = fwrite(corrupted_inputs[i], 1, strlen(corrupted_inputs[i]), f);
            (void)w;
        }
        fclose(f);

        weread_auth_session_t sess;
        memset(&sess, 0, sizeof(sess));
        esp_err_t err = weread_auth_load_session(&sess);
        // Must fail gracefully without crash, segfault, or UB
        assert(err != ESP_OK);
        assert(!weread_auth_is_authenticated());
        assert(!weread_auth_is_expired());
    }

    unlink(auth_path);
    printf("  -> Corrupted auth.json resilience tests passed.\n");
}

/* ===========================================================================
 * Group 7: Session Renewal on -2013 / -2012 & Single Replay
 * =========================================================================== */
static void test_session_renewal_and_single_replay(void) {
    printf("[7/8] Testing automatic session renewal on -2013/-2012 and single replay...\n");
    mock_http_reset();

    // Re-initialize active authenticated session
    weread_auth_session_t sess;
    memset(&sess, 0, sizeof(sess));
    snprintf(sess.vid, sizeof(sess.vid), "88776655");
    snprintf(sess.access_token, sizeof(sess.access_token), "old_expired_token_001");
    snprintf(sess.refresh_token, sizeof(sess.refresh_token), "valid_refresh_token_002");
    weread_auth_save_session(&sess);
    weread_auth_load_session(&sess);

    // Rule 1: First request to /shelf/sync returns -2013
    mock_http_rule_t *r1 = mock_http_add_rule(
        "/shelf/sync", "GET", 200, "{\"errcode\": -2013, \"errmsg\": \"session expired\"}", ESP_OK
    );

    // Rule 2: Renewal request to /weblogin (deviceType=7) returns new tokens
    mock_http_rule_t *r2 = mock_http_add_rule(
        "/weblogin", "POST", 401,
        "{\"errcode\":-2013,\"accessToken\": \"new_active_token_999\", \"refreshToken\": \"new_refresh_token_888\"}", ESP_OK
    );

    // Rule 3: Replayed request to /shelf/sync succeeds
    mock_http_rule_t *r3 = mock_http_add_rule(
        "/shelf/sync", "GET", 200, "{\"books\": [{\"bookId\": \"B101\"}]}", ESP_OK
    );

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;

    esp_err_t err = weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &resp_len, &status);
    assert(err == ESP_OK);
    assert(status == 200);
    assert(resp != NULL);
    assert(strstr(resp, "B101") != NULL);
    free(resp);

    // Verify 3 total requests dispatched
    assert(s_mock_ctx.total_dispatched_requests == 3);
    assert(r1->call_count == 1);
    assert(r2->call_count == 1);
    assert(r3->call_count == 1);

    // Verify renewal request parameters
    assert(strstr(r2->recorded_url, "deviceType=7") != NULL);
    assert(strstr(r2->recorded_url, "isFromweb=0") != NULL);
    assert(strstr(r2->recorded_headers, "fingerprint:") != NULL);

    // Verify replayed request used the new renewed accessToken
    assert(strstr(r3->recorded_headers, "accessToken: new_active_token_999") != NULL);

    // Verify memory and disk updated with renewed tokens
    const weread_auth_session_t *cur = weread_auth_get_current_session();
    assert(cur != NULL);
    assert(strcmp(cur->access_token, "new_active_token_999") == 0);

    printf("  -> Session renewal and single replay tests passed.\n");
}

/* ===========================================================================
 * Group 8: Single-Replay Limit Guard / Infinite Loop Prevention
 * =========================================================================== */
static void test_replay_limit_guard_infinite_loop_prevention(void) {
    printf("[8/8] Testing single-replay limit guard and loop prevention...\n");
    mock_http_reset();

    weread_auth_session_t sess;
    memset(&sess, 0, sizeof(sess));
    snprintf(sess.vid, sizeof(sess.vid), "88776655");
    snprintf(sess.access_token, sizeof(sess.access_token), "old_token");
    snprintf(sess.refresh_token, sizeof(sess.refresh_token), "stale_refresh_token");
    weread_auth_save_session(&sess);
    weread_auth_load_session(&sess);

    // Rule 1: Initial request returns -2013
    mock_http_add_rule("/shelf/sync", "GET", 200, "{\"errcode\": -2013}", ESP_OK);

    // Rule 2: Renewal succeeds and issues temporary key
    mock_http_add_rule("/weblogin", "POST", 200, "{\"accessToken\": \"temp_key\"}", ESP_OK);

    // Rule 3: Replayed request STILL returns -2013
    mock_http_add_rule("/shelf/sync", "GET", 200, "{\"errcode\": -2013}", ESP_OK);

    char *resp = NULL;
    size_t resp_len = 0;
    int status = 0;

    esp_err_t err = weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &resp_len, &status);

    // Must fail, must NOT initiate a 4th request or recurse
    assert(err != ESP_OK);
    if (resp) free(resp);

    assert(s_mock_ctx.total_dispatched_requests == 3);
    // On second failure, session must be wiped
    assert(!weread_auth_is_authenticated());

    printf("  -> Single-replay limit guard tests passed.\n");
}

static void test_transient_and_untrusted_errors(void) {
    weread_auth_session_t s = {0};
    strcpy(s.vid, "123"); strcpy(s.access_token, "old-token"); strcpy(s.refresh_token, "rt+a/b=");
    assert(weread_auth_save_session(&s) == ESP_OK && weread_auth_load_session(&s) == ESP_OK);
    mock_http_reset();
    mock_http_add_rule("/shelf/sync", "GET", 200, "{\"errcode\":-2012}", ESP_OK);
    mock_http_add_rule("/weblogin", "POST", 0, NULL, ESP_ERR_TIMEOUT);
    char *response = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &response, &len, &status) != ESP_OK);
    assert(weread_auth_is_authenticated() && response && strstr(response, "-2012")); free(response);
    assert(s_mock_ctx.total_dispatched_requests == 2);
    mock_http_reset();
    mock_http_add_rule("/assets", "GET", 200, "{\"errcode\":-2013}", ESP_OK);
    assert(weread_client_request("https://outside.example/assets", "GET", NULL, NULL, &response, &len, &status) == ESP_OK);
    free(response); assert(s_mock_ctx.total_dispatched_requests == 1 && weread_auth_is_authenticated());
    assert(!weread_client_is_trusted_url("https://qq.com:443evil/path"));
    assert(!weread_client_is_trusted_url("https://qq.com:999999999999999999999/path"));
    assert(!weread_client_is_trusted_url("https://qq.com\\@outside.example/path"));
    mock_http_reset();
    mock_http_rule_t *rule = mock_http_add_rule("/weblogin", "POST", 200, "{\"accessToken\":\"new-token\"}", ESP_OK);
    assert(weread_auth_renew_session(NULL) == ESP_OK);
    assert(strstr(rule->recorded_url, "refreshToken=rt%2Ba%2Fb%3D"));
    assert(strstr(rule->recorded_headers, "wr_rt=rt%2Ba%2Fb%3D"));
    puts("  -> Transient errors, external responses and credential encoding passed.");
}

static void test_http401_session_classification(void) {
    const char *bodies[] = {"{\"errcode\":-2013}", "{\"errCode\":-2013}",
        "{\"ret\":-2013}", "{\"errcode\":-2012}", "{}"};
    for (size_t i = 0; i < sizeof(bodies) / sizeof(*bodies); ++i) {
        weread_auth_session_t session = {0};
        strcpy(session.vid, "123"); strcpy(session.access_token, "old-token");
        strcpy(session.refresh_token, "refresh-token");
        assert(weread_auth_save_session(&session) == ESP_OK);
        assert(weread_auth_load_session(&session) == ESP_OK);
        mock_http_reset();
        mock_http_add_rule("/shelf/sync", "GET", 401, bodies[i], ESP_OK);
        mock_http_add_rule("/weblogin", "POST", 200, "{\"accessToken\":\"new-token\"}", ESP_OK);
        mock_http_add_rule("/shelf/sync", "GET", 401, bodies[i], ESP_OK);
        char *response = NULL; size_t length = 0; int status = 0;
        assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL,
            &response, &length, &status) != ESP_OK);
        assert(status == 401 && response && length == strlen(bodies[i])); free(response);
        assert(s_mock_ctx.total_dispatched_requests == 3);
        // Only explicit revocation erases credentials, including their persisted copy.
        assert(weread_auth_is_authenticated() == (i >= 3));
        weread_auth_session_t loaded = {0};
        assert((weread_auth_load_session(&loaded) == ESP_OK) == (i >= 3));
    }
    const char *renewals[] = {"{\"errcode\":-2013}", "{\"ret\":-2012}", "{}", NULL};
    for (size_t i = 0; i < sizeof(renewals) / sizeof(*renewals); ++i) {
        weread_auth_session_t session = {0};
        strcpy(session.vid, "123"); strcpy(session.access_token, "old-token");
        strcpy(session.refresh_token, "refresh-token");
        assert(weread_auth_save_session(&session) == ESP_OK);
        assert(weread_auth_load_session(&session) == ESP_OK);
        mock_http_reset();
        mock_http_add_rule("/shelf/sync", "GET", 401, "{\"errcode\":-2012}", ESP_OK);
        mock_http_add_rule("/weblogin", "POST", 401, renewals[i], i == 3 ? ESP_ERR_TIMEOUT : ESP_OK);
        char *response = NULL;
        assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL,
            &response, NULL, NULL) != ESP_OK); free(response);
        assert(s_mock_ctx.total_dispatched_requests == 2);
        assert(weread_auth_is_authenticated() == (i != 0));
        assert((weread_auth_load_session(&session) == ESP_OK) == (i != 0));
    }
    puts("  -> HTTP401 revocation aliases and transient replay preservation passed.");
}

static void test_storage_failure_and_recovery(const char *dir) {
    char path[512], tmp[512], previous[512];
    snprintf(path, sizeof(path), "%s/auth.json", dir);
    snprintf(tmp, sizeof(tmp), "%s/auth.json.tmp", dir);
    snprintf(previous, sizeof(previous), "%s/auth.json.previous", dir);
    weread_auth_session_t old = {0}; assert(weread_auth_load_session(&old) == ESP_OK);
    weread_auth_session_t newer = old; strcpy(newer.access_token, "must-not-commit");
    assert(!mkdir(tmp, 0700)); assert(weread_auth_save_session(&newer) != ESP_OK); assert(!rmdir(tmp));
    weread_auth_session_t loaded;
    assert(weread_auth_load_session(&loaded) == ESP_OK && !strcmp(loaded.access_token, old.access_token));
    assert(!rename(path, previous));
    assert(weread_auth_load_session(&loaded) == ESP_OK && !strcmp(loaded.access_token, old.access_token));
    assert(weread_auth_clear_session() == ESP_OK);
    assert(access(previous, F_OK) != 0 && weread_auth_load_session(&loaded) != ESP_OK);
    strcpy(newer.access_token, "bad\r\nHeader: injected"); assert(weread_auth_save_session(&newer) == ESP_ERR_INVALID_ARG);
    puts("  -> Failed-write preservation, FAT recovery and complete logout passed.");
}

static int s_activity_hits = 0;
static bool s_activity_clear_self = false;
static void auth_test_activity_cb(void *ctx) {
    int *hits = ctx;
    WR_GUARD;
    (*hits)++;
    if (s_activity_clear_self) {
        weread_client_set_activity_cb(NULL, NULL);
    }
}
static esp_err_t offline_network_cb(void *ctx) {
    (void)ctx;
    return ESP_ERR_INVALID_STATE;
}
static bool preconnect_cancel_cb(void *ctx) {
    (void)ctx;
    return true;
}
static esp_err_t mock_dl_cb(const char *url, const char *headers, FILE *file, size_t limit,
    weread_cancel_fn cancel, void *cancel_ctx, weread_download_info_t *info, void *ctx) {
    (void)url; (void)headers; (void)limit; (void)cancel; (void)cancel_ctx; (void)ctx;
    const char *payload = "OK_DL_BYTES";
    info->status = 200;
    info->bytes = strlen(payload);
    assert(fwrite(payload, 1, info->bytes, file) == info->bytes);
    return ESP_OK;
}

static void test_expired_readonly_and_activity_cb(const char *dir) {
    printf("[12/12] Testing WR-UX-01a expired read-only state and WR-UX-06a activity callback...\n");
    char auth_path[512], prev_path[512];
    snprintf(auth_path, sizeof(auth_path), "%s/auth.json", dir);
    snprintf(prev_path, sizeof(prev_path), "%s/auth.json.previous", dir);

    weread_auth_session_t sess = {0};
    strcpy(sess.vid, "88776655");
    strcpy(sess.access_token, "tok-active");
    strcpy(sess.refresh_token, "ref-active");
    strcpy(sess.device_id, "eink-dev-01");
    strcpy(sess.device_name, "KiikoReader");
    strcpy(sess.user_name, "ReaderOne");
    sess.updated_at = 1700001234;
    assert(weread_auth_save_session(&sess) == ESP_OK);
    assert(weread_auth_load_session(&sess) == ESP_OK);
    /* Simulate pre-existing FAT .previous copy with old tokens */
    assert(wr_write_bytes(prev_path, "{\"accessToken\":\"stale\"}", 23) == ESP_OK);
    assert(access(prev_path, F_OK) == 0);

    mock_http_reset();
    mock_http_add_rule("/shelf/sync", "GET", 200, "{\"errcode\":-2013}", ESP_OK);
    mock_http_add_rule("/weblogin", "POST", 401, "{\"errcode\":-2013}", ESP_OK);
    char *resp = NULL; size_t len = 0; int status = 0;
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &len, &status) != ESP_OK);
    free(resp);

    assert(!weread_auth_is_authenticated());
    assert(weread_auth_is_expired());
    assert(weread_auth_owner_vid() && strcmp(weread_auth_owner_vid(), "88776655") == 0);
    const weread_auth_session_t *ro = weread_auth_get_current_session();
    assert(ro != NULL);
    assert(strcmp(ro->vid, "88776655") == 0);
    assert(strcmp(ro->device_id, "eink-dev-01") == 0);
    assert(strcmp(ro->user_name, "ReaderOne") == 0);
    assert(ro->access_token[0] == '\0' && ro->refresh_token[0] == '\0' && ro->skey[0] == '\0');
    assert(access(auth_path, F_OK) == 0);
    assert(access(prev_path, F_OK) != 0);

    /* Verify auth.json content has expired:true and no tokens */
    cJSON *disk = NULL;
    assert(wr_read_json_primary(auth_path, &disk) == ESP_OK && disk != NULL);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(disk, "expired")));
    assert(strcmp(wr_string(disk, "vid"), "88776655") == 0);
    assert(!cJSON_GetObjectItemCaseSensitive(disk, "accessToken"));
    assert(!cJSON_GetObjectItemCaseSensitive(disk, "refreshToken"));
    assert(!cJSON_GetObjectItemCaseSensitive(disk, "skey"));
    cJSON_Delete(disk);

    /* Owner matching succeeds while network identity capture rejects */
    weread_client_identity_t cid;
    assert(!weread_client_identity_get(&cid));
    cJSON *owned = cJSON_CreateObject();
    assert(wr_set_owner(owned));
    assert(wr_owner_matches(owned));
    cJSON_Delete(owned);

    /* Reboot simulation: weread_auth_init loads expired read-only state; load_session returns INVALID_STATE */
    weread_auth_forget_session();
    assert(!weread_auth_is_expired() && !weread_auth_owner_vid() && !weread_auth_get_current_session());
    assert(weread_auth_init(dir) == ESP_OK);
    assert(!weread_auth_is_authenticated());
    assert(weread_auth_is_expired());
    assert(weread_auth_owner_vid() && strcmp(weread_auth_owner_vid(), "88776655") == 0);
    assert(weread_auth_get_current_session() != NULL);
    weread_auth_session_t out_load;
    assert(weread_auth_load_session(&out_load) == ESP_ERR_INVALID_STATE);
    assert(out_load.vid[0] == '\0' && out_load.access_token[0] == '\0');
    assert(weread_auth_is_expired());

    /* Re-login via exchange_code clears expired flag */
    mock_http_reset();
    mock_http_add_rule("/weblogin", "POST", 200,
        "{\"vid\":\"88776655\",\"accessToken\":\"relogin-tok\",\"refreshToken\":\"relogin-ref\"}", ESP_OK);
    assert(weread_auth_exchange_code("RELOGIN_CODE", &out_load) == ESP_OK);
    assert(weread_auth_is_authenticated() && !weread_auth_is_expired());

    /* Explicit logout wipes everything including expired state */
    mock_http_reset();
    mock_http_add_rule("/shelf/sync", "GET", 200, "{\"errcode\":-2013}", ESP_OK);
    mock_http_add_rule("/weblogin", "POST", 401, "{\"errcode\":-2013}", ESP_OK);
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &len, &status) != ESP_OK);
    free(resp);
    assert(weread_auth_is_expired());
    assert(weread_auth_clear_session() == ESP_OK);
    assert(!weread_auth_is_authenticated() && !weread_auth_is_expired());
    assert(weread_auth_owner_vid() == NULL && weread_auth_get_current_session() == NULL);
    assert(access(auth_path, F_OK) != 0);

    /* WR-UX-06a: activity callback fires on real HTTP & download, never on offline/cancel */
    s_activity_hits = 0;
    s_activity_clear_self = false;
    weread_client_set_activity_cb(auth_test_activity_cb, &s_activity_hits);

    weread_client_set_network(offline_network_cb, NULL);
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &len, &status) != ESP_OK);
    assert(s_activity_hits == 0);

    weread_client_set_network(NULL, NULL);
    weread_client_set_cancel(preconnect_cancel_cb, NULL);
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &len, &status) == ESP_ERR_INVALID_STATE);
    assert(s_activity_hits == 0);
    weread_client_set_cancel(NULL, NULL);

    mock_http_reset();
    mock_http_add_rule("/shelf/sync", "GET", 0, NULL, ESP_ERR_TIMEOUT);
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &len, &status) == ESP_ERR_TIMEOUT);
    assert(s_activity_hits == 0);

    mock_http_reset();
    mock_http_add_rule("/shelf/sync", "GET", 200, "{\"synckey\":1}", ESP_OK);
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &len, &status) == ESP_OK);
    free(resp);
    assert(s_activity_hits == 1);

    weread_client_set_download_transport(mock_dl_cb, NULL);
    FILE *dlf = tmpfile(); assert(dlf);
    weread_download_info_t dinfo = {0};
    s_activity_clear_self = true;
    assert(weread_client_download("https://res.weread.qq.com/cover.png", dlf, 4096, NULL, NULL, &dinfo) == ESP_OK);
    assert(s_activity_hits == 2);
    /* Self-cleared inside callback -> next request should not increment */
    mock_http_reset();
    mock_http_add_rule("/shelf/sync", "GET", 200, "{\"synckey\":2}", ESP_OK);
    assert(weread_client_request("https://i.weread.qq.com/shelf/sync", "GET", NULL, NULL, &resp, &len, &status) == ESP_OK);
    free(resp);
    assert(s_activity_hits == 2);
    fclose(dlf);
    weread_client_set_download_transport(NULL, NULL);
    weread_client_set_activity_cb(NULL, NULL);
    printf("  -> WR-UX-01a expired read-only & WR-UX-06a activity callback tests passed.\n");
}

int main(void) {
    printf("========================================================\n");
    printf(" Running WeRead Milestone 2 Auth & Security Client Tests\n");
    printf("========================================================\n");

    char temp_dir[] = "/tmp/weread_auth_test_XXXXXX";
    char *res = mkdtemp(temp_dir);
    assert(res != NULL);

    weread_client_set_transport(mock_http_transport_callback, NULL);
    esp_err_t err = weread_auth_init(temp_dir);
    assert(err == ESP_OK);

    test_security_header_stripping_and_url_trust();
    test_qr_code_url_generation();
    test_qr_polling_state_machine();
    test_initial_device_login_token_exchange();
    test_atomic_persistence_boot_load_and_wipe(temp_dir);
    test_corrupted_auth_json_resilience(temp_dir);
    test_session_renewal_and_single_replay();
    test_replay_limit_guard_infinite_loop_prevention();
    test_http401_session_classification();
    test_transient_and_untrusted_errors();
    test_storage_failure_and_recovery(temp_dir);
    test_expired_readonly_and_activity_cb(temp_dir);

    // Clean up temporary test files
    char path_buf[512];
    snprintf(path_buf, sizeof(path_buf), "%s/auth.json", temp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/auth.json.tmp", temp_dir);
    unlink(path_buf);
    snprintf(path_buf, sizeof(path_buf), "%s/device_id", temp_dir);
    unlink(path_buf);
    rmdir(temp_dir);

    printf("========================================================\n");
    printf(" ALL 11 AUTH TEST GROUPS PASSED SUCCESSFULLY! (Zero errors)\n");
    printf("========================================================\n");
    return 0;
}
