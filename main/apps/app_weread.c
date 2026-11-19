/*
 * SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：微信扫码、离线书架、封面、搜索与进度选择；render只画已接管的快照。
 * English: WeChat QR, offline shelf, covers, search and progress choices; render paints owned snapshots only.
 * 冻结：内容下载由单一worker执行；退页/锁屏/拔卡先取消并等待，只有进入阅读时保留自有WiFi。
 * Frozen: One worker downloads content; cancel/join on exit/lock/media loss, retaining owned WiFi only for reader entry.
 */
#if defined(ESP_PLATFORM) && defined(__GNUC__) && !defined(__clang__) && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
#pragma GCC optimize ("Os")
#endif
#include "kiiko_board_config.h"
#include "app.h"
#include "display.h"
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
#include "ui_menu.h"
#include "ui_shelf.h"
#include "ui_root_style.h"
#include "settings.h"
#include "ui_gesture.h"
#include "ui_wifi_qr.h"
#include "ui_keyboard.h"
#include "read_pico_search.h"
#include "read_pico_transfer.h"
#include "read_pico_sd.h"
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
#include "metalio_cover_cache.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ROWS 9
#define QR_AREA ((EpdRect){72,218,540,540})
#define QUERY_FIELD ((EpdRect){36,200,612,76})
#define MAIN_ACTION ((EpdRect){36,1008,292,64})
#define SIDE_ACTION ((EpdRect){356,1008,292,64})
extern const app_desc_t app_transfer;
static weread_shelf_item_t *s_books;
static size_t s_count, s_visible;
static uint16_t *s_matches;
static int s_page;
static int s_style=6;
static uint8_t s_order=1;
static bool s_sort_menu,s_more_menu,s_key_mode;
static bool s_auth, s_search, s_wifi, s_keep_network, s_wake, s_sync_after_init, s_init_pending;
static bool s_clear, s_logout;
static bool s_more_sync,s_syncing;
static uint64_t s_next_sync;
static uint64_t s_sync_check, s_sync_not_before, s_last_input, s_owner;
static unsigned s_epoch;
static bool s_force_cover;
static char s_search_draft[65],s_query[65], s_uid[64], s_qr[256], s_id[64], s_requested[64], s_message[128];
static ui_text_edit_t s_edit;
static uint64_t s_next_poll, s_next_action;
static EpdRect s_area;
static bool s_cover_remote, s_requested_home, s_open_home, s_reader_failed;
static bool s_cover_warm, s_preconnected, s_cover_dirty, s_open_queued;
// 只有 open_id 发起的开书在途时才刷新阶段提示行，其他任务/错误消息不受影响。
// The open-stage line refreshes only while an open_id()-initiated open is in flight, never after it ends.
static bool s_open_stage;
// 下载字节每 32 KiB 变化一次，书架消息行最多每 2 秒重绘一次，阶段切换仍立即显示。
// Download bytes change every 32 KiB, so repaint the shelf message line at most every 2 s; a stage change still shows immediately.
static uint64_t s_open_stage_at;
static unsigned s_open_stage_kind;
static uint64_t s_open_owner;static unsigned s_open_epoch;
static uint64_t s_warm_page, s_cover_paint_at;
static unsigned s_warm_slot;
static int s_warm_direction=1;
typedef struct { uint8_t *pixels; bool checked; weread_cover_retry_t retry; uint64_t key; } row_cover_t;
static row_cover_t s_covers[ROWS];
static int display_style(void){return s_query[0]?5:s_style;}
static int page_rows(void){return ui_shelf_rows(display_style());}
static int page_count(void){return s_visible?(int)((s_visible+page_rows()-1)/page_rows()):1;}
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
static unsigned s_cover_cache_epoch;
static const weread_shelf_item_t *find_shelf_book(const char *book_id) {
    if (!book_id || !book_id[0]) return NULL;
    for (size_t i = 0; i < s_count; ++i)
        if (!strcmp(s_books[i].book_id, book_id)) return &s_books[i];
    return NULL;
}
static uint64_t book_id_hash64(const char *id) {
    uint64_t h = UINT64_C(14695981039346656037);
    if (!id) return h;
    for (const unsigned char *p = (const unsigned char *)id; *p; ++p)
        h = (h ^ *p) * UINT64_C(1099511628211);
    return h;
}
static metalio_cover_key_t raw_cover_ram_key(const weread_shelf_item_t *b) {
    // 共享缓存按 key.field[0] 归一族、field[2]/field[3] 判旧版本；家族标记必须按书号区分，
    // 否则同族其他书会被当成旧版本清掉（只剩一本命中）。书号、封面键、账号代次、所有者与
    // 176x240 尺寸仍全部入键。
    // The shared cache groups one key.field[0] family and retires stale versions on field[2]/field[3];
    // the family marker must be per book or sibling books get discarded as stale versions. Book id,
    // cover key, owner/epoch and the 176x240 dimensions all stay in the key.
    uint64_t book = book_id_hash64(b ? b->book_id : "");
    return (metalio_cover_key_t){{
        UINT64_C(0x5752435652310001) ^ book,
        book,
        b ? b->cover_key : 0,
        ((uint64_t)s_cover_cache_epoch << 32) | (uint32_t)s_epoch,
        s_owner,
        WEREAD_COVER_WIDTH,
        WEREAD_COVER_HEIGHT,
        0
    }};
}
static void cache_store_raw_cover(const char *book_id, const uint8_t *pixels) {
    if (!pixels) return;
    const weread_shelf_item_t *b = find_shelf_book(book_id);
    if (!b || !b->has_cover) return;
    metalio_cover_key_t key = raw_cover_ram_key(b);
    (void)metalio_cover_cache_put(&key, pixels, (size_t)WEREAD_COVER_WIDTH * WEREAD_COVER_HEIGHT,
        (metalio_cover_meta_t){WEREAD_COVER_WIDTH, WEREAD_COVER_HEIGHT, false});
}
static void prime_covers_from_ram(void) {
    const size_t bytes = (size_t)WEREAD_COVER_WIDTH * WEREAD_COVER_HEIGHT;
    for (int i = 0; i < page_rows(); ++i) {
        weread_shelf_item_t *b = s_visible && s_matches && (size_t)s_page * page_rows() + (size_t)i < s_visible
            ? &s_books[s_matches[(size_t)s_page * page_rows() + (size_t)i]] : NULL;
        if (!b || !b->has_cover || s_covers[i].pixels || s_covers[i].checked) continue;
        metalio_cover_key_t key = raw_cover_ram_key(b);
        metalio_cover_meta_t meta = {0};
        if (!metalio_cover_cache_get(&key, NULL, 0, &meta) ||
            meta.width != WEREAD_COVER_WIDTH || meta.height != WEREAD_COVER_HEIGHT) continue;
        uint8_t *buf = wr_alloc(bytes);
        if (!buf) continue;
        if (metalio_cover_cache_get(&key, buf, bytes, &meta)) {
            s_covers[i].pixels = buf;
            s_covers[i].key = b->cover_key;
            s_covers[i].checked = true;
        } else {
            free(buf);
        }
    }
}
#endif

bool app_weread_request_open(const char *uri) {
    if (!book_path_is_weread(uri) || strlen(uri + 9) >= sizeof(s_requested)) return false;
    strcpy(s_requested, uri + 9); s_requested_home=true; return true;
}
void app_weread_reader_failed(void){s_reader_failed=true;}
static void message(const char *s) { wr_display_copy(s_message, sizeof(s_message), s); }
static void release_covers(void) { for (int i = 0; i < ROWS; ++i) { free(s_covers[i].pixels); memset(&s_covers[i], 0, sizeof(s_covers[i])); } s_cover_dirty=false;s_cover_paint_at=0;s_next_action=0; }
static void stop_job(void) { s_open_queued=s_init_pending=s_open_stage=false;weread_runtime_cancel_join();s_cover_warm=false; weread_result_t r; if (weread_runtime_poll(&r)) weread_runtime_result_free(&r);s_syncing=false;s_more_sync=weread_shelf_sync_pending(); }
static int compare_match(const void *a,const void *b){
    const weread_shelf_item_t *x=&s_books[*(const uint16_t*)a],*y=&s_books[*(const uint16_t*)b];
    int mode=ui_shelf_order_mode(s_order),c=0;
    uint64_t xt=mode==1?x->read_update_time:mode==2?x->update_time:x->progress;
    uint64_t yt=mode==1?y->read_update_time:mode==2?y->update_time:y->progress;
    if(mode)c=xt<yt?-1:xt>yt?1:0;else c=strcasecmp(x->title,y->title);
    if(ui_shelf_order_desc(s_order))c=-c;
    return c?c:strcmp(x->book_id,y->book_id);
}
static bool filter_page(bool reset) {
    uint16_t *matches=s_count?wr_alloc(s_count*sizeof(*matches)):NULL;
    if(s_count&&!matches){message("内存不足，请重试");return false;}
    size_t count=0;
    for(size_t i=0;i<s_count;++i)
        if(!s_query[0]||read_pico_search_match(s_books[i].title,s_query)||read_pico_search_match(s_books[i].author,s_query))matches[count++]=(uint16_t)i;
    if(count>1)qsort(matches,count,sizeof(*matches),compare_match);
    free(s_matches);s_matches=matches;s_visible=count;
    if(reset){s_page=0;release_covers();
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
        prime_covers_from_ram();
#endif
    }
    else if(s_page>=page_count())s_page=page_count()-1;
    return true;
}
static bool filter(void){return filter_page(true);}
static weread_shelf_item_t *row_book(int row) {
    if(row<0||row>=page_rows())return NULL;
    size_t i=(size_t)s_page*page_rows()+(size_t)row;
    return s_matches&&i<s_visible?&s_books[s_matches[i]]:NULL;
}
static bool covers_checked(void) {
    for(int i=0;i<ROWS;++i)if(row_book(i)&&!s_covers[i].checked)return false;
    return true;
}
static bool covers_waiting(uint64_t now){
    for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);
        if(b&&b->has_cover&&!s_covers[i].pixels&&(!s_covers[i].checked||now>=s_covers[i].retry.not_before_ms))return true;
    }
    return false;
}
static uint64_t page_signature(void) {
    uint64_t h=14695981039346656037ULL;
    for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);if(!b)break;
        const char *fields[]={b->book_id,b->title,b->author};
        for(unsigned j=0;j<3;++j)for(const unsigned char *p=(const unsigned char *)fields[j];;p++){
            h=(h^*p)*1099511628211ULL;if(!*p)break;
        }
        h=(h^b->progress^b->cover_key)*1099511628211ULL;
    }
    return h^s_visible^((uint64_t)s_page<<32);
}
static void adopt_books(weread_result_t *r) {
    row_cover_t old[ROWS];char ids[ROWS][64]={{0}};
    for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);if(b)strcpy(ids[i],b->book_id);}
    weread_shelf_item_t *previous=s_books;size_t previous_count=s_count;
    s_books=r->books;s_count=r->count;
    if(!filter_page(false)){s_books=previous;s_count=previous_count;r->error=ESP_ERR_NO_MEM;return;}
    r->books=NULL;free(previous);
    memcpy(old,s_covers,sizeof(old));memset(s_covers,0,sizeof(s_covers));
    for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);if(!b)continue;
        for(int j=0;j<ROWS;++j)if(!strcmp(ids[j],b->book_id)&&old[j].key==b->cover_key){s_covers[i]=old[j];old[j].pixels=NULL;break;}
        s_covers[i].key=b->cover_key;
    }
    for(int i=0;i<ROWS;++i)free(old[i].pixels);
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
    prime_covers_from_ram();
#endif
}
static EpdRect row_rect(int i){return ui_shelf_entry(display_style(),i);}
static EpdRect more_rect(int i){return i<0?(EpdRect){378,160,270,242}:(EpdRect){390,172+i*70,246,70};}
static void more_draw(uint8_t *fb){
    const char *labels[]={"刷新书架","连接 WiFi","退出登录"};EpdRect r=more_rect(-1);
    ui_fill_round_rect(fb,r,18,UI_GRAY_WHITE);ui_draw_control_frame(fb,r,18,0x70);
    for(int i=0;i<3;++i)ui_text_box(fb,ui_inset_rect(more_rect(i),10),26,labels[i],EPD_DRAW_ALIGN_LEFT,false);
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
    s_open_home=false;strcpy(s_id,id);s_clear=false;s_more_sync=false;
    message("正在准备阅读…");
    s_open_stage_at=0;s_open_stage_kind=0;
    if(weread_runtime_busy()){
        s_open_queued=true;s_open_owner=s_owner;s_open_epoch=s_epoch;s_open_stage=true;weread_runtime_cancel();
    }else{stop_job();s_open_stage=start(WR_JOB_OPEN,id,NULL);}
}
static void enter(app_ctx_t *ctx) {
    int first=s_page*page_rows();s_style=app_settings_shelf_style();s_page=first/page_rows();
    s_last_input=ctx->now_ms;s_sync_check=ctx->now_ms+1200;s_force_cover=false;
    s_preconnected=s_cover_warm=s_open_queued=false;s_warm_page=0;s_warm_slot=0;s_warm_direction=1;
    s_keep_network = s_wifi = s_search = s_clear = s_logout = s_auth = false;
    s_uid[0] = s_qr[0] = s_message[0] = 0; s_next_poll = s_next_action = 0;
    s_sync_after_init = true; s_wake=false;s_sort_menu=s_more_menu=s_key_mode=false;
    if (weread_runtime_busy() || !weread_runtime_start(WR_JOB_INIT, NULL, NULL)) s_init_pending = true;
    else { s_init_pending = false; s_syncing = false; }
    s_more_sync=false;
}
static void leave(app_ctx_t *ctx) {
    s_open_queued=s_init_pending=s_open_stage=false;
    wifi_close(ctx); ui_keyboard_end(); ui_wifi_qr_clear(); s_requested[0]=0;
    if (!s_keep_network) weread_runtime_stop(); else stop_job();
    release_covers(); free(s_books); s_books = NULL; free(s_matches); s_matches = NULL; s_count = s_visible = 0;
    s_more_sync=s_syncing=false;
}
static void media_lost(app_ctx_t *ctx) {
    s_open_queued=s_init_pending=s_open_stage=false;
    wifi_close(ctx); weread_runtime_media_lost(); release_covers();
    free(s_books); s_books = NULL; free(s_matches); s_matches = NULL; s_count = s_visible = 0;
    s_auth = false; s_requested[0] = s_uid[0] = s_qr[0] = 0;ui_wifi_qr_clear();message("TF 卡不可用，请重新插入");
}
static void before_lock(app_ctx_t *ctx) {
    s_open_queued=s_init_pending=s_open_stage=false;
    wifi_close(ctx); weread_runtime_stop(); s_wake = true; ui_keyboard_end(); s_search = false;
}
static void media_ready(app_ctx_t *ctx) { (void)ctx; s_wake = true; }
static void render(app_ctx_t *ctx,uint8_t *fb) {
    if(s_wifi){app_transfer.render(ctx,fb);if(!app_transfer.navigation_visible||app_transfer.navigation_visible(ctx))ui_nav_draw(fb,6);return;}
    ui_clear_page(fb);if(display_style()!=5)ui_root_background(fb);ui_nav_status(fb);ui_nav_draw(fb,6);
    ui_text(fb,36,90,52,"微信读书",EPD_DRAW_ALIGN_LEFT,false);
    if(s_message[0])ui_text_box(fb,(EpdRect){36,990,612,20},17,s_message,EPD_DRAW_ALIGN_LEFT,false);
    if(s_sync_after_init){ui_text_vc(fb,342,480,26,s_message[0]?s_message:"正在加载书架…",EPD_DRAW_ALIGN_CENTER,false);return;}
    if(s_search){ui_fill_rect_fast((EpdRect){0,76,684,80},UI_GRAY_WHITE,fb);ui_nav_back(fb,36,79);ui_text_vc(fb,342,107,34,"搜索",EPD_DRAW_ALIGN_CENTER,false);ui_text_input_draw(fb,&s_edit,QUERY_FIELD,26,false,NULL);ui_keyboard_draw(fb,560);return;}
    if(s_clear||s_logout){
        ui_text_box(fb,(EpdRect){60,350,564,180},28,s_clear?"清理这本书的离线内容？阅读进度会保留":"退出微信读书登录？",EPD_DRAW_ALIGN_CENTER,false);
        ui_draw_button(fb,MAIN_ACTION,"取消",false);
        ui_draw_button(fb,SIDE_ACTION,s_clear?"清理缓存":"退出登录",false);return;
    }
    if(!s_auth){
        if(s_qr[0])ui_wifi_qr_draw(fb,QR_AREA);else ui_text_box(fb,(EpdRect){60,340,564,130},26,"正在准备扫码登录…",EPD_DRAW_ALIGN_CENTER,false);
        ui_text_vc(fb,342,820,21,"请在手机上完成扫码和验证码确认",EPD_DRAW_ALIGN_CENTER,false);
        ui_draw_button(fb,MAIN_ACTION,"连接 WiFi",false);ui_draw_button(fb,SIDE_ACTION,s_qr[0]?"刷新二维码":"扫码登录",false);return;
    }
    EpdRect more={420,94,92,54};ui_fill_round_rect(fb,more,18,UI_GRAY_WHITE);ui_draw_control_frame(fb,more,18,0x70);
    ui_text_vc(fb,466,121,20,"更多",EPD_DRAW_ALIGN_CENTER,false);
    ui_shelf_order_button(fb,(EpdRect){524,94,124,54},s_order,s_sort_menu,false,"更新时间",true);
    ui_shelf_begin();
    bool binary=app_settings_main_fast_refresh();
    for(int i=0;i<page_rows();++i){weread_shelf_item_t *book=row_book(i);if(!book)break;
        EpdRect cover=ui_shelf_cover(display_style(),i);ui_shelf_cover_begin(fb,cover,display_style());
        if(s_covers[i].pixels){
#if defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47 && defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
            EpdRect physical=ui_rotate_rect_to_fb(cover);
            bool raster=display_pipeline_enabled()&&
                !(physical.x&1)&&!(physical.width&1)&&physical.x>=0&&physical.y>=0&&
                physical.x+physical.width<=epd_width()&&physical.y+physical.height<=epd_height();
            metalio_cover_key_t raster_key={0};
            uint8_t *physical_pixels=raster?fb+(size_t)physical.y*epd_width()/2+physical.x/2:NULL;
            if(raster){
                raster_key=(metalio_cover_key_t){{
                    UINT64_C(0x5333315752726173),
                    book->cover_key,
                    book_id_hash64(book->book_id),
                    ((uint64_t)s_cover_cache_epoch<<32)|((uint32_t)display_style()<<1)|(uint32_t)binary,
                    s_owner,
                    (uint64_t)(unsigned)epd_get_rotation()<<32|((unsigned)cover.width<<16)|(unsigned)cover.height,
                    (uint64_t)(unsigned)app_settings_system_contrast()<<32|((cover.x&3)|((cover.y&3)<<2)),
                    (uint64_t)(unsigned)physical.width<<32|(unsigned)physical.height
                }};
                if(metalio_cover_cache_get_rows(&raster_key,physical_pixels,epd_width()/2,physical.width/2,physical.height))goto weread_cover_raster_done;
            }
#endif
            ui_shelf_cover_pixels(fb,cover,s_covers[i].pixels,WEREAD_COVER_WIDTH,WEREAD_COVER_HEIGHT,binary);
#if defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47 && defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
            if(raster)(void)metalio_cover_cache_put_rows(&raster_key,physical_pixels,epd_width()/2,physical.width/2,physical.height);
weread_cover_raster_done:;
#endif
        } else ui_shelf_cover_placeholder(fb,cover,book->title);
        ui_shelf_cover_end(fb,cover,display_style(),binary);
    }
    ui_shelf_furniture(fb,display_style(),192,binary,NULL);
#if defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47
    if(display_style()!=5&&app_settings_main_refresh_mode()==APP_MAIN_REFRESH_NORMAL){
        const EpdRect artwork={0,192,UI_LOCK_WIDTH,800};
        display_main_transition_cover_bands(&artwork,1);
    }
#endif
    for(int i=0;i<page_rows();++i){weread_shelf_item_t *book=row_book(i);if(!book)break;
        if(display_style()==5)ui_shelf_list_row(fb,row_rect(i),book->title,book->author,book->progress,book->chapter_index,0);
        else ui_shelf_progress(fb,ui_shelf_cover(display_style(),i),display_style(),book->progress);
    }
    if(!s_visible)ui_text_vc(fb,342,480,26,s_count?"未找到匹配书籍":"书架暂无缓存，请刷新",EPD_DRAW_ALIGN_CENTER,false);
    ui_shelf_pager(fb,s_page,page_count(),s_query[0]!=0,s_key_mode,UI_PAGER_NONE,false);
    if(s_sort_menu)ui_shelf_order_menu(fb,s_order,"更新时间");
    if(s_more_menu)more_draw(fb);
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
        if(e->y0<160&&e->x0<120){ui_keyboard_end();s_search=false;return APP_REDRAW_PAGE;}
        ui_keyboard_release();
        if(ui_text_input_tap(&s_edit,QUERY_FIELD,26,false,NULL,e->x0,e->y0))return keyboard_update(ctx);
        ui_keyboard_result_t r=ui_keyboard_tap(e->x0,e->y0,560,ctx->now_ms);
        if(r==UI_KEYBOARD_DONE){
            char previous[sizeof(s_query)];wr_copy(previous,sizeof(previous),s_query);
            ui_keyboard_end();s_search=false;wr_copy(s_query,sizeof(s_query),s_search_draft);
            if(!filter())wr_copy(s_query,sizeof(s_query),previous);
            return APP_REDRAW_PAGE;
        }
        return keyboard_update(ctx);
    }
    if(e->type!=UI_GESTURE_TAP&&e->type!=UI_GESTURE_LONG_PRESS)return APP_REDRAW_NONE;
    unsigned x=e->x0,y=e->y0;
    int tab=ui_nav_hit(x,y);if(tab>=0){ui_nav_request(ctx,tab);return APP_REDRAW_NONE;}
    if(s_sync_after_init)return APP_REDRAW_NONE;
    if(s_open_queued){
        for(int i=0;i<page_rows();++i){weread_shelf_item_t *b=row_book(i);
            if(b&&ui_rect_hit(row_rect(i),x,y)){open_id(b->book_id);return APP_REDRAW_PAGE;}
        }
        return APP_REDRAW_NONE;
    }
    if(s_clear||s_logout) {
        if(ui_rect_hit(MAIN_ACTION,x,y)){
            s_clear=s_logout=false; return APP_REDRAW_PAGE;
        }
        if(ui_rect_hit(SIDE_ACTION,x,y)){
            start(s_clear?WR_JOB_CLEAR:WR_JOB_LOGOUT,s_clear?s_id:NULL,NULL);
            s_clear=s_logout=false;return APP_REDRAW_PAGE;
        }
        return APP_REDRAW_NONE;
    }
    if(!s_auth){
        if(ui_rect_hit(MAIN_ACTION,x,y)){wifi_open(ctx);return APP_REDRAW_PAGE;}
        if(ui_rect_hit(SIDE_ACTION,x,y)){stop_job();s_uid[0]=s_qr[0]=0;ui_wifi_qr_clear();if(start(WR_JOB_QR,NULL,NULL))message("正在生成二维码…");return APP_REDRAW_PAGE;}
        return APP_REDRAW_NONE;
    }
    if(s_sort_menu){unsigned selected=s_order;
        if(ui_shelf_order_select(x,y,s_order,&selected)&&selected!=s_order){uint8_t previous=s_order;s_order=(uint8_t)selected;if(!filter())s_order=previous;}
        s_sort_menu=false;return APP_REDRAW_PAGE;
    }
    if(s_more_menu){s_more_menu=false;
        if(ui_rect_hit(more_rect(0),x,y)){
            if(s_syncing&&weread_runtime_busy())return APP_REDRAW_PAGE;
            stop_job();s_force_cover=true;s_sync_not_before=0;
            for(int i=0;i<ROWS;++i)if(!s_covers[i].pixels){memset(&s_covers[i].retry,0,sizeof(s_covers[i].retry));s_covers[i].checked=false;}
            start(WR_JOB_SYNC,NULL,NULL);message("正在同步书架…");
        }else if(ui_rect_hit(more_rect(1),x,y))wifi_open(ctx);
        else if(ui_rect_hit(more_rect(2),x,y)){stop_job();s_logout=true;}
        return APP_REDRAW_PAGE;
    }
    if(ui_rect_hit((EpdRect){420,94,92,54},x,y)){s_more_menu=true;return APP_REDRAW_PAGE;}
    if(ui_rect_hit((EpdRect){524,94,124,54},x,y)){s_sort_menu=true;return APP_REDRAW_PAGE;}
    EpdRect search=ui_shelf_search_rect(s_page,page_count());
    if(ui_rect_hit(search,x,y)){
        stop_job();
        if(s_query[0]&&x<(unsigned)(search.x+40)){wr_copy(s_search_draft,sizeof(s_search_draft),s_query);s_query[0]=0;if(!filter())wr_copy(s_query,sizeof(s_query),s_search_draft);return APP_REDRAW_PAGE;}
        s_search=true;wr_copy(s_search_draft,sizeof(s_search_draft),s_query);ui_text_edit_init(&s_edit,s_search_draft,sizeof(s_search_draft));ui_keyboard_begin(&s_edit,false);return APP_REDRAW_PAGE;
    }
    int hit=ui_pager_hit(x,y),target=ui_pager_target(hit,s_page,page_count());
    if(hit>=0&&target!=s_page){stop_job();s_warm_direction=target<s_page?-1:1;s_page=target;release_covers();
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
        prime_covers_from_ram();
#endif
        return APP_REDRAW_PAGE;}
    for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);if(b&&ui_rect_hit(row_rect(i),x,y)){
        if(e->type==UI_GESTURE_LONG_PRESS){stop_job();strcpy(s_id,b->book_id);s_clear=true;}
        else open_id(b->book_id);
        return APP_REDRAW_PAGE;
    }}
    return APP_REDRAW_NONE;
}
static app_redraw_t tick(app_ctx_t *ctx) {
    // 已接受的导航优先，迟到的开书/扫码结果不能覆盖同轮切页。/ Accepted navigation wins over late open/login results in the same tick.
    if(ctx->request_app||ctx->request_return||ctx->request_menu)return APP_REDRAW_NONE;
    if(s_wake){s_wake=false;s_sync_after_init=!s_auth&&!s_uid[0];if(weread_runtime_busy()||!start(WR_JOB_INIT,NULL,NULL))s_init_pending=true;else s_init_pending=false;return APP_REDRAW_PAGE;}
    if(s_wifi){
        app_redraw_t r=app_transfer.on_tick?app_transfer.on_tick(ctx):APP_REDRAW_NONE;
        read_pico_transfer_status_t net;read_pico_transfer_get_status(&net);
        if(!ctx->consumed&&!ctx->request_return&&!ctx->request_app&&!ctx->request_menu&&net.network_ready&&
           net.mode==READ_PICO_TRANSFER_MODE_STA&&net.state==READ_PICO_TRANSFER_READY){
            wifi_close(ctx);start(s_auth?WR_JOB_SYNC:WR_JOB_QR,NULL,NULL);return APP_REDRAW_PAGE;
        }return r;
    }
    weread_result_t r;
    if(s_open_queued){
        if(weread_runtime_busy())return APP_REDRAW_NONE;
        if(weread_runtime_poll(&r))weread_runtime_result_free(&r);
        s_open_queued=s_cover_warm=false;
        if(s_open_owner!=weread_cover_key("","")||s_open_epoch!=weread_runtime_cache_epoch()){
            s_sync_after_init=true;s_auth=false;s_open_stage=false;
            start(WR_JOB_INIT,NULL,NULL);message("登录或存储卡已变化，正在重新加载…");return APP_REDRAW_PAGE;
        }
        s_open_stage=start(WR_JOB_OPEN,s_id,NULL);return APP_REDRAW_NONE;
    }
    if(weread_runtime_poll(&r)){
        if(r.job==WR_JOB_OPEN)s_open_stage=false;
        if(r.job==WR_JOB_NOTE||r.job==WR_JOB_PREFETCH||r.job==WR_JOB_CLOUD_SYNC||r.job==WR_JOB_CHAPTER){
            weread_runtime_result_free(&r);
            if(s_init_pending&&!weread_runtime_busy()&&weread_runtime_start(WR_JOB_INIT,NULL,NULL))s_init_pending=false;
            return APP_REDRAW_NONE;
        }
        bool live_auth=false;
        if(!r.authenticated&&r.job!=WR_JOB_REPORT){WR_GUARD;live_auth=weread_auth_is_authenticated();}
        if(!r.authenticated&&live_auth&&r.job!=WR_JOB_REPORT){
            bool failed_init=r.job==WR_JOB_INIT&&r.error!=ESP_OK;
            weread_runtime_result_free(&r);s_auth=true;s_sync_after_init=true;s_open_queued=false;
            s_requested[0]=s_uid[0]=s_qr[0]=0;ui_wifi_qr_clear();
            if(failed_init)message("TF 卡不可用，请重新挂载后进入");
            else{start(WR_JOB_INIT,NULL,NULL);message("登录已更新，正在加载书架…");}
            return APP_REDRAW_PAGE;
        }
        bool expired_job=r.job==WR_JOB_COVER||r.job==WR_JOB_OPEN||r.job==WR_JOB_LOCAL||r.job==WR_JOB_CLOUD||r.job==WR_JOB_SYNC||(r.job==WR_JOB_INIT&&r.error==ESP_OK);
        if(expired_job&&s_auth&&!r.authenticated&&!live_auth){
            s_auth=s_cover_warm=false;s_uid[0]=s_qr[0]=0;ui_wifi_qr_clear();
            s_open_queued=false;s_requested[0]=0;
            weread_runtime_result_free(&r);start(WR_JOB_QR,NULL,NULL);message("登录已失效，请重新扫码");return APP_REDRAW_PAGE;
        }
        // 不可见预热与预连接不改变当前画面，也不弹出后台网络错误。/ Invisible warming and preconnection never repaint or surface background network errors.
        if((r.job==WR_JOB_COVER&&s_cover_warm)||r.job==WR_JOB_CONNECT||r.job==WR_JOB_DISCONNECT){
            bool expired=s_auth&&!r.authenticated;
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
            if(r.job==WR_JOB_COVER&&s_cover_warm&&r.cover)cache_store_raw_cover(r.id,r.cover);
#endif
            s_cover_warm=false;weread_runtime_result_free(&r);
            if(expired){s_auth=false;start(WR_JOB_QR,NULL,NULL);message("登录已失效，请重新扫码");return APP_REDRAW_PAGE;}
            return APP_REDRAW_NONE;
        }
        if(r.job==WR_JOB_REPORT){
            const char *text=NULL;
            if(r.id[0]){
                static const char *states[]={"进度已保存本地","进度待同步，联网后自动重试","进度已同步","进度同步受阻，请检查存储卡"};
                text=!r.local.chapter_uid&&r.error!=ESP_OK?"进度保存失败，请检查存储卡":states[weread_progress_state(&r.local)];
            }
            bool changed=text&&strcmp(s_message,text);
            if(changed){message(text);s_area=(EpdRect){36,988,612,24};}
            weread_runtime_result_free(&r);return changed?APP_REDRAW_AREA:APP_REDRAW_NONE;
        }
        if(r.job==WR_JOB_SYNC||(r.job==WR_JOB_COVER&&s_cover_remote))s_preconnected=true;
        uint64_t old_page=page_signature();char old_message[sizeof(s_message)];strcpy(old_message,s_message);
        bool local_cover=r.job==WR_JOB_COVER&&!s_cover_remote;
        s_auth=r.authenticated;
        if(r.job==WR_JOB_INIT||r.job==WR_JOB_LOGOUT||(r.job==WR_JOB_POLL&&r.authenticated)){uint64_t owner=weread_cover_key("","");unsigned epoch=weread_runtime_cache_epoch();
            if(owner!=s_owner||epoch!=s_epoch){
                if(owner!=s_owner){s_query[0]=s_search_draft[0]=0;s_page=0;s_order=1;}
                s_sync_not_before=0;release_covers();s_owner=owner;s_epoch=epoch;
            }
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
        if(r.open_ready&&r.error==ESP_OK){
            char uri[80];snprintf(uri,sizeof(uri),"weread://%s",r.id);
            if(s_open_home?app_book_request_open_from_home(uri):app_book_request_open_from_weread(uri)){
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE && !(defined(KIIKO_BOARD_METALIO47) && KIIKO_BOARD_METALIO47)
                // Pico 开书前把 RAM 封面缓存收到 128 KiB，阅读占用才不会跌破 3 MiB 字体缓存回收阈值；
                // S31 在 app_book 开书路径里自己回收 512 KiB。
                // Pico trims the RAM cover cache to 128 KiB before opening so the reader stays above the
                // 3 MiB font-cache reclaim threshold; the S31 trims 512 KiB in app_book itself.
                metalio_cover_cache_trim(128u*1024u);
#endif
                s_keep_network=true;ctx->request_app=app_at(1);}
        }
        if(r.job==WR_JOB_COVER){
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
            if(r.cover)cache_store_raw_cover(r.id,r.cover);
#endif
            for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);if(b&&!strcmp(b->book_id,r.id)){
            s_covers[i].checked=true;
            if(r.cover){free(s_covers[i].pixels);s_covers[i].pixels=r.cover;r.cover=NULL;
                // 本地命中不叠加 150 ms 合并刷新，封面齐了就直接画；远端封面仍等 150 ms。
                // Local hits add no 150 ms merge delay and paint once checked; remote fetches keep it.
                if(!s_cover_dirty)s_cover_paint_at=local_cover?0:ctx->now_ms+150;
                s_cover_dirty=true;
            }
            else if(r.error==ESP_ERR_NO_MEM||r.error==ESP_ERR_INVALID_STATE)
                s_covers[i].retry.not_before_ms=ctx->now_ms+(r.error==ESP_ERR_NO_MEM?15000:250);
            else if(s_cover_remote)weread_cover_retry_result(&s_covers[i].retry,
                r.error!=ESP_OK&&r.http_status>=200&&r.http_status<300?0:r.http_status,ctx->now_ms);
            if(r.cover_retry_seconds&&(!s_cover_remote||s_covers[i].retry.failures>=3))
                s_covers[i].retry.not_before_ms=ctx->now_ms+(uint64_t)r.cover_retry_seconds*1000;
        }}}
        if(r.job==WR_JOB_CLEAR){
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
            if(r.error==ESP_OK)++s_cover_cache_epoch;
#endif
            release_covers();
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
            if(r.error!=ESP_OK)prime_covers_from_ram();
#endif
            message(r.error==ESP_OK?"离线内容已清理":"清理失败，请重试");}
        if(r.error!=ESP_OK&&r.job!=WR_JOB_COVER&&r.job!=WR_JOB_POLL)
            message(r.error==ESP_ERR_NO_MEM?"内存不足，请稍后重试":r.error==WEREAD_ERR_POSITION?"阅读位置与目录不匹配，请先在手机打开本书后重试":r.error==ESP_ERR_INVALID_RESPONSE?"返回数据不完整，请稍后重试":r.error==WEREAD_ERR_PURCHASE?"请在微信读书手机端购买或领取此章节":r.error==WEREAD_ERR_SOLD_OUT?"这本书已下架，暂时无法下载":r.error==WEREAD_ERR_VERSION?"书籍版本已更新，请同步书架后重试":r.error==ESP_ERR_NOT_SUPPORTED?"暂不支持此书籍格式":"网络暂不可用，可阅读已缓存章节");
        if(r.job==WR_JOB_INIT){
            bool initial=s_sync_after_init;
            s_init_pending=false;
            s_sync_after_init=r.error!=ESP_OK;
            if(r.error==ESP_OK){
                if(s_auth){message("单击阅读，长按清理离线内容");s_sync_check=ctx->now_ms+1200;
                    if(!s_count&&!weread_shelf_cache_fresh((uint64_t)time(NULL))&&ctx->now_ms>=s_sync_not_before){start(WR_JOB_SYNC,NULL,NULL);message("正在加载第一页…");}}
                else if(initial||!s_uid[0]){start(WR_JOB_QR,NULL,NULL);message("正在生成二维码…");}
            }
            else message("TF 卡不可用，请重新挂载后进入");
        }
        if(r.job==WR_JOB_SYNC){
            s_syncing=false;s_more_sync=r.shelf_loading;s_next_sync=ctx->now_ms+(covers_waiting(ctx->now_ms)?800:0);
            s_sync_check=ctx->now_ms+(r.error==ESP_OK?300000:60000);
            s_sync_not_before=r.error==ESP_OK?0:ctx->now_ms+60000;
            if(r.error!=ESP_OK)s_more_sync=false;
            if(r.error==ESP_OK){
                char text[128];
                if(s_more_sync)snprintf(text,sizeof(text),"后台同步 %u / %u 本，可直接阅读",(unsigned)r.shelf_done,(unsigned)r.shelf_total);
                else if(r.shelf_skipped)snprintf(text,sizeof(text),"已加载 %u 本，部分书籍暂时不可用",(unsigned)s_count);
                else snprintf(text,sizeof(text),"单击阅读，长按清理离线内容");
                message(text);
            }
        }
        if(r.job==WR_JOB_LOGOUT){s_requested[0]=0;s_requested_home=s_open_home=false;
            if(r.error==ESP_OK){s_query[0]=s_search_draft[0]=0;s_page=0;s_order=1;start(WR_JOB_QR,NULL,NULL);message("正在生成二维码…");}
        }
        if(r.job==WR_JOB_INIT&&s_reader_failed){message("打开失败，请检查存储卡后重试");s_reader_failed=false;}
        bool requested_cached=false;
        for(size_t i=0;i<s_count && s_requested[0];++i)if(!strcmp(s_books[i].book_id,s_requested))requested_cached=true;
        if(s_requested[0]&&s_auth&&((r.job==WR_JOB_INIT&&requested_cached)||r.job==WR_JOB_SYNC)){
            stop_job();open_id(s_requested);s_open_home=s_requested_home;s_requested_home=false;s_requested[0]=0;
        }
        bool unchanged=r.job==WR_JOB_SYNC&&old_page==page_signature()&&!strcmp(old_message,s_message);
        bool cover_result=r.job==WR_JOB_COVER;
        bool batch_cover=cover_result&&(!local_cover||!covers_checked());
        bool paint_cover=cover_result&&local_cover&&covers_checked()&&s_cover_dirty;
        // 本地封面结果一回来就继续取下一本，不留 150 ms 节流；远端封面和内存重试仍按原节奏。
        // Continue the local cover queue immediately on completion; remote fetches and memory retries keep their pacing.
        if(local_cover)s_next_action=ctx->now_ms;
        if(paint_cover){s_cover_dirty=false;s_cover_paint_at=0;}
        weread_runtime_result_free(&r);return unchanged||batch_cover||(cover_result&&!paint_cover)?APP_REDRAW_NONE:APP_REDRAW_PAGE;
    }
    // 开书在途时把 worker 的当前阶段写到 render() 的消息行；文字变化才刷该行，结束/取消后不再覆盖错误消息。
    // While an open is in flight, show the worker's current stage on render()'s message line; repaint only on change.
    // 搜索/菜单/确认框/扫码页打开时暂不占用消息行，避免阶段提示盖住子页面。
    // Hold the stage line while a subpage (search, menus, confirm, Wi-Fi) owns the screen.
    if(s_open_stage&&!s_search&&!s_sort_menu&&!s_more_menu&&!s_clear&&!s_logout&&!s_wifi){
        weread_stage_t kind=WEREAD_STAGE_NONE;
        char stage[64];
        if(weread_reader_stage(&kind,NULL)&&weread_reader_stage_text(stage,sizeof(stage))&&strcmp(stage,s_message)){
            // 下载阶段的 KB 文案按 2 秒节奏合并，避免每 32 KiB 局刷一次消息行。
            // Coalesce download KB labels to 2 s so the message line is not repainted every 32 KiB.
            if((unsigned)kind==s_open_stage_kind&&kind==WEREAD_STAGE_CHAPTER&&ctx->now_ms<s_open_stage_at)return APP_REDRAW_NONE;
            s_open_stage_kind=(unsigned)kind;s_open_stage_at=ctx->now_ms+2000;
            message(stage);
            if(ctx->fb){
                ui_text_set_system_scale(true);
                ui_fill_rect_fast((EpdRect){36,990,612,20},UI_GRAY_WHITE,ctx->fb);
                ui_text_box(ctx->fb,(EpdRect){36,990,612,20},17,s_message,EPD_DRAW_ALIGN_LEFT,false);
                s_area=(EpdRect){36,988,612,24};
                return APP_REDRAW_AREA;
            }
        }
    }
    if(s_cover_dirty&&covers_checked()&&ctx->now_ms>=s_cover_paint_at&&!s_search&&!s_sort_menu&&!s_more_menu&&!s_clear&&!s_logout){
        s_cover_dirty=false;s_cover_paint_at=0;return APP_REDRAW_PAGE;
    }
    if(s_init_pending&&!weread_runtime_busy()&&weread_runtime_start(WR_JOB_INIT,NULL,NULL)){s_init_pending=false;return APP_REDRAW_NONE;}
    if(weread_runtime_busy()||s_sync_after_init||s_search||s_sort_menu||s_more_menu||s_clear||s_logout||ctx->request_app||ctx->request_return)return APP_REDRAW_NONE;
    bool idle=ctx->now_ms>=s_last_input&&ctx->now_ms-s_last_input>=1000;
    if(s_auth&&s_more_sync&&ctx->now_ms>=s_next_sync&&idle&&covers_checked()){start(WR_JOB_SYNC,NULL,"continue");return APP_REDRAW_NONE;}
    if(s_auth&&!s_more_sync&&idle&&covers_checked()&&ctx->now_ms>=s_sync_check&&ctx->now_ms>=s_sync_not_before){
        s_sync_check=ctx->now_ms+300000;
        if(!weread_shelf_cache_fresh((uint64_t)time(NULL))){start(WR_JOB_SYNC,NULL,NULL);return APP_REDRAW_NONE;}
    }
    if(!s_auth&&s_uid[0]&&(uint64_t)ctx->now_ms>=s_next_poll){s_next_poll=ctx->now_ms+2000;start(WR_JOB_POLL,NULL,s_uid);return APP_REDRAW_NONE;}
    if(s_auth&&(uint64_t)ctx->now_ms>=s_next_action){
        for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);if(!b||s_covers[i].pixels||s_covers[i].checked)continue;
            s_covers[i].key=b->cover_key;s_cover_remote=false;
            start(WR_JOB_COVER,b->book_id,s_force_cover?"retry":NULL);s_next_action=ctx->now_ms+150;return APP_REDRAW_NONE;
        }
        for(int i=0;i<ROWS;++i){weread_shelf_item_t *b=row_book(i);if(!b||s_covers[i].pixels)continue;
            if(!b->has_cover||ctx->now_ms<s_covers[i].retry.not_before_ms)continue;
            if(!idle)continue;
            s_cover_remote=true;start(WR_JOB_COVER,b->book_id,s_covers[i].retry.original?"original":"remote");break;
        }s_next_action=ctx->now_ms+150;
        if(covers_checked())s_force_cover=false;
    }
    if(s_auth&&idle&&!weread_runtime_busy()&&covers_checked()&&!covers_waiting(ctx->now_ms)&&!s_more_sync){
        if(weread_runtime_idle(ctx->now_ms))return APP_REDRAW_NONE;
        if(!s_preconnected&&ctx->now_ms-s_last_input>=1500){
            s_preconnected=true;(void)weread_runtime_start(WR_JOB_CONNECT,NULL,NULL);return APP_REDRAW_NONE;
        }
        uint64_t signature=page_signature();
        if(signature!=s_warm_page){s_warm_page=signature;s_warm_slot=0;}
        int next=s_page+s_warm_direction;
        if(next>=0&&next<page_count()&&s_warm_slot<2){
            size_t index=(size_t)next*page_rows()+s_warm_slot++;
            if(index<s_visible){weread_shelf_item_t *b=&s_books[s_matches[index]];
                if(b->has_cover)s_cover_warm=weread_runtime_start(WR_JOB_COVER,b->book_id,"warm-disk");
            }
        }
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t key(app_ctx_t *ctx,int key) {
    s_open_queued=false;
    s_last_input=ctx->now_ms;
    if(s_wifi&&app_transfer.on_key)return app_transfer.on_key(ctx,key);
    if(s_search){ui_keyboard_end();s_search=false;return APP_REDRAW_PAGE;}
    if(s_clear||s_logout||s_sort_menu||s_more_menu){s_clear=s_logout=s_sort_menu=s_more_menu=false;return APP_REDRAW_PAGE;}
    ui_nav_request(ctx,0);return APP_REDRAW_NONE;
}
static bool navigation(app_ctx_t *ctx){return !s_wifi||!app_transfer.navigation_visible||app_transfer.navigation_visible(ctx);}
static bool main_page(app_ctx_t *ctx){(void)ctx;return !s_wifi&&!s_search&&!s_clear&&!s_logout&&!s_sort_menu&&!s_more_menu;}
static bool main_key(app_ctx_t *ctx,int key,app_redraw_t *redraw){
    s_last_input=ctx->now_ms;
    if(!s_auth||s_sync_after_init||s_open_queued||!main_page(ctx))return false;
    if(key==UI_KEY_2){s_key_mode=!s_key_mode;*redraw=APP_REDRAW_PAGE;return true;}
    if(!s_key_mode)return false;
    int target=s_page+(key==UI_KEY_1?-1:1);
    if(target>=0&&target<page_count()&&target!=s_page){stop_job();s_warm_direction=target<s_page?-1:1;s_page=target;release_covers();
#if defined(KIIKO_BOARD_RAM_COVER_CACHE) && KIIKO_BOARD_RAM_COVER_CACHE
        prime_covers_from_ram();
#endif
        *redraw=APP_REDRAW_PAGE;}
    else *redraw=APP_REDRAW_NONE;
    return true;
}
static bool present(app_ctx_t *ctx,app_redraw_t redraw){return s_wifi&&app_transfer.present&&app_transfer.present(ctx,redraw);}
static EpdRect area(app_ctx_t *ctx){(void)ctx;return s_area;}
const app_desc_t app_weread={.title="微信读书",.detail="扫码登录与离线阅读",.enter_full=false,.persistent_navigation=true,.white_child_transition=true,.main_page_visible=main_page,.on_main_key=main_key,
    .navigation_visible=navigation,.on_enter=enter,.on_exit=leave,.on_media_lost=media_lost,
    .on_media_ready=media_ready,.on_before_lock=before_lock,.render=render,.on_tick=tick,
    .on_gesture=gesture,.on_key=key,.present=present,.area_hint=area};
