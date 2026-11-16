/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 *
 * 中文：微信读书传输加解密、ZipCrypto/TAR 解包与 Web 进度签名实现。
 * English: WeRead transport decryption, ZipCrypto/TAR unpacking, and Web progress signing.
 */
// 网络和缓存按体积优化，保留双板原应用分区。/ Optimize network/cache code for size within the original app slots.
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "book_weread_crypto.h"

#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

#if defined(ESP_PLATFORM) || defined(WEREAD_TEST_MEMORY)
#include "esp_heap_caps.h"
#define WR_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define WR_CALLOC(n, s) heap_caps_calloc((n), (s), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define WR_ALLOC(n) malloc(n)
#define WR_CALLOC(n, s) calloc((n), (s))
#endif

#define WR_ARCHIVE_ENTRY_MAX (8u * 1024u * 1024u)
#define WR_ZIP_ENTRIES_MAX 2048u

static voidpf wr_zalloc(voidpf opaque,uInt items,uInt size) {
    (void)opaque;if(size&&items>SIZE_MAX/size)return NULL;
    return WR_CALLOC(items,size);
}
static void wr_zfree(voidpf opaque,voidpf pointer){(void)opaque;free(pointer);}

static uint16_t le16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32(p + 4);
}

/* ---- AES-128-CBC 解密 / AES-128-CBC Decryption ---------------------------- */

static const uint8_t kAesSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16,
};

static const uint8_t kAesInvSbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d,
};

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    uint8_t res = 0;
    for (int i = 0; i < 8; ++i) {
        if (b & 1u) res ^= a;
        a = (uint8_t)((a << 1) ^ ((a & 0x80u) ? 0x1bu : 0u));
        b >>= 1;
    }
    return res;
}

static void aes128_expand_key(const uint8_t key[16], uint8_t rk[11][16]) {
    static const uint8_t rcon[10] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36};
    uint8_t words[44][4];
    for (int i = 0; i < 4; ++i) memcpy(words[i], key + i * 4, 4);
    for (int i = 4; i < 44; ++i) {
        uint8_t temp[4];
        memcpy(temp, words[i - 1], 4);
        if (i % 4 == 0) {
            uint8_t t0 = temp[0];
            temp[0] = kAesSbox[temp[1]] ^ rcon[i / 4 - 1];
            temp[1] = kAesSbox[temp[2]];
            temp[2] = kAesSbox[temp[3]];
            temp[3] = kAesSbox[t0];
        }
        for (int j = 0; j < 4; ++j) words[i][j] = words[i - 4][j] ^ temp[j];
    }
    for (int r = 0; r < 11; ++r)
        for (int c = 0; c < 4; ++c)
            memcpy(rk[r] + c * 4, words[r * 4 + c], 4);
}

static void aes128_decrypt_block(const uint8_t in[16], uint8_t out[16], const uint8_t rk[11][16]) {
    uint8_t s[4][4];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            s[r][c] = in[c * 4 + r] ^ rk[10][c * 4 + r];

    for (int rnd = 9; rnd >= 0; --rnd) {
        uint8_t t;
        t = s[1][3]; s[1][3] = s[1][2]; s[1][2] = s[1][1]; s[1][1] = s[1][0]; s[1][0] = t;
        t = s[2][0]; s[2][0] = s[2][2]; s[2][2] = t;
        t = s[2][1]; s[2][1] = s[2][3]; s[2][3] = t;
        t = s[3][0]; s[3][0] = s[3][1]; s[3][1] = s[3][2]; s[3][2] = s[3][3]; s[3][3] = t;

        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                s[r][c] = kAesInvSbox[s[r][c]] ^ rk[rnd][c * 4 + r];

        if (rnd > 0) {
            for (int c = 0; c < 4; ++c) {
                uint8_t s0 = s[0][c], s1 = s[1][c], s2 = s[2][c], s3 = s[3][c];
                s[0][c] = gf_mul(0x0e, s0) ^ gf_mul(0x0b, s1) ^ gf_mul(0x0d, s2) ^ gf_mul(0x09, s3);
                s[1][c] = gf_mul(0x09, s0) ^ gf_mul(0x0e, s1) ^ gf_mul(0x0b, s2) ^ gf_mul(0x0d, s3);
                s[2][c] = gf_mul(0x0d, s0) ^ gf_mul(0x09, s1) ^ gf_mul(0x0e, s2) ^ gf_mul(0x0b, s3);
                s[3][c] = gf_mul(0x0b, s0) ^ gf_mul(0x0d, s1) ^ gf_mul(0x09, s2) ^ gf_mul(0x0e, s3);
            }
        }
    }
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            out[c * 4 + r] = s[r][c];
}

static bool aes128_cbc_decrypt_pkcs7(const uint8_t key[16], const uint8_t iv[16],
                                     const uint8_t *ct, size_t ct_len,
                                     uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!ct || ct_len == 0 || (ct_len % 16u) != 0 || !out || !out_len) return false;
    uint8_t rk[11][16];
    aes128_expand_key(key, rk);
    uint8_t prev[16];
    memcpy(prev, iv, 16);
    uint8_t *plain = WR_ALLOC(ct_len);
    if (!plain) return false;
    for (size_t off = 0; off < ct_len; off += 16u) {
        uint8_t dec[16];
        aes128_decrypt_block(ct + off, dec, rk);
        for (size_t i = 0; i < 16u; ++i) plain[off + i] = dec[i] ^ prev[i];
        memcpy(prev, ct + off, 16);
    }
    uint8_t pad = plain[ct_len - 1];
    bool valid = pad >= 1 && pad <= 16 && pad <= ct_len;
    for (size_t i = 0; valid && i < pad; ++i) {
        if (plain[ct_len - 1 - i] != pad) valid = false;
    }
    size_t len = valid ? ct_len - pad : 0;
    if (valid && len <= out_cap) {
        if (len) memcpy(out, plain, len);
        *out_len = len;
    } else {
        valid = false;
    }
    free(plain);
    return valid;
}

/* ---- Base64 & Hex 解码 / Base64 & Hex Decoding ---------------------------- */

static int b64_val(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

bool book_weread_base64_decode(const char *in, uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!in || !out || !out_len) return false;
    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if (isspace(*p) || *p == '=') continue;
        int v = b64_val(*p);
        if (v < 0) return false;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= out_cap) return false;
            out[n++] = (uint8_t)((acc >> bits) & 0xffu);
        }
    }
    *out_len = n;
    return true;
}

static int hex_val(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool decode_encrypt_payload(const char *s, uint8_t *out, size_t cap, size_t *out_len) {
    while (*s && isspace((unsigned char)*s)) ++s;
    size_t slen = strlen(s);
    while (slen && isspace((unsigned char)s[slen - 1])) --slen;
    if (!slen) return false;

    bool is_hex = (slen % 2u) == 0;
    for (size_t i = 0; is_hex && i < slen; ++i) {
        if (hex_val((unsigned char)s[i]) < 0) is_hex = false;
    }
    if (is_hex) {
        size_t n = slen / 2u;
        if (n > cap) return false;
        for (size_t i = 0; i < n; ++i) {
            out[i] = (uint8_t)((hex_val((unsigned char)s[i * 2]) << 4) |
                               hex_val((unsigned char)s[i * 2 + 1]));
        }
        *out_len = n;
        return true;
    }
    return book_weread_base64_decode(s, out, cap, out_len);
}

bool book_weread_decrypt_encrypt_key(const char *encrypt_key, const char *secret,
                                     uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!encrypt_key || !*encrypt_key || !secret || !*secret || !out || !out_len) return false;
    size_t slen = strlen(secret);
    uint8_t key_iv[32];
    for (size_t i = 0; i < sizeof(key_iv); ++i) key_iv[i] = (uint8_t)secret[i % slen];

    size_t ek_len = strlen(encrypt_key);
    size_t ct_cap = ek_len + 16u;
    uint8_t *ct = WR_ALLOC(ct_cap);
    if (!ct) return false;
    size_t ct_len = 0;
    bool ok = decode_encrypt_payload(encrypt_key, ct, ct_cap, &ct_len) &&
              aes128_cbc_decrypt_pkcs7(key_iv, key_iv + 16, ct, ct_len, out, out_cap, out_len);
    free(ct);
    return ok;
}

bool book_weread_decrypt_encrypt_key_candidates(const char *encrypt_key,
                                                const char *const *secrets, size_t secret_count,
                                                uint8_t *out, size_t out_cap, size_t *out_len) {
    if (!encrypt_key || !*encrypt_key || !secrets) return false;
    for (size_t i = 0; i < secret_count; ++i) {
        if (secrets[i] && secrets[i][0] &&
            book_weread_decrypt_encrypt_key(encrypt_key, secrets[i], out, out_cap, out_len)) {
            return true;
        }
    }
    return false;
}

/* ---- GilbertVernam & QDataStream 信封 / GilbertVernam & Envelope ---------- */

void book_weread_gilbert_vernam(uint8_t *data, size_t len,
                                const uint8_t *key, size_t key_len, size_t start) {
    if (!data || !len || !key || !key_len) return;
    for (size_t i = 0; i < len; ++i) {
        size_t pos = start + i;
        size_t idx = (pos + pos / key_len) % key_len;
        data[i] ^= key[idx];
    }
}

bool book_weread_unwrap_envelope(const uint8_t *blob, size_t len,
                                 const char *secret,
                                 const uint8_t *fallback_key, size_t fallback_len,
                                 uint8_t **out, size_t *out_len) {
    if (!blob || len < 4 || !out || !out_len) return false;
    *out = NULL;
    *out_len = 0;
    int32_t magic = (int32_t)be32(blob);
    if (magic == 0) {
        uint8_t *copy = WR_ALLOC(len);
        if (!copy) return false;
        memcpy(copy, blob, len);
        *out = copy;
        *out_len = len;
        return true;
    }
    if (len < 16) return false;
    int32_t count = (int32_t)be32(blob + 4);
    if (count < 1 || count > 1000) return false;
    size_t offset = 8u + (size_t)count * 4u;
    if (offset + 4u > len) return false;
    int32_t key_len = (int32_t)be32(blob + offset);
    offset += 4u;

    uint8_t key_buf[256];
    const uint8_t *key = fallback_key;
    size_t klen = fallback_len;
    if (key_len >= 1 && key_len <= 1023) {
        if (offset + (size_t)key_len + 8u > len) return false;
        if (secret && *secret) {
            char *enc = WR_ALLOC((size_t)key_len + 1u);
            if (!enc) return false;
            memcpy(enc, blob + offset, (size_t)key_len);
            enc[key_len] = 0;
            bool dec_ok = book_weread_decrypt_encrypt_key(enc, secret, key_buf, sizeof(key_buf), &klen);
            free(enc);
            if (!dec_ok) return false;
            key = key_buf;
        } else {
            if ((size_t)key_len > sizeof(key_buf)) return false;
            memcpy(key_buf, blob + offset, (size_t)key_len);
            key = key_buf;
            klen = (size_t)key_len;
        }
        offset += (size_t)key_len;
    }
    if (offset + 8u > len) return false;
    uint64_t body_len = be64(blob + offset);
    offset += 8u;
    if (body_len == 0 || body_len > WR_ARCHIVE_ENTRY_MAX || offset + (size_t)body_len > len)
        return false;
    uint8_t *plain = WR_ALLOC((size_t)body_len);
    if (!plain) return false;
    memcpy(plain, blob + offset, (size_t)body_len);
    book_weread_gilbert_vernam(plain, (size_t)body_len, key, klen, offset);
    *out = plain;
    *out_len = (size_t)body_len;
    return true;
}

/* ---- 路径规范化 / Archive Path Normalization ------------------------------ */

static bool normalize_archive_path(const char *raw, size_t raw_len, char *out, size_t cap) {
    if (!raw || !raw_len || !out || cap < 2) return false;
    char tmp[512];
    if (raw_len >= sizeof(tmp)) return false;
    for (size_t i = 0; i < raw_len; ++i) {
        char c = raw[i];
        if (c == '\0') return false;
        tmp[i] = (c == '\\') ? '/' : c;
    }
    tmp[raw_len] = 0;

    size_t len = 0;
    unsigned depth = 0;
    const char *p = tmp;
    while (*p == '/') ++p;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t seg_len = slash ? (size_t)(slash - p) : strlen(p);
        if (seg_len == 0 || (seg_len == 1 && p[0] == '.')) {
            // Skip empty or "." segments.
        } else if (seg_len == 2 && p[0] == '.' && p[1] == '.') {
            return false;
        } else {
            if (++depth > 32) return false;
            if (len > 0) {
                if (len + 1u >= cap) return false;
                out[len++] = '/';
            }
            if (len + seg_len >= cap) return false;
            memcpy(out + len, p, seg_len);
            len += seg_len;
        }
        if (!slash) break;
        p = slash + 1;
    }
    out[len] = 0;
    return len > 0;
}

/* ---- PKWARE ZipCrypto + ZIP 解包 / ZipCrypto + ZIP Unpacker --------------- */

static uint32_t pk_crc32_byte(uint32_t crc, uint8_t b) {
    // IEEE 802.3 CRC-32 single-step without pre/post complement for ZipCrypto key schedule.
    crc ^= b;
    for (int i = 0; i < 8; ++i)
        crc = (crc >> 1) ^ ((crc & 1u) ? UINT32_C(0xedb88320) : 0u);
    return crc;
}

typedef struct {
    uint32_t k[3];
} pk_keys_t;

static void pk_init(pk_keys_t *pk, const uint8_t *pwd, size_t pwd_len) {
    pk->k[0] = UINT32_C(0x12345678);
    pk->k[1] = UINT32_C(0x23456789);
    pk->k[2] = UINT32_C(0x34567890);
    for (size_t i = 0; i < pwd_len; ++i) {
        pk->k[0] = pk_crc32_byte(pk->k[0], pwd[i]);
        pk->k[1] = (pk->k[1] + (pk->k[0] & 0xffu)) * UINT32_C(134775813) + 1u;
        pk->k[2] = pk_crc32_byte(pk->k[2], (uint8_t)(pk->k[1] >> 24));
    }
}

static uint8_t pk_decrypt_byte(const pk_keys_t *pk) {
    uint16_t temp = (uint16_t)((pk->k[2] & 0xffffu) | 2u);
    return (uint8_t)(((uint32_t)temp * (uint32_t)(temp ^ 1u)) >> 8);
}

static void pk_update(pk_keys_t *pk, uint8_t plain) {
    pk->k[0] = pk_crc32_byte(pk->k[0], plain);
    pk->k[1] = (pk->k[1] + (pk->k[0] & 0xffu)) * UINT32_C(134775813) + 1u;
    pk->k[2] = pk_crc32_byte(pk->k[2], (uint8_t)(pk->k[1] >> 24));
}

static esp_err_t inflate_raw_buf(const uint8_t *comp, size_t comp_len,
                                 uint8_t *out, size_t out_len) {
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.zalloc=wr_zalloc;zs.zfree=wr_zfree;
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return ESP_ERR_NO_MEM;
    zs.next_in = (Bytef *)comp;
    zs.avail_in = (uInt)comp_len;
    zs.next_out = (Bytef *)out;
    zs.avail_out = (uInt)out_len;
    int rc = inflate(&zs, Z_FINISH);
    size_t total_out = zs.total_out;
    inflateEnd(&zs);
    if (rc != Z_STREAM_END || total_out != out_len) return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

esp_err_t book_weread_unpack_zip(const uint8_t *payload, size_t len,
                                 const uint8_t *password, size_t password_len,
                                 book_weread_archive_entry_fn on_entry, void *ctx) {
    if (!payload || len < 22 || !on_entry) return ESP_ERR_INVALID_ARG;

    // Locate EOCD signature 0x06054b50 from the tail (comment up to 65535 bytes).
    size_t min_eocd = len > 65557u ? len - 65557u : 0u;
    size_t eocd = SIZE_MAX;
    for (size_t pos = len - 22u;; --pos) {
        if (le32(payload + pos) == UINT32_C(0x06054b50)) {
            uint16_t comment_len = le16(payload + pos + 20);
            if (pos + 22u + comment_len == len) {
                eocd = pos;
                break;
            }
        }
        if (pos == min_eocd) break;
    }
    if (eocd == SIZE_MAX) return ESP_ERR_INVALID_RESPONSE;

    uint16_t disk_no = le16(payload + eocd + 4);
    uint16_t cd_disk = le16(payload + eocd + 6);
    uint16_t entries_disk = le16(payload + eocd + 8);
    uint16_t entry_count = le16(payload + eocd + 10);
    uint32_t cd_size = le32(payload + eocd + 12);
    uint32_t cd_offset = le32(payload + eocd + 16);
    if (disk_no != 0 || cd_disk != 0 || entries_disk != entry_count ||
        entry_count > WR_ZIP_ENTRIES_MAX ||
        cd_offset > eocd || cd_size > eocd - cd_offset) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    size_t cd_pos = cd_offset;
    size_t cd_end = cd_offset + cd_size;
    for (uint16_t idx = 0; idx < entry_count; ++idx) {
        if (cd_pos > cd_end || cd_end - cd_pos < 46u || le32(payload + cd_pos) != UINT32_C(0x02014b50))
            return ESP_ERR_INVALID_RESPONSE;
        uint16_t flags = le16(payload + cd_pos + 8);
        uint16_t method = le16(payload + cd_pos + 10);
        uint16_t mod_time = le16(payload + cd_pos + 12);
        uint32_t expected_crc = le32(payload + cd_pos + 16);
        uint32_t comp_size = le32(payload + cd_pos + 20);
        uint32_t uncomp_size = le32(payload + cd_pos + 24);
        uint16_t name_len = le16(payload + cd_pos + 28);
        uint16_t extra_len = le16(payload + cd_pos + 30);
        uint16_t cmt_len = le16(payload + cd_pos + 32);
        uint32_t ext_attr = le32(payload + cd_pos + 38);
        uint32_t local_off = le32(payload + cd_pos + 42);

        size_t cd_rec_len = 46u + (size_t)name_len + (size_t)extra_len + (size_t)cmt_len;
        if (cd_end - cd_pos < cd_rec_len)
            return ESP_ERR_INVALID_RESPONSE;
        const char *raw_name = (const char *)(payload + cd_pos + 46u);
        bool is_dir = (name_len > 0 && raw_name[name_len - 1] == '/') || ((ext_attr & 0x10u) != 0);
        cd_pos += cd_rec_len;
        if (is_dir) continue;

        char norm_name[512];
        if (!normalize_archive_path(raw_name, name_len, norm_name, sizeof(norm_name)))
            return ESP_ERR_INVALID_RESPONSE;

        if (uncomp_size > WR_ARCHIVE_ENTRY_MAX || comp_size > WR_ARCHIVE_ENTRY_MAX)
            return ESP_ERR_INVALID_SIZE;
        if (method != 0 && method != 8) return ESP_ERR_NOT_SUPPORTED;
        if (local_off > cd_offset || cd_offset - local_off < 30u ||
            le32(payload + local_off) != UINT32_C(0x04034b50))
            return ESP_ERR_INVALID_RESPONSE;

        uint16_t l_name_len = le16(payload + local_off + 26);
        uint16_t l_extra_len = le16(payload + local_off + 28);
        size_t l_hdr_len = 30u + (size_t)l_name_len + (size_t)l_extra_len;
        if (cd_offset - local_off < l_hdr_len)
            return ESP_ERR_INVALID_RESPONSE;
        size_t data_off = (size_t)local_off + l_hdr_len;
        if (comp_size > cd_offset - data_off)
            return ESP_ERR_INVALID_RESPONSE;

        const uint8_t *src = payload + data_off;
        size_t src_len = comp_size;
        uint8_t *dec_comp = NULL;

        if (flags & 0x1u) {
            if (!password || !password_len || src_len < 12u) return ESP_ERR_INVALID_STATE;
            pk_keys_t pk;
            pk_init(&pk, password, password_len);
            uint8_t hdr[12];
            for (int i = 0; i < 12; ++i) {
                uint8_t plain_b = src[i] ^ pk_decrypt_byte(&pk);
                pk_update(&pk, plain_b);
                hdr[i] = plain_b;
            }
            uint8_t check = (flags & 0x8u) ? (uint8_t)(mod_time >> 8) : (uint8_t)(expected_crc >> 24);
            if (hdr[11] != check) return ESP_ERR_INVALID_STATE;

            src_len -= 12u;
            if (src_len > 0) {
                dec_comp = WR_ALLOC(src_len);
                if (!dec_comp) return ESP_ERR_NO_MEM;
                for (size_t i = 0; i < src_len; ++i) {
                    uint8_t plain_b = src[12u + i] ^ pk_decrypt_byte(&pk);
                    pk_update(&pk, plain_b);
                    dec_comp[i] = plain_b;
                }
                src = dec_comp;
            }
        }

        uint8_t *out = NULL;
        esp_err_t err = ESP_OK;
        if (uncomp_size == 0) {
            uint8_t empty = 0;
            if (expected_crc != 0) err = ESP_ERR_INVALID_CRC;
            else if (!on_entry(ctx, norm_name, &empty, 0)) err = ESP_ERR_INVALID_STATE;
            free(dec_comp);
            if (err != ESP_OK) return err;
            continue;
        }

        if (method == 0) {
            if (src_len != uncomp_size) {
                free(dec_comp);
                return ESP_ERR_INVALID_RESPONSE;
            }
            if (dec_comp) {
                out = dec_comp;
                dec_comp = NULL;
            } else {
                out = WR_ALLOC(uncomp_size);
                if (!out) return ESP_ERR_NO_MEM;
                memcpy(out, src, uncomp_size);
            }
        } else {
            out = WR_ALLOC(uncomp_size);
            if (!out) {
                free(dec_comp);
                return ESP_ERR_NO_MEM;
            }
            err = inflate_raw_buf(src, src_len, out, uncomp_size);
            free(dec_comp);
            dec_comp = NULL;
            if (err != ESP_OK) {
                free(out);
                return err;
            }
        }

        uint32_t actual_crc = (uint32_t)crc32(0L, out, (uInt)uncomp_size);
        if (actual_crc != expected_crc) {
            free(out);
            return ESP_ERR_INVALID_CRC;
        }
        bool keep_going = on_entry(ctx, norm_name, out, uncomp_size);
        free(out);
        if (!keep_going) return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

/* ---- USTAR TAR 解包 / USTAR TAR Unpacker ---------------------------------- */

static bool parse_tar_octal(const uint8_t *p, size_t n, uint64_t *out) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) ++i;
    if (i == n) { *out = 0; return true; }
    for (; i < n && p[i] != '\0' && p[i] != ' '; ++i) {
        if (p[i] < '0' || p[i] > '7') return false;
        v = (v << 3) | (uint64_t)(p[i] - '0');
    }
    *out = v;
    return true;
}

esp_err_t book_weread_unpack_tar(const uint8_t *payload, size_t len,
                                 book_weread_archive_entry_fn on_entry, void *ctx) {
    if (!payload || len < 512 || !on_entry) return ESP_ERR_INVALID_ARG;
    size_t pos = 0;
    bool saw_entry = false;
    while (pos + 512u <= len) {
        const uint8_t *hdr = payload + pos;
        bool all_zero = true;
        for (size_t i = 0; i < 512u; ++i) {
            if (hdr[i] != 0) { all_zero = false; break; }
        }
        if (all_zero) break;

        uint64_t expected_sum = 0;
        if (!parse_tar_octal(hdr + 148, 8, &expected_sum)) return ESP_ERR_INVALID_RESPONSE;
        uint32_t actual_sum = 0;
        for (size_t i = 0; i < 512u; ++i)
            actual_sum += (i >= 148 && i < 156) ? (uint32_t)' ' : (uint32_t)hdr[i];
        if (actual_sum != expected_sum) return ESP_ERR_INVALID_RESPONSE;

        uint64_t size = 0;
        if (!parse_tar_octal(hdr + 124, 12, &size) || size > WR_ARCHIVE_ENTRY_MAX)
            return ESP_ERR_INVALID_RESPONSE;

        pos += 512u;
        size_t padded = (size_t)((size + 511u) & ~UINT64_C(511));
        if (padded > len - pos) return ESP_ERR_INVALID_RESPONSE;

        char typeflag = (char)hdr[156];
        if (typeflag == '0' || typeflag == '\0') {
            char raw_path[260];
            size_t plen = strnlen((const char *)(hdr + 345), 155);
            size_t nlen = strnlen((const char *)hdr, 100);
            size_t total = 0;
            if (plen > 0) {
                memcpy(raw_path, hdr + 345, plen);
                raw_path[plen] = '/';
                total = plen + 1u;
            }
            memcpy(raw_path + total, hdr, nlen);
            total += nlen;
            raw_path[total] = 0;

            char norm_name[512];
            if (!normalize_archive_path(raw_path, total, norm_name, sizeof(norm_name)))
                return ESP_ERR_INVALID_RESPONSE;
            saw_entry = true;
            if (!on_entry(ctx, norm_name, payload + pos, (size_t)size))
                return ESP_ERR_INVALID_STATE;
        }
        pos += padded;
    }
    return saw_entry ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* ---- 文件流归档 / File-stream Archives --------------------------------- */

static bool archive_read(FILE *f, size_t offset, void *data, size_t len) {
    return offset <= 64u * 1024u * 1024u && !fseek(f, (long)offset, SEEK_SET) &&
        fread(data, 1, len, f) == len;
}
static bool archive_size(FILE *f, size_t *size) {
    if (!f || fseek(f, 0, SEEK_END)) return false;
    long n = ftell(f);
    if (n < 0 || n > 64 * 1024 * 1024) return false;
    *size = (size_t)n; return true;
}

esp_err_t book_weread_unpack_zip_file(FILE *f, const uint8_t *password, size_t password_len,
                                     book_weread_archive_chunk_fn sink, void *ctx) {
    if (!f || !sink) return ESP_ERR_INVALID_ARG;
    size_t size;
    if (!archive_size(f, &size) || size < 22) return ESP_ERR_INVALID_SIZE;
    size_t tail_len = size < 65557u ? size : 65557u;
    uint8_t *tail = WR_ALLOC(tail_len);
    if (!tail) return ESP_ERR_NO_MEM;
    if (!archive_read(f, size - tail_len, tail, tail_len)) { free(tail); return ESP_FAIL; }
    size_t end = SIZE_MAX;
    for (size_t i = tail_len - 22u;; --i) {
        if (le32(tail + i) == UINT32_C(0x06054b50) && i + 22u + le16(tail + i + 20) == tail_len) {
            end = i; break;
        }
        if (!i) break;
    }
    if (end == SIZE_MAX) { free(tail); return ESP_ERR_INVALID_RESPONSE; }
    uint8_t eocd[22]; memcpy(eocd, tail + end, 22);
    size_t eocd_offset = size - tail_len + end;
    free(tail);
    uint16_t count = le16(eocd + 10);
    size_t cd = le32(eocd + 16), cd_size = le32(eocd + 12);
    if (le16(eocd + 4) || le16(eocd + 6) || le16(eocd + 8) != count ||
        count > WR_ZIP_ENTRIES_MAX || cd > eocd_offset || cd_size > eocd_offset - cd)
        return ESP_ERR_INVALID_RESPONSE;
    uint8_t *scratch = WR_ALLOC(8192);
    if (!scratch) return ESP_ERR_NO_MEM;
    uint8_t *in = scratch, *out = scratch + 4096;
    size_t pos = cd, cd_end = cd + cd_size, expanded = 0;
    esp_err_t err = ESP_OK;
    for (unsigned entry = 0; entry < count && err == ESP_OK; ++entry) {
        uint8_t header[46], local[30];
        if (pos > cd_end || cd_end - pos < sizeof(header) || !archive_read(f, pos, header, sizeof(header)) ||
            le32(header) != UINT32_C(0x02014b50)) { err = ESP_ERR_INVALID_RESPONSE; break; }
        uint16_t flags = le16(header + 8), method = le16(header + 10), name_len = le16(header + 28);
        uint32_t crc = le32(header + 16), compressed = le32(header + 20), plain = le32(header + 24);
        size_t local_pos = le32(header + 42);
        size_t record = 46u + name_len + le16(header + 30) + le16(header + 32);
        char raw[512], name[512];
        if (record > cd_end - pos || !name_len || name_len >= sizeof(raw) ||
            !archive_read(f, pos + 46, raw, name_len)) { err = ESP_ERR_INVALID_RESPONSE; break; }
        raw[name_len] = 0; pos += record;
        if (raw[name_len - 1] == '/') continue;
        unsigned kind = (le32(header + 38) >> 16) & 0170000u;
        if ((kind && kind != 0100000u) || !normalize_archive_path(raw, name_len, name, sizeof(name))) {
            err = ESP_ERR_INVALID_RESPONSE; break;
        }
        if ((flags & 0x2040u) || (method != 0 && method != 8)) { err = ESP_ERR_NOT_SUPPORTED; break; }
        if (compressed > WR_ARCHIVE_ENTRY_MAX || plain > WR_ARCHIVE_ENTRY_MAX ||
            plain > 64u * 1024u * 1024u - expanded) { err = ESP_ERR_INVALID_SIZE; break; }
        expanded += plain;
        if (local_pos > cd || cd - local_pos < 30 || !archive_read(f, local_pos, local, sizeof(local)) ||
            le32(local) != UINT32_C(0x04034b50) || le16(local + 6) != flags || le16(local + 8) != method ||
            le16(local + 26) != name_len) { err = ESP_ERR_INVALID_RESPONSE; break; }
        size_t data = local_pos + 30u + name_len + le16(local + 28);
        if (data > cd || compressed > cd - data ||
            !archive_read(f, local_pos + 30, in, name_len) || memcmp(in, raw, name_len)) {
            err = ESP_ERR_INVALID_RESPONSE; break;
        }
        pk_keys_t pk;
        if (flags & 1u) {
            if (!password || !password_len || compressed < 12 || !archive_read(f, data, in, 12)) {
                err = ESP_ERR_INVALID_STATE; break;
            }
            pk_init(&pk, password, password_len);
            for (size_t i = 0; i < 12; ++i) { uint8_t b = in[i] ^ pk_decrypt_byte(&pk); pk_update(&pk, b); in[i] = b; }
            uint8_t check = flags & 8u ? (uint8_t)(le16(header + 12) >> 8) : (uint8_t)(crc >> 24);
            if (in[11] != check) { err = ESP_ERR_INVALID_STATE; break; }
            data += 12; compressed -= 12;
        }
        z_stream zs = {0};
        zs.zalloc=wr_zalloc;zs.zfree=wr_zfree;
        if (method == 8 && inflateInit2(&zs, -MAX_WBITS) != Z_OK) { err = ESP_ERR_NO_MEM; break; }
        size_t remaining = compressed, done = 0;
        uint32_t actual_crc = (uint32_t)crc32(0, NULL, 0);
        bool ended = false;
        while (remaining && err == ESP_OK) {
            size_t n = remaining < 4096 ? remaining : 4096;
            if (!archive_read(f, data, in, n)) { err = ESP_FAIL; break; }
            data += n; remaining -= n;
            if (flags & 1u) for (size_t i = 0; i < n; ++i) {
                uint8_t b = in[i] ^ pk_decrypt_byte(&pk); pk_update(&pk, b); in[i] = b;
            }
            if (!method) {
                if (n > plain - done) { err = ESP_ERR_INVALID_RESPONSE; break; }
                actual_crc = (uint32_t)crc32(actual_crc, in, (uInt)n);
                if (!sink(ctx, name, in, n, done, plain)) { err = ESP_ERR_INVALID_STATE; break; }
                done += n;
            } else {
                zs.next_in = in; zs.avail_in = (uInt)n;
                do {
                    zs.next_out = out; zs.avail_out = 4096;
                    uLong before = zs.total_in;
                    int rc = inflate(&zs, Z_NO_FLUSH);
                    size_t produced = 4096 - zs.avail_out;
                    // 输出恰好填满块后，zlib可在无新输入时返回BUF_ERROR；应续读，不能当损坏。
                    // After an exact output block, no-input BUF_ERROR means read the next input chunk.
                    if((rc==Z_BUF_ERROR||rc==Z_OK)&&!produced&&zs.total_in==before&&!zs.avail_in)break;
                    if (produced > plain - done || (rc != Z_OK && rc != Z_STREAM_END) ||
                        (!produced && zs.total_in == before && rc != Z_STREAM_END)) {
                        err = ESP_ERR_INVALID_RESPONSE; break;
                    }
                    actual_crc = (uint32_t)crc32(actual_crc, out, (uInt)produced);
                    if (produced && !sink(ctx, name, out, produced, done, plain)) { err = ESP_ERR_INVALID_STATE; break; }
                    done += produced;
                    if (rc == Z_STREAM_END) { ended = true; break; }
                } while (zs.avail_in || !zs.avail_out);
                if (ended && (remaining || zs.avail_in)) err = ESP_ERR_INVALID_RESPONSE;
                if (ended) break;
            }
        }
        if (method == 8) inflateEnd(&zs);
        if (err == ESP_OK && (done != plain || (method == 8 && !ended))) err = ESP_ERR_INVALID_RESPONSE;
        if (err == ESP_OK && actual_crc != crc) err = ESP_ERR_INVALID_CRC;
        if (err == ESP_OK && !plain && !sink(ctx, name, out, 0, 0, 0)) err = ESP_ERR_INVALID_STATE;
    }
    free(scratch);
    return err;
}

esp_err_t book_weread_unpack_tar_file(FILE *f, book_weread_archive_chunk_fn sink, void *ctx) {
    if (!f || !sink) return ESP_ERR_INVALID_ARG;
    size_t size;
    if (!archive_size(f, &size) || size < 512) return ESP_ERR_INVALID_SIZE;
    uint8_t *buf = WR_ALLOC(4096);
    if (!buf) return ESP_ERR_NO_MEM;
    esp_err_t err = ESP_OK;
    size_t pos = 0, entries = 0;
    while (size - pos >= 512 && err == ESP_OK) {
        if (!archive_read(f, pos, buf, 512)) { err = ESP_FAIL; break; }
        bool empty = true;
        for (size_t i = 0; i < 512; ++i) if (buf[i]) { empty = false; break; }
        if (empty) break;
        uint64_t crc, plain;
        unsigned sum = 0;
        for (size_t i = 0; i < 512; ++i) sum += i >= 148 && i < 156 ? ' ' : buf[i];
        if (!parse_tar_octal(buf + 148, 8, &crc) || crc != sum ||
            !parse_tar_octal(buf + 124, 12, &plain) || plain > WR_ARCHIVE_ENTRY_MAX ||
            ++entries > WR_ZIP_ENTRIES_MAX) { err = ESP_ERR_INVALID_RESPONSE; break; }
        size_t padded = ((size_t)plain + 511u) & ~(size_t)511u;
        pos += 512;
        if (padded > size - pos) { err = ESP_ERR_INVALID_RESPONSE; break; }
        char raw[260], name[512];
        size_t prefix = strnlen((const char *)buf + 345, 155), leaf = strnlen((const char *)buf, 100);
        size_t n = 0;
        if (prefix) { memcpy(raw, buf + 345, prefix); raw[prefix] = '/'; n = prefix + 1; }
        memcpy(raw + n, buf, leaf); n += leaf; raw[n] = 0;
        char type = (char)buf[156];
        if (type != '0' && type != 0 && type != '5') { err = ESP_ERR_NOT_SUPPORTED; break; }
        if (!normalize_archive_path(raw, n, name, sizeof(name))) { err = ESP_ERR_INVALID_RESPONSE; break; }
        if (type != '5') {
            size_t done = 0;
            do {
                n = plain - done < 4096 ? (size_t)plain - done : 4096;
                if (!archive_read(f, pos + done, buf, n)) { err = ESP_FAIL; break; }
                if (!sink(ctx, name, buf, n, done, (size_t)plain)) { err = ESP_ERR_INVALID_STATE; break; }
                done += n;
            } while (done < plain);
        }
        pos += padded;
    }
    free(buf);
    return err != ESP_OK ? err : entries ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

/* ---- RFC 1321 MD5（供 Web ID 混淆使用）/ RFC 1321 MD5 --------------------- */

static void md5_transform(uint32_t state[4], const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391,
    };
    static const uint8_t s[64] = {
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21,
    };
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) m[i] = le32(block + i * 4);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f, g;
        if (i < 16)      { f = (b & c) | ((~b) & d); g = (uint32_t)i; }
        else if (i < 32) { f = (d & b) | ((~d) & c); g = (uint32_t)((5 * i + 1) % 16); }
        else if (i < 48) { f = b ^ c ^ d;            g = (uint32_t)((3 * i + 5) % 16); }
        else             { f = c ^ (b | (~d));       g = (uint32_t)((7 * i) % 16); }
        f += a + k[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += (f << s[i]) | (f >> (32 - s[i]));
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void md5_hex(const char *in, size_t len, char out_hex[33]) {
    uint32_t state[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint8_t buf[64];
    size_t pos = 0;
    while (pos + 64u <= len) {
        md5_transform(state, (const uint8_t *)in + pos);
        pos += 64u;
    }
    size_t rem = len - pos;
    memset(buf, 0, sizeof(buf));
    if (rem) memcpy(buf, in + pos, rem);
    buf[rem] = 0x80;
    if (rem >= 56u) {
        md5_transform(state, buf);
        memset(buf, 0, sizeof(buf));
    }
    uint64_t bit_len = (uint64_t)len * 8u;
    for (int i = 0; i < 8; ++i) buf[56 + i] = (uint8_t)(bit_len >> (i * 8));
    md5_transform(state, buf);

    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < 4; ++i) {
        for (int b = 0; b < 4; ++b) {
            uint8_t v = (uint8_t)(state[i] >> (b * 8));
            out_hex[(i * 4 + b) * 2] = kHex[v >> 4];
            out_hex[(i * 4 + b) * 2 + 1] = kHex[v & 0xf];
        }
    }
    out_hex[32] = 0;
}

/* ---- Web `/web/book/read` 签名 / Web Progress Obfuscation & Signing ------- */

bool book_weread_encode_web_id(const char *value, char *out, size_t cap) {
    if (!value || !out || cap < 32) return false;
    size_t vlen = strlen(value);
    char md5h[33];
    md5_hex(value, vlen, md5h);

    bool all_digits = vlen > 0;
    for (size_t i = 0; i < vlen; ++i) {
        if (!isdigit((unsigned char)value[i])) { all_digits = false; break; }
    }

    char body[256];
    size_t pos = 0;
    memcpy(body, md5h, 3); pos += 3;
    body[pos++] = all_digits ? '3' : '4';
    body[pos++] = '2';
    memcpy(body + pos, md5h + 30, 2); pos += 2;

    if (all_digits) {
        for (size_t i = 0; i < vlen; i += 9u) {
            char seg[10];
            size_t take = vlen - i < 9u ? vlen - i : 9u;
            memcpy(seg, value + i, take);
            seg[take] = 0;
            unsigned long val = strtoul(seg, NULL, 10);
            char hex_seg[16];
            int hn = snprintf(hex_seg, sizeof(hex_seg), "%lx", val);
            if (hn <= 0) return false;
            if (i > 0) {
                if (pos + 1u >= sizeof(body)) return false;
                body[pos++] = 'g';
            }
            if (pos + 2u + (size_t)hn >= sizeof(body)) return false;
            int wr = snprintf(body + pos, sizeof(body) - pos, "%02x%s", hn, hex_seg);
            if (wr <= 0) return false;
            pos += (size_t)wr;
        }
    } else {
        // Encode UTF-8 string as UTF-16LE uint16 hex chunks matching Python client.py.
        char hex_buf[200] = {0};
        size_t hpos = 0;
        const uint8_t *p = (const uint8_t *)value;
        while (*p) {
            uint32_t cp = 0;
            if (*p < 0x80) { cp = *p++; }
            else if ((*p & 0xe0) == 0xc0 && p[1]) {
                cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
                p += 2;
            } else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
                cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
                p += 3;
            } else if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
                cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
                     ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
                p += 4;
            } else { cp = *p++; }
            if (cp <= 0xffffu) {
                int w = snprintf(hex_buf + hpos, sizeof(hex_buf) - hpos, "%x", (unsigned)cp);
                if (w <= 0 || hpos + (size_t)w >= sizeof(hex_buf)) return false;
                hpos += (size_t)w;
            } else {
                uint32_t u = cp - 0x10000u;
                uint16_t hi = (uint16_t)(0xd800u + (u >> 10));
                uint16_t lo = (uint16_t)(0xdc00u + (u & 0x3ffu));
                int w = snprintf(hex_buf + hpos, sizeof(hex_buf) - hpos, "%x%x", hi, lo);
                if (w <= 0 || hpos + (size_t)w >= sizeof(hex_buf)) return false;
                hpos += (size_t)w;
            }
        }
        if (pos + 2u + hpos >= sizeof(body)) return false;
        int wr = snprintf(body + pos, sizeof(body) - pos, "%02x%s", (unsigned)hpos, hex_buf);
        if (wr <= 0) return false;
        pos += (size_t)wr;
    }

    if (pos < 20u) {
        size_t need = 20u - pos;
        if (pos + need >= sizeof(body)) return false;
        memcpy(body + pos, md5h, need);
        pos += need;
    }
    body[pos] = 0;

    char tail_md5[33];
    md5_hex(body, pos, tail_md5);
    int total = snprintf(out, cap, "%s%.3s", body, tail_md5);
    return total > 0 && (size_t)total < cap;
}

bool book_weread_generate_web_app_id(const char *user_agent, char *out, size_t cap) {
    if (!user_agent || !out || cap < 32) return false;
    char prefix[16];
    size_t plen = 0;
    const char *p = user_agent;
    while (plen < 12) {
        const char *sp = strchr(p, ' ');
        size_t wlen = sp ? (size_t)(sp - p) : strlen(p);
        prefix[plen++] = (char)('0' + (wlen % 10u));
        if (!sp) break;
        p = sp + 1;
    }
    prefix[plen] = 0;

    uint32_t acc = 0;
    for (const unsigned char *u = (const unsigned char *)user_agent; *u; ++u)
        acc = (0x83u * acc + *u) & UINT32_C(0x7fffffff);

    int n = snprintf(out, cap, "wb%sh%u", prefix, (unsigned)acc);
    return n > 0 && (size_t)n < cap;
}

static size_t url_encode_safe(const char *in, char *out, size_t cap) {
    static const char kSafe[] = "~!*()'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.";
    static const char kHex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        if (*p < 0x80 && strchr(kSafe, (char)*p) != NULL) {
            if (pos + 1u >= cap) return 0;
            out[pos++] = (char)*p;
        } else {
            if (pos + 3u >= cap) return 0;
            out[pos++] = '%';
            out[pos++] = kHex[*p >> 4];
            out[pos++] = kHex[*p & 0xf];
        }
    }
    out[pos] = 0;
    return pos;
}

static size_t utf8_prefix_chars(const char *s, size_t max_chars, char *out, size_t cap) {
    if (!s || !out || !cap) return 0;
    size_t chars = 0, bytes = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && chars < max_chars) {
        size_t n = *p < 0x80 ? 1 : (*p < 0xe0 ? 2 : (*p < 0xf0 ? 3 : 4));
        if (strnlen((const char *)p, n) < n || bytes + n >= cap) break;
        memcpy(out + bytes, p, n);
        bytes += n;
        p += n;
        ++chars;
    }
    out[bytes] = 0;
    return bytes;
}

static bool json_escape(const char *in, char *out, size_t cap) {
    size_t pos = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; ++p) {
        const char *rep = NULL;
        char esc[7];
        if (*p == '"') rep = "\\\"";
        else if (*p == '\\') rep = "\\\\";
        else if (*p == '\n') rep = "\\n";
        else if (*p == '\r') rep = "\\r";
        else if (*p == '\t') rep = "\\t";
        else if (*p < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", *p);
            rep = esc;
        }
        if (rep) {
            size_t rlen = strlen(rep);
            if (pos + rlen >= cap) return false;
            memcpy(out + pos, rep, rlen);
            pos += rlen;
        } else {
            if (pos + 1u >= cap) return false;
            out[pos++] = (char)*p;
        }
    }
    out[pos] = 0;
    return true;
}

bool book_weread_build_web_read_body(const char *book_id, uint32_t chapter_uid,
                                     uint32_t chapter_idx, uint32_t chapter_offset,
                                     uint32_t progress, const char *summary,
                                     int64_t timestamp, const char *user_agent,
                                     char *out, size_t out_cap) {
    if (!book_id || !*book_id || !user_agent || !out || out_cap < 256) return false;
    char app_id[64], enc_b[256], enc_c[96], enc_ps[96];
    char uid_str[24], ts_str[32];
    snprintf(uid_str, sizeof(uid_str), "%u", (unsigned)chapter_uid);
    snprintf(ts_str, sizeof(ts_str), "%lld", (long long)timestamp);

    if (!book_weread_generate_web_app_id(user_agent, app_id, sizeof(app_id)) ||
        !book_weread_encode_web_id(book_id, enc_b, sizeof(enc_b)) ||
        !book_weread_encode_web_id(uid_str, enc_c, sizeof(enc_c)) ||
        !book_weread_encode_web_id(ts_str, enc_ps, sizeof(enc_ps))) {
        return false;
    }

    char sm_trim[96];
    utf8_prefix_chars(summary ? summary : "", 20u, sm_trim, sizeof(sm_trim));

    char sm_url[288];
    if (sm_trim[0]) {
        if (!url_encode_safe(sm_trim, sm_url, sizeof(sm_url))) return false;
    } else {
        sm_url[0] = 0;
    }

    // Keys in strict alphabetical order: appId, b, c, ci, co, ct, pc, pr, ps, sm.
    char query[1024];
    int qlen = snprintf(query, sizeof(query),
                        "appId=%s&b=%s&c=%s&ci=%u&co=%u&ct=%s&pc=%s&pr=%u&ps=%s&sm=%s",
                        app_id, enc_b, enc_c, (unsigned)chapter_idx, (unsigned)chapter_offset,
                        ts_str, enc_ps, (unsigned)progress, enc_ps, sm_url);
    if (qlen <= 0 || (size_t)qlen >= sizeof(query)) return false;

    uint32_t a = UINT32_C(0x15051505);
    uint32_t b = UINT32_C(0x15051505);
    size_t length = (size_t)qlen;
    for (int idx = qlen - 1; idx > 0; idx -= 2) {
        a = UINT32_C(0x7fffffff) & (a ^ ((uint32_t)(uint8_t)query[idx] << ((length - (size_t)idx) % 0x1eu)));
        b = UINT32_C(0x7fffffff) & (b ^ ((uint32_t)(uint8_t)query[idx - 1] << ((size_t)idx % 0x1eu)));
    }
    char sig[16];
    snprintf(sig, sizeof(sig), "%lx", (unsigned long)(a + b));

    char sm_json[220];
    if (!json_escape(sm_trim, sm_json, sizeof(sm_json))) return false;

    int n = snprintf(out, out_cap,
                     "{\"appId\":\"%s\",\"b\":\"%s\",\"c\":\"%s\",\"ci\":%u,\"co\":%u,"
                     "\"sm\":\"%s\",\"pr\":%u,\"ct\":%s,\"ps\":\"%s\",\"pc\":\"%s\",\"s\":\"%s\"}",
                     app_id, enc_b, enc_c, (unsigned)chapter_idx, (unsigned)chapter_offset,
                     sm_json, (unsigned)progress, ts_str, enc_ps, enc_ps, sig);
    return n > 0 && (size_t)n < out_cap;
}
