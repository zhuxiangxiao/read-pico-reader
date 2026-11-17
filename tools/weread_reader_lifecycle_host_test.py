#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Execute both boards' production leave/lock gates, including orphaned RAM progress.

Hardware/storage boundaries are instrumented for order; these tests do not invent
long network-held WR locks or replace the runtime's delayed-HTTP timing tests.
"""
from pathlib import Path
import os
import subprocess
import tempfile
from home_cover_cache_host_test import function

root=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
enum {ESP_OK=0,ESP_FAIL=-1,APP_REDRAW_PAGE=2};
typedef struct {uint64_t now_ms;void *hl,*fb;} app_ctx_t;
typedef struct app_desc {int tab;void(*on_before_lock)(app_ctx_t*);}app_desc_t;
typedef struct {uint8_t tab,reader,fullscreen;char path[128];}pico_resume_t;
static const char *s_text;
static char s_path[128],s_reader_notice[128],s_message[128];
static uint64_t s_reader_notice_until;
static bool ram_pending,save_fails,resume_ready;
static unsigned ram_revision,samples,saves,joins,cancels,callbacks,checkpoints,presents;
static char trace[128];static size_t trace_at;
static pico_resume_t committed;
static void mark(char event){assert(trace_at+1<sizeof(trace));trace[trace_at++]=event;trace[trace_at]=0;}
static bool book_path_is_weread(const char *path){return path&&!strncmp(path,"weread://",9);}
static bool weread_reader_pending_save(void){return ram_pending;}
static void wr_chapter_cancel(void){++cancels;mark('C');}
static void weread_runtime_cancel_join(void){++joins;mark('J');}
static void wr_reader_sample(uint64_t now,bool active){assert(now==1234&&!active);++samples;mark('N');}
static esp_err_t weread_reader_save(uint64_t now){assert(now==1234);++saves;mark('S');if(save_fails)return ESP_FAIL;ram_pending=false;return ESP_OK;}
static void copy_text(char *to,size_t cap,const char *from){snprintf(to,cap,"%s",from);}
static void app_present(app_ctx_t *ctx,const app_desc_t *app,int redraw){(void)ctx;(void)app;assert(redraw==APP_REDRAW_PAGE);++presents;mark('P');}
static void app_lock_feedback(void *hl){(void)hl;mark('F');}
static int app_index_of(const app_desc_t *app){return app->tab;}
static int app_nav_position(int tab){return tab>=0&&tab<7?tab:-1;}
static bool app_book_resume_context(char *path,size_t cap,bool *fullscreen){mark('R');snprintf(path,cap,"%s",s_path);*fullscreen=true;return s_text!=NULL;}
static uint8_t app_book_resume_origin(void){return 6;}
static esp_err_t pico_boot_save_resume(const pico_resume_t *resume){++checkpoints;mark('K');committed=*resume;return ESP_OK;}
static void __attribute__((unused)) app_book_shelf_prefetch_cancel(void){mark('I');}
static void __attribute__((unused)) app_lock_set_resume_ready(bool ready){resume_ready=ready;mark('A');}
static int64_t __attribute__((unused)) esp_timer_get_time(void){return 100000;}
#define ESP_LOGI(...) ((void)0)
#define TAG "lifecycle"
static void callback(app_ctx_t *ctx){(void)ctx;++callbacks;mark('B');}
'''
main=r'''
static void reset(bool reading,bool pending,bool fail){
 s_text=reading?"presented complete body":NULL;
 snprintf(s_path,sizeof(s_path),"%s",reading?"weread://book_A":"/sdcard/local.epub");
 ram_pending=pending;save_fails=fail;ram_revision=77;samples=saves=joins=cancels=callbacks=checkpoints=presents=0;
 trace_at=0;trace[0]=s_reader_notice[0]=s_message[0]=0;s_reader_notice_until=0;resume_ready=false;memset(&committed,0,sizeof(committed));
}
int main(void){
 app_ctx_t ctx={.now_ms=1234};
 // Ordinary local books never touch the WeRead worker, sampling, saving or network.
 reset(false,false,false);s_text="local body";
 assert(app_book_prepare_leave(&ctx)&&!trace[0]&&!saves&&!samples&&!joins&&!cancels);
 for(int tab=0;tab<7;++tab){
  app_desc_t app={.tab=tab,.on_before_lock=callback};reset(false,false,false);s_text="local body";
  assert(prepare_lock(&ctx,&app,true)&&!saves&&!samples&&!joins&&!cancels&&callbacks==1&&checkpoints==1&&!presents);
  assert(strchr(trace,'F')<strchr(trace,'B')&&strchr(trace,'B')<strchr(trace,'K'));
 }
 puts("PASS: local books on every tab add no additional WeRead worker/save/sample entry points");
 // A failed orphaned WeRead save remains in RAM even while another tab/local book is current.
 for(int tab=0;tab<7;++tab){
  app_desc_t app={.tab=tab,.on_before_lock=callback};reset(false,true,true);
  assert(!prepare_lock(&ctx,&app,true)&&ram_pending&&ram_revision==77);
  assert(!strcmp(trace,"CJSP")&&joins==1&&cancels==1&&saves==1&&!samples&&!callbacks&&!checkpoints&&presents==1);
  assert(strstr(s_message,"TF")&&s_reader_notice_until==4234);
  // Repeated attempts cannot discard the failed sample or proceed to callback/checkpoint.
  assert(!prepare_lock(&ctx,&app,false)&&ram_pending&&ram_revision==77&&!callbacks&&!checkpoints);
 }
 puts("PASS: failed orphaned RAM progress blocks lock across all tabs before feedback, callbacks or checkpoints");
 for(int tab=0;tab<7;++tab){
  app_desc_t app={.tab=tab,.on_before_lock=callback};reset(false,true,false);
  assert(prepare_lock(&ctx,&app,false)&&!ram_pending&&saves==1&&!samples&&callbacks==1&&checkpoints==1&&!presents);
  assert(!strncmp(trace,"CJS",3)&&strchr(trace,'S')<strchr(trace,'B')&&strchr(trace,'B')<strchr(trace,'K'));
 }
 puts("PASS: durable orphan recovery precedes lock callbacks and resume checkpoint on every tab");
 app_desc_t reader={.tab=1,.on_before_lock=callback};reset(true,true,true);
 assert(!prepare_lock(&ctx,&reader,true)&&ram_pending&&!strcmp(trace,"CJNSP")&&!callbacks&&!checkpoints&&!s_message[0]);
 reset(true,true,false);assert(prepare_lock(&ctx,&reader,true)&&!ram_pending&&samples==1&&saves==1);
 assert(!strncmp(trace,"CJNS",4)&&strchr(trace,'S')<strchr(trace,'F')&&strchr(trace,'B')<strchr(trace,'K'));
 assert(committed.reader==1&&committed.tab==6&&committed.fullscreen&&!strcmp(committed.path,s_path));
#if KIIKO_BOARD_METALIO47
 assert(resume_ready);
#else
 assert(strchr(trace,'I')>strchr(trace,'S')&&strchr(trace,'I')<strchr(trace,'F'));
#endif
 puts("PASS: active reader samples only after cancellation/join; saved position precedes lock and preserves WeRead resume origin");
}
'''
with tempfile.TemporaryDirectory(prefix='weread-reader-lifecycle-') as folder:
    build=Path(folder)
    for board,app,loop in (
        ('metalio47',root/'main/apps/app_book.c',root/'main/app/app_loop.c'),
        ('pico',root/'boards/pico/src/main/apps/app_book.c',root/'boards/pico/src/main/app/app_loop.c'),
    ):
        source=unit+function('app_book_prepare_leave',app)+'\n'+function('prepare_lock',loop)+'\n'+main
        path=build/f'{board}.c';path.write_text(source)
        binary=build/board
        subprocess.run([os.environ.get('CC','cc'),'-std=c11',f'-DKIIKO_BOARD_METALIO47={int(board=="metalio47")}',
            '-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(path),'-o',str(binary)],check=True)
        print(f'Production reader lifecycle: {board}',flush=True)
        subprocess.run([str(binary)],check=True)
