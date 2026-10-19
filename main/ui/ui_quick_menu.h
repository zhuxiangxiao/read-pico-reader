/* SPDX-License-Identifier: Apache-2.0
 * 中文：顶部快捷菜单的独立手势和纯绘图；动作由主循环执行。
 * English: Independent gesture recognition and pure drawing for the top quick layer; the loop owns actions.
 * 用户修订：展开时生成液态玻璃；菜单存续期间只保留四圆键4bpp小缓存，点按立即局部反馈，状态变化不重画底页。
 * User revision: generate glass on open; retain four small packed key snapshots while open, acknowledge presses locally and never repaint the underlay for status changes.
 * 冻结：仅边缘起手；不做连续动画或整帧缓存；关闭、切页、睡眠和错误时释放控件缓存。
 * Frozen: edge-origin capture only, no continuous animation or full-frame cache; release key snapshots on close, navigation, sleep and errors.
 */
#pragma once
#include "app.h"

#define UI_QUICK_EDGE_PX 32
#define UI_QUICK_SWIPE_PX 72
#define UI_QUICK_HEIGHT 264
#define UI_QUICK_CIRCLE_Y 156
#define UI_QUICK_RADIUS 54
#define UI_QUICK_HIT_RADIUS 66

typedef enum {
    UI_QUICK_NONE, UI_QUICK_OPEN, UI_QUICK_CLOSE,
    UI_QUICK_WIFI, UI_QUICK_BLUETOOTH, UI_QUICK_FULL, UI_QUICK_LOCK,
} ui_quick_action_t;

typedef struct {
    bool open, tracking, swallow, moved;
    uint16_t x0, y0;
    int hit;
    int64_t started_ms;
} ui_quick_menu_t;

/// 清除层和手势；切页、睡眠、媒体失效时使用。/ Clear layer and contact state at navigation, sleep and media-loss boundaries.
void ui_quick_menu_reset(ui_quick_menu_t *menu);
/// 返回true表示整条触摸序列被接管；多指/读错不执行按钮，收起之后仍吞到抬手。
/// True owns the whole contact stream; multitouch/read errors never activate controls, and dismissal still swallows until release.
bool ui_quick_menu_feed(ui_quick_menu_t *menu, const app_ctx_t *ctx, bool allowed,
                       ui_quick_action_t *action);
/// 取消接触但保留已打开的层。/ Cancel a contact while retaining an already open layer.
void ui_quick_menu_cancel_input(ui_quick_menu_t *menu);
/// 只覆盖顶部，保留下层其他像素；44px图标与底栏相同，状态栏正常绘制。
/// Cover the top only, leaving other pixels intact; use the same 44px icon size as navigation and draw the normal status bar.
void ui_quick_menu_draw(uint8_t *fb, bool wifi, bool bluetooth);
/// 当前按住的圆键，取消/滑出/释放时为-1。/ The held key, or -1 after cancellation, movement or release.
int ui_quick_menu_pressed(const ui_quick_menu_t *menu);
/// 恢复原玻璃小块并画状态/圆内按压，不分配、不模糊、不排字。/ Restore a glass tile and paint its state/interior press without allocating, blurring or shaping text.
bool ui_quick_menu_draw_button(uint8_t *fb, int button, bool active, bool pressed, EpdRect *dirty);
/// 只重画清晰状态栏，供网络状态变化时局刷。/ Repaint only the sharp status strip for network changes.
void ui_quick_menu_draw_status(uint8_t *fb);
/// 释放菜单存续期的控件缓存。/ Release the control cache retained while the menu is open.
void ui_quick_menu_end(void);
EpdRect ui_quick_menu_rect(void);
