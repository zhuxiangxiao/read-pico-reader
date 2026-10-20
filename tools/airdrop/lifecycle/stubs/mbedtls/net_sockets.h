#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
#include <stdint.h>
typedef struct mbedtls_net_context { int fd; } mbedtls_net_context;
int mbedtls_net_recv(void *, unsigned char *, size_t);
int mbedtls_net_send(void *, const unsigned char *, size_t);
int mbedtls_net_recv_timeout(void *, unsigned char *, size_t, uint32_t);

#ifdef __cplusplus
}
#endif