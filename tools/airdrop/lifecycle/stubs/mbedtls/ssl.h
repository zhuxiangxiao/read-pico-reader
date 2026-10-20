#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>
typedef struct mbedtls_ssl_context { int dummy; } mbedtls_ssl_context;
typedef struct mbedtls_ssl_config { int dummy; } mbedtls_ssl_config;
typedef struct mbedtls_ssl_session { int dummy; } mbedtls_ssl_session;
typedef struct mbedtls_x509_crt mbedtls_x509_crt;
typedef struct mbedtls_pk_context mbedtls_pk_context;
#define MBEDTLS_SSL_IS_SERVER 0
#define MBEDTLS_SSL_TRANSPORT_STREAM 0
#define MBEDTLS_SSL_PRESET_DEFAULT 0
#define MBEDTLS_SSL_VERIFY_NONE 0
#define MBEDTLS_SSL_VERSION_TLS1_2 0x0303
#define MBEDTLS_ERR_SSL_WANT_READ -0x6900
#define MBEDTLS_ERR_SSL_WANT_WRITE -0x6880
#define MBEDTLS_ERR_SSL_TIMEOUT -0x6800
void mbedtls_ssl_init(mbedtls_ssl_context *);
void mbedtls_ssl_free(mbedtls_ssl_context *);
int  mbedtls_ssl_setup(mbedtls_ssl_context *, const mbedtls_ssl_config *);
void mbedtls_ssl_config_init(mbedtls_ssl_config *);
void mbedtls_ssl_config_free(mbedtls_ssl_config *);
int  mbedtls_ssl_config_defaults(mbedtls_ssl_config *, int, int, int);
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *, int);
void mbedtls_ssl_conf_rng(mbedtls_ssl_config *, int (*)(void *, unsigned char *, size_t), void *);
void mbedtls_ssl_conf_min_tls_version(mbedtls_ssl_config *, int);
void mbedtls_ssl_conf_read_timeout(mbedtls_ssl_config *, uint32_t);
int  mbedtls_ssl_conf_own_cert(mbedtls_ssl_config *, mbedtls_x509_crt *, mbedtls_pk_context *);
void mbedtls_ssl_conf_session_cache(mbedtls_ssl_config *, void *,
        int (*)(void *, const unsigned char *, size_t, mbedtls_ssl_session *),
        int (*)(void *, const unsigned char *, size_t, const mbedtls_ssl_session *));
int  mbedtls_ssl_handshake(mbedtls_ssl_context *);
int  mbedtls_ssl_read(mbedtls_ssl_context *, unsigned char *, size_t);
int  mbedtls_ssl_write(mbedtls_ssl_context *, const unsigned char *, size_t);
int  mbedtls_ssl_close_notify(mbedtls_ssl_context *);
void mbedtls_ssl_session_reset(mbedtls_ssl_context *);
const char *mbedtls_ssl_get_version(const mbedtls_ssl_context *);
void mbedtls_ssl_set_bio(mbedtls_ssl_context *, void *,
        int (*)(void *, const unsigned char *, size_t),
        int (*)(void *, unsigned char *, size_t),
        int (*)(void *, unsigned char *, size_t, uint32_t));

#ifdef __cplusplus
}
#endif