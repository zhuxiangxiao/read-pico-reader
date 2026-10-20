#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include <stddef.h>
int mbedtls_platform_set_calloc_free(void *(*)(size_t, size_t), void (*)(void *));

#ifdef __cplusplus
}
#endif