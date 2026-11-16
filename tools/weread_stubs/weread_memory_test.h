/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 受限内存池测试观测。/ Constrained memory-pool test observations.
 */
#pragma once
#include <stddef.h>
#include <stdint.h>
uint32_t weread_test_memory_capability(void *pointer);
size_t weread_test_memory_used(uint32_t caps);
void weread_test_memory_fail_after(int count);
void weread_test_memory_report(void);
