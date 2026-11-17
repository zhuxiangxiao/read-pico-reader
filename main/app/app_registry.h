/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 正式产品页面的清单。菜单、主循环和开机页都从这里取。
 *
 * Registry of production pages used by the menu, app loop, and boot route.
 */

#pragma once

#include "app.h"

#ifdef __cplusplus
extern "C" {
#endif

int app_count(void);
const app_desc_t* app_at(int index);
/// 找不到时返回 -1。/ Returns -1 when not found.
int app_index_of(const app_desc_t* app);

/// 导航顺序独立于持久页面编号，保留旧文件/设置的恢复身份。/ Visual order is independent of persisted page IDs.
enum { APP_NAV_COUNT = 5 };
int app_nav_id(int position);
int app_nav_position(int page_id);
int app_menu_id(int position);
int app_menu_position(int page_id);

/// 开机默认产品页。/ Default production page at boot.
const app_desc_t* app_home_page(void);

#ifdef __cplusplus
}
#endif
