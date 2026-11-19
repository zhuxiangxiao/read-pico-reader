/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 *
 * 中文：微信读书传输解密、ZipCrypto/TAR 解包与 Web 签名宿主单元测试。
 * English: Host unit tests for WeRead transport decryption, ZipCrypto/TAR unpacking,
 *          and Web progress signing.
 */

#include <assert.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "book/book_weread_crypto.h"
#include "book/book_weread_uri.h"

/* ---------------------------------------------------------------------------
 * Test 1: Base64 Encode / Decode Tests
 * --------------------------------------------------------------------------- */
static void test_base64(void) {
    printf("[1/10] Testing Base64 decoding...\n");

    // Standard vector 1: "Hello WeRead!"
    const char *b64_1 = "SGVsbG8gV2VSZWFkIQ==";
    uint8_t out[64];
    size_t out_len = 0;
    assert(book_weread_base64_decode(b64_1, out, sizeof(out), &out_len));
    assert(out_len == 13);
    assert(memcmp(out, "Hello WeRead!", 13) == 0);

    // Padding variants: 0, 1, 2 '=' padding
    const char *b64_pad0 = "YW55"; // "any"
    assert(book_weread_base64_decode(b64_pad0, out, sizeof(out), &out_len));
    assert(out_len == 3 && memcmp(out, "any", 3) == 0);

    const char *b64_pad2 = "YW55Yw=="; // "anyc"
    assert(book_weread_base64_decode(b64_pad2, out, sizeof(out), &out_len));
    assert(out_len == 4 && memcmp(out, "anyc", 4) == 0);

    const char *b64_pad1 = "YW55Y2Q="; // "anycd"
    assert(book_weread_base64_decode(b64_pad1, out, sizeof(out), &out_len));
    assert(out_len == 5 && memcmp(out, "anycd", 5) == 0);

    // Whitespace tolerance (spaces, tabs, newlines)
    const char *b64_ws = "  SGVs\r\nbG8 gV2\tVSZWFkIQ== \n";
    assert(book_weread_base64_decode(b64_ws, out, sizeof(out), &out_len));
    assert(out_len == 13 && memcmp(out, "Hello WeRead!", 13) == 0);

    // URL-safe alphabet ('-' -> '+', '_' -> '/')
    // Standard: "ab+c/d==" -> URL-safe: "ab-c_d=="
    const char *b64_url = "ab-c_d==";
    assert(book_weread_base64_decode(b64_url, out, sizeof(out), &out_len));
    uint8_t expected_url[4];
    size_t exp_len = 0;
    assert(book_weread_base64_decode("ab+c/d==", expected_url, sizeof(expected_url), &exp_len));
    assert(out_len == exp_len && memcmp(out, expected_url, out_len) == 0);

    // Error cases:
    assert(!book_weread_base64_decode(NULL, out, sizeof(out), &out_len));
    assert(!book_weread_base64_decode(b64_1, NULL, sizeof(out), &out_len));
    assert(!book_weread_base64_decode(b64_1, out, sizeof(out), NULL));
    assert(!book_weread_base64_decode(b64_1, out, 5, &out_len)); // Buffer too small
    assert(!book_weread_base64_decode("Invalid!Char#", out, sizeof(out), &out_len));

    printf("  -> Base64 decoding passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 2: AES-128-CBC Decrypt encryptkey
 * --------------------------------------------------------------------------- */
static void test_aes128_cbc_encryptkey(void) {
    printf("[2/10] Testing AES-128-CBC encryptkey decryption...\n");

    const char *skey = "gSqTestLp";
    const char *b64_ct = "EO0RNDAOdH6dgrwJKqktz2QDOksjyP4WftL/uRXWPDw=";
    const char *expected_key = "0123456789abcdef_secret_key!";
    uint8_t dec_key[64];
    size_t dec_len = 0;

    // Test Base64-encoded ciphertext
    assert(book_weread_decrypt_encrypt_key(b64_ct, skey, dec_key, sizeof(dec_key), &dec_len));
    assert(dec_len == strlen(expected_key));
    assert(memcmp(dec_key, expected_key, dec_len) == 0);

    // Test Hex-encoded ciphertext
    // Decoded 32-byte ciphertext in hex:
    // 10ed1134300e747e9d82bc092aa92dcf64033a4b23c8fe167ed2ffb915d63c3c
    const char *hex_ct = "10ed1134300e747e9d82bc092aa92dcf64033a4b23c8fe167ed2ffb915d63c3c";
    memset(dec_key, 0, sizeof(dec_key));
    dec_len = 0;
    assert(book_weread_decrypt_encrypt_key(hex_ct, skey, dec_key, sizeof(dec_key), &dec_len));
    assert(dec_len == strlen(expected_key));
    assert(memcmp(dec_key, expected_key, dec_len) == 0);

    // Error case: Wrong secret (fails PKCS#7 unpadding)
    assert(!book_weread_decrypt_encrypt_key(b64_ct, "wrong_secret", dec_key, sizeof(dec_key), &dec_len));

    // Error case: NULL arguments or small capacity
    assert(!book_weread_decrypt_encrypt_key(NULL, skey, dec_key, sizeof(dec_key), &dec_len));
    assert(!book_weread_decrypt_encrypt_key(b64_ct, NULL, dec_key, sizeof(dec_key), &dec_len));
    assert(!book_weread_decrypt_encrypt_key(b64_ct, skey, dec_key, 10, &dec_len));

    printf("  -> AES-128-CBC encryptkey decryption passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 3: Multi-Secret Candidate Trial
 * --------------------------------------------------------------------------- */
static void test_candidate_secrets_trial(void) {
    printf("[3/10] Testing multi-secret candidate trial...\n");

    const char *b64_ct = "EO0RNDAOdH6dgrwJKqktz2QDOksjyP4WftL/uRXWPDw=";
    const char *expected_key = "0123456789abcdef_secret_key!";
    uint8_t dec_key[64];
    size_t dec_len = 0;

    // Trial with multiple candidates where 3rd is the valid key
    const char *candidates_1[] = {"invalid_token_1", "bad_skey_2", "gSqTestLp", "other_key"};
    assert(book_weread_decrypt_encrypt_key_candidates(b64_ct, candidates_1, 4,
                                                      dec_key, sizeof(dec_key), &dec_len));
    assert(dec_len == strlen(expected_key));
    assert(memcmp(dec_key, expected_key, dec_len) == 0);

    // Trial with first candidate matching
    const char *candidates_2[] = {"gSqTestLp", "fallback_token"};
    memset(dec_key, 0, sizeof(dec_key));
    assert(book_weread_decrypt_encrypt_key_candidates(b64_ct, candidates_2, 2,
                                                      dec_key, sizeof(dec_key), &dec_len));
    assert(dec_len == strlen(expected_key));
    assert(memcmp(dec_key, expected_key, dec_len) == 0);

    // Trial where all candidates fail
    const char *candidates_fail[] = {"bad1", "bad2", "bad3"};
    assert(!book_weread_decrypt_encrypt_key_candidates(b64_ct, candidates_fail, 3,
                                                       dec_key, sizeof(dec_key), &dec_len));

    // Zero count or NULL
    assert(!book_weread_decrypt_encrypt_key_candidates(b64_ct, candidates_1, 0,
                                                       dec_key, sizeof(dec_key), &dec_len));
    assert(!book_weread_decrypt_encrypt_key_candidates(b64_ct, NULL, 4,
                                                       dec_key, sizeof(dec_key), &dec_len));

    printf("  -> Multi-secret candidate trial passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 4: GilbertVernam XOR Stream Cipher
 * --------------------------------------------------------------------------- */
static void test_gilbert_vernam(void) {
    printf("[4/10] Testing GilbertVernam XOR stream cipher...\n");

    const char *plain_msg = "商见曜和蒋白棉查看了912房间。";
    size_t msg_len = strlen(plain_msg);
    const char *key = "0123456789abcdef_secret_key!";
    size_t key_len = strlen(key);

    uint8_t *buf = malloc(msg_len);
    assert(buf != NULL);
    memcpy(buf, plain_msg, msg_len);

    // Encrypt starting at offset 64
    book_weread_gilbert_vernam(buf, msg_len, (const uint8_t *)key, key_len, 64);
    assert(memcmp(buf, plain_msg, msg_len) != 0);

    // Decrypt (involutive) starting at same offset 64
    book_weread_gilbert_vernam(buf, msg_len, (const uint8_t *)key, key_len, 64);
    assert(memcmp(buf, plain_msg, msg_len) == 0);

    // Test with start offset 0 and 1024
    book_weread_gilbert_vernam(buf, msg_len, (const uint8_t *)key, key_len, 0);
    book_weread_gilbert_vernam(buf, msg_len, (const uint8_t *)key, key_len, 0);
    assert(memcmp(buf, plain_msg, msg_len) == 0);

    book_weread_gilbert_vernam(buf, msg_len, (const uint8_t *)key, key_len, 1024);
    book_weread_gilbert_vernam(buf, msg_len, (const uint8_t *)key, key_len, 1024);
    assert(memcmp(buf, plain_msg, msg_len) == 0);

    // Zero length and NULL safety
    book_weread_gilbert_vernam(buf, 0, (const uint8_t *)key, key_len, 0);
    book_weread_gilbert_vernam(NULL, 10, (const uint8_t *)key, key_len, 0);
    book_weread_gilbert_vernam(buf, 10, NULL, 0, 0);

    free(buf);
    printf("  -> GilbertVernam XOR stream cipher passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 5: QDataStream Envelope Unwrapping
 * --------------------------------------------------------------------------- */
static void test_qdatastream_envelope(void) {
    printf("[5/10] Testing QDataStream envelope unwrapping...\n");

    const char *skey = "gSqTestLp";
    const char *b64_ct = "EO0RNDAOdH6dgrwJKqktz2QDOksjyP4WftL/uRXWPDw=";
    const char *chapter_key = "0123456789abcdef_secret_key!";
    const char *plain_html = "<html><body><p>Hello WeRead Chapter</p></body></html>";
    size_t plain_len = strlen(plain_html);

    // 1. Plain pass-through (magic == 0)
    uint8_t plain_buf[128];
    memset(plain_buf, 0, 4); // magic 0
    memcpy(plain_buf + 4, plain_html, plain_len);
    size_t plain_buf_len = 4 + plain_len;
    uint8_t *unwrapped = NULL;
    size_t unwrapped_len = 0;
    assert(book_weread_unwrap_envelope(plain_buf, plain_buf_len, skey, NULL, 0,
                                       &unwrapped, &unwrapped_len));
    assert(unwrapped_len == plain_buf_len);
    assert(memcmp(unwrapped, plain_buf, plain_buf_len) == 0);
    free(unwrapped);

    // 2. Encrypted QDataStream envelope (magic != 0)
    // Structure:
    // int32 magic = 1
    // int32 count = 1
    // int32 skip = 0
    // int32 key_len = strlen(b64_ct)
    // bytes key_b64
    // int64 body_len = plain_len
    // bytes cipher = GilbertVernam(plain_html, chapter_key, start_offset)
    size_t key_len = strlen(b64_ct);
    size_t hdr_before_body = 4 * 4 + key_len;
    size_t start_offset = hdr_before_body + 8;
    size_t total_env_len = start_offset + plain_len;

    uint8_t *enc_env = malloc(total_env_len);
    assert(enc_env != NULL);
    uint8_t *p = enc_env;

    // magic = 1
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1; p += 4;
    // count = 1
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 1; p += 4;
    // skip = 0
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; p += 4;
    // key_len
    p[0] = (uint8_t)(key_len >> 24);
    p[1] = (uint8_t)(key_len >> 16);
    p[2] = (uint8_t)(key_len >> 8);
    p[3] = (uint8_t)(key_len);
    p += 4;
    // key bytes
    memcpy(p, b64_ct, key_len);
    p += key_len;
    // body_len (int64)
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0;
    p[4] = (uint8_t)(plain_len >> 24);
    p[5] = (uint8_t)(plain_len >> 16);
    p[6] = (uint8_t)(plain_len >> 8);
    p[7] = (uint8_t)(plain_len);
    p += 8;

    // Encrypt body with GilbertVernam
    memcpy(p, plain_html, plain_len);
    book_weread_gilbert_vernam(p, plain_len, (const uint8_t *)chapter_key, strlen(chapter_key), start_offset);

    // Unwrap envelope
    unwrapped = NULL;
    unwrapped_len = 0;
    assert(book_weread_unwrap_envelope(enc_env, total_env_len, skey, NULL, 0,
                                       &unwrapped, &unwrapped_len));
    assert(unwrapped_len == plain_len);
    assert(memcmp(unwrapped, plain_html, plain_len) == 0);
    free(unwrapped);
    free(enc_env);

    // Truncated envelope check
    assert(!book_weread_unwrap_envelope(plain_buf, 3, skey, NULL, 0, &unwrapped, &unwrapped_len));

    printf("  -> QDataStream envelope unwrapping passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 6: ZipCrypto ZIP Archive Unpacking
 * --------------------------------------------------------------------------- */
typedef struct {
    char names[8][64];
    uint8_t *data[8];
    size_t sizes[8];
    size_t count;
    bool abort_on_match;
    const char *abort_name;
} test_zip_collector_t;

typedef struct {
    test_zip_collector_t *col;
    uint8_t *cur;
    size_t cur_cap;
    size_t cur_len;
    char cur_name[64];
} test_zip_chunk_t;

static bool test_zip_chunk_callback(void *ctx, const char *name, const uint8_t *bytes, size_t n, size_t offset, size_t total) {
    test_zip_chunk_t *st = (test_zip_chunk_t *)ctx;
    test_zip_collector_t *c = st->col;
    if (c->abort_on_match && c->abort_name && strcmp(name, c->abort_name) == 0) return false;
    if (offset == 0) {
        free(st->cur);
        st->cur = total ? malloc(total) : NULL;
        st->cur_cap = total;
        st->cur_len = 0;
        snprintf(st->cur_name, sizeof(st->cur_name), "%s", name);
    }
    assert(offset == st->cur_len && offset + n <= st->cur_cap);
    if (n) {
        memcpy(st->cur + offset, bytes, n);
        st->cur_len += n;
    }
    if (offset + n == total) {
        if (c->count < 8) {
            snprintf(c->names[c->count], sizeof(c->names[c->count]), "%s", st->cur_name);
            c->data[c->count] = st->cur;
            c->sizes[c->count] = total;
            c->count++;
            st->cur = NULL;
        } else {
            free(st->cur);
            st->cur = NULL;
        }
    }
    return true;
}

static bool test_zip_callback(void *ctx, const char *name, const uint8_t *data, size_t size) {
    test_zip_collector_t *c = (test_zip_collector_t *)ctx;
    if (c->abort_on_match && c->abort_name && strcmp(name, c->abort_name) == 0) {
        return false;
    }
    if (c->count < 8) {
        snprintf(c->names[c->count], sizeof(c->names[c->count]), "%s", name);
        c->data[c->count] = malloc(size > 0 ? size : 1);
        if (size > 0) memcpy(c->data[c->count], data, size);
        c->sizes[c->count] = size;
        c->count++;
    }
    return true;
}

static void test_zip_collector_free(test_zip_collector_t *c) {
    for (size_t i = 0; i < c->count; ++i) {
        free(c->data[i]);
        c->data[i] = NULL;
    }
    c->count = 0;
}

// ZipCrypto encryption helper for building test ZIPs
static uint32_t helper_crc32_byte(uint32_t crc, uint8_t b) {
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    return crc;
}

static void helper_zipcrypto_encrypt(const uint8_t *plain, size_t plain_len,
                                     const uint8_t *pwd, size_t pwd_len,
                                     uint32_t crc, uint8_t *out) {
    uint32_t k0 = UINT32_C(0x12345678);
    uint32_t k1 = UINT32_C(0x23456789);
    uint32_t k2 = UINT32_C(0x34567890);

    for (size_t i = 0; i < pwd_len; ++i) {
        k0 = helper_crc32_byte(k0, pwd[i]);
        k1 = (k1 + (k0 & 0xffu)) * UINT32_C(134775813) + 1u;
        k2 = helper_crc32_byte(k2, (uint8_t)(k1 >> 24));
    }

    uint8_t hdr[12] = {0};
    hdr[10] = (uint8_t)((crc >> 16) & 0xff);
    hdr[11] = (uint8_t)((crc >> 24) & 0xff);

    for (int i = 0; i < 12; ++i) {
        uint16_t t = (uint16_t)((k2 & 0xffffu) | 2u);
        uint8_t key_byte = (uint8_t)(((uint32_t)t * (uint32_t)(t ^ 1u)) >> 8);
        out[i] = hdr[i] ^ key_byte;
        k0 = helper_crc32_byte(k0, hdr[i]);
        k1 = (k1 + (k0 & 0xffu)) * UINT32_C(134775813) + 1u;
        k2 = helper_crc32_byte(k2, (uint8_t)(k1 >> 24));
    }

    for (size_t i = 0; i < plain_len; ++i) {
        uint16_t t = (uint16_t)((k2 & 0xffffu) | 2u);
        uint8_t key_byte = (uint8_t)(((uint32_t)t * (uint32_t)(t ^ 1u)) >> 8);
        out[12 + i] = plain[i] ^ key_byte;
        k0 = helper_crc32_byte(k0, plain[i]);
        k1 = (k1 + (k0 & 0xffu)) * UINT32_C(134775813) + 1u;
        k2 = helper_crc32_byte(k2, (uint8_t)(k1 >> 24));
    }
}

// Build a valid 2-entry ZIP (entry 1: stored, entry 2: deflated)
static uint8_t *build_test_zip(const char *name1, const uint8_t *data1, size_t len1,
                               const char *name2, const uint8_t *data2, size_t len2,
                               const uint8_t *password, size_t password_len,
                               size_t *out_zip_len) {
    // Compress data2 with raw deflate
    uint8_t deflated[4096];
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = (Bytef *)data2;
    zs.avail_in = (uInt)len2;
    zs.next_out = (Bytef *)deflated;
    zs.avail_out = sizeof(deflated);
    assert(deflate(&zs, Z_FINISH) == Z_STREAM_END);
    size_t deflated_len = zs.total_out;
    deflateEnd(&zs);

    uint32_t crc1 = (uint32_t)crc32(0L, data1, (uInt)len1);
    uint32_t crc2 = (uint32_t)crc32(0L, data2, (uInt)len2);

    bool encrypt = (password && password_len > 0);
    uint16_t flags = encrypt ? 0x0001 : 0x0000;

    size_t enc_len1 = encrypt ? len1 + 12 : len1;
    size_t enc_len2 = encrypt ? deflated_len + 12 : deflated_len;

    uint8_t *body1 = malloc(enc_len1);
    uint8_t *body2 = malloc(enc_len2);
    assert(body1 && body2);

    if (encrypt) {
        helper_zipcrypto_encrypt(data1, len1, password, password_len, crc1, body1);
        helper_zipcrypto_encrypt(deflated, deflated_len, password, password_len, crc2, body2);
    } else {
        memcpy(body1, data1, len1);
        memcpy(body2, deflated, deflated_len);
    }

    uint8_t *zip = malloc(16384);
    assert(zip != NULL);
    uint8_t *p = zip;

    size_t nlen1 = strlen(name1);
    size_t nlen2 = strlen(name2);

    // Entry 1 Local Header (offset = 0)
    size_t off1 = 0;
    p[0] = 0x50; p[1] = 0x4b; p[2] = 0x03; p[3] = 0x04; p += 4;
    p[0] = 20; p[1] = 0; p += 2; // version
    p[0] = (uint8_t)(flags & 0xff); p[1] = (uint8_t)(flags >> 8); p += 2;
    p[0] = 0; p[1] = 0; p += 2; // method = 0 (stored)
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; p += 4; // time/date
    memcpy(p, &crc1, 4); p += 4;
    uint32_t comp1 = (uint32_t)enc_len1;
    uint32_t uncomp1 = (uint32_t)len1;
    memcpy(p, &comp1, 4); p += 4;
    memcpy(p, &uncomp1, 4); p += 4;
    uint16_t nl1 = (uint16_t)nlen1;
    uint16_t extra1 = 0;
    memcpy(p, &nl1, 2); p += 2;
    memcpy(p, &extra1, 2); p += 2;
    memcpy(p, name1, nlen1); p += nlen1;
    memcpy(p, body1, enc_len1); p += enc_len1;

    // Entry 2 Local Header
    size_t off2 = (size_t)(p - zip);
    p[0] = 0x50; p[1] = 0x4b; p[2] = 0x03; p[3] = 0x04; p += 4;
    p[0] = 20; p[1] = 0; p += 2;
    p[0] = (uint8_t)(flags & 0xff); p[1] = (uint8_t)(flags >> 8); p += 2;
    p[0] = 8; p[1] = 0; p += 2; // method = 8 (deflate)
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; p += 4;
    memcpy(p, &crc2, 4); p += 4;
    uint32_t comp2 = (uint32_t)enc_len2;
    uint32_t uncomp2 = (uint32_t)len2;
    memcpy(p, &comp2, 4); p += 4;
    memcpy(p, &uncomp2, 4); p += 4;
    uint16_t nl2 = (uint16_t)nlen2;
    uint16_t extra2 = 0;
    memcpy(p, &nl2, 2); p += 2;
    memcpy(p, &extra2, 2); p += 2;
    memcpy(p, name2, nlen2); p += nlen2;
    memcpy(p, body2, enc_len2); p += enc_len2;

    // Central Directory
    size_t cd_offset = (size_t)(p - zip);

    // CD Entry 1
    p[0] = 0x50; p[1] = 0x4b; p[2] = 0x01; p[3] = 0x02; p += 4;
    p[0] = 20; p[1] = 0; p += 2; // version made by
    p[0] = 20; p[1] = 0; p += 2; // version needed
    p[0] = (uint8_t)(flags & 0xff); p[1] = (uint8_t)(flags >> 8); p += 2;
    p[0] = 0; p[1] = 0; p += 2; // method 0
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; p += 4;
    memcpy(p, &crc1, 4); p += 4;
    memcpy(p, &comp1, 4); p += 4;
    memcpy(p, &uncomp1, 4); p += 4;
    memcpy(p, &nl1, 2); p += 2;
    uint16_t zero16 = 0;
    memcpy(p, &zero16, 2); p += 2; // extra len
    memcpy(p, &zero16, 2); p += 2; // comment len
    memcpy(p, &zero16, 2); p += 2; // disk
    memcpy(p, &zero16, 2); p += 2; // int attr
    uint32_t zero32 = 0;
    memcpy(p, &zero32, 4); p += 4; // ext attr
    uint32_t l_off1 = (uint32_t)off1;
    memcpy(p, &l_off1, 4); p += 4;
    memcpy(p, name1, nlen1); p += nlen1;

    // CD Entry 2
    p[0] = 0x50; p[1] = 0x4b; p[2] = 0x01; p[3] = 0x02; p += 4;
    p[0] = 20; p[1] = 0; p += 2;
    p[0] = 20; p[1] = 0; p += 2;
    p[0] = (uint8_t)(flags & 0xff); p[1] = (uint8_t)(flags >> 8); p += 2;
    p[0] = 8; p[1] = 0; p += 2; // method 8
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = 0; p += 4;
    memcpy(p, &crc2, 4); p += 4;
    memcpy(p, &comp2, 4); p += 4;
    memcpy(p, &uncomp2, 4); p += 4;
    memcpy(p, &nl2, 2); p += 2;
    memcpy(p, &zero16, 2); p += 2;
    memcpy(p, &zero16, 2); p += 2;
    memcpy(p, &zero16, 2); p += 2;
    memcpy(p, &zero16, 2); p += 2;
    memcpy(p, &zero32, 4); p += 4;
    uint32_t l_off2 = (uint32_t)off2;
    memcpy(p, &l_off2, 4); p += 4;
    memcpy(p, name2, nlen2); p += nlen2;

    size_t cd_size = (size_t)(p - zip) - cd_offset;

    // EOCD
    p[0] = 0x50; p[1] = 0x4b; p[2] = 0x05; p[3] = 0x06; p += 4;
    p[0] = 0; p[1] = 0; p += 2; // disk
    p[0] = 0; p[1] = 0; p += 2; // cd start disk
    uint16_t entries = 2;
    memcpy(p, &entries, 2); p += 2;
    memcpy(p, &entries, 2); p += 2;
    uint32_t cds = (uint32_t)cd_size;
    uint32_t cdo = (uint32_t)cd_offset;
    memcpy(p, &cds, 4); p += 4;
    memcpy(p, &cdo, 4); p += 4;
    memcpy(p, &zero16, 2); p += 2; // comment len

    *out_zip_len = (size_t)(p - zip);
    free(body1);
    free(body2);
    return zip;
}

static void test_zipcrypto_unpacking(void) {
    printf("[6/10] Testing ZipCrypto ZIP unpacking (stored + deflated)...\n");

    const char *pwd = "secret_password_123";
    size_t pwd_len = strlen(pwd);

    const char *plain_info = "{\"bookId\":\"3300001297\",\"synckey\":100}";
    const char *plain_html = "<html><body><h1>Chapter One</h1><p>Text content for reader testing.</p></body></html>";

    size_t zip_len = 0;
    uint8_t *enc_zip = build_test_zip("info.txt", (const uint8_t *)plain_info, strlen(plain_info),
                                      "Text/Chapter1.xhtml", (const uint8_t *)plain_html, strlen(plain_html),
                                      (const uint8_t *)pwd, pwd_len, &zip_len);
    assert(enc_zip != NULL);

    // 1. Successful unpack with password
    test_zip_collector_t col = {0};
    esp_err_t err = book_weread_unpack_zip(enc_zip, zip_len, (const uint8_t *)pwd, pwd_len,
                                           test_zip_callback, &col);
    assert(err == ESP_OK);
    assert(col.count == 2);
    assert(strcmp(col.names[0], "info.txt") == 0);
    assert(col.sizes[0] == strlen(plain_info));
    assert(memcmp(col.data[0], plain_info, col.sizes[0]) == 0);
    assert(strcmp(col.names[1], "Text/Chapter1.xhtml") == 0);
    assert(col.sizes[1] == strlen(plain_html));
    assert(memcmp(col.data[1], plain_html, col.sizes[1]) == 0);
    test_zip_collector_free(&col);

    // 2. Unpack with wrong password
    err = book_weread_unpack_zip(enc_zip, zip_len, (const uint8_t *)"wrong_pwd", 9,
                                 test_zip_callback, &col);
    assert(err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_CRC);
    test_zip_collector_free(&col);

    // 2b. ZipCrypto check-byte mismatch (e.g. data-descriptor flag 0x08 with CRC MSB instead of mod_time MSB)
    // must still succeed when deflate + full 32-bit CRC-32 match, while wrong password still fails.
    {
        assert(((uint32_t)crc32(0L, (const Bytef *)plain_html, (uInt)strlen(plain_html)) >> 24) != 0);
        uint8_t *dd_zip = malloc(zip_len);
        assert(dd_zip != NULL);
        memcpy(dd_zip, enc_zip, zip_len);
        size_t eocd = zip_len - 22u;
        uint16_t cnt = (uint16_t)dd_zip[eocd + 10] | ((uint16_t)dd_zip[eocd + 11] << 8);
        size_t cd_off = (uint32_t)dd_zip[eocd + 16] | ((uint32_t)dd_zip[eocd + 17] << 8) |
                        ((uint32_t)dd_zip[eocd + 18] << 16) | ((uint32_t)dd_zip[eocd + 19] << 24);
        size_t p_cd = cd_off;
        for (uint16_t idx = 0; idx < cnt; ++idx) {
            dd_zip[p_cd + 8] |= 0x08;
            size_t l_off = (uint32_t)dd_zip[p_cd + 42] | ((uint32_t)dd_zip[p_cd + 43] << 8) |
                           ((uint32_t)dd_zip[p_cd + 44] << 16) | ((uint32_t)dd_zip[p_cd + 45] << 24);
            dd_zip[l_off + 6] |= 0x08;
            uint16_t nlen = (uint16_t)dd_zip[p_cd + 28] | ((uint16_t)dd_zip[p_cd + 29] << 8);
            uint16_t elen = (uint16_t)dd_zip[p_cd + 30] | ((uint16_t)dd_zip[p_cd + 31] << 8);
            uint16_t clen = (uint16_t)dd_zip[p_cd + 32] | ((uint16_t)dd_zip[p_cd + 33] << 8);
            p_cd += 46u + nlen + elen + clen;
        }
        err = book_weread_unpack_zip(dd_zip, zip_len, (const uint8_t *)pwd, pwd_len, test_zip_callback, &col);
        assert(err == ESP_OK && col.count == 2);
        assert(col.sizes[1] == strlen(plain_html) && memcmp(col.data[1], plain_html, col.sizes[1]) == 0);
        test_zip_collector_free(&col);

        FILE *zf = tmpfile();
        assert(zf && fwrite(dd_zip, 1, zip_len, zf) == zip_len && fflush(zf) == 0);
        rewind(zf);
        test_zip_chunk_t chunk_ctx = { .col = &col };
        err = book_weread_unpack_zip_file(zf, (const uint8_t *)pwd, pwd_len, test_zip_chunk_callback, &chunk_ctx);
        free(chunk_ctx.cur);
        assert(err == ESP_OK && col.count == 2);
        assert(col.sizes[0] == strlen(plain_info) && memcmp(col.data[0], plain_info, col.sizes[0]) == 0);
        assert(col.sizes[1] == strlen(plain_html) && memcmp(col.data[1], plain_html, col.sizes[1]) == 0);
        test_zip_collector_free(&col);

        rewind(zf);
        memset(&chunk_ctx, 0, sizeof(chunk_ctx));
        chunk_ctx.col = &col;
        err = book_weread_unpack_zip_file(zf, (const uint8_t *)"wrong_pwd", 9, test_zip_chunk_callback, &chunk_ctx);
        free(chunk_ctx.cur);
        assert(err == ESP_ERR_INVALID_STATE || err == ESP_ERR_INVALID_CRC);
        test_zip_collector_free(&col);
        fclose(zf);
        free(dd_zip);
    }

    // 3. Unpack unencrypted ZIP (password = NULL)
    size_t plain_zip_len = 0;
    uint8_t *plain_zip = build_test_zip("info.txt", (const uint8_t *)plain_info, strlen(plain_info),
                                        "Styles/main.css", (const uint8_t *)"body { font-size: 16px; }", 26,
                                        NULL, 0, &plain_zip_len);
    assert(plain_zip != NULL);
    err = book_weread_unpack_zip(plain_zip, plain_zip_len, NULL, 0, test_zip_callback, &col);
    assert(err == ESP_OK);
    assert(col.count == 2);
    assert(strcmp(col.names[0], "info.txt") == 0);
    assert(strcmp(col.names[1], "Styles/main.css") == 0);
    test_zip_collector_free(&col);

    // 4. Callback abort
    col.abort_on_match = true;
    col.abort_name = "Styles/main.css";
    err = book_weread_unpack_zip(plain_zip, plain_zip_len, NULL, 0, test_zip_callback, &col);
    assert(err == ESP_ERR_INVALID_STATE);
    test_zip_collector_free(&col);

    // 5. Path traversal protection: entry name containing "../../etc/passwd"
    size_t bad_zip_len = 0;
    uint8_t *bad_zip = build_test_zip("../../etc/passwd", (const uint8_t *)"root", 4,
                                      "valid.txt", (const uint8_t *)"ok", 2,
                                      NULL, 0, &bad_zip_len);
    assert(bad_zip != NULL);
    err = book_weread_unpack_zip(bad_zip, bad_zip_len, NULL, 0, test_zip_callback, &col);
    assert(err == ESP_ERR_INVALID_RESPONSE);
    test_zip_collector_free(&col);

    // 6. Integer overflow protection in Central Directory local_off bounds check
    {
        size_t craft_len = 92;
        uint8_t *craft_zip = calloc(1, craft_len);
        assert(craft_zip != NULL);
        size_t cd_off = 20;
        // Central directory file header
        craft_zip[cd_off + 0] = 0x50; craft_zip[cd_off + 1] = 0x4b;
        craft_zip[cd_off + 2] = 0x01; craft_zip[cd_off + 3] = 0x02;
        craft_zip[cd_off + 28] = 4; // file name length = 4
        // local_off = 0xFFFFFFF0 (wrapping local_off + 30u to 14u < cd_offset)
        craft_zip[cd_off + 42] = 0xF0; craft_zip[cd_off + 43] = 0xFF;
        craft_zip[cd_off + 44] = 0xFF; craft_zip[cd_off + 45] = 0xFF;
        memcpy(craft_zip + cd_off + 46, "test", 4);
        // EOCD record
        size_t eocd_off = 70;
        craft_zip[eocd_off + 0] = 0x50; craft_zip[eocd_off + 1] = 0x4b;
        craft_zip[eocd_off + 2] = 0x05; craft_zip[eocd_off + 3] = 0x06;
        craft_zip[eocd_off + 8] = 1; craft_zip[eocd_off + 10] = 1; // 1 entry
        craft_zip[eocd_off + 12] = 50; // CD size = 50
        craft_zip[eocd_off + 16] = (uint8_t)cd_off; // CD offset = 20
        err = book_weread_unpack_zip(craft_zip, craft_len, NULL, 0, test_zip_callback, &col);
        assert(err == ESP_ERR_INVALID_RESPONSE);

        // Also test local_off = UINT32_MAX
        craft_zip[cd_off + 42] = 0xFF; craft_zip[cd_off + 43] = 0xFF;
        craft_zip[cd_off + 44] = 0xFF; craft_zip[cd_off + 45] = 0xFF;
        err = book_weread_unpack_zip(craft_zip, craft_len, NULL, 0, test_zip_callback, &col);
        assert(err == ESP_ERR_INVALID_RESPONSE);

        free(craft_zip);
    }

    free(enc_zip);
    free(plain_zip);
    free(bad_zip);
    printf("  -> ZipCrypto ZIP unpacking passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 7: USTAR TAR Archive Unpacking
 * --------------------------------------------------------------------------- */
static uint8_t *build_test_tar(const char *name1, const uint8_t *data1, size_t len1,
                               const char *name2, const uint8_t *data2, size_t len2,
                               size_t *out_tar_len) {
    size_t pad1 = (len1 + 511u) & ~511u;
    size_t pad2 = (len2 + 511u) & ~511u;
    size_t total = 512 + pad1 + 512 + pad2 + 1024; // 2 files + 2 zero blocks at end

    uint8_t *tar = calloc(1, total);
    assert(tar != NULL);

    uint8_t *p = tar;

    // File 1 header
    snprintf((char *)p, 100, "%s", name1);
    snprintf((char *)p + 100, 8, "0000644");
    snprintf((char *)p + 108, 8, "0000000");
    snprintf((char *)p + 116, 8, "0000000");
    snprintf((char *)p + 124, 12, "%011o", (unsigned)len1);
    snprintf((char *)p + 136, 12, "%011o", 0);
    p[156] = '0'; // normal file
    memcpy(p + 257, "ustar\0", 6);
    memcpy(p + 263, "00", 2);

    // Compute checksum (header checksum field treated as 8 spaces)
    memset(p + 148, ' ', 8);
    uint32_t sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += p[i];
    snprintf((char *)p + 148, 8, "%06o", sum);
    p[154] = '\0';
    p[155] = ' ';

    p += 512;
    memcpy(p, data1, len1);
    p += pad1;

    // File 2 header
    snprintf((char *)p, 100, "%s", name2);
    snprintf((char *)p + 100, 8, "0000644");
    snprintf((char *)p + 108, 8, "0000000");
    snprintf((char *)p + 116, 8, "0000000");
    snprintf((char *)p + 124, 12, "%011o", (unsigned)len2);
    snprintf((char *)p + 136, 12, "%011o", 0);
    p[156] = '0';
    memcpy(p + 257, "ustar\0", 6);
    memcpy(p + 263, "00", 2);

    memset(p + 148, ' ', 8);
    sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += p[i];
    snprintf((char *)p + 148, 8, "%06o", sum);
    p[154] = '\0';
    p[155] = ' ';

    p += 512;
    memcpy(p, data2, len2);
    p += pad2;

    *out_tar_len = total;
    return tar;
}

static void test_tar_unpacking(void) {
    printf("[7/10] Testing USTAR TAR unpacking...\n");

    const char *f1_name = "info.txt";
    const char *f1_content = "{\"bookId\":\"3300001297\"}";
    const char *f2_name = "3300001297_1_o";
    const char *f2_content = "This is a plain TXT chapter content stored in a TAR archive.";

    size_t tar_len = 0;
    uint8_t *tar = build_test_tar(f1_name, (const uint8_t *)f1_content, strlen(f1_content),
                                  f2_name, (const uint8_t *)f2_content, strlen(f2_content),
                                  &tar_len);
    assert(tar != NULL);

    test_zip_collector_t col = {0};
    esp_err_t err = book_weread_unpack_tar(tar, tar_len, test_zip_callback, &col);
    assert(err == ESP_OK);
    assert(col.count == 2);
    assert(strcmp(col.names[0], f1_name) == 0);
    assert(col.sizes[0] == strlen(f1_content));
    assert(memcmp(col.data[0], f1_content, col.sizes[0]) == 0);
    assert(strcmp(col.names[1], f2_name) == 0);
    assert(col.sizes[1] == strlen(f2_content));
    assert(memcmp(col.data[1], f2_content, col.sizes[1]) == 0);
    test_zip_collector_free(&col);

    // Corrupted checksum test
    tar[148] = '9'; // Corrupt checksum
    err = book_weread_unpack_tar(tar, tar_len, test_zip_callback, &col);
    assert(err == ESP_ERR_INVALID_RESPONSE);
    test_zip_collector_free(&col);

    // Callback abort test
    tar[148] = '0'; // Restore roughly
    free(tar);

    // Rebuild clean TAR for abort test
    tar = build_test_tar(f1_name, (const uint8_t *)f1_content, strlen(f1_content),
                         f2_name, (const uint8_t *)f2_content, strlen(f2_content),
                         &tar_len);
    col.abort_on_match = true;
    col.abort_name = f2_name;
    err = book_weread_unpack_tar(tar, tar_len, test_zip_callback, &col);
    assert(err == ESP_ERR_INVALID_STATE);
    test_zip_collector_free(&col);

    free(tar);
    printf("  -> USTAR TAR unpacking passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 8: Web ID Encoding
 * --------------------------------------------------------------------------- */
static void test_web_id_encoding(void) {
    printf("[8/10] Testing WeRead Web ID obfuscation...\n");

    // Vector 1: Numeric ID "3300001297" -> "fe1327f0813ab6701g01728b"
    char out[256];
    assert(book_weread_encode_web_id("3300001297", out, sizeof(out)));
    assert(strcmp(out, "fe1327f0813ab6701g01728b") == 0);

    // Vector 2: Numeric ID "3300239533" -> "64532b80813abc411g013acf"
    assert(book_weread_encode_web_id("3300239533", out, sizeof(out)));
    assert(strcmp(out, "64532b80813abc411g013acf") == 0);

    // Vector 3: Non-numeric alphanumeric string "CB_FPU7gW7fb4Qt6d96ce"
    assert(book_weread_encode_web_id("CB_FPU7gW7fb4Qt6d96ce", out, sizeof(out)));
    assert(strlen(out) >= 20);
    // Non-numeric encoding starts with "3b6" + length prefix + ...
    assert(out[3] == '4' && out[4] == '2');

    // Vector 4: Timestamp encoding "1700000000" -> "24932c107a21fe80g010f72"
    assert(book_weread_encode_web_id("1700000000", out, sizeof(out)));
    assert(strcmp(out, "24932c107a21fe80g010f72") == 0);

    // Vector 5: Empty string "" -> "d41427e00d41d8cd98f055d" (matches Python reference)
    assert(book_weread_encode_web_id("", out, sizeof(out)));
    assert(strcmp(out, "d41427e00d41d8cd98f055d") == 0);

    // Error handling
    assert(!book_weread_encode_web_id(NULL, out, sizeof(out)));
    assert(!book_weread_encode_web_id("3300001297", NULL, sizeof(out)));
    assert(!book_weread_encode_web_id("3300001297", out, 10)); // Buffer too small

    printf("  -> WeRead Web ID obfuscation passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 9: Web App ID & Signed Read Body Generation
 * --------------------------------------------------------------------------- */
static void test_web_app_id_and_signed_body(void) {
    printf("[9/10] Testing Web App ID and signed read body generation...\n");

    const char *ua = "WeRead/1.0.0 WRBrand/remarkable wr_eink";
    char app_id[64];
    assert(book_weread_generate_web_app_id(ua, app_id, sizeof(app_id)));
    assert(strcmp(app_id, "wb287h96730816") == 0);

    // Generate signed read body for known test vector:
    // book_id: "3300239533"
    // chapter_uid: 10
    // chapter_idx: 10
    // chapter_offset: 42
    // progress: 12
    // summary: 30 'x's (trimmed to 20 'x's)
    // timestamp: 1700000000
    char body[1024];
    assert(book_weread_build_web_read_body("3300239533", 10, 10, 42, 12,
                                           "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
                                           1700000000LL, ua, body, sizeof(body)));

    const char *expected_json =
        "{\"appId\":\"wb287h96730816\",\"b\":\"64532b80813abc411g013acf\","
        "\"c\":\"d3d322001ad3d9446802347\",\"ci\":10,\"co\":42,"
        "\"sm\":\"xxxxxxxxxxxxxxxxxxxx\",\"pr\":12,\"ct\":1700000000,"
        "\"ps\":\"24932c107a21fe80g010f72\",\"pc\":\"24932c107a21fe80g010f72\","
        "\"s\":\"9fefef38\"}";

    assert(strcmp(body, expected_json) == 0);

    // Test with JSON-escaped summary characters and UTF-8 within 20 chars
    assert(book_weread_build_web_read_body("3300001297", 1, 0, 0, 5,
                                           "Ch1: \"T\" \\ \n ok",
                                           1700000000LL, ua, body, sizeof(body)));
    assert(strstr(body, "\\\"T\\\"") != NULL);
    assert(strstr(body, "\\\\") != NULL);
    assert(strstr(body, "\\n") != NULL);

    // Error handling
    assert(!book_weread_build_web_read_body(NULL, 1, 0, 0, 5, "s", 1700000000LL, ua, body, sizeof(body)));
    assert(!book_weread_build_web_read_body("3300001297", 1, 0, 0, 5, "s", 1700000000LL, NULL, body, sizeof(body)));
    assert(!book_weread_build_web_read_body("3300001297", 1, 0, 0, 5, "s", 1700000000LL, ua, body, 50));

    printf("  -> Web App ID and signed read body generation passed.\n");
}

/* ---------------------------------------------------------------------------
 * Test 10: URI Helpers in main/book/book_weread_uri.h
 * --------------------------------------------------------------------------- */
static void test_uri_helpers(void) {
    printf("[10/10] Testing virtual URI helpers in book_weread_uri.h...\n");

    // 1. Valid WeRead URIs
    assert(book_path_is_weread("weread://3300001297"));
    assert(book_path_is_weread("weread://CB_FPU7gW7fb4Qt6d96ce"));
    assert(book_path_is_weread("weread://book_123_abc"));

    // 2. Invalid WeRead URIs
    assert(!book_path_is_weread(NULL));
    assert(!book_path_is_weread(""));
    assert(!book_path_is_weread("weread://")); // empty id
    assert(!book_path_is_weread("weread://book/123")); // invalid slash
    assert(!book_path_is_weread("weread://book?param=1")); // invalid query
    assert(!book_path_is_weread("http://weread.qq.com"));
    assert(!book_path_is_weread("/sdcard/books/test.epub"));

    // 3. Supported target checking (files or weread://)
    assert(book_path_is_supported_target("weread://3300001297"));
    assert(book_path_is_supported_target("/sdcard/books/sample.epub"));
    assert(book_path_is_supported_target("/sdcard/books/sample.txt"));
    assert(book_path_is_supported_target("/sdcard/books/sample.EPUB"));
    assert(book_path_is_supported_target("/sdcard/books/sample.TXT"));

    // Unsupported targets
    assert(!book_path_is_supported_target("/sdcard/books/sample.pdf"));
    assert(!book_path_is_supported_target("/sdcard/books/sample.mobi"));
    assert(!book_path_is_supported_target("/sdcard/books/sample"));
    assert(!book_path_is_supported_target(""));
    assert(!book_path_is_supported_target(NULL));

    // CRITICAL: Ensure book_file_path_supported() is NOT polluted by weread://
    assert(!book_file_path_supported("weread://3300001297"));
    assert(!book_file_path_supported("weread://CB_FPU7gW7fb4Qt6d96ce"));

    printf("  -> Virtual URI helpers passed.\n");
}

/* ---------------------------------------------------------------------------
 * Main Test Runner
 * --------------------------------------------------------------------------- */
int main(void) {
    printf("========================================================\n");
    printf(" Running WeRead Milestone 1 Crypto & Infrastructure Tests\n");
    printf("========================================================\n");

    test_base64();
    test_aes128_cbc_encryptkey();
    test_candidate_secrets_trial();
    test_gilbert_vernam();
    test_qdatastream_envelope();
    test_zipcrypto_unpacking();
    test_tar_unpacking();
    test_web_id_encoding();
    test_web_app_id_and_signed_body();
    test_uri_helpers();

    printf("========================================================\n");
    printf(" ALL 10 TESTS PASSED SUCCESSFULLY! (Zero errors)\n");
    printf("========================================================\n");
    return 0;
}
