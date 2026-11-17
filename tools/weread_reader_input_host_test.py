#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise actual text/IO boundary classification and the progress-only scheduling exception."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
enum{READING,TOC,READER_PANEL_NONE,READER_PANEL_TOOLS,WR_JOB_NOTE=10,WR_JOB_PREFETCH=12,WR_JOB_REPORT=16,WR_JOB_CHAPTER=11};
static int s_view=READING,s_reader_panel=READER_PANEL_NONE;
static const char*s_text="loaded text";static bool s_clear_confirm,busy;static size_t s_page;
static unsigned count=3,images[3],joins,cancels;static struct{unsigned job;}s_work;
static size_t book_layout_page_count(void){return count;}
static unsigned book_layout_page_image_count(size_t page){assert(page<count);return images[page];}
static bool weread_runtime_busy(void){return busy;}
static void weread_runtime_cancel_join(void){++joins;busy=false;}
static void weread_runtime_cancel(void){++cancels;}
'''
for name in ('wr_reader_text_page','wr_reader_text_turn'):
    unit+=function(name,root/'main/apps/book_weread_bridge.inc')+'\n'
unit+=function('weread_reader_input',root/'main/weread/weread_runtime.c')+'\n'
unit+=r'''
int main(void){
 assert(wr_reader_text_page()&&wr_reader_text_turn(1)&&!wr_reader_text_turn(-1));
 busy=true;s_work.job=WR_JOB_NOTE;weread_reader_input(wr_reader_text_turn(1));assert(busy&&!joins);
 s_page=1;assert(wr_reader_text_turn(-1)&&wr_reader_text_turn(1));
 images[2]=1;assert(!wr_reader_text_turn(1));weread_reader_input(wr_reader_text_turn(1));assert(!busy&&joins==1);
 images[2]=0;s_page=2;assert(!wr_reader_text_turn(1)&&wr_reader_text_turn(-1));
 images[2]=1;assert(!wr_reader_text_page()&&!wr_reader_text_turn(-1));images[2]=0;
 s_page=0;s_reader_panel=READER_PANEL_TOOLS;assert(!wr_reader_text_page());s_reader_panel=READER_PANEL_NONE;
 s_clear_confirm=true;assert(!wr_reader_text_page());s_clear_confirm=false;
 s_view=TOC;assert(!wr_reader_text_page());s_view=READING;s_text=NULL;assert(!wr_reader_text_page());s_text="loaded text";
 count=0;assert(!wr_reader_text_page());count=3;
 busy=true;s_work.job=WR_JOB_PREFETCH;weread_reader_input(true);assert(busy&&cancels==1&&joins==1);
 busy=true;s_work.job=WR_JOB_NOTE;weread_reader_input(false);assert(!busy&&joins==2);
 puts("PASS: loaded text never joins background IO; images and lifecycle retain mandatory joins");
}
'''
for path in ('main/apps/app_book.c','boards/pico/src/main/apps/app_book.c'):
    source=root/path
    assert 'weread_reader_input(wr_reader_text_turn(dir))' in function('turn_page',source)
    for name in ('gesture_event','on_key'):
        assert 'weread_reader_input(wr_reader_text_page())' in function(name,source)
    assert 'weread_runtime_cancel_join()' in function('before_lock',source)
with tempfile.TemporaryDirectory(prefix='weread-input-') as d:
    p=Path(d);(p/'test.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
