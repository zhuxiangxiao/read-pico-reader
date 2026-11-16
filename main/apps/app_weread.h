/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：首页请求微信读书开书，仍经过云端冲突检查。
 * English: Home requests a WeRead book through cloud conflict checking.
 */
#pragma once
#include <stdbool.h>
bool app_weread_request_open(const char *uri);
