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
typedef struct {void(*on_enter)(app_ctx_t*);void(*on_exit)(app_ctx_t*);app_redraw_t(*on_tick)(app_ctx_t*);app_redraw_t(*on_key)(app_ctx_t*,int);app_redraw_t(*on_gesture)(app_ctx_t*,const ui_gesture_event_t*);} app_desc_t;
const app_desc_t app_transfer={0};
typedef struct {EpdRect area;bool layout;} ui_keyboard_update_t;
typedef enum{UI_KEYBOARD_NONE,UI_KEYBOARD_DONE} ui_keyboard_result_t;
typedef struct{bool network_ready;int mode,state;}read_pico_transfer_status_t;
#define READ_PICO_TRANSFER_MODE_STA 1
#define READ_PICO_TRANSFER_READY 2
static bool mock_busy,mock_ready,mock_fresh=true,mock_pending;
static unsigned mock_epoch=1;
static weread_result_t mock_result;
static struct{weread_job_t job;char id[64],value[600];} starts[256];
static int nstarts,nsync;
static weread_auth_session_t mock_session={.vid="123"};
const weread_auth_session_t *weread_auth_get_current_session(void){return &mock_session;}
static void wr_display_copy(char *out,size_t cap,const char*value){snprintf(out,cap,"%s",value?value:"");}
static void *wr_alloc(size_t n){return calloc(n,1);}
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
static ui_keyboard_result_t ui_keyboard_tap(int x,int y,int t,uint64_t n){return UI_KEYBOARD_NONE;}
static bool ui_text_input_tap(ui_text_edit_t*e,EpdRect r,int p,bool b,const char*s,int x,int y){return false;}
static ui_keyboard_update_t ui_keyboard_update(void*f,int t,EpdRect r,int p,bool b,const char*s,bool q){return(ui_keyboard_update_t){0};}
static void read_pico_transfer_get_status(read_pico_transfer_status_t*s){memset(s,0,sizeof(*s));}
static bool app_book_request_open(const char*s){return true;}
static const void *app_at(int i){return(void*)1;}
bool weread_runtime_start(weread_job_t job,const char*id,const char*value){
    if(mock_busy||mock_ready)return false;assert(nstarts<256);
    starts[nstarts].job=job;snprintf(starts[nstarts].id,64,"%s",id?id:"");snprintf(starts[nstarts].value,600,"%s",value?value:"");nstarts++;if(job==WR_JOB_SYNC)nsync++;mock_busy=true;return true;
}
bool weread_runtime_busy(void){return mock_busy;}
bool weread_runtime_poll(weread_result_t*out){if(!mock_ready)return false;*out=mock_result;memset(&mock_result,0,sizeof(mock_result));mock_ready=false;return true;}
void weread_runtime_result_free(weread_result_t*r){free(r->books);free(r->cover);memset(r,0,sizeof(*r));}
void weread_runtime_cancel_join(void){mock_busy=false;}
void weread_runtime_stop(void){mock_busy=false;if(mock_ready)weread_runtime_result_free(&mock_result);mock_ready=false;}
void weread_runtime_media_lost(void){weread_runtime_stop();mock_epoch++;}
unsigned weread_runtime_cache_epoch(void){return mock_epoch;}
bool weread_shelf_cache_fresh(uint64_t now){return mock_fresh;}
bool weread_shelf_sync_pending(void){return mock_pending;}
bool weread_runtime_choose(const char*id,const weread_progress_t*p,bool cloud){return true;}
#include "cover.inc"
#include "production.inc"
static void complete(weread_result_t r){assert(mock_busy&&!mock_ready);mock_result=r;mock_busy=false;mock_ready=true;}
static weread_book_t *books(int n,bool reverse){weread_book_t*b=calloc((size_t)n,sizeof(*b));assert(b);for(int i=0;i<n;i++){int j=reverse?n-i-1:i;snprintf(b[i].book_id,sizeof(b[i].book_id),"id_%d",j);snprintf(b[i].title,sizeof(b[i].title),"Book %d",j);snprintf(b[i].cover,sizeof(b[i].cover),"https://cdn.qq.com/s_%d.jpg",j);}return b;}
static app_ctx_t ctx={.now_ms=10000};
static void reset(void){release_covers();free(s_books);free(s_matches);s_books=NULL;s_matches=NULL;s_count=s_visible=0;s_page=0;s_query[0]=s_requested[0]=s_message[0]=0;s_owner=0;s_epoch=0;s_sync_not_before=0;s_more_sync=false;s_next_action=s_next_sync=s_sync_check=0;s_cover_remote=s_force_cover=false;weread_runtime_stop();memset(starts,0,sizeof(starts));nstarts=nsync=0;mock_fresh=true;mock_pending=false;ctx=(app_ctx_t){.now_ms=10000};}
static void initialized(int n){enter(&ctx);assert(nstarts==1&&starts[0].job==WR_JOB_INIT);complete((weread_result_t){.job=WR_JOB_INIT,.authenticated=true,.books=books(n,false),.count=(size_t)n});assert(tick(&ctx)==APP_REDRAW_PAGE);assert(nsync==0);}
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
    ui_gesture_event_t e={.type=UI_GESTURE_TAP,.x0=500,.y0=200};assert(gesture(&ctx,&e)==APP_REDRAW_PAGE);for(int i=0;i<6;i++)assert(s_covers[i].pixels==old[i]);puts("layout retains six rasters");
    assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=books(6,true),.count=6});assert(tick(&ctx)==APP_REDRAW_PAGE);for(int i=0;i<6;i++)assert(s_covers[i].pixels==old[5-i]);puts("reorder transfers rasters by ID+URL");
    assert(weread_runtime_start(WR_JOB_SYNC,NULL,NULL));weread_book_t*changed=books(6,true);strcpy(changed[2].cover,"https://cdn.qq.com/s_changed.jpg");complete((weread_result_t){.job=WR_JOB_SYNC,.authenticated=true,.books=changed,.count=6});assert(tick(&ctx)==APP_REDRAW_PAGE);assert(!s_covers[2].pixels&&!s_covers[2].checked);for(int i=0;i<6;i++)if(i!=2)assert(s_covers[i].pixels==old[5-i]);puts("changed URL invalidates only matching raster");
    reset();initialized(1);ctx.now_ms+=10;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_COVER);complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_ERR_NOT_FOUND});strcpy(mock_result.id,"id_0");tick(&ctx);
    ctx.now_ms+=1200;tick(&ctx);assert(starts[nstarts-1].job==WR_JOB_COVER&&starts[nstarts-1].value[0]);s_cover_remote=true;
    uint64_t now=ctx.now_ms;complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_ERR_INVALID_RESPONSE,.http_status=200,.cover_retry_seconds=300});strcpy(mock_result.id,"id_0");tick(&ctx);assert(s_covers[0].retry.failures==1&&s_covers[0].retry.not_before_ms==now+5000);
    ctx.now_ms=now+4999;int calls=nstarts;tick(&ctx);assert(nstarts==calls);ctx.now_ms=now+5150;tick(&ctx);assert(nstarts==calls+1);puts("HTTP 200 decode failure backs off 5 seconds");
    complete((weread_result_t){.job=WR_JOB_COVER,.authenticated=true,.error=ESP_FAIL,.http_status=500});strcpy(mock_result.id,"id_0");tick(&ctx);
    ctx.now_ms+=10;e=(ui_gesture_event_t){.type=UI_GESTURE_TAP,.x0=100,.y0=1080};assert(gesture(&ctx,&e)==APP_REDRAW_PAGE);assert(starts[nstarts-1].job==WR_JOB_SYNC&&s_force_cover&&!s_covers[0].checked&&!s_covers[0].retry.failures);
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
    reset();puts("production cache UI probe passed");return 0;
}
