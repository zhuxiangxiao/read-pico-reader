#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include "ssl.h"
typedef struct mbedtls_ssl_cache_context { int dummy; } mbedtls_ssl_cache_context;
void mbedtls_ssl_cache_init(mbedtls_ssl_cache_context *);
void mbedtls_ssl_cache_free(mbedtls_ssl_cache_context *);
int  mbedtls_ssl_cache_get(void *, const unsigned char *, size_t, mbedtls_ssl_session *);
int  mbedtls_ssl_cache_set(void *, const unsigned char *, size_t, const mbedtls_ssl_session *);
void mbedtls_ssl_cache_set_max_entries(mbedtls_ssl_cache_context *, int);

#ifdef __cplusplus
}
#endif