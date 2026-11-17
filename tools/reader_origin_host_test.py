#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Actual reader requests and returns preserve Home/local/WeRead origins and failure routing."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "book/book_file_path.h"
#include "book/book_weread_uri.h"
#define BOOK_STORE_PATH_MAX 288
enum{SHELF,READING,READER_PANEL_NONE};
typedef enum{APP_REDRAW_NONE,APP_REDRAW_PAGE}app_redraw_t;
typedef struct{int64_t now_ms;int destination;}app_ctx_t;
static char s_requested_open[288];
static uint8_t s_requested_open_origin=1,s_reader_return_origin=1;
static bool s_requested_fullscreen,s_requested_resume_mode,fail_open,open_failed;
static int s_view=READING,s_reader_panel,freed,saved,returned;
static bool app_settings_reader_default_fullscreen(void){return false;}
static void copy_text(char*out,size_t cap,const char*in){snprintf(out,cap,"%s",in);}
static bool open_book(app_ctx_t*c,const char*p){(void)c;assert(p[0]);s_reader_return_origin=1;return !fail_open;}
static void app_weread_reader_failed(void){open_failed=true;}
static void ui_nav_request(app_ctx_t*c,int page){c->destination=page;}
static void app_book_ticket_sample(int64_t now,bool active){(void)now;assert(!active);}
static void save_progress(void){++saved;}
static void free_book(void){++freed;}
static void return_to_cached_shelf(app_ctx_t*c){(void)c;++returned;}
'''
post=r'''
int main(void){app_ctx_t c={.destination=-1};const char *local="/sdcard/books/local.epub",*cloud="weread://test_book_101";
 assert(app_book_request_open(local)&&open_requested_book(&c)&&app_book_resume_origin()==1);
 assert(reader_return(&c)==APP_REDRAW_PAGE&&c.destination==-1&&s_view==SHELF&&freed==1&&returned==1);
 assert(app_book_request_open_from_weread(cloud)&&open_requested_book(&c)&&app_book_resume_origin()==6);
 assert(reader_return(&c)==APP_REDRAW_NONE&&c.destination==6);
 assert(app_book_request_open_from_home(cloud)&&open_requested_book(&c)&&app_book_resume_origin()==0);
 assert(reader_return(&c)==APP_REDRAW_NONE&&c.destination==0);
 for(int home=0;home<2;++home){assert(app_book_request_resume(cloud,true,home?0:6)&&s_requested_fullscreen&&s_requested_resume_mode);assert(open_requested_book(&c)&&app_book_resume_origin()==(home?0:6));}
 assert(app_book_request_resume(local,false,1)&&open_requested_book(&c)&&app_book_resume_origin()==1);
 assert(!app_book_request_open_from_weread(local));
 fail_open=true;open_failed=false;c.destination=-1;assert(app_book_request_open_from_weread(cloud)&&!open_requested_book(&c)&&c.destination==6&&open_failed&&!s_requested_open[0]);
 c.destination=-1;assert(app_book_request_open_from_home(local)&&!open_requested_book(&c)&&c.destination==0);
 c.destination=-1;assert(app_book_request_open(local)&&!open_requested_book(&c)&&c.destination==-1);
 puts("PASS: Home/local/WeRead opens, matching returns, explicit sleep origins, invalid source and failed-open recovery");}
'''
for relative in ('main/apps/app_book.c','boards/pico/src/main/apps/app_book.c'):
 source=root/relative
 body=pre+'\n'.join(function(n,source) for n in ('app_book_request_open','app_book_request_open_from_home','app_book_request_open_from_weread','app_book_resume_origin','app_book_request_resume','open_requested_book','reader_return'))+post
 with tempfile.TemporaryDirectory(prefix='reader-origin-') as d:
  p=Path(d);(p/'test.c').write_text(body)
  subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(root/'main'),str(p/'test.c'),'-o',str(p/'probe')],check=True)
  subprocess.run([str(p/'probe')],check=True)
