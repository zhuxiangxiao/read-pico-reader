/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信扫码、离线书架、封面、搜索与进度选择；render只画已接管的快照。
 * English: WeChat QR, offline shelf, covers, search and progress choices; render paints owned snapshots only.
 * 冻结：内容下载由单一worker执行；退页/锁屏/拔卡先取消并等待，只有进入阅读时保留自有WiFi。
 * Frozen: One worker downloads content; cancel/join on exit/lock/media loss, retaining owned WiFi only for reader entry.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize ("Os")
#endif
#include "app.h"
#include "esp_log.h"
#include "app_registry.h"
#include "app_content_open.h"
#include "app_transfer_mode.h"
#include "app_weread.h"
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_chapter.h"
#include "weread_store.h"
#include "book_weread_uri.h"
#include "ui_kit.h"
#include "ui_nav.h"
#include "ui_gesture.h"
#include "ui_wifi_qr.h"
#include "ui_keyboard.h"
#include "read_pico_search.h"
#include "read_pico_transfer.h"
#include "read_pico_sd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROWS 6
#define QR_AREA ((EpdRect){72,218,540,540})
#define QUERY_FIELD ((EpdRect){36,200,612,76})
#define MAIN_ACTION ((EpdRect){36,1050,292,72})
#define SIDE_ACTION ((EpdRect){356,1050,292,72})
extern const app_desc_t app_transfer;
static weread_book_t *s_books;
static size_t s_count, s_visible;
static uint16_t *s_matches;
static int s_page;
static bool s_grid = true;
static bool s_auth, s_search, s_wifi, s_keep_network, s_wake, s_sync_after_init;
static bool s_conflict, s_clear, s_logout;
static bool s_more_sync,s_syncing;
static uint64_t s_next_sync;
static uint64_t s_sync_check, s_sync_not_before, s_last_input, s_owner;
static unsigned s_epoch;
static bool s_force_cover;
static char s_query[65], s_uid[64], s_qr[256], s_id[64], s_requested[64], s_message[128];
static weread_progress_t s_local, s_cloud;
static ui_text_edit_t s_edit;
static uint64_t s_next_poll, s_next_action;
static EpdRect s_area;
static bool s_cover_remote;
typedef struct { uint8_t *pixels; bool checked; weread_cover_retry_t retry; uint64_t key; } row_cover_t;
static row_cover_t s_covers[ROWS];

bool app_weread_request_open(const char *uri) {
    if (!book_path_is_weread(uri) || strlen(uri + 9) >= sizeof(s_requested)) return false;
    strcpy(s_requested, uri + 9); return true;
}
static void message(const char *s) { wr_display_copy(s_message, sizeof(s_message), s); }
static void release_covers(void) { for (int i = 0; i < ROWS; ++i) { free(s_covers[i].pixels); memset(&s_covers[i], 0, sizeof(s_covers[i])); } }
static void stop_job(void) { weread_runtime_cancel_join(); weread_result_t r; if (weread_runtime_poll(&r)) weread_runtime_result_free(&r);s_syncing=false;s_more_sync=weread_shelf_sync_pending(); }
static void filter_page(bool reset) {
    free(s_matches); s_matches = s_count ? wr_alloc(s_count * sizeof(*s_matches)) : NULL; s_visible = 0;
    if (s_count && !s_matches) { message("内存不足，请重试"); return; }
    for (size_t i = 0; i < s_count; ++i)
        if (!s_query[0] || read_pico_search_match(s_books[i].title, s_query) || read_pico_search_match(s_books[i].author, s_query))
            s_matches[s_visible++] = (uint16_t)i;
    if(reset){s_page=0;release_covers();}
    else {int pages=(int)((s_visible+ROWS-1)/ROWS);if(s_page>=pages)s_page=pages?pages-1:0;}
}
static void filter(void){filter_page(true);}
static weread_book_t *row_book(int row) {
    size_t i = (size_t)s_page * ROWS + (size_t)row;
    return s_matches && i < s_visible ? &s_books[s_matches[i]] : NULL;
}
static bool covers_checked(void) {
    for(int i=0;i<ROWS;++i)if(row_book(i)&&!s_covers[i].checked)return false;
    return true;
}
static uint64_t page_signature(void) {
    uint64_t h=14695981039346656037ULL;
    for(int i=0;i<ROWS;++i){weread_book_t *b=row_book(i);if(!b)break;
        const char *fields[]={b->book_id,b->title,b->author,b->cover};
        for(unsigned j=0;j<4;++j)for(const unsigned char *p=(const unsigned char *)fields[j];;p++){
            h=(h^*p)*1099511628211ULL;if(!*p)break;
        }
        h=(h^b->progress)*1099511628211ULL;
    }
    return h^s_visible^((uint64_t)s_page<<32);
}
static void adopt_books(weread_result_t *r) {
    row_cover_t old[ROWS];char ids[ROWS][64]={{0}};
    memcpy(old,s_covers,sizeof(old));memset(s_covers,0,sizeof(s_covers));
    for(int i=0;i<ROWS;++i){weread_book_t *b=row_book(i);if(b)strcpy(ids[i],b->book_id);}
    free(s_books);s_books=r->books;r->books=NULL;s_count=r->count;filter_page(false);
    for(int i=0;i<ROWS;++i){weread_book_t *b=row_book(i);if(!b)continue;
        uint64_t k=weread_cover_key(b->book_id,b->cover);
        for(int j=0;j<ROWS;++j)if(!strcmp(ids[j],b->book_id)&&old[j].key==k){s_covers[i]=old[j];old[j].pixels=NULL;break;}
        s_covers[i].key=k;
    }
    for(int i=0;i<ROWS;++i)free(old[i].pixels);
}
static EpdRect row_rect(int i) {
    return s_grid ? (EpdRect){36 + i % 3 * 212,246 + i / 3 * 304,188,286} :
        (EpdRect){36,246 + i * 110,612,100};
}
static void wifi_close(app_ctx_t *ctx) { if (s_wifi && app_transfer.on_exit) app_transfer.on_exit(ctx); s_wifi = false; }
static void wifi_open(app_ctx_t *ctx) {
    weread_runtime_stop(); ui_wifi_qr_clear(); s_uid[0]=s_qr[0]=0;
    s_more_sync=s_syncing=false;
    s_wifi = true; app_transfer_request_wifi_setup(); if (app_transfer.on_enter) app_transfer.on_enter(ctx);
}
static bool start(weread_job_t job, const char *id, const char *value) {
    if (weread_runtime_start(job, id, value)) {s_syncing=job==WR_JOB_SYNC;if(s_syncing)s_more_sync=false;return true;}
    message("操作暂不可用，请稍后重试"); return false;
}
static void open_id(const char *id) {
    stop_job(); strcpy(s_id, id); s_conflict = s_clear = false;
    s_more_sync=false;
    message("正在准备阅读…"); start(WR_JOB_OPEN, id, NULL);
}
static void enter(app_ctx_t *ctx) {
    s_last_input=ctx->now_ms;s_sync_check=ctx->now_ms+1200;s_force_cover=false;
    s_keep_network = s_wifi = s_search = s_conflict = s_clear = s_logout = s_auth = false;
    s_uid[0] = s_qr[0] = s_message[0] = 0; s_next_poll = s_next_action = 0;
    s_sync_after_init = true; s_wake=false; start(WR_JOB_INIT, NULL, NULL);
    s_more_sync=false;
}
static void leave(app_ctx_t *ctx) {
    wifi_close(ctx); ui_keyboard_end(); ui_wifi_qr_clear(); s_requested[0]=0;
    if (!s_keep_network) weread_runtime_stop(); else stop_job();
    release_covers(); free(s_books); s_books = NULL; free(s_matches); s_matches = NULL; s_count = s_visible = 0;
    s_more_sync=s_syncing=false;
}
static void media_lost(app_ctx_t *ctx) {
    wifi_close(ctx); weread_runtime_media_lost(); release_covers();
    free(s_books); s_books = NULL; free(s_matches); s_matches = NULL; s_count = s_visible = 0;
    s_auth = false; s_requested[0] = s_uid[0] = s_qr[0] = 0;ui_wifi_qr_clear();message("TF 卡不可用，请重新插入");
}
static void before_lock(app_ctx_t *ctx) {
    wifi_close(ctx); weread_runtime_stop(); s_wake = true; ui_keyboard_end(); s_search = false;
}
static void media_ready(app_ctx_t *ctx) { (void)ctx; s_wake = true; }
static void cover_draw(uint8_t *fb, EpdRect box, const uint8_t *pixels) {
    if (!pixels) { ui_fill_round_rect(fb, box, 8, 0xe0); return; }
    for (int y = 0; y < box.height; ++y) for (int x = 0; x < box.width; ++x)
        epd_draw_pixel(box.x + x, box.y + y,
            pixels[(size_t)(y * WEREAD_COVER_HEIGHT / box.height) * WEREAD_COVER_WIDTH + x * WEREAD_COVER_WIDTH / box.width], fb);
}
static void render(app_ctx_t *ctx, uint8_t *fb) {
    if (s_wifi) { app_transfer.render(ctx, fb); return; }
    ui_clear_page(fb); ui_nav_status(fb); ui_nav_back(fb,36,79);
    ui_text_vc(fb,342,107,34,"微信读书",EPD_DRAW_ALIGN_CENTER,false);
    if (s_message[0]) ui_text_box(fb,(EpdRect){36,140,612,48},19,s_message,EPD_DRAW_ALIGN_CENTER,false);
    if (s_search) {
        ui_text_input_draw(fb,&s_edit,QUERY_FIELD,26,false,NULL); ui_keyboard_draw(fb,560);
        ui_text_vc(fb,342,350,23,"搜索书名或作者",EPD_DRAW_ALIGN_CENTER,false); return;
    }
    if (s_conflict || s_clear || s_logout) {
        ui_text_box(fb,(EpdRect){60,350,564,180},28,s_conflict?"手机与本机阅读位置不同，请选择继续位置":
            s_clear?"清理这本书的离线内容？阅读进度会保留":"退出微信读书登录？",EPD_DRAW_ALIGN_CENTER,false);
        if(s_conflict){char info[128];snprintf(info,sizeof(info),"本机 %u%%  ·  云端 %u%%",(unsigned)s_local.progress,(unsigned)s_cloud.progress);
            ui_text_vc(fb,342,590,24,info,EPD_DRAW_ALIGN_CENTER,false);}
        ui_draw_button(fb,MAIN_ACTION,s_conflict?"继续本机":"取消",false);
        ui_draw_button(fb,SIDE_ACTION,s_conflict?"使用云端":s_clear?"清理缓存":"退出登录",false); return;
    }
    if (!s_auth) {
        if (s_qr[0]) ui_wifi_qr_draw(fb,QR_AREA);
        else ui_text_box(fb,(EpdRect){60,340,564,130},26,"用微信扫码登录后，可浏览书架并阅读已缓存章节",EPD_DRAW_ALIGN_CENTER,false);
        ui_text_vc(fb,342,820,21,"请在手机上完成扫码和验证码确认",EPD_DRAW_ALIGN_CENTER,false);
        ui_draw_button(fb,MAIN_ACTION,"连接 WiFi",false);
        ui_draw_button(fb,SIDE_ACTION,s_qr[0]?"刷新二维码":"扫码登录",false); return;
    }
    ui_draw_button(fb,(EpdRect){36,188,390,48},s_query[0]?s_query:"搜索书名 / 作者",false);
    ui_draw_button(fb,(EpdRect){450,188,198,48},s_grid?"切换列表":"切换网格",false);
    for (int i = 0; i < ROWS; ++i) {
        weread_book_t *book = row_book(i); if (!book) break;
        EpdRect r = row_rect(i), cover = s_grid ? (EpdRect){r.x+38,r.y+8,112,154} : (EpdRect){r.x+8,r.y+5,64,88};
        cover_draw(fb,cover,s_covers[i].pixels);
        EpdRect title = s_grid ? (EpdRect){r.x+4,r.y+170,r.width-8,66} : (EpdRect){r.x+90,r.y+8,498,48};
        ui_text_box(fb,title,s_grid?21:24,book->title,EPD_DRAW_ALIGN_LEFT,false);
        char info[160]; snprintf(info,sizeof(info),"%u%%  %s",(unsigned)book->progress,book->author);
        ui_text_box(fb,s_grid?(EpdRect){r.x+4,r.y+238,r.width-8,40}:(EpdRect){r.x+90,r.y+60,498,32},17,info,EPD_DRAW_ALIGN_LEFT,false);
    }
    if (!s_visible) ui_text_vc(fb,342,480,26,s_count?"未找到匹配书籍":"书架暂无缓存，请刷新",EPD_DRAW_ALIGN_CENTER,false);
    int pages = (int)((s_visible + ROWS - 1) / ROWS); if (!pages) pages = 1;
    char label[64]; snprintf(label,sizeof(label),"%d / %d · %u 本",s_page+1,pages,(unsigned)s_visible);
    ui_draw_button(fb,(EpdRect){36,924,120,64},"上一页",false);
    ui_draw_button(fb,(EpdRect){528,924,120,64},"下一页",false);
    ui_text_vc(fb,342,956,23,label,EPD_DRAW_ALIGN_CENTER,false);
    unsigned loaded=0,available=0;
    for(int i=0;i<ROWS;++i)if(row_book(i)){++available;if(s_covers[i].pixels)++loaded;}
    char loading[64];snprintf(loading,sizeof(loading),"封面 %u / %u",loaded,available);
    ui_text_vc(fb,342,1014,18,loading,EPD_DRAW_ALIGN_CENTER,false);
    ui_draw_button(fb,MAIN_ACTION,"刷新书架",false); ui_draw_button(fb,SIDE_ACTION,"连接 WiFi",false);
    ui_text_vc(fb,160,1164,19,"长按书籍清理缓存",EPD_DRAW_ALIGN_CENTER,false);
    ui_text_vc(fb,560,1164,19,"退出登录",EPD_DRAW_ALIGN_CENTER,false);
}
static app_redraw_t keyboard_update(app_ctx_t *ctx) {
    ui_keyboard_update_t u=ui_keyboard_update(ctx->fb,560,QUERY_FIELD,26,false,NULL,true);
    s_area=u.area; return u.layout?APP_REDRAW_PAGE:u.area.width?APP_REDRAW_AREA:APP_REDRAW_NONE;
}
static app_redraw_t gesture(app_ctx_t *ctx,const ui_gesture_event_t *e) {
    s_last_input=ctx->now_ms;
    if (s_wifi) return app_transfer.on_gesture?app_transfer.on_gesture(ctx,e):APP_REDRAW_NONE;
    if (s_search) {
        if(e->type==UI_GESTURE_PRESS){ui_keyboard_press(e->x0,e->y0,560,ctx->now_ms);return keyboard_update(ctx);}
        if(e->type==UI_GESTURE_CANCEL){ui_keyboard_release();return keyboard_update(ctx);}
        if(e->type!=UI_GESTURE_TAP)return APP_REDRAW_NONE;
        if(e->y0<160&&e->x0<120){ui_keyboard_end();s_search=false;filter();return APP_REDRAW_PAGE;}
        ui_keyboard_release();
        if(ui_text_input_tap(&s_edit,QUERY_FIELD,26,false,NULL,e->x0,e->y0))return keyboard_update(ctx);
        ui_keyboard_result_t r=ui_keyboard_tap(e->x0,e->y0,560,ctx->now_ms);
        if(r==UI_KEYBOARD_DONE){ui_keyboard_end();s_search=false;filter();return APP_REDRAW_PAGE;}
        return keyboard_update(ctx);
    }
    if(e->type!=UI_GESTURE_TAP&&e->type!=UI_GESTURE_LONG_PRESS)return APP_REDRAW_NONE;
    unsigned x=e->x0,y=e->y0;
    if(y<160&&x<120){ctx->request_return=true;return APP_REDRAW_NONE;}
    if(s_conflict||s_clear||s_logout) {
        if(ui_rect_hit(MAIN_ACTION,x,y)){
            if(s_conflict) {weread_runtime_choose(s_id,&s_local,false);message("正在准备阅读…");}
            s_conflict=s_clear=s_logout=false; return APP_REDRAW_PAGE;
        }
        if(ui_rect_hit(SIDE_ACTION,x,y)){
            if(s_conflict){weread_runtime_choose(s_id,&s_cloud,true);message("正在准备阅读…");}
            else start(s_clear?WR_JOB_CLEAR:WR_JOB_LOGOUT,s_clear?s_id:NULL,NULL);
            s_conflict=s_clear=s_logout=false;return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if(!s_auth){
        if(ui_rect_hit(MAIN_ACTION,x,y))wifi_open(ctx);
        else if(ui_rect_hit(SIDE_ACTION,x,y)){stop_job();s_uid[0]=s_qr[0]=0;ui_wifi_qr_clear();if(start(WR_JOB_QR,NULL,NULL))message("正在生成二维码…");}
        return APP_REDRAW_PAGE;
    }
    if(ui_rect_hit((EpdRect){36,188,390,48},x,y)){
        stop_job();s_search=true;ui_text_edit_init(&s_edit,s_query,sizeof(s_query));ui_keyboard_begin(&s_edit,false);return APP_REDRAW_PAGE;}
    if(ui_rect_hit((EpdRect){450,188,198,48},x,y)){stop_job();s_grid=!s_grid;return APP_REDRAW_PAGE;}
    if(ui_rect_hit(MAIN_ACTION,x,y)){if(s_syncing&&weread_runtime_busy())return APP_REDRAW_NONE;stop_job();
        s_force_cover=true;s_sync_not_before=0;
        for(int i=0;i<ROWS;++i)if(!s_covers[i].pixels){memset(&s_covers[i].retry,0,sizeof(s_covers[i].retry));s_covers[i].checked=false;}
        start(WR_JOB_SYNC,NULL,NULL);message("正在同步书架…");return APP_REDRAW_PAGE;}
    if(ui_rect_hit(SIDE_ACTION,x,y)){wifi_open(ctx);return APP_REDRAW_PAGE;}
    if(y>=1128&&x>=456){stop_job();s_logout=true;return APP_REDRAW_PAGE;}
    if(y>=924&&y<988){
        int pages=(int)((s_visible+ROWS-1)/ROWS);if(!pages)pages=1;
        int target=s_page+(x<180?-1:x>500?1:0);
        if(target>=0&&target<pages&&target!=s_page){stop_job();s_page=target;release_covers();return APP_REDRAW_PAGE;}
    }
    for(int i=0;i<ROWS;++i){weread_book_t *b=row_book(i);if(b&&ui_rect_hit(row_rect(i),x,y)){
        if(e->type==UI_GESTURE_LONG_PRESS){stop_job();strcpy(s_id,b->book_id);s_clear=true;}
        else open_id(b->book_id);
        return APP_REDRAW_PAGE;
    }}
    return APP_REDRAW_NONE;
}
static app_redraw_t tick(app_ctx_t *ctx) {
    if(s_wake){s_wake=false;s_sync_after_init=false;start(WR_JOB_INIT,NULL,NULL);return APP_REDRAW_PAGE;}
    if(s_wifi){
        app_redraw_t r=app_transfer.on_tick?app_transfer.on_tick(ctx):APP_REDRAW_NONE;
        read_pico_transfer_status_t net;read_pico_transfer_get_status(&net);
        if(!ctx->consumed&&!ctx->request_return&&!ctx->request_app&&!ctx->request_menu&&net.network_ready&&
           net.mode==READ_PICO_TRANSFER_MODE_STA&&net.state==READ_PICO_TRANSFER_READY){
            wifi_close(ctx);start(s_auth?WR_JOB_SYNC:WR_JOB_QR,NULL,NULL);return APP_REDRAW_PAGE;
        }return r;
    }
    weread_result_t r;
    if(weread_runtime_poll(&r)){
        uint64_t old_page=page_signature();char old_message[sizeof(s_message)];strcpy(old_message,s_message);
        bool local_cover=r.job==WR_JOB_COVER&&!s_cover_remote;
        s_auth=r.authenticated;
        if(r.job==WR_JOB_INIT){uint64_t owner=weread_cover_key("","");unsigned epoch=weread_runtime_cache_epoch();
            if(owner!=s_owner||epoch!=s_epoch){s_sync_not_before=0;release_covers();s_owner=owner;s_epoch=epoch;}
        }
        if(!(r.error==ESP_ERR_NO_MEM&&!r.books)&&(r.books||r.job==WR_JOB_INIT||r.job==WR_JOB_SYNC||r.job==WR_JOB_LOGOUT||(r.job==WR_JOB_POLL&&r.authenticated))){
            adopt_books(&r);
        }
        if(r.job==WR_JOB_QR&&r.error==ESP_OK){
            bool encoded=ui_wifi_qr_prepare_text(r.qr_url);
            ESP_LOGI("weread","QR encoding ready=%d",encoded);
            if(encoded){strcpy(s_uid,r.uid);strcpy(s_qr,r.qr_url);message("请用微信扫码");s_next_poll=ctx->now_ms+2000;}
            else{s_uid[0]=s_qr[0]=0;message("二维码生成失败，请刷新重试");}
        }
        if(r.job==WR_JOB_POLL){
            s_next_poll=ctx->now_ms+2000;
            if(r.authenticated){s_uid[0]=s_qr[0]=0;ui_wifi_qr_clear();start(WR_JOB_SYNC,NULL,NULL);message("正在同步书架…");}
            else if(r.error!=ESP_OK)message("获取登录结果失败，正在重试");
            else if(r.qr_state==WEREAD_QR_STATE_WAITING_SCAN)message("请用微信扫码");
            else if(r.qr_state==WEREAD_QR_STATE_WAITING_CONFIRM)message("已扫码，请在手机上确认登录");
            else if(r.qr_state==WEREAD_QR_STATE_NEED_OTP)message("请在手机上完成验证码确认");
            else if(r.qr_state==WEREAD_QR_STATE_TIMEOUT||r.qr_state==WEREAD_QR_STATE_CANCELLED){s_uid[0]=0;message("二维码已失效，请刷新");}
            else if(r.qr_state==WEREAD_QR_STATE_ERROR)message("登录未完成，请刷新二维码重试");
        }
        if(r.conflict){s_local=r.local;s_cloud=r.cloud;strcpy(s_id,r.id);s_conflict=true;message("");}
        if(r.open_ready&&r.error==ESP_OK){
            char uri[80];snprintf(uri,sizeof(uri),"weread://%s",r.id);
            if(app_book_request_open(uri)){s_keep_network=true;ctx->request_app=app_at(1);}
        }
        if(r.job==WR_JOB_COVER){for(int i=0;i<ROWS;++i){weread_book_t *b=row_book(i);if(b&&!strcmp(b->book_id,r.id)){
            s_covers[i].checked=true;
            if(r.cover){free(s_covers[i].pixels);s_covers[i].pixels=r.cover;r.cover=NULL;}
            else if(s_cover_remote)weread_cover_retry_result(&s_covers[i].retry,
                r.error!=ESP_OK&&r.http_status>=200&&r.http_status<300?0:r.http_status,ctx->now_ms);
            if(r.cover_retry_seconds&&(!s_cover_remote||s_covers[i].retry.failures>=3))
                s_covers[i].retry.not_before_ms=ctx->now_ms+(uint64_t)r.cover_retry_seconds*1000;
        }}}
        if(r.job==WR_JOB_CLEAR){release_covers();message(r.error==ESP_OK?"离线内容已清理":"清理失败，请重试");}
        if(r.error!=ESP_OK&&r.job!=WR_JOB_COVER&&r.job!=WR_JOB_POLL)
            message(r.error==ESP_ERR_NO_MEM?"内存不足，请稍后重试":r.error==WEREAD_ERR_POSITION?"阅读位置与目录不匹配，请先在手机打开本书后重试":r.error==ESP_ERR_INVALID_RESPONSE?"返回数据不完整，请稍后重试":r.error==WEREAD_ERR_PURCHASE?"请在微信读书手机端购买或领取此章节":r.error==WEREAD_ERR_SOLD_OUT?"这本书已下架，暂时无法下载":"网络暂不可用，可阅读已缓存章节");
        if(r.job==WR_JOB_INIT){
            bool initial=s_sync_after_init;
            s_sync_after_init=false;
            if(r.error==ESP_OK){
                if(s_auth){message("单击阅读，长按清理离线内容");s_sync_check=ctx->now_ms+1200;
                    if(!s_count&&!weread_shelf_cache_fresh((uint64_t)time(NULL))&&ctx->now_ms>=s_sync_not_before){start(WR_JOB_SYNC,NULL,NULL);message("正在加载第一页…");}}
                else if(initial){start(WR_JOB_QR,NULL,NULL);message("正在生成二维码…");}
            }
            else message("TF 卡不可用，请重新挂载后进入");
        }
        if(r.job==WR_JOB_SYNC){
            s_syncing=false;s_more_sync=r.shelf_loading;s_next_sync=ctx->now_ms+800;
            s_sync_check=ctx->now_ms+(r.error==ESP_OK?300000:60000);
            s_sync_not_before=r.error==ESP_OK?0:ctx->now_ms+60000;
            if(r.error!=ESP_OK)s_more_sync=false;
            if(r.error==ESP_OK){
                char text[128];
                if(s_more_sync)snprintf(text,sizeof(text),"已加载 %u / %u 本，后台继续加载…",(unsigned)s_count,(unsigned)r.shelf_total);
                else if(r.shelf_skipped)snprintf(text,sizeof(text),"已加载 %u 本，部分书籍暂时不可用",(unsigned)s_count);
                else snprintf(text,sizeof(text),"单击阅读，长按清理离线内容");
                message(text);
            }
        }
        if(r.job==WR_JOB_LOGOUT)s_requested[0]=0;
        bool requested_cached=false;
        for(size_t i=0;i<s_count && s_requested[0];++i)if(!strcmp(s_books[i].book_id,s_requested))requested_cached=true;
        if(s_requested[0]&&s_auth&&((r.job==WR_JOB_INIT&&requested_cached)||r.job==WR_JOB_SYNC)){
            stop_job();open_id(s_requested);s_requested[0]=0;
        }
        bool unchanged=r.job==WR_JOB_SYNC&&old_page==page_signature()&&!strcmp(old_message,s_message);
        bool batch_cover=local_cover&&!covers_checked();
        if(local_cover)s_next_action=ctx->now_ms;
        weread_runtime_result_free(&r);return unchanged||batch_cover?APP_REDRAW_NONE:APP_REDRAW_PAGE;
    }
    if(weread_runtime_busy()||s_search||s_conflict||s_clear||s_logout||ctx->request_app||ctx->request_return)return APP_REDRAW_NONE;
    bool idle=ctx->now_ms>=s_last_input&&ctx->now_ms-s_last_input>=1000;
    if(s_auth&&s_more_sync&&ctx->now_ms>=s_next_sync&&idle&&covers_checked()){start(WR_JOB_SYNC,NULL,"continue");return APP_REDRAW_NONE;}
    if(s_auth&&!s_more_sync&&idle&&covers_checked()&&ctx->now_ms>=s_sync_check&&ctx->now_ms>=s_sync_not_before){
        s_sync_check=ctx->now_ms+300000;
        if(!weread_shelf_cache_fresh((uint64_t)time(NULL))){start(WR_JOB_SYNC,NULL,NULL);return APP_REDRAW_NONE;}
    }
    if(!s_auth&&s_uid[0]&&(uint64_t)ctx->now_ms>=s_next_poll){s_next_poll=ctx->now_ms+2000;start(WR_JOB_POLL,NULL,s_uid);return APP_REDRAW_NONE;}
    if(s_auth&&(uint64_t)ctx->now_ms>=s_next_action){
        for(int i=0;i<ROWS;++i){weread_book_t *b=row_book(i);if(!b||s_covers[i].pixels||s_covers[i].checked)continue;
            s_covers[i].key=weread_cover_key(b->book_id,b->cover);s_cover_remote=false;
            start(WR_JOB_COVER,b->book_id,s_force_cover?"retry":NULL);s_next_action=ctx->now_ms+150;return APP_REDRAW_NONE;
        }
        for(int i=0;i<ROWS;++i){weread_book_t *b=row_book(i);if(!b||s_covers[i].pixels)continue;
            char url[512];if(!b->cover[0]||weread_cover_retry_url(b->cover,&s_covers[i].retry,ctx->now_ms,url,sizeof(url))!=ESP_OK)continue;
            if(!idle)continue;
            s_cover_remote=true;start(WR_JOB_COVER,b->book_id,url);break;
        }s_next_action=ctx->now_ms+150;
        if(covers_checked())s_force_cover=false;
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t key(app_ctx_t *ctx,int key) {
    s_last_input=ctx->now_ms;
    if(s_wifi&&app_transfer.on_key)return app_transfer.on_key(ctx,key);
    if(s_search){ui_keyboard_end();s_search=false;filter();return APP_REDRAW_PAGE;}
    if(s_conflict||s_clear||s_logout){s_conflict=s_clear=s_logout=false;return APP_REDRAW_PAGE;}
    ctx->request_return=true;return APP_REDRAW_NONE;
}
static bool navigation(app_ctx_t *ctx){return s_wifi&&(!app_transfer.navigation_visible||app_transfer.navigation_visible(ctx));}
static bool present(app_ctx_t *ctx,app_redraw_t redraw){return s_wifi&&app_transfer.present&&app_transfer.present(ctx,redraw);}
static EpdRect area(app_ctx_t *ctx){(void)ctx;return s_area;}
const app_desc_t app_weread={.title="微信读书",.detail="扫码登录与离线阅读",.enter_full=true,
    .navigation_visible=navigation,.on_enter=enter,.on_exit=leave,.on_media_lost=media_lost,
    .on_media_ready=media_ready,.on_before_lock=before_lock,.render=render,.on_tick=tick,
    .on_gesture=gesture,.on_key=key,.present=present,.area_hint=area};
