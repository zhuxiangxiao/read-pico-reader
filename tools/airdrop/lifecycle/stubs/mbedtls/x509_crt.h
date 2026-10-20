#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
typedef struct mbedtls_x509_crt { int dummy; } mbedtls_x509_crt;
void mbedtls_x509_crt_init(mbedtls_x509_crt *);
void mbedtls_x509_crt_free(mbedtls_x509_crt *);
int  mbedtls_x509_crt_parse(mbedtls_x509_crt *, const unsigned char *, size_t);
int  mbedtls_x509_crt_parse_der(mbedtls_x509_crt *, const unsigned char *, size_t);

#ifdef __cplusplus
}
#endif