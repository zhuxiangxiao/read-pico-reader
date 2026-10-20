#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
typedef struct mbedtls_ctr_drbg_context { int dummy; } mbedtls_ctr_drbg_context;
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *);
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *);
int  mbedtls_ctr_drbg_random(void *, unsigned char *, size_t);
int  mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *,
        int (*)(void *, unsigned char *, size_t), void *,
        const unsigned char *, size_t);

#ifdef __cplusplus
}
#endif