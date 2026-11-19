/* SPDX-FileCopyrightText: 2026 kiikoread
 * SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * Production cache regression; UI/platform boundaries use host stubs.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "weread_runtime.h"
#include "weread_auth.h"
#include "weread_chapter.h"
#define WR_GUARD
#define ESP_LOGI(...) ((void)0)
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {char *text;size_t capacity,cursor;} ui_text_edit_t;
typedef enum {APP_REDRAW_NONE,APP_REDRAW_AREA,APP_REDRAW_PAGE} app_redraw_t;
typedef struct {uint64_t now_ms;void *fb;bool consumed,request_return,request_menu;const void *request_app;} app_ctx_t;
typedef enum {UI_GESTURE_PRESS,UI_GESTURE_CANCEL,UI_GESTURE_TAP,UI_GESTURE_LONG_PRESS} ui_gesture_type_t;
typedef struct {ui_gesture_type_t type;unsigned x0,y0;} ui_gesture_event_t;
typedef struct {void(*render)(app_ctx_t*,uint8_t*);bool(*navigation_visible)(app_ctx_t*);void(*on_enter)(app_ctx_t*);void(*on_exit)(app_ctx_t*);app_redraw_t(*on_tick)(app_ctx_t*);app_redraw_t(*on_key)(app_ctx_t*,int);app_redraw_t(*on_gesture)(app_ctx_t*,const ui_gesture_event_t*);} app_desc_t;
const app_desc_t app_transfer={0};
typedef struct {EpdRect area;bool layout;} ui_keyboard_update_t;
typedef enum{UI_KEYBOARD_NONE,UI_KEYBOARD_DONE} ui_keyboard_result_t;
typedef struct{bool network_ready;int mode,state;}read_pico_transfer_status_t;
#define UI_KEY_1 0
#define UI_KEY_2 1
#define UI_KEY_3 2
#define READ_PICO_TRANSFER_MODE_STA 1
#define READ_PICO_TRANSFER_READY 2
static int mock_style=6;static bool fail_alloc,mock_keyboard_done;
static bool mock_busy,mock_ready,mock_fresh=true,mock_pending;
static unsigned mock_epoch=1;
static weread_result_t mock_result;
static struct{weread_job_t job;char id[64],value[600];} starts[256];
static int nstarts,nsync;
static weread_auth_session_t mock_session={.vid="123"};
static bool mock_authenticated=true;
const weread_auth_session_t *weread_auth_get_current_session(void){return mock_authenticated?&mock_session:NULL;}
bool weread_auth_is_authenticated(void){return mock_authenticated;}
static void wr_display_copy(char *out,size_t cap,const char*value){snprintf(out,cap,"%s",value?value:"");}
static bool wr_copy(char *out,size_t cap,const char *in){if(!in||strlen(in)>=cap)return false;memcpy(out,in,strlen(in)+1);return true;}
static void *wr_alloc(size_t n){if(fail_alloc)return NULL;return calloc(n,1);}
static bool read_pico_search_match(const char*a,const char*b){return strstr(a,b)!=NULL;}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
static void ui_wifi_qr_clear(void){}
static bool ui_wifi_qr_prepare_text(const char*s){return true;}
static void app_transfer_request_wifi_setup(void){}
static void ui_keyboard_end(void){}
static void ui_keyboard_begin(ui_text_edit_t*e,bool b){}
static void ui_text_edit_init(ui_text_edit_t*e,char*s,size_t c){e->text=s;e->capacity=c;}
static void ui_keyboard_press(int x,int y,int t,uint64_t n){}
static void ui_keyboard_release(void){}
static ui_keyboard_result_t ui_keyboard_tap(int x,int y,int t,uint64_t n){return mock_keyboard_done?UI_KEYBOARD_DONE:UI_KEYBOARD_NONE;}
static bool ui_text_input_tap(ui_text_edit_t*e,EpdRect r,int p,bool b,const char*s,int x,int y){return false;}
static ui_keyboard_update_t ui_keyboard_update(void*f,int t,EpdRect r,int p,bool b,const char*s,bool q){return(ui_keyboard_update_t){0};}
static void read_pico_transfer_get_status(read_pico_transfer_status_t*s){memset(s,0,sizeof(*s));}
static bool app_book_request_open_from_home(const char*s){return true;}
static bool app_book_request_open_from_weread(const char*s){return true;}
static int app_settings_shelf_style(void){return mock_style;}
#define UI_LOCK_WIDTH 684
#define UI_GAP 12
#define UI_MARGIN 40
static int ui_content_width(void){return 604;}
static int ui_text_fixed_width_px(int px,const char *s){return (int)strlen(s)*px/2;}
static int ui_nav_hit(int x,int y){(void)x;(void)y;return -1;}
static void ui_nav_request(app_ctx_t*c,int page){(void)page;c->request_app=(void*)1;}
static int ui_pager_hit(int x,int y){(void)x;(void)y;return -1;}
static int ui_pager_target(int hit,int page,int pages){(void)hit;(void)pages;return page;}

#define EPD_DRAW_ALIGN_LEFT 0
#define EPD_DRAW_ALIGN_CENTER 1
#define UI_GRAY_WHITE 255
#define UI_PAGER_NONE -1
static char drawn_text[4096];static int drawn_nav,drawn_qr;
static void remember_text(const char*s){if(s){size_t n=strlen(drawn_text);snprintf(drawn_text+n,sizeof(drawn_text)-n,"%s\n",s);}}
static void ui_clear_page(uint8_t*f){drawn_text[0]=0;drawn_nav=drawn_qr=0;}
static void ui_root_background(uint8_t*f){}
static void ui_nav_status(uint8_t*f){}
static void ui_nav_draw(uint8_t*f,int id){assert(id==6);++drawn_nav;}
static void ui_text(uint8_t*f,int x,int y,int px,const char*s,int align,bool inverse){remember_text(s);}
static void ui_text_set_system_scale(bool enabled){(void)enabled;}
static void ui_text_vc(uint8_t*f,int x,int y,int px,const char*s,int align,bool inverse){remember_text(s);}
static void ui_text_box(uint8_t*f,EpdRect r,int px,const char*s,int align,bool inverse){remember_text(s);}
static void ui_fill_rect_fast(EpdRect r,int gray,uint8_t*f){}
static void ui_nav_back(uint8_t*f,int x,int y){}
static void ui_text_input_draw(uint8_t*f,ui_text_edit_t*e,EpdRect r,int px,bool b,const char*s){}
static void ui_keyboard_draw(uint8_t*f,int top){}
static void ui_draw_button(uint8_t*f,EpdRect r,const char*s,bool b){remember_text(s);}
static void ui_wifi_qr_draw(uint8_t*f,EpdRect r){++drawn_qr;}
static void ui_fill_round_rect(uint8_t*f,EpdRect r,int radius,int gray){}
static void ui_draw_control_frame(uint8_t*f,EpdRect r,int radius,int gray){}
static EpdRect ui_inset_rect(EpdRect r,int inset){return r;}
static bool app_settings_main_fast_refresh(void){return false;}
static void ui_shelf_order_button(uint8_t*f,EpdRect r,unsigned o,bool open,bool pressed,const char*s,bool compact){}
static void ui_shelf_begin(void){}
static void ui_shelf_cover_begin(uint8_t*f,EpdRect r,int style){}
static void ui_shelf_cover_pixels(uint8_t*f,EpdRect r,const uint8_t*p,unsigned w,unsigned h,bool b){}
static void ui_shelf_cover_placeholder(uint8_t*f,EpdRect r,const char*s){remember_text(s);}
static void ui_shelf_cover_end(uint8_t*f,EpdRect r,int style,bool b){}
static void ui_shelf_furniture(uint8_t*f,int style,int top,bool b,void(*guard)(uint8_t*,EpdRect,int)){}
static void ui_shelf_list_row(uint8_t*f,EpdRect r,const char*t,const char*a,unsigned p,unsigned c,unsigned total){remember_text(t);}
static void ui_shelf_progress(uint8_t*f,EpdRect r,int style,unsigned p){}
static void ui_shelf_pager(uint8_t*f,int p,int pages,bool filtered,bool key,int pressed,bool search){}
static void ui_shelf_order_menu(uint8_t*f,unsigned order,const char*s){}

#include "shared.inc"
static const void *app_at(int i){return(void*)1;}
bool weread_runtime_start(weread_job_t job,const char*id,const char*value){
    if(mock_busy||mock_ready)return false;assert(nstarts<256);
    starts[nstarts].job=job;snprintf(starts[nstarts].id,64,"%s",id?id:"");snprintf(starts[nstarts].value,600,"%s",value?value:"");nstarts++;if(job==WR_JOB_SYNC)nsync++;mock_busy=true;return true;
}
bool weread_runtime_busy(void){return mock_busy;}
bool weread_runtime_idle(uint64_t now){(void)now;return false;}
weread_progress_state_t weread_progress_state(const weread_progress_t*p){return p->sync_conflict?WR_PROGRESS_CONFLICT:p->position_pending?WR_PROGRESS_PENDING:p->last_report_ms?WR_PROGRESS_SYNCED:WR_PROGRESS_LOCAL;}
bool weread_runtime_poll(weread_result_t*out){if(!mock_ready)return false;*out=mock_result;memset(&mock_result,0,sizeof(mock_result));mock_ready=false;return true;}
void weread_runtime_result_free(weread_result_t*r){free(r->books);free(r->cover);memset(r,0,sizeof(*r));}
void weread_runtime_cancel(void){}
void weread_runtime_cancel_join(void){mock_busy=false;}
void weread_runtime_stop(void){mock_busy=false;if(mock_ready)weread_runtime_result_free(&mock_result);mock_ready=false;}
void weread_runtime_media_lost(void){weread_runtime_stop();mock_epoch++;}
unsigned weread_runtime_cache_epoch(void){return mock_epoch;}
bool weread_shelf_cache_fresh(uint64_t now){return mock_fresh;}
bool weread_shelf_sync_pending(void){return mock_pending;}
bool weread_runtime_choose(const char*id,const weread_progress_t*p,bool cloud){return true;}
/// 运行时的开书阶段文案由真实worker提供；本测试没有开书阶段，保持既有提示。
/// The runtime owns the open-stage text; this test has no open stage, so keep existing notices.
bool weread_reader_stage_text(char*out,size_t cap){(void)out;(void)cap;return false;}
bool weread_reader_stage(weread_stage_t *stage,uint64_t *bytes){(void)stage;(void)bytes;return false;}
/// 书架消息行沿用根底浅灰色；宿主桩与 ui_root_style.h 保持一致。/ Shelf message line reuses the root canvas gray; keep the host stub equal to ui_root_style.h.
#define UI_ROOT_GRAY 0xf0
#include "cover.inc"
#include "production.inc"
static void complete(weread_result_t r){assert(mock_busy&&!mock_ready);mock_authenticated=r.authenticated;mock_result=r;mock_busy=false;mock_ready=true;}
static weread_shelf_item_t *books(int n,bool reverse){weread_shelf_item_t*b=calloc((size_t)n,sizeof(*b));assert(b);for(int i=0;i<n;i++){int j=reverse?n-i-1:i;snprintf(b[i].book_id,sizeof(b[i].book_id),"id_%d",j);snprintf(b[i].title,sizeof(b[i].title),"Book %d",j);char url[80];snprintf(url,sizeof(url),"https://cdn.qq.com/s_%d.jpg",j);b[i].read_update_time=(uint64_t)(n-i);b[i].has_cover=true;b[i].cover_key=weread_cover_key(b[i].book_id,url);}return b;}
static app_ctx_t ctx={.now_ms=10000};
static void reset(void){mock_authenticated=true;release_covers();free(s_books);free(s_matches);s_books=NULL;s_matches=NULL;s_count=s_visible=0;s_page=0;s_query[0]=s_requested[0]=s_message[0]=0;s_owner=0;s_epoch=0;s_sync_not_before=0;s_more_sync=false;s_next_action=s_next_sync=s_sync_check=0;s_cover_remote=s_force_cover=false;fail_alloc=mock_keyboard_done=false;s_order=1;s_style=mock_style=6;s_sort_menu=s_more_menu=false;weread_runtime_stop();memset(starts,0,sizeof(starts));nstarts=nsync=0;mock_fresh=true;mock_pending=false;ctx=(app_ctx_t){.now_ms=10000};}
static void initialized(int n){enter(&ctx);assert(nstarts==1&&starts[0].job==WR_JOB_INIT);complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(n,false),.count=(size_t)n});assert(tick(&ctx)==APP_REDRAW_PAGE);assert(nsync==0);s_preconnected=true;}
static void run_cover_batch(void){
    for(int i=0;i<6;i++){
        ctx.now_ms+=10;int prev=nstarts;assert(tick(&ctx)==APP_REDRAW_NONE);assert(nstarts==prev+1&&starts[prev].job==WR_JOB_COVER&&!starts[prev].value[0]);
        uint8_t *pixel=malloc(1);assert(pixel);*pixel=(uint8_t)i;
        complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.cover=pixel});snprintf(mock_result.id,sizeof(mock_result.id),"id_%d",i);
        assert(tick(&ctx)==(i==5?APP_REDRAW_PAGE:APP_REDRAW_NONE));
    }
    assert(covers_checked()&&nsync==0);
}
int main(void){
    reset();initialized(6);run_cover_batch();puts("fresh cache: no SYNC; six local covers: one PAGE redraw");
    uint8_t*old[6];for(int i=0;i<6;i++)old[i]=s_covers[i].pixels;
    ui_gesture_event_t e={0};assert(filter_page(false));for(int i=0;i<6;i++)assert(s_covers[i].pixels==old[i]);puts("layout retains six rasters");
    assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=books(6,true),.count=6});assert(tick(&ctx)==APP_REDRAW_PAGE);for(int i=0;i<6;i++)assert(s_covers[i].pixels==old[5-i]);puts("reorder transfers rasters by ID+URL");
    assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));weread_shelf_item_t*changed=books(6,true);changed[2].cover_key=weread_cover_key(changed[2].book_id,"https://cdn.qq.com/s_changed.jpg");complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=changed,.count=6});assert(tick(&ctx)==APP_REDRAW_PAGE);assert(!s_covers[2].pixels&&!s_covers[2].checked);for(int i=0;i<6;i++)if(i!=2)assert(s_covers[i].pixels==old[5-i]);puts("changed URL invalidates only matching raster");
    reset();initialized(1);ctx.now_ms+=10;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_COVER);complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_ERR_NOT_FOUND});strcpy(mock_result.id,"id_0");tick(&ctx);
    ctx.now_ms+=1200;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_COVER&&starts[nstarts-1].value[0]);s_cover_remote=true;
    uint64_t now=ctx.now_ms;complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_ERR_INVALID_RESPONSE,.http_status=200,.cover_retry_seconds=300});strcpy(mock_result.id,"id_0");tick(&ctx);assert(s_covers[0].retry.failures==1&&s_covers[0].retry.not_before_ms==now+5000);
    ctx.now_ms=now+4999;int calls=nstarts;tick(&ctx);assert(nstarts==calls);ctx.now_ms=now+5150;tick(&ctx);assert(nstarts==calls+1);puts("HTTP 200 decode failure backs off 5 seconds");
    complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_FAIL,.http_status=500});strcpy(mock_result.id,"id_0");tick(&ctx);
    ctx.now_ms+=10;e=(ui_gesture_event_t){.type=UI_GESTURE_TAP,.x0=466,.y0=120};assert(gesture(&ctx,&e)==APP_REDRAW_PAGE);e.x0=500;e.y0=200;assert(gesture(&ctx,&e)==APP_REDRAW_PAGE);assert(starts[nstarts-1].job==WR_JOB_SYNC&&s_force_cover&&!s_covers[0].checked&&!s_covers[0].retry.failures);
    complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=books(1,false),.count=1});tick(&ctx);ctx.now_ms+=200;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_COVER&&!strcmp(starts[nstarts-1].value,"retry"));puts("manual refresh resets row backoff and forces local negative-cache retry");
    reset();initialized(6);run_cover_batch();
    for(int i=0;i<6;i++)old[i]=s_covers[i].pixels;
    assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=books(6,false),.count=6});
    assert(tick(&ctx)==APP_REDRAW_NONE);for(int i=0;i<6;i++)assert(s_covers[i].pixels==old[i]);puts("unchanged SYNC snapshot suppresses redundant PAGE redraw");
    assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(6,false),.count=6});tick(&ctx);for(int i=0;i<6;i++)assert(s_covers[i].pixels==old[i]);
    mock_epoch++;assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(6,false),.count=6});tick(&ctx);for(int i=0;i<6;i++)assert(!s_covers[i].pixels&&!s_covers[i].checked);puts("same epoch preserves and changed epoch invalidates memory rasters");
    reset();initialized(1);ctx.now_ms+=10;tick(&ctx);complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_ERR_NOT_FOUND,.cover_retry_seconds=300});strcpy(mock_result.id,"id_0");tick(&ctx);
    ctx.now_ms+=1000;calls=nstarts;tick(&ctx);assert(nstarts==calls&&s_covers[0].retry.not_before_ms>ctx.now_ms);puts("persisted negative cover marker blocks automatic remote retry");
    reset();initialized(6);assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=books(6,false),.count=6,.shelf_loading=true,.shelf_total=45});tick(&ctx);ctx.now_ms+=1100;
    calls=nstarts;tick(&ctx);assert(nstarts==calls+1&&starts[nstarts-1].job==WR_JOB_COVER);complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.cover=malloc(1)});strcpy(mock_result.id,"id_0");tick(&ctx);
    for(int i=1;i<6;i++){ctx.now_ms+=10;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_COVER);complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.cover=malloc(1)});snprintf(mock_result.id,sizeof(mock_result.id),"id_%d",i);tick(&ctx);}
    ctx.now_ms+=10;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_SYNC&&!strcmp(starts[nstarts-1].value,"continue"));puts("staged SYNC waits for first-page local covers, then continues");
    reset();initialized(1);mock_fresh=false;assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));uint64_t failed_at=ctx.now_ms;complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=books(1,false),.count=1,.error=ESP_ERR_TIMEOUT});tick(&ctx);s_covers[0].checked=true;s_covers[0].retry.not_before_ms=UINT64_MAX;
    calls=nsync;ctx.now_ms=failed_at+59999;tick(&ctx);assert(nsync==calls);ctx.now_ms=failed_at+300000;tick(&ctx);assert(nsync==calls+1);puts("failed SYNC waits for periodic idle check and permits later retry");
    reset();initialized(30);strcpy(s_query,"Book");s_order=2;assert(filter_page(false));s_page=2;
    leave(&ctx);assert(s_page==2&&!strcmp(s_query,"Book")&&s_order==2);
    int begin=nstarts;enter(&ctx);assert(nstarts==begin+1&&starts[begin].job==WR_JOB_INIT);
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(30,false),.count=30});tick(&ctx);
    assert(s_page==2&&!strcmp(s_query,"Book")&&s_order==2&&page_rows()==8);
    puts("independent query/page/order survive root leave/re-entry");
    mock_session.vid[0]='9';assert(weread_runtime_start(WR_JOB_INIT,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(2,false),.count=2});tick(&ctx);
    assert(!s_query[0]&&!s_page&&s_order==1);strcpy(mock_session.vid,"123");
    puts("account switch resets the prior account browser state");
    reset();enter(&ctx);complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});tick(&ctx);
    assert(starts[nstarts-1].job==WR_JOB_QR&&navigation(&ctx));
    puts("unauthenticated root enters QR immediately and admits navigation");
    reset();initialized(1);assert(weread_runtime_start(WR_JOB_OPEN,"id_0",NULL));
    complete((weread_result_t){.job=WR_JOB_OPEN,.authenticated=true,.open_ready=true});strcpy(mock_result.id,"id_0");
    ctx.request_app=(void*)2;assert(tick(&ctx)==APP_REDRAW_NONE&&ctx.request_app==(void*)2&&mock_ready);
    leave(&ctx);assert(!mock_ready);puts("accepted navigation discards a late open result without rerouting");
    reset();initialized(20);strcpy(s_query,"Book");assert(filter_page(false));s_page=1;
    uint16_t *previous_matches=s_matches;weread_shelf_item_t *previous_books=s_books;
    s_sort_menu=true;fail_alloc=true;e=(ui_gesture_event_t){.type=UI_GESTURE_TAP,.x0=500,.y0=347};
    gesture(&ctx,&e);assert(s_order==1&&s_page==1&&s_matches==previous_matches);
    s_search=true;strcpy(s_search_draft,"Book 2");mock_keyboard_done=true;e.x0=200;e.y0=700;
    gesture(&ctx,&e);assert(!strcmp(s_query,"Book")&&s_page==1&&s_matches==previous_matches);
    s_search=false;EpdRect search=ui_shelf_search_rect(s_page,page_count());
    e=(ui_gesture_event_t){.type=UI_GESTURE_TAP,.x0=(unsigned)search.x+20,.y0=(unsigned)search.y+20};
    gesture(&ctx,&e);assert(!strcmp(s_query,"Book")&&s_page==1&&s_matches==previous_matches);
    weread_result_t replacement={.books=books(30,false),.count=30};adopt_books(&replacement);
    assert(replacement.error==ESP_ERR_NO_MEM&&s_books==previous_books&&s_count==20&&s_matches==previous_matches&&replacement.books);
    weread_runtime_result_free(&replacement);fail_alloc=mock_keyboard_done=false;
    puts("match OOM rolls back search/order/adoption while keeping the displayed books and page");
    reset();initialized(30);s_page=3;leave(&ctx);mock_style=5;enter(&ctx);
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(30,false),.count=30});tick(&ctx);
    assert(page_rows()==8&&s_page==3);assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));
    complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=books(2,false),.count=2});tick(&ctx);
    assert(s_page==0&&s_count==2);puts("shared theme changes retain the first visible range; shrinking sync clamps the page");
    reset();enter(&ctx);complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});tick(&ctx);
    complete((weread_result_t){.job=WR_JOB_QR,.authenticated=false,.uid="qr-test",.qr_url="https://weread.qq.com/test"});tick(&ctx);
    assert(s_uid[0]&&s_qr[0]);ctx.now_ms+=2000;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_POLL);
    complete((weread_result_t){.job=WR_JOB_POLL,.qr_state=WEREAD_QR_STATE_NEED_OTP});tick(&ctx);
    assert(strstr(s_message,"验证码")&&navigation(&ctx));ctx.now_ms+=2000;tick(&ctx);
    complete((weread_result_t){.job=WR_JOB_POLL,.qr_state=WEREAD_QR_STATE_TIMEOUT});tick(&ctx);
    assert(!s_uid[0]&&strstr(s_message,"失效")&&navigation(&ctx));
    assert(start(WR_JOB_LOGOUT,NULL,NULL));complete((weread_result_t){.job=WR_JOB_LOGOUT});tick(&ctx);
    assert(starts[nstarts-1].job==WR_JOB_QR&&!s_query[0]&&!s_page&&s_order==1);
    leave(&ctx);assert(!mock_busy&&!mock_ready);puts("QR verification/timeout keep root navigation; logout requests a fresh QR and exit joins the job");

    reset();enter(&ctx);render(&ctx,NULL);
    assert(s_sync_after_init&&drawn_nav==1&&!drawn_qr&&strstr(drawn_text,"正在加载书架")&&!strstr(drawn_text,"扫码")&&!strstr(drawn_text,"验证码"));
    calls=nstarts;e=(ui_gesture_event_t){.type=UI_GESTURE_TAP,.x0=400,.y0=1030};
    assert(gesture(&ctx,&e)==APP_REDRAW_NONE&&nstarts==calls);
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(1,false),.count=1});tick(&ctx);render(&ctx,NULL);
    assert(!s_sync_after_init&&s_auth&&strstr(drawn_text,"Book 0")&&!strstr(drawn_text,"扫码"));
    leave(&ctx);enter(&ctx);render(&ctx,NULL);assert(drawn_nav==1&&!strstr(drawn_text,"扫码"));
    complete((weread_result_t){.job=WR_JOB_INIT,.error=ESP_FAIL});tick(&ctx);render(&ctx,NULL);
    assert(s_sync_after_init&&strstr(drawn_text,"TF 卡")&&!strstr(drawn_text,"扫码")&&navigation(&ctx));
    before_lock(&ctx);tick(&ctx);render(&ctx,NULL);assert(s_sync_after_init&&!strstr(drawn_text,"扫码"));
    complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=false});tick(&ctx);render(&ctx,NULL);
    assert(!s_sync_after_init&&!s_auth&&starts[nstarts-1].job==WR_JOB_QR&&strstr(drawn_text,"扫码登录")&&drawn_nav==1);

    reset();enter(&ctx);complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.error=ESP_ERR_NO_MEM});tick(&ctx);
    calls=nstarts;ctx.now_ms+=600000;render(&ctx,NULL);assert(tick(&ctx)==APP_REDRAW_NONE&&nstarts==calls);
    app_redraw_t redraw=APP_REDRAW_NONE;assert(!main_key(&ctx,UI_KEY_2,&redraw));
    assert(s_sync_after_init&&!strstr(drawn_text,"扫码")&&navigation(&ctx));
    puts("actual entry render hides login before local auth resolves, survives init failure/wake, admits tabs and starts QR only for confirmed logout");
    reset();puts("production cache UI probe passed");return 0;
}
