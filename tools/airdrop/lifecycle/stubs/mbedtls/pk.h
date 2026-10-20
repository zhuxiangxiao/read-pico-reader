#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
typedef struct mbedtls_pk_context { int dummy; } mbedtls_pk_context;
void mbedtls_pk_init(mbedtls_pk_context *);
void mbedtls_pk_free(mbedtls_pk_context *);
int  mbedtls_pk_parse_key(mbedtls_pk_context *, const unsigned char *, size_t,
        const unsigned char *, size_t,
        int (*)(void *, unsigned char *, size_t), void *);

#ifdef __cplusplus
}
#endif