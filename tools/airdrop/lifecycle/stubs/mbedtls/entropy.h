#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
typedef struct mbedtls_entropy_context { int dummy; } mbedtls_entropy_context;
void mbedtls_entropy_init(mbedtls_entropy_context *);
void mbedtls_entropy_free(mbedtls_entropy_context *);
int  mbedtls_entropy_func(void *, unsigned char *, size_t);

#ifdef __cplusplus
}
#endif