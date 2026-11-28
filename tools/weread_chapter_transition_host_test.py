#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise the production cold-chapter controller with delayed and stale completions."""
from pathlib import Path
import subprocess, tempfile
from home_cover_cache_host_test import function

root=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK,ESP_FAIL,ESP_ERR_NO_MEM,ESP_ERR_INVALID_STATE,WEREAD_ERR_PURCHASE,WEREAD_ERR_SOLD_OUT,WEREAD_ERR_VERSION};
enum {READING,TOC,READER_PANEL_NONE,WR_JOB_NOTE,WR_JOB_CHAPTER,WR_JOB_PREFETCH,MODE_GL16};
typedef enum {APP_REDRAW_NONE,APP_REDRAW_PAGE,APP_REDRAW_AREA} app_redraw_t;
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {bool touched;} touch_t;
typedef struct {int64_t now_ms;bool consumed,request_app;touch_t *touch;uint8_t *fb;int leaf;} app_ctx_t;
typedef struct {int job,error;char id[64];uint32_t chapter_uid;} weread_result_t;
static struct {bool pending,started,committing,last_page,cloud_commit;esp_err_t error;size_t chapter,offset,source_offset,toc;int toc_page;char *anchor;}s_wr_jump;
static struct {bool valid;uint8_t mode;size_t chapter,offset,target_chapter,target_offset;int64_t until_ms;}s_chapter_jump_undo;
static bool busy,done,save_fail,load_fail,alloc_fail,s_reader_text_frame=true,cached_flag,prefetch_busy,cloud_jump_ready,cloud_jump_committed,mock_has_turned,rebase_fail;
static bool stage_ready;static char stage_text[64];static EpdRect s_area;static int s_mode,renders;
static int stage_kind;static uint64_t stage_bytes;
typedef enum {WEREAD_STAGE_NONE=0,WEREAD_STAGE_PROGRESS,WEREAD_STAGE_TOC,WEREAD_STAGE_CHAPTER,WEREAD_STAGE_PARSE,WEREAD_STAGE_RETRY} weread_stage_t;
typedef enum {WEREAD_CLOUD_SYNC_IDLE=0,WEREAD_CLOUD_SYNC_ACTIVE,WEREAD_CLOUD_SYNC_EXPIRED,WEREAD_CLOUD_SYNC_TIMEOUT,WEREAD_CLOUD_SYNC_OFFLINE,WEREAD_CLOUD_SYNC_VERSION,WEREAD_CLOUD_SYNC_FAILED} weread_cloud_sync_ui_state_t;
static weread_cloud_sync_ui_state_t mock_cloud_sync_ui=WEREAD_CLOUD_SYNC_IDLE;
static bool s_reader_fullscreen=false,mock_immersive=false,s_wr_sync_pill_shown=false;
static weread_cloud_sync_ui_state_t s_wr_sync_pill_last=WEREAD_CLOUD_SYNC_IDLE;
static void wr_sync_ui_reset(void){s_wr_sync_pill_last=WEREAD_CLOUD_SYNC_IDLE;s_wr_sync_pill_shown=false;}
static weread_cloud_sync_ui_state_t weread_reader_cloud_sync_state(void){return mock_cloud_sync_ui;}
static bool app_settings_reader_immersive(void){return mock_immersive;}
static bool weread_reader_stage(weread_stage_t *stage,uint64_t *bytes){if(!stage_ready)return false;*stage=(weread_stage_t)stage_kind;if(bytes)*bytes=stage_bytes;return true;}
static bool weread_reader_stage_text(char *out,size_t cap){if(!stage_ready)return false;snprintf(out,cap,"%s",stage_text);return true;}
static void render(app_ctx_t *ctx,uint8_t *fb){(void)ctx;(void)fb;++renders;}
static size_t cloud_jump_ch;static uint32_t cloud_jump_off,adopted_uid;
static int s_view=READING,s_reader_panel=READER_PANEL_NONE;
static bool s_clear_confirm;
static char s_path[]="weread://book_A",s_message[128],s_reader_notice[64],s_title[64];
static int64_t s_reader_notice_until;
static size_t s_chapter=2,s_page=7,s_selected_toc,s_selected_toc_offset;
static unsigned starts,cancels,loads,moves,saves,rebases,cloud_commits_false,toc_prepares;
static const char *s_text="old complete body";
static weread_result_t ready;
static void *wr_alloc(size_t bytes){return alloc_fail?NULL:calloc(1,bytes);}
static void copy_text(char *to,size_t cap,const char *from){snprintf(to,cap,"%s",from);}
static bool weread_runtime_busy(void){return busy;}
static bool weread_reader_cached(const char *uri,size_t ch){(void)uri;(void)ch;return cached_flag;}
static bool book_path_is_weread(const char *p){return p&&strncmp(p,"weread://",9)==0;}
static bool weread_runtime_prefetch_active(const char *id,uint32_t uid){(void)id;(void)uid;return prefetch_busy;}
static void weread_runtime_prefetch_adopt(uint32_t uid){adopted_uid=uid;}
static bool weread_reader_poll_cloud_jump(const char *uri,size_t *ch,uint32_t *off){(void)uri;if(!cloud_jump_ready)return false;*ch=cloud_jump_ch;*off=cloud_jump_off;return true;}
static void weread_reader_commit_cloud_jump(const char *uri,bool committed){(void)uri;cloud_jump_ready=false;if(committed)cloud_jump_committed=true;else ++cloud_commits_false;}
static bool weread_runtime_reader_has_turned(const char *uri){(void)uri;return mock_has_turned;}
static esp_err_t weread_runtime_cloud_jump_rebase(const char *uri){(void)uri;++rebases;if(rebase_fail)return ESP_FAIL;cloud_jump_ready=false;return ESP_OK;}
static size_t book_navigation_count(void){return 12;}
static size_t current_toc_position(void){return 0;}
static void book_toc_prepare_page(size_t chapters,size_t current,int page){(void)chapters;(void)current;(void)page;++toc_prepares;}
static void weread_runtime_cancel(void){++cancels;}
static bool weread_runtime_poll(weread_result_t *r){if(busy||!done)return false;done=false;*r=ready;return true;}
static void weread_runtime_result_free(weread_result_t *r){memset(r,0,sizeof(*r));}
static uint32_t book_weread_uid(size_t ch){return (uint32_t)(10+ch*7);}
static void wr_reader_sample(uint64_t now,bool active){(void)now;(void)active;}
static esp_err_t weread_reader_save(uint64_t now){(void)now;++saves;return save_fail?ESP_FAIL:ESP_OK;}
static bool weread_runtime_start(int job,const char *id,const char *uid){
 assert(!busy&&job==WR_JOB_CHAPTER&&!strcmp(id,"book_A"));
 ++starts;busy=true;done=false;ready=(weread_result_t){.job=job,.chapter_uid=(uint32_t)strtoul(uid,NULL,10)};strcpy(ready.id,id);return true;
}
static size_t reader_page_offset(size_t page){return page*100;}
static esp_err_t book_navigation_title(size_t toc,char *out,size_t cap){(void)toc;copy_text(out,cap,"new title");return ESP_OK;}
static void wr_reader_move(void){++moves;}
static void save_progress(void){++saves;}
static bool load_chapter_at(app_ctx_t *ctx,size_t chapter,size_t offset,bool last,const char *anchor,size_t source){
 (void)ctx;(void)offset;(void)source;assert(s_wr_jump.committing);++loads;
 if(load_fail)return false;
 if(anchor)assert(!strcmp(anchor,"owned-anchor"));
 s_chapter=chapter;s_page=last?9:0;s_text="new complete body";return true;
}
'''
unit+=function('wr_chapter_cancel',root/'main/apps/book_weread_bridge.inc')+'\n'
unit+=(root/'main/apps/book_weread_chapter.inc').read_text()+'\n'
unit+=r'''
static void complete(int err){busy=false;done=true;ready.error=err;}
int main(void){
 app_ctx_t ctx={.now_ms=1000,.fb=(uint8_t*)1};
 busy=true;assert(wr_chapter_queue(&ctx,3,0,false,NULL,SIZE_MAX));
 // 未启动本页异步任务的阶段提示不得借用其他任务的文案。
 // A stage notice must not be adopted while this page's asynchronous job has not started.
 stage_ready=true;snprintf(stage_text,sizeof(stage_text),"下载章节正文…");
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&!strcmp(s_reader_notice,"正在准备章节…"));
 stage_ready=false;
 assert(s_wr_jump.pending&&!starts&&!loads&&s_chapter==2&&s_page==7);
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&!starts);
 busy=false;assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&starts==1&&busy);
 // Cold-stage toast: only a changed worker label repaints the toast region, never every tick.
 stage_ready=true;snprintf(stage_text,sizeof(stage_text),"同步阅读进度…");
 int renders_before=renders;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&renders==renders_before+1);
 assert(!strcmp(s_reader_notice,"同步阅读进度…")&&s_mode==MODE_GL16&&s_area.x==208&&s_area.width==268);
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&renders==renders_before+1);
 ctx.consumed=true;snprintf(stage_text,sizeof(stage_text),"下载章节正文…");
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&!strcmp(s_reader_notice,"同步阅读进度…"));
 ctx.consumed=false;assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&renders==renders_before+2);
 assert(!strcmp(s_reader_notice,"下载章节正文…"));
 // 同阶段 KB 文案按 3 秒合并；触摸/消费态在限速写入之前返回，不能吞掉下一次更新。
 // Same-stage KB labels coalesce to 3 s; touch/consumed return before the limiter is armed so the next update survives.
 stage_kind=WEREAD_STAGE_CHAPTER;snprintf(stage_text,sizeof(stage_text),"正在下载章节 32 KB…");
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&renders==renders_before+3);
 assert(!strcmp(s_reader_notice,"正在下载章节 32 KB…"));
 ctx.now_ms=5000;touch_t touch={.touched=true};ctx.touch=&touch;
 snprintf(stage_text,sizeof(stage_text),"正在下载章节 384 KB…");
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&renders==renders_before+3);
 touch.touched=false;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&renders==renders_before+4);
 assert(!strcmp(s_reader_notice,"正在下载章节 384 KB…"));
 snprintf(stage_text,sizeof(stage_text),"正在下载章节 416 KB…");
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&renders==renders_before+4);
 ctx.touch=NULL;ctx.now_ms=9000;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&renders==renders_before+5);
 stage_ready=false;snprintf(stage_text,sizeof(stage_text),"解析章节…");
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&renders==renders_before+5);
 complete(WEREAD_ERR_PURCHASE);assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE);
 assert(!s_wr_jump.pending&&!loads&&!moves&&s_chapter==2&&s_page==7&&!strcmp(s_reader_notice,"手机端购买本章后，再翻页即可加载")&&s_reader_notice_until==ctx.now_ms+5000);
 char anchor[]="owned-anchor";assert(wr_chapter_queue(&ctx,3,0,false,anchor,SIZE_MAX));anchor[0]='X';
 assert(starts==2&&busy); // No tick or feedback presentation is needed to start the request.
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&busy);
 assert(wr_chapter_queue(&ctx,5,0,true,NULL,SIZE_MAX));
 complete(ESP_OK);assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&busy&&starts==3&&!loads);
 complete(ESP_OK);assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&loads==1&&moves==1&&s_chapter==5&&s_page==9);
 assert(!s_wr_jump.pending&&!s_reader_notice[0]);
 save_fail=true;assert(wr_chapter_queue(&ctx,6,0,false,NULL,SIZE_MAX));
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&!busy&&starts==3&&s_chapter==5&&!s_wr_jump.pending);
 save_fail=false;assert(wr_chapter_queue(&ctx,7,0,false,NULL,SIZE_MAX));
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&busy);
 // A done flag alone, or a completion belonging to another book/UID, is never readiness.
 complete(ESP_OK);strcpy(ready.id,"other_book");assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&loads==1&&s_wr_jump.pending);
 strcpy(ready.id,"book_A");done=true;ready.chapter_uid=1;assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&loads==1);
 done=true;ready.chapter_uid=book_weread_uid(7);load_fail=true;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&loads==2&&moves==1&&s_chapter==5);
 load_fail=false;assert(wr_chapter_queue(&ctx,8,0,false,NULL,SIZE_MAX));
 ctx.request_app=true;assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&!s_wr_jump.pending&&!s_reader_notice[0]);ctx.request_app=false;
 complete(ESP_ERR_INVALID_STATE); // The cancelled worker finishes without committing its old target.
 alloc_fail=true;assert(!wr_chapter_queue(&ctx,9,0,false,"owned-anchor",SIZE_MAX)&&!s_wr_jump.pending);
 alloc_fail=false;assert(wr_chapter_queue(&ctx,9,0,false,"owned-anchor",SIZE_MAX));
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE);complete(ESP_OK);
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&s_chapter==9&&moves==2);
 wr_chapter_cancel();
 // Adopting an in-flight same-chapter prefetch never cancels or restarts it.
 unsigned c_before=cancels,s_before=starts;busy=true;prefetch_busy=true;
 ready=(weread_result_t){.job=WR_JOB_PREFETCH,.chapter_uid=book_weread_uid(10)};strcpy(ready.id,"book_A");
 assert(wr_chapter_queue(&ctx,10,0,false,NULL,SIZE_MAX));
 assert(cancels==c_before&&starts==s_before&&s_wr_jump.started&&adopted_uid==book_weread_uid(10));
 prefetch_busy=false;complete(ESP_OK);
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&s_chapter==10&&!s_wr_jump.pending&&!adopted_uid);
 // Adopted prefetch failure falls back once to foreground WR_JOB_CHAPTER instead of surfacing a prefetch-only error.
 busy=true;prefetch_busy=true;s_before=starts;
 ready=(weread_result_t){.job=WR_JOB_PREFETCH,.chapter_uid=book_weread_uid(11)};strcpy(ready.id,"book_A");
 assert(wr_chapter_queue(&ctx,11,0,false,NULL,SIZE_MAX));prefetch_busy=false;complete(ESP_FAIL);
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&starts==s_before+1&&busy&&s_wr_jump.pending&&!adopted_uid);
 complete(ESP_OK);assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&s_chapter==11&&!s_wr_jump.pending);
 // Cloud-jump load failure never commits or overwrites progress.
 cloud_jump_ready=true;cloud_jump_committed=false;cloud_jump_ch=12;cloud_jump_off=40;load_fail=true;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&!cloud_jump_ready&&!cloud_jump_committed&&s_chapter==11);
 load_fail=false;
 // Async cloud-sync forward jump when not turned auto-jumps, commits, and shows 6s [返回原处] banner.
 s_chapter_jump_undo.valid=false;mock_has_turned=false;s_page=7;
 cloud_jump_ready=true;cloud_jump_ch=12;cloud_jump_off=40;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&cloud_jump_committed&&s_chapter==12);
 assert(s_chapter_jump_undo.valid&&s_chapter_jump_undo.mode==1&&s_chapter_jump_undo.chapter==12);
 assert(s_chapter_jump_undo.target_chapter==11&&s_chapter_jump_undo.target_offset==700&&s_chapter_jump_undo.until_ms==ctx.now_ms+6000);
 // Async cloud-sync when user already turned rebases without jumping and shows 6s [点击跳转] banner.
 s_chapter_jump_undo.valid=false;mock_has_turned=true;cloud_jump_ready=true;cloud_jump_ch=14;cloud_jump_off=88;
 unsigned loads_before=loads;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&loads==loads_before&&rebases==1);
 assert(s_chapter_jump_undo.valid&&s_chapter_jump_undo.mode==2&&s_chapter_jump_undo.chapter==12);
 assert(s_chapter_jump_undo.target_chapter==14&&s_chapter_jump_undo.target_offset==88&&s_chapter_jump_undo.until_ms==ctx.now_ms+6000);
 s_chapter_jump_undo.valid=false;mock_has_turned=false;
 // Offline/network failure on started job gives 5000ms retry prompt; TOC failure auto-returns to original TOC leaf.
 assert(wr_chapter_queue(&ctx,15,0,false,NULL,SIZE_MAX));
 s_wr_jump.toc=4;s_wr_jump.toc_page=3;ctx.leaf=3;s_view=READING;
 complete(ESP_ERR_INVALID_STATE);assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE);
 assert(!s_wr_jump.pending&&s_view==TOC&&ctx.leaf==3&&toc_prepares==1);
 assert(!strcmp(s_reader_notice,"本章未缓存且网络不可用，连网后再翻页重试")&&s_reader_notice_until==ctx.now_ms+5000);
 s_view=READING;
 // Cold-chapter cloud_commit cancels cleanly and commits false.
 assert(wr_chapter_queue(&ctx,16,0,false,NULL,SIZE_MAX));
 s_wr_jump.cloud_commit=true;unsigned cf=cloud_commits_false;
 wr_chapter_cancel();assert(!s_wr_jump.pending&&cloud_commits_false==cf+1);
 complete(ESP_ERR_INVALID_STATE);
 // Cold-chapter OOM surfaces the retry-later toast instead of generic load failure.
 assert(wr_chapter_queue(&ctx,13,0,false,NULL,SIZE_MAX));
 complete(ESP_ERR_NO_MEM);assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE);
 assert(!s_wr_jump.pending&&s_chapter==12&&!strcmp(s_reader_notice,"内存不足，请稍后重试"));
 // Challenge 2: cloud jumps defer cleanly while an overlay panel or clear-confirm modal is open, then surface on close.
 s_chapter_jump_undo.valid=false;mock_has_turned=true;cloud_jump_ready=true;cloud_jump_ch=15;cloud_jump_off=50;
 s_reader_panel=1;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&cloud_jump_ready&&!s_chapter_jump_undo.valid);
 s_reader_panel=READER_PANEL_NONE;s_clear_confirm=true;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&cloud_jump_ready&&!s_chapter_jump_undo.valid);
 s_clear_confirm=false;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&!cloud_jump_ready&&s_chapter_jump_undo.valid&&s_chapter_jump_undo.mode==2);
 s_chapter_jump_undo.valid=false;
 mock_has_turned=false;cloud_jump_ready=true;cloud_jump_committed=false;cloud_jump_ch=16;cloud_jump_off=60;
 s_reader_panel=1;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&cloud_jump_ready&&!cloud_jump_committed&&s_chapter==12&&s_reader_panel==1);
 s_reader_panel=READER_PANEL_NONE;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_PAGE&&!cloud_jump_ready&&cloud_jump_committed&&s_chapter==16&&s_chapter_jump_undo.mode==1);
 s_chapter_jump_undo.valid=false;
 // Bug 1 regression: active WR_UNDO_LOCAL must be cleared when queuing a cold chapter load.
 s_chapter_jump_undo.valid=true;s_chapter_jump_undo.mode=0;
 assert(wr_chapter_queue(&ctx,14,0,false,NULL,SIZE_MAX));
 assert(s_wr_jump.pending&&!s_chapter_jump_undo.valid);
 wr_chapter_cancel();
 complete(ESP_ERR_INVALID_STATE);
 // Immersive cloud-sync pill & error/expired toasts
 s_reader_notice[0]=0;s_reader_notice_until=0;wr_sync_ui_reset();
 s_reader_fullscreen=false;mock_immersive=false;mock_cloud_sync_ui=WEREAD_CLOUD_SYNC_ACTIVE;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE&&s_reader_notice[0]==0);
 s_reader_fullscreen=true;mock_immersive=true;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&!strcmp(s_reader_notice,"正在同步云端进度…"));
 mock_cloud_sync_ui=WEREAD_CLOUD_SYNC_IDLE;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&s_reader_notice[0]==0);
 mock_cloud_sync_ui=WEREAD_CLOUD_SYNC_ACTIVE;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&!strcmp(s_reader_notice,"正在同步云端进度…"));
 mock_cloud_sync_ui=WEREAD_CLOUD_SYNC_TIMEOUT;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&!strcmp(s_reader_notice,"云端同步超时")&&s_reader_notice_until==ctx.now_ms+3000);
 s_reader_notice[0]=0;s_reader_notice_until=0;s_reader_fullscreen=false;mock_immersive=false;
 mock_cloud_sync_ui=WEREAD_CLOUD_SYNC_EXPIRED;
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_AREA&&!strcmp(s_reader_notice,"登录已过期，暂无法同步"));
 assert(wr_chapter_tick(&ctx)==APP_REDRAW_NONE);
 puts("PASS: production chapter controller adopts prefetch, applies cloud jumps, coalesces targets, and cancels safely");
}
'''
with tempfile.TemporaryDirectory(prefix='weread-chapter-transition-') as folder:
    path=Path(folder);(path/'test.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(path/'test.c'),'-o',str(path/'test')],check=True)
    subprocess.run([str(path/'test')],check=True)
