/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0 */
/* Host I/O syscall spy header force-included during performance benchmarks. */
#pragma once

#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <dirent.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    _Atomic uint64_t fopen_calls;
    _Atomic uint64_t fseek_calls;
    _Atomic uint64_t fread_calls;
    _Atomic uint64_t fread_bytes;
    _Atomic uint64_t fwrite_calls;
    _Atomic uint64_t fwrite_bytes;
    _Atomic uint64_t stat_calls;
    _Atomic uint64_t lstat_calls;
    _Atomic uint64_t rename_calls;
    _Atomic uint64_t fsync_calls;
    _Atomic uint64_t opendir_calls;
    _Atomic uint64_t shelf_writes;
    _Atomic uint64_t shelf_bytes;
} wr_perf_io_stats_t;

extern wr_perf_io_stats_t g_wr_perf_io;

static inline void wr_perf_io_reset(void) {
    memset(&g_wr_perf_io, 0, sizeof(g_wr_perf_io));
}

static inline FILE *wr_spy_fopen(const char *path, const char *mode) {
    atomic_fetch_add_explicit(&g_wr_perf_io.fopen_calls, 1, memory_order_relaxed);
    return fopen(path, mode);
}

static inline int wr_spy_fseek(FILE *f, long off, int whence) {
    atomic_fetch_add_explicit(&g_wr_perf_io.fseek_calls, 1, memory_order_relaxed);
    return fseek(f, off, whence);
}

static inline int wr_spy_fseeko(FILE *f, off_t off, int whence) {
    atomic_fetch_add_explicit(&g_wr_perf_io.fseek_calls, 1, memory_order_relaxed);
    return fseeko(f, off, whence);
}

static inline size_t wr_spy_fread(void *ptr, size_t sz, size_t nm, FILE *f) {
    size_t got = fread(ptr, sz, nm, f);
    atomic_fetch_add_explicit(&g_wr_perf_io.fread_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_wr_perf_io.fread_bytes, (uint64_t)(got * sz), memory_order_relaxed);
    return got;
}

static inline size_t wr_spy_fwrite(const void *ptr, size_t sz, size_t nm, FILE *f) {
    size_t put = fwrite(ptr, sz, nm, f);
    atomic_fetch_add_explicit(&g_wr_perf_io.fwrite_calls, 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_wr_perf_io.fwrite_bytes, (uint64_t)(put * sz), memory_order_relaxed);
    return put;
}

static inline int wr_spy_stat(const char *path, struct stat *st) {
    atomic_fetch_add_explicit(&g_wr_perf_io.stat_calls, 1, memory_order_relaxed);
    return stat(path, st);
}

static inline int wr_spy_lstat(const char *path, struct stat *st) {
    atomic_fetch_add_explicit(&g_wr_perf_io.lstat_calls, 1, memory_order_relaxed);
    return lstat(path, st);
}

static inline int wr_spy_rename(const char *oldp, const char *newp) {
    atomic_fetch_add_explicit(&g_wr_perf_io.rename_calls, 1, memory_order_relaxed);
    int rc = rename(oldp, newp);
    if (rc == 0 && newp) {
        size_t len = strlen(newp);
        if ((len == 10 && !strcmp(newp, "shelf.json")) ||
            (len > 11 && !strcmp(newp + len - 11, "/shelf.json"))) {
            struct stat st;
            if (stat(newp, &st) == 0) {
                atomic_fetch_add_explicit(&g_wr_perf_io.shelf_writes, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&g_wr_perf_io.shelf_bytes, (uint64_t)st.st_size, memory_order_relaxed);
            }
        }
    }
    return rc;
}

static inline int wr_spy_fsync(int fd) {
    atomic_fetch_add_explicit(&g_wr_perf_io.fsync_calls, 1, memory_order_relaxed);
    return fsync(fd);
}

static inline DIR *wr_spy_opendir(const char *path) {
    atomic_fetch_add_explicit(&g_wr_perf_io.opendir_calls, 1, memory_order_relaxed);
    return opendir(path);
}

#ifndef WR_PERF_NO_SPY_MACROS
#define fopen(p, m) wr_spy_fopen((p), (m))
#define fseek(f, o, w) wr_spy_fseek((f), (o), (w))
#define fseeko(f, o, w) wr_spy_fseeko((f), (o), (w))
#define fread(p, s, n, f) wr_spy_fread((p), (s), (n), (f))
#define fwrite(p, s, n, f) wr_spy_fwrite((p), (s), (n), (f))
#define stat(p, s) wr_spy_stat((p), (s))
#define lstat(p, s) wr_spy_lstat((p), (s))
#define rename(a, b) wr_spy_rename((a), (b))
#define fsync(fd) wr_spy_fsync((fd))
#define opendir(p) wr_spy_opendir((p))
#endif
