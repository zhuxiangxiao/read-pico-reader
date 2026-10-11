/* SPDX-License-Identifier: Apache-2.0
 * 中文：运行真实主循环，用确定性触摸和任务延时退出验证调度。
 * English: Run the real loop with deterministic touch and task-delay exit.
 */
#include "app_loop.h"
#include "ui_gesture.h"
#include "ui_menu.h"
#include "ui_power_dialog.h"
#include "ui_quick_menu.h"
#include "boot_state.h"
uint8_t app_settings_system_contrast(void) {return 100;}
void pico_boot_ready(void) {}
void app_book_cover_mode_changed(void) {}
void app_home_cover_mode_changed(void) {}
esp_err_t pico_boot_save_resume(const pico_resume_t *resume) { (void)resume; return ESP_OK; }
void pico_boot_clear_resume(void) {}
bool app_book_resume_context(char *p,size_t c,bool *f) { (void)p;(void)c;(void)f;return false; }
#include "read_pico_airdrop.h"
#include "read_pico_transfer.h"
#include "ble_page_turner.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

const app_desc_t app_book = {.title="book"};
const app_desc_t app_transfer = {.title="transfer"};
EpdRect ui_power_dialog_rect(void) {return (EpdRect){72,240,540,700};}
void ui_power_dialog_draw(uint8_t* fb) {(void)fb;}
ui_power_action_t ui_power_dialog_handle(const ui_gesture_event_t* event) {(void)event;return UI_POWER_ACTION_NONE;}
void ui_power_final_draw(uint8_t* fb,bool restarting) {(void)fb;(void)restarting;}

static jmp_buf done;
typedef struct { int x, y, count, error; } sample_t;
static sample_t samples[32];
static int sample_count, step, ticks, enters, exits, touch_calls, keys[3], events[16];
static int menus, highlights, restores, fulls, mode, tick_consumed[32];
static int long_keys;
static uint8_t idle_minutes;
static int idle_locks, before_locks;
static bool online_busy, airdrop_active;
static bool ble_enabled, saved_wifi, upload_busy, click_enabled, click_active;
static read_pico_transfer_status_t transfer_status;
static int quick_draws, quick_gl, quick_du, wifi_starts, wifi_stops, ble_resets, click_presses, click_restores;

static app_redraw_t long_key(app_ctx_t* c, int k) { assert(k==UI_KEY_2); ++long_keys; c->request_menu=true; return APP_REDRAW_NONE; }
static app_redraw_t response;
static bool request_on_touch, request_on_tick, menu_on_tick, menu_on_touch, cancel_clobber;
static bool home_on_touch, home_on_tick, home_on_key, start_second;
static int home_enters, home_renders, last_menu_leaf, rendered_leaf;
static const app_desc_t* menu_background;
static app_desc_t first, second;
static bool full_tick, lock_due, font_due, shake_activity;
static int64_t time_offset, time_step;
static int du_areas, gl_areas;
static bool main_fast, main_water;
int app_settings_main_refresh_mode(void) {return main_water?APP_MAIN_REFRESH_WATER:main_fast?APP_MAIN_REFRESH_FAST:APP_MAIN_REFRESH_NORMAL;}
static int shelf_exit_arms;
static int nav_arms, nav_draws, nav_full_arms, nav_area_draws;
static const void *nav_armed;
static bool main_source_visible, main_target_visible;
static bool media_test, sd_font, saved_sd_font;
static bool mounted_steps[32], present_steps[32];
static int media_lost, media_ready, builtin_opens, font_opens, probes, loss_step;
int E0470_WAVEFORM;
int E0470_FOLLOW_WAVEFORM;
enum EpdDrawError update_display_area_diff_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {
    if (a.height==UI_QUICK_HEIGHT) {assert(a.x==0&&a.y==0&&a.width==684);++quick_du;}
    if (a.y==1120) assert(w==&E0470_FOLLOW_WAVEFORM&&m==MODE_DU&&a.height==48&&a.width==50);
    return update_display_area_with(h,w,m,a);
}
enum EpdDrawError update_display_area_full_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {
    assert(a.x==0&&a.y==0&&a.width==684&&a.height==UI_QUICK_HEIGHT);++quick_gl;
    return update_display_area_with(h,w,m,a);
}
void ui_quick_menu_draw(uint8_t *fb,bool wifi,bool bluetooth) {(void)fb;(void)wifi;(void)bluetooth;++quick_draws;}
bool app_settings_ble_turner(void) {return ble_enabled;}
void app_settings_set_ble_turner(bool enabled) {ble_enabled=enabled;}
void ble_pt_reset_failure(void) {++ble_resets;}
bool ble_pt_pop_key(ble_pt_event_t *e) {(void)e;return false;}
bool ble_pt_pop_raw(ble_pt_raw_t *e) {(void)e;return false;}
void read_pico_transfer_get_status(read_pico_transfer_status_t *s) {*s=transfer_status;}
int read_pico_transfer_get_saved_wifi(char *ssid,bool *configured) {strcpy(ssid,"saved");*configured=saved_wifi;return ESP_OK;}
int read_pico_transfer_start(const read_pico_transfer_cfg_t *cfg) {assert(cfg->mode==READ_PICO_TRANSFER_MODE_STA&&cfg->network_only);++wifi_starts;transfer_status.mode=cfg->mode;transfer_status.state=1;return ESP_OK;}
bool read_pico_transfer_try_stop_if_idle(void) {if(upload_busy)return false;++wifi_stops;transfer_status.state=0;return true;}
void ui_click_feedback_reset(void) {click_active=false;}
void ui_click_feedback_begin(uint8_t *fb) {(void)fb;ui_click_feedback_reset();}
bool ui_click_feedback_active(void) {return click_active;}
bool ui_click_feedback_press(uint8_t *fb,int x,int y,EpdRect *r) {(void)fb;if(!click_enabled||y!=1141)return false;(void)x;*r=(EpdRect){60,1120,50,48};click_active=true;++click_presses;return true;}
bool ui_click_feedback_release(uint8_t *fb,EpdRect *r) {(void)fb;if(!click_active)return false;click_active=false;++click_restores;*r=(EpdRect){60,1120,50,48};return true;}
bool ui_click_feedback_cancel_at(int x,int y) {return x>120||y!=1141;}
int cst836u_read(void* h, cst836u_touch_t* t) {
    (void)h;
    sample_t s = samples[step];
    *t = (cst836u_touch_t){ .touched=s.count>0, .count=s.count, .x=s.x, .y=s.y };
    t->points[0].active=s.count>0;
    return s.error;
}
int cst836u_get_info(void* h,cst836u_info_t* i) {(void)h;(void)i;return 0;}
const char* esp_err_to_name(int e) {(void)e;return "test";}
int64_t esp_timer_get_time(void) {return time_offset+(int64_t)(step+1)*time_step;}
void vTaskDelay(int ms) {(void)ms;if (++step>=sample_count) longjmp(done,1);}
bool continuous_du_init(void) {return true;}
void guard_draw_result(EpdiyHighlevelState* h,enum EpdDrawError e) {(void)h;assert(e==0);}
enum EpdDrawError update_display_area_with(EpdiyHighlevelState*h,const void*w,int m,EpdRect a) {(void)h;(void)w;(void)a;nav_area_draws+=nav_armed!=NULL;nav_armed=NULL;if(m==MODE_DU)du_areas++;else if(m==MODE_GL16)gl_areas++;return 0;}
enum EpdDrawError update_display_full(EpdiyHighlevelState*h) {(void)h;fulls++;nav_full_arms+=nav_armed!=NULL;nav_armed=NULL;return 0;}
enum EpdDrawError update_display_mode(EpdiyHighlevelState*h,int m) {(void)h;(void)m;mode++;return 0;}
enum EpdDrawError update_display_mode_diff(EpdiyHighlevelState*h,int m) {(void)h;(void)m;mode++;return 0;}
enum EpdDrawError update_display_fast_page(EpdiyHighlevelState*h) {(void)h;mode++;nav_draws+=nav_armed!=NULL;nav_armed=NULL;return 0;}
enum EpdDrawError update_display_white(EpdiyHighlevelState*h) {(void)h;return 0;}
bool display_take_white_exit(void) {return false;}
void display_main_transition_cancel(void) {nav_armed=NULL;}
void display_main_transition_arm(const void* owner, bool shelf_exit, bool changing_page) {(void)changing_page;display_main_transition_cancel();nav_armed=owner;++nav_arms;shelf_exit_arms+=shelf_exit;}
void display_main_transition_disarm(void) {nav_armed=NULL;}
void rails_idle_check(int64_t n) {(void)n;}
bool read_pico_pmu_ready(void) {return lock_due;}
read_pico_pmu_key_action_t read_pico_pmu_take_key_action(void) {return READ_PICO_PMU_KEY_SHORT;}
void enter_lock_and_sleep(EpdiyHighlevelState*h,int64_t*t,void*a,void*tp,bool reader) {(void)tp;(void)h;(void)t;(void)a;(void)reader;assert(!nav_armed);++idle_locks;lock_due=false;time_offset+=1000000;}
void app_lock_wait_key_idle(int ms) {(void)ms;}
void app_enter_host_sleep(app_sleep_mode_t mode) {(void)mode;longjmp(done,1);}
void app_restart_host(void) {longjmp(done,1);}
void epd_poweroff(void) {}
const char* app_settings_font_path(void) {return "builtin";}
bool app_settings_reader_power_turn(void) {return false;}
bool app_settings_staged_shutdown(void) {return false;}
bool ttf_font_path_is_builtin(const char*p) {(void)p;return !font_due&&!saved_sd_font;}
bool ttf_font_ready(void) {return true;}
bool ttf_font_is_builtin(void) {return !sd_font;}
const char* ttf_font_path(void) {return "other";}
int ttf_font_open(const char*p) {(void)p;font_opens++;sd_font=true;return 0;}
int ttf_font_open_builtin(void) {assert(media_lost>0);builtin_opens++;sd_font=font_due=false;return 0;}
int read_pico_sd_get_info(read_pico_sd_info_t*i) {i->present=media_test?present_steps[step]:font_due;i->mounted=media_test?mounted_steps[step]:font_due;return media_test&&!i->mounted?ESP_ERR_INVALID_STATE:0;}
void read_pico_sd_start_probe(void) {probes++;}
static void lost(app_ctx_t*c) {(void)c;media_lost++;loss_step=step;}
static void ready(app_ctx_t*c) {(void)c;media_ready++;}
EpdRect ui_content_refresh_area(void) {return (EpdRect){0,0,684,1000};}
const app_desc_t* app_home_page(void) {return &first;}
const app_desc_t* app_at(int i) {return i==0?&first:i==1?&second:NULL;}
int app_index_of(const app_desc_t* app) {return app==&first?0:app==&second?1:-1;}
int ui_key_hit_test(uint16_t x,uint16_t y) {return y>=1300 && x<480?x/160:-1;}
bool ui_menu_handle_hit_test(uint16_t x,uint16_t y) {return x>600 && y>1100 && y<1300;}
int ui_menu_leaf_for_app(const app_desc_t*a) {return a==&second?1:0;}
int ui_menu_leaf_count(void) {return 2;}
void ui_draw_menu_page(uint8_t*f,const app_desc_t*a,int l) {(void)f;menu_background=a;last_menu_leaf=l;menus++;}
int ui_menu_hit_test(uint16_t x,uint16_t y,int l) {(void)l;if(x==550&&y==500)return UI_MENU_HIT_NEXT;return x<500&&y>=100&&y<300?(y-100)/100:-1;}
bool ui_menu_row_rect(int l,int r,EpdRect*out) {(void)l;*out=(EpdRect){0,r*100+100,500,100};return r>=0&&r<2;}
void ui_draw_menu_row_pressed(uint8_t*f,const app_desc_t*a,int l,int r,bool p) {(void)f;(void)a;(void)l;(void)r;if(p)highlights++;else restores++;}
static void enter(app_ctx_t*c) {(void)c;enters++;}
static void leave(app_ctx_t*c) {(void)c;exits++;}
static void render(app_ctx_t*c,uint8_t*f) {(void)c;(void)f;}
static void home_enter(app_ctx_t*c) {c->leaf=0;enters++;home_enters++;}
static void home_render(app_ctx_t*c,uint8_t*f) {(void)f;rendered_leaf=c->leaf;home_renders++;}
static app_redraw_t touch(app_ctx_t*c,const cst836u_touch_t*t) {
    (void)t;touch_calls++;assert(c->pressed&&c->consumed);
    if(request_on_touch)c->request_app=&second;
    if(menu_on_touch)c->request_menu=true;
    if(home_on_touch)c->request_return=true;
    return response;
}
static app_redraw_t gesture(app_ctx_t*c,const ui_gesture_event_t*e) {
    events[e->type]++;
    if(e->type==UI_GESTURE_CANCEL&&cancel_clobber) {c->request_app=NULL;c->request_menu=true;c->request_return=false;}
    if(e->type==UI_GESTURE_PRESS&&request_on_touch)c->request_app=&second;
    if(e->type==UI_GESTURE_PRESS&&home_on_touch)c->request_return=true;
    return response;
}
static app_redraw_t key(app_ctx_t*c,int k) {keys[k]++;if(home_on_key)c->request_return=true;return APP_REDRAW_NONE;}
static app_redraw_t tick(app_ctx_t*c) {
    ticks++;tick_consumed[step]=c->consumed;
    if (shake_activity && step == 0) c->user_activity = true;
    if(request_on_tick)c->request_app=&second;
    if(menu_on_tick)c->request_menu=true;
    if(home_on_tick){c->request_return=true;home_on_tick=false;}
    return full_tick ? APP_REDRAW_FULL : APP_REDRAW_NONE;
}
static void reset(void) {
    ble_enabled=saved_wifi=upload_busy=click_enabled=click_active=false;
    transfer_status=(read_pico_transfer_status_t){0};
    quick_draws=quick_gl=quick_du=wifi_starts=wifi_stops=ble_resets=click_presses=click_restores=0;
    main_fast=main_water=false;shelf_exit_arms=0;idle_minutes=0;idle_locks=before_locks=0;online_busy=airdrop_active=false;
    nav_arms=nav_draws=nav_full_arms=nav_area_draws=0;nav_armed=NULL;
    main_source_visible=main_target_visible=true;
    media_test=sd_font=saved_sd_font=false;media_lost=media_ready=builtin_opens=font_opens=probes=0;loss_step=-1;
    memset(mounted_steps,0,sizeof(mounted_steps));memset(present_steps,0,sizeof(present_steps));
    long_keys=0;
    home_on_touch=home_on_tick=home_on_key=start_second=false;
    home_enters=home_renders=0;rendered_leaf=-1;last_menu_leaf=-1;menu_background=NULL;
    full_tick=lock_due=font_due=shake_activity=false;time_offset=0;time_step=10000;du_areas=gl_areas=0;
    memset(samples,0,sizeof(samples));memset(keys,0,sizeof(keys));memset(events,0,sizeof(events));memset(tick_consumed,0,sizeof(tick_consumed));
    sample_count=step=ticks=enters=exits=touch_calls=menus=highlights=restores=fulls=mode=0;
    request_on_touch=request_on_tick=menu_on_tick=menu_on_touch=cancel_clobber=false;response=APP_REDRAW_NONE;
    first=(app_desc_t){.title="first",.render=render,.on_enter=enter,.on_exit=leave,.on_touch=touch,.on_tick=tick,.on_key=key};
    second=(app_desc_t){.title="second",.render=render,.on_enter=enter};
}
static void add(int x,int y,int count,int error) {samples[sample_count++]=(sample_t){x,y,count,error};}
static void run(void) {app_loop_config_t cfg={.first_app=start_second?&second:&first};if(!setjmp(done))app_loop_run(&cfg);}
static app_redraw_t consumed_return_tick(app_ctx_t*c) { if(step==0){c->leaf=7;c->request_app=&second;}else c->request_return=true;return APP_REDRAW_NONE; }
static app_redraw_t return_tick(app_ctx_t*c) { c->request_return=true; return APP_REDRAW_NONE; }
static app_redraw_t origin_tick(app_ctx_t*c) { c->leaf=7; if(step==0 && request_on_tick)c->request_app=&second; return APP_REDRAW_NONE; }
static void home_case(void) {
    reset();start_second=true;
    second=first;second.title="transfer";
    first.on_enter=home_enter;first.render=home_render;first.on_tick=NULL;first.on_touch=NULL;
}
static bool product_menu_disabled(app_ctx_t *ctx) {(void)ctx;return false;}
static void before_lock(app_ctx_t *ctx) {(void)ctx;++before_locks;}
static bool main_first(app_ctx_t* ctx) {(void)ctx;return main_source_visible;}
static bool main_second(app_ctx_t* ctx) {(void)ctx;return main_target_visible;}
static void enter_subview(app_ctx_t* ctx) {enter(ctx);main_target_visible=false;}
static void enable_main_tabs(void) {main_fast=true;first.main_page_visible=main_first;second.main_page_visible=main_second;}
static app_redraw_t nav_return_tick(app_ctx_t* ctx) {
    if(step==0)ctx->request_app=&second;
    else if(step==1)ctx->request_return=true;
    return APP_REDRAW_NONE;
}
static app_redraw_t main_late_tick(app_ctx_t* ctx) {(void)ctx;return step == 1 ? response : APP_REDRAW_NONE;}
int main(void) {
    reset();add(100,400,1,0);add(110,400,1,0);add(0,0,0,0);run();
    assert(touch_calls==1&&ticks==3&&!tick_consumed[0]);
    reset();response=APP_REDRAW_AREA;add(100,400,1,0);run();assert(touch_calls==1&&tick_consumed[0]);
    reset();first.on_gesture=gesture;response=APP_REDRAW_AREA;add(100,400,1,0);add(0,0,0,0);run();
    assert(!touch_calls&&events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1&&tick_consumed[0]);
    reset();first.on_gesture=gesture;add(100,400,1,0);add(0,0,0,9);add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();request_on_touch=true;menu_on_touch=true;add(100,400,1,0);run();assert(enters==2&&exits==1&&ticks==0&&!menus);
    reset();request_on_tick=true;menu_on_tick=true;add(0,0,0,0);run();assert(enters==2&&exits==1&&ticks==1&&!menus);
    reset();first.on_gesture=gesture;request_on_touch=true;cancel_clobber=true;add(100,400,1,0);run();
    assert(enters==2&&events[UI_GESTURE_CANCEL]==1&&!menus);
    reset();menu_on_tick=true;add(0,0,0,0);add(0,0,0,0);run();assert(menus==1&&ticks==1);
    reset();first.owns_keys=true;for(int k=0;k<3;k++){add(k*160+80,1500,1,0);add(0,0,0,0);}run();
    assert(keys[0]==1&&keys[1]==1&&keys[2]==1&&!menus&&fulls==1);
    reset();add(240,1500,1,0);add(0,0,0,0);add(400,1500,1,0);run();assert(!keys[1]&&!keys[2]&&menus==1&&fulls==2);
    reset();add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(0,0,0,0);run();
    assert(highlights==1&&restores==1&&enters==2&&exits==1);
    reset();add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(550,220,1,0);add(100,220,1,0);add(0,0,0,0);run();
    assert(highlights==1&&restores==1&&enters==1&&!exits);
    reset();add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(0,0,0,9);run();assert(restores==1&&enters==1);
    reset();first.on_gesture=gesture;full_tick=true;add(100,400,1,0);add(0,0,0,0);run();
    assert(events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();first.on_gesture=gesture;font_due=true;time_offset=4000000;add(100,400,1,0);add(0,0,0,0);run();
    // 开机首帧前已加载保存字体，因此首个手势无需再被延迟字体切换取消。
    // The saved font is ready before the first frame, so the first gesture is no longer cancelled.
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1&&font_opens==1);
    reset();first.owns_keys=true;add(650,1150,1,0);add(0,0,0,0);add(240,1500,1,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(!keys[1]&&!keys[2]&&fulls==2&&menus==2);
    reset();add(650,1150,1,0);add(0,0,0,0);
    for(int i=0;i<3;i++){add(100,220,1,0);add(550,220,1,0);add(0,0,0,0);}run();
    assert(du_areas==6&&gl_areas==1);
    reset();time_step=1000000;add(650,1150,1,0);add(0,0,0,0);add(100,220,1,0);add(550,220,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(du_areas==2&&gl_areas==1);
    reset();first.on_gesture=gesture;time_step=1000000;lock_due=true;
    add(100,400,1,0);add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(!lock_due&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]&&tick_consumed[2]);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=200000;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();
    assert(keys[1]==1&&long_keys==1&&menus==1);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=200000;
    add(240,1500,1,0);add(0,0,0,0);run();assert(keys[1]==1&&!long_keys&&!menus);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=300000;
    add(240,1500,1,0);add(80,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);run();assert(!long_keys);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=300000;
    add(240,1500,1,0);add(0,0,0,9);add(240,1500,1,0);add(240,1500,1,0);run();assert(!long_keys);
    reset();first.owns_keys=true;first.on_key_long=long_key;time_step=1000000;lock_due=true;
    add(0,0,0,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(0,0,0,0);run();assert(!lock_due&&!long_keys);
    home_case();home_on_tick=true;add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&last_menu_leaf==1&&menu_background==&second);
    assert(home_renders==0&&ticks==1&&fulls==1&&mode==1);
    home_case();home_on_tick=true;add(0,0,0,0);add(400,1500,1,0);add(0,0,0,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&home_renders==0);
    home_case();home_on_touch=true;request_on_touch=true;menu_on_touch=true;add(100,400,1,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&home_renders==0&&ticks==0);
    home_case();second.on_gesture=gesture;home_on_touch=true;cancel_clobber=true;response=APP_REDRAW_PAGE;add(100,400,1,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&events[UI_GESTURE_CANCEL]==1&&home_renders==0);
    home_case();second.owns_keys=true;second.on_key_long=long_key;home_on_key=true;time_step=300000;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);run();
    assert(exits==0&&home_enters==0&&menus==1&&!long_keys&&ticks==0);
    reset();first.render=home_render;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(550,500,1,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==2&&enters==3&&last_menu_leaf==1&&menu_background==&first&&home_renders==1);
    reset();first.render=home_render;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(550,500,1,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(exits==2&&enters==3&&rendered_leaf==7&&home_renders==2);
    reset();request_on_tick=true;first.render=home_render;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==2&&enters==3&&!menus&&home_renders==2&&rendered_leaf==7);
    reset();first.render=home_render;first.on_enter=home_enter;first.on_tick=origin_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(550,500,1,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);
    add(100,220,1,0);add(0,0,0,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(exits==4&&enters==5&&last_menu_leaf==1&&menu_background==&first&&home_renders==2&&rendered_leaf==7);
    reset();start_second=true;second.on_tick=origin_tick;second.on_exit=leave;second.on_enter=home_enter;second.render=home_render;first.on_tick=return_tick;
    add(0,0,0,0);add(650,1150,1,0);add(0,0,0,0);add(100,120,1,0);add(0,0,0,0);add(0,0,0,0);add(400,1500,1,0);run();
    assert(exits==2&&enters==3&&last_menu_leaf==1&&menu_background==&second&&home_renders==2&&rendered_leaf==7);
    reset();first.on_tick=consumed_return_tick;second.on_tick=return_tick;second.on_exit=leave;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(exits==2&&enters==3&&menus==1&&last_menu_leaf==0&&menu_background==&first);
    // 拔卡先关资源再回退字体，取消本轮按键，持续无卡不重复通知。
    // Removal closes consumers before fallback, drops the current key and notifies only once.
    reset();media_test=sd_font=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(80,1500,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==1&&builtin_opens==1&&!keys[0]&&fulls==2);
    // 后台页即使被菜单遮盖也释放资源，仍保留菜单视图。
    // A menu-covered page still releases resources and retains the menu view.
    reset();media_test=sd_font=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;
    add(650,1150,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&builtin_opens==1&&menus==2&&ticks==0);
    // 延迟挂载后拔卡同样被捕获，使用内置字体无需再打开一次。
    // Removal after a late mount is detected without reopening an already built-in font.
    reset();media_test=true;mounted_steps[1]=true;time_step=600000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&loss_step==2&&!builtin_opens);
    // 首次无卡时后插卡应触发探测，挂载完成只通知页面一次。
    // First insertion after an empty boot probes, then notifies the active page once.
    reset();media_test=true;time_step=600000;first.on_media_ready=ready;
    present_steps[1]=present_steps[2]=present_steps[3]=true;
    mounted_steps[2]=mounted_steps[3]=true;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(probes>=1&&media_ready==1&&!media_lost);
    // 手势和长按在拔卡边界取消，不能把旧按住状态变成新的动作。
    // Removal cancels gestures and holds without turning an old contact into another action.
    reset();media_test=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;first.on_gesture=gesture;
    add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(media_lost==1&&events[UI_GESTURE_CANCEL]==1&&!events[UI_GESTURE_TAP]);
    reset();media_test=true;mounted_steps[0]=true;time_step=600000;first.on_media_lost=lost;first.owns_keys=true;first.on_key_long=long_key;
    add(240,1500,1,0);add(240,1500,1,0);add(240,1500,1,0);run();
    assert(media_lost==1&&keys[1]==1&&!long_keys);
    // 失效后不自动探测；显式恢复的挂载可重新加载保存字体并再次检测拔卡。
    // No automatic probing after invalidation; an explicit remount restores saved-font loading and loss detection.
    reset();media_test=sd_font=saved_sd_font=true;mounted_steps[0]=true;time_step=4000000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==1&&builtin_opens==1&&!probes&&!font_opens);
    reset();media_test=sd_font=saved_sd_font=true;mounted_steps[0]=mounted_steps[2]=true;time_step=4000000;first.on_media_lost=lost;
    add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(media_lost==2&&builtin_opens==2&&!probes&&font_opens==1);
    reset();idle_minutes=1;time_step=60000000;first.on_before_lock=before_lock;
    add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&before_locks==1); // Real scheduler flushes before automatic lock.
    reset();idle_minutes=1;time_step=60000000;shake_activity=true;
    add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=60000000;shake_activity=true;
    add(0,0,0,0);add(0,0,0,0);run();assert(idle_locks==1); /* Activity is consumed once, not latched. */
    reset();idle_minutes=0;time_step=600000000;
    add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=600000000;online_busy=true;
    add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=600000000;airdrop_active=true;
    add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=600000000;first.holds_pmu=true;
    add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();idle_minutes=1;time_step=30000000;
    add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);add(0,0,0,0);run();assert(!idle_locks);
    reset();first.owns_keys=true;first.on_gesture=gesture;first.menu_handle_enabled=product_menu_disabled;
    add(636,1150,1,0);add(0,0,0,0);run();
    assert(!menus&&events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1);
    // 首帧保持全刷；切页一次完成，按住、松开、空闲都不安排第二次刷新。
    // Retain the initial full update; finish navigation once without another draw on hold, release or idle.
    reset();enable_main_tabs();add(0,0,0,0);run();assert(!nav_arms&&!nav_draws&&!nav_full_arms);
    reset();enable_main_tabs();request_on_touch=true;add(100,400,1,0);add(100,400,1,0);run();
    assert(nav_arms==1&&nav_draws==1&&!nav_armed);
    reset();enable_main_tabs();request_on_touch=true;add(100,400,1,0);add(100,400,1,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_arms==1&&nav_draws==1&&!nav_armed&&!nav_full_arms);
    reset();enable_main_tabs();first.on_tick=nav_return_tick;add(0,0,0,0);add(0,0,0,0);second.on_tick=return_tick;add(0,0,0,0);run();
    assert(nav_arms==2&&nav_draws==2&&!nav_armed&&shelf_exit_arms==1);
    // 目录延后完成重绘和同页局部更新也获得固定底栏许可，没有额外 idle 推屏。
    // Deferred directory completion and local repaint receive frozen-navigation permission without an idle redraw.
    reset();enable_main_tabs();first.on_tick=nav_return_tick;second.on_tick=main_late_tick;
    response=APP_REDRAW_PAGE;add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_draws==2&&nav_arms==2&&!nav_armed&&!nav_full_arms);
    reset();enable_main_tabs();first.on_tick=nav_return_tick;second.on_tick=main_late_tick;
    response=APP_REDRAW_AREA;add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_draws==1&&nav_area_draws==1&&nav_arms==2&&!nav_armed&&!nav_full_arms);
    // 子视图、非主页面保留普通出口，显式全刷优先。
    // Subviews and non-main pages retain ordinary output; explicit full refresh has priority.
    reset();enable_main_tabs();main_source_visible=false;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(!nav_arms&&!nav_draws);
    reset();enable_main_tabs();second.on_enter=enter_subview;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(!nav_arms&&!nav_draws);
    reset();enable_main_tabs();second.main_page_visible=NULL;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(!nav_arms&&!nav_draws);
    reset();enable_main_tabs();second.enter_full=true;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();assert(nav_arms==1&&nav_full_arms==1&&!nav_draws);
    reset();enable_main_tabs();main_fast=false;request_on_touch=true;add(100,400,1,0);add(0,0,0,0);run();
    assert(nav_arms==1&&nav_draws==1&&!shelf_exit_arms&&!nav_full_arms);
    reset();enable_main_tabs();main_fast=false;first.on_tick=nav_return_tick;second.on_tick=main_late_tick;
    response=APP_REDRAW_AREA;add(0,0,0,0);add(0,0,0,0);add(0,0,0,0);run();
    assert(nav_draws==1&&nav_area_draws==1&&nav_arms==2&&!nav_full_arms);
    reset();enable_main_tabs();lock_due=true;time_step=3000000;add(0,0,0,0);add(0,0,0,0);run();
    assert(idle_locks==1&&!nav_arms&&!nav_draws);
    puts("main-tab scheduler: single immediate draw, no hold/release/idle redraw, rapid return, root/subview boundaries and full-output priority passed");
    // 跑真实边缘识别和主循环，验证不会漏给底层页面或按住重复动作。
    // Run the real edge recognizer and loop to reject underlay leakage and repeated hold actions.
    reset();first.on_gesture=gesture;
    add(300,20,1,0);add(300,110,1,0);add(300,120,1,0);add(0,0,0,0);
    add(300,500,1,0);add(0,0,0,0);add(100,400,1,0);add(0,0,0,0);run();
    assert(quick_draws==1&&quick_gl==2&&!quick_du);
    assert(events[UI_GESTURE_PRESS]==1&&events[UI_GESTURE_TAP]==1);
    reset();main_fast=true;saved_wifi=true;
    add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);
    add(85,156,1,0);add(85,156,1,0);add(0,0,0,0);
    add(85,156,1,0);add(0,0,0,0);run();
    assert(wifi_starts==1&&wifi_stops==1&&quick_gl==3&&!quick_du&&!touch_calls);
    reset();upload_busy=true;transfer_status=(read_pico_transfer_status_t){.state=1,.mode=READ_PICO_TRANSFER_MODE_STA};
    add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);add(85,156,1,0);add(0,0,0,0);run();
    assert(!wifi_starts&&!wifi_stops&&transfer_status.state==1);
    reset();add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);add(85,156,1,0);add(0,0,0,0);run();
    assert(!wifi_starts&&!wifi_stops&&!touch_calls);
    reset();add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);
    add(256,156,1,0);add(0,0,0,0);add(256,156,1,0);add(0,0,0,0);run();
    assert(!ble_enabled&&ble_resets==1&&!touch_calls);
    reset();first.on_before_lock=before_lock;
    add(300,20,1,0);add(300,110,1,0);add(0,0,0,0);
    add(427,156,1,0);add(0,0,0,0);add(598,156,1,0);add(0,0,0,0);run();
    assert(idle_locks==1&&before_locks==1&&fulls>=2&&!touch_calls);
    for(int policy=0;policy<3;++policy){
        reset();main_fast=policy==1;main_water=policy==2;first.on_gesture=gesture;click_enabled=true;
        add(85,1141,1,0);add(85,1141,1,0);add(0,0,0,0);run();
        assert(click_presses==1&&click_restores==1&&!click_active&&ticks==1&&du_areas==2&&!gl_areas);
        reset();enable_main_tabs();main_fast=policy==1;main_water=policy==2;click_enabled=true;request_on_touch=true;
        add(85,1141,1,0);add(0,0,0,0);run();
        assert(click_presses==1&&click_restores==1&&!click_active&&du_areas==2&&!gl_areas&&nav_draws==1);
    }
    reset();click_enabled=true;add(85,1141,1,0);add(85,1141,2,0);add(0,0,0,0);run();
    assert(click_presses==1&&click_restores==1&&!click_active);
    reset();click_enabled=true;add(85,1141,1,0);add(0,0,0,1);add(0,0,0,0);run();
    assert(click_presses==1&&click_restores==1&&!click_active);
    puts("quick layer/click scheduler: modal ownership, local BW/gray, one saved-network connect/disconnect, upload/no-credential guards, BLE toggle, full/lock and release/multitouch/read-error restoration passed");
    puts("app_loop: 50 scheduler scenarios passed, including automatic lock, save callback, input reset and busy guards");
    return 0;
}

/* 中文：硬件 BLE 与联网任务替身。/ English: Hardware BLE and online worker shims. */
void ble_pt_sync(bool enabled) { (void)enabled; }
void ble_pt_poll(void) {}
bool pico_online_busy(void) { return online_busy; }
bool read_pico_airdrop_active(void) { return airdrop_active; }

uint8_t app_settings_auto_lock_minutes(void){return idle_minutes;}
uint32_t ble_pt_input_serial(void){return 0;}
