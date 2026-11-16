/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: Apache-2.0
 * WeRead host test parameterized storage path helpers.
 */
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define WEREAD_DEFAULT_STORAGE_ROOT "/tmp/weread_test_storage"

static inline const char *weread_storage_root(void)
{
    const char *env_root = getenv("WEREAD_STORAGE_ROOT");
    if (env_root && env_root[0] != '\0') {
        return env_root;
    }
    return WEREAD_DEFAULT_STORAGE_ROOT;
}

static inline void weread_storage_mkdir_recursive(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp, 0755);
}

static inline int weread_storage_path(const char *subpath, char *out, size_t cap)
{
    const char *root = weread_storage_root();
    if (!subpath || !out || cap == 0) {
        return -1;
    }
    while (*subpath == '/') {
        subpath++;
    }
    int n = snprintf(out, cap, "%s/%s", root, subpath);
    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }
    return 0;
}

static inline int weread_storage_init(void)
{
    const char *root = weread_storage_root();
    weread_storage_mkdir_recursive(root);

    char sub[512];
    snprintf(sub, sizeof(sub), "%s/cache", root);
    mkdir(sub, 0755);
    return 0;
}
