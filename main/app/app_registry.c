/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 产品页清单。五栏导航单独映射，持久页面编号保持兼容。
 *
 * Five-tab visual order is separate from stable persisted page IDs.
 */

#include "app_registry.h"

extern const app_desc_t app_book;
extern const app_desc_t app_image;
extern const app_desc_t app_cloud_sync;
extern const app_desc_t app_weread;
extern const app_desc_t app_dashboard;
extern const app_desc_t app_files;
extern const app_desc_t app_device_settings;

// 正式产品页面清单；工厂测试与历史示例不进入用户导航。
// Production page registry; factory diagnostics and legacy examples are not user-navigable.
static const app_desc_t* const s_apps[] = {
    &app_dashboard,
    &app_book,
    &app_files,
    &app_device_settings,
    &app_image,
    &app_cloud_sync,
    &app_weread,
};

#define APP_COUNT ((int)(sizeof(s_apps) / sizeof(s_apps[0])))
static const uint8_t s_nav_ids[APP_NAV_COUNT] = {0, 1, 6, 2, 3};
int app_nav_id(int position) {
    return position >= 0 && position < APP_NAV_COUNT ? s_nav_ids[position] : -1;
}
int app_nav_position(int page_id) {
    for (int i=0;i<APP_NAV_COUNT;++i) if (s_nav_ids[i]==page_id) return i;
    return -1;
}
int app_menu_id(int position) {
    if(position<APP_NAV_COUNT)return app_nav_id(position);
    return position<APP_COUNT ? position-1 : -1;
}
int app_menu_position(int page_id) {
    for(int i=0;i<APP_COUNT;++i)if(app_menu_id(i)==page_id)return i;
    return -1;
}

int app_count(void) {
    return APP_COUNT;
}

const app_desc_t* app_at(int index) {
    if (index < 0 || index >= APP_COUNT) return NULL;
    return s_apps[index];
}

int app_index_of(const app_desc_t* app) {
    for (int i = 0; i < APP_COUNT; i++) {
        if (s_apps[i] == app) return i;
    }
    return -1;
}

const app_desc_t* app_home_page(void) {
    return &app_dashboard;
}
