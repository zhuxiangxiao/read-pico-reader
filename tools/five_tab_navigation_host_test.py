#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise stable page identities, five-column boundaries and actual root-key dispatch."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ui_nav_layout.h"
#define UI_LOCK_WIDTH 684
#define UI_LOCK_HEIGHT 1216
#define UI_KEY_1 0
#define UI_KEY_2 1
#define UI_KEY_3 2
#define APP_NAV_COUNT 5
#define UI_NAV_BACKGROUND_GRAY 0xe0
#define UI_GRAY_LIGHT 0xc0
#define UI_GRAY_WHITE 255
#define UI_GRAY_BLACK 0
#define EPD_DRAW_ALIGN_CENTER 1
enum{APP_MAIN_REFRESH_NORMAL,APP_MAIN_REFRESH_FAST,APP_MAIN_REFRESH_WATER};
typedef struct{int x,y,width,height;}EpdRect;
typedef enum{APP_REDRAW_NONE,APP_REDRAW_AREA,APP_REDRAW_PAGE,APP_REDRAW_FULL}app_redraw_t;
struct app_desc_s;
typedef struct{const struct app_desc_s *request_app;}app_ctx_t;
typedef struct app_desc_s{bool(*main_page_visible)(app_ctx_t*);bool(*on_main_key)(app_ctx_t*,int,app_redraw_t*);}app_desc_t;
static bool root_visible=true;
static bool visible(app_ctx_t*c){(void)c;return root_visible;}
const app_desc_t app_dashboard={visible,NULL},app_book={visible,NULL},app_files={visible,NULL},app_device_settings={visible,NULL},app_weread={visible,NULL},app_image={visible,NULL},app_cloud_sync={visible,NULL};
static uint8_t frame[684*1216];static int mode,icons,texts;
static const char *labels[]={"首页","书架","微信读书","文件","设置"};
static int scale=100;
static int ui_text_effective_px(int px){return (px+2)*scale/100;}
static int ui_text_fixed_width_px(int px,const char*s){return px*(int)(strlen(s)/3);}
static int app_settings_main_refresh_mode(void){return mode;}
static void ui_fill_rect_fast(EpdRect r,int gray,uint8_t *fb){assert(r.x>=0&&r.y>=0&&r.x+r.width<=684&&r.y+r.height<=1216);for(int y=r.y;y<r.y+r.height;++y)for(int x=r.x;x<r.x+r.width;++x)fb[y*684+x]=gray;}
static void ui_hairline(uint8_t*f,int y,int x,int w,int gray){ui_fill_rect_fast((EpdRect){x,y,w,1},gray,f);}
static void icon(uint8_t*f,int tab,int x,int y){(void)f;(void)y;assert(tab>=0&&tab<5&&x>=0&&x<684);++icons;}
static void ui_text(uint8_t*f,int x,int y,int px,const char*s,int a,bool inv){(void)f;(void)x;(void)y;(void)a;(void)inv;assert(ui_text_fixed_width_px(ui_text_effective_px(px),s)<=124);++texts;}
EpdRect ui_nav_tab_area(int id);
'''
unit+=(root/'main/app/app_registry.c').read_text().replace('#include "app_registry.h"','')
for name in ('ui_nav_tab_area','ui_nav_hit','ui_nav_request','ui_nav_draw'):unit+=function(name,root/'main/ui/ui_nav.c')+'\n'
for name in ('main_page_ready','main_key_action'):unit+=function(name,root/'main/app/app_loop.c')+'\n'
unit+=r'''
#define UI_MENU_ITEMS_PER_PAGE 8
#define UI_MENU_HIT_NONE -1
#define UI_MENU_HIT_PREV -2
#define UI_MENU_HIT_NEXT -3
#define UI_MARGIN 36
#define UI_GAP 12
#define UI_MENU_LIST_TOP 176
#define UI_MENU_ROW_H 100
static int epd_rotated_display_width(void){return 684;}
static int ui_content_width(void){return 612;}
static EpdRect menu_nav_rect(bool next){return(EpdRect){next?360:36,1100,288,60};}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&x<r.x+r.width&&y>=r.y&&y<r.y+r.height;}
'''
for name in ('ui_menu_leaf_count','leaf_range','clamp_leaf','ui_menu_leaf_for_app','ui_menu_row_rect','ui_menu_hit_test'):unit+=function(name,root/'main/ui/ui_menu.c')+'\n'

unit+=r'''
int main(void){const int ids[]={0,1,6,2,3};const int menu[]={0,1,6,2,3,4,5};app_ctx_t ctx={0};
 for(int i=0;i<7;++i){assert(app_menu_id(i)==menu[i]&&app_menu_position(menu[i])==i);
  EpdRect r;assert(ui_menu_row_rect(0,i,&r));assert(ui_menu_hit_test(100,r.y+20,0)==menu[i]);assert(ui_menu_leaf_for_app(app_at(menu[i]))==0);
  assert(ui_menu_hit_test(100,r.y+r.height,0)==UI_MENU_HIT_NONE);
 }
 for(int i=0;i<5;++i){int id=ids[i];assert(app_nav_id(i)==id&&app_nav_position(id)==i);assert(main_page_ready(&ctx,app_at(id)));
  EpdRect r=ui_nav_tab_area(id);assert(r.x==i*684/5&&r.x+r.width==(i+1)*684/5);
  for(int x=r.x;x<r.x+r.width;++x)assert(ui_nav_hit(x,UI_NAV_TOUCH_TOP)==id);
  assert(main_key_action(&ctx,app_at(id),UI_KEY_1,false)==APP_REDRAW_NONE&&ctx.request_app==app_at(ids[(i+4)%5]));
  assert(main_key_action(&ctx,app_at(id),UI_KEY_3,false)==APP_REDRAW_NONE&&ctx.request_app==app_at(ids[(i+1)%5]));
  assert(main_key_action(&ctx,app_at(id),UI_KEY_2,true)==APP_REDRAW_FULL);
  for(scale=100;scale<=200;scale+=10)for(mode=0;mode<3;++mode){icons=texts=0;ui_nav_draw(frame,id);assert(icons==5&&texts==5);assert(frame[(UI_NAV_TOP+2)*684+r.x+r.width/2]==0);for(int y=UI_NAV_TOP+8;y<1216;++y)for(int x=0;x<684;++x)assert(frame[y*684+x]==0xe0);}
 }
 assert(app_at(2)==&app_files&&app_at(3)==&app_device_settings&&app_at(6)==&app_weread);
 for(int i=4;i<=5;++i){assert(!main_page_ready(&ctx,app_at(i)));ctx.request_app=NULL;ui_nav_request(&ctx,i);assert(!ctx.request_app);}
 root_visible=false;assert(!main_page_ready(&ctx,&app_book)&&!main_page_ready(&ctx,&app_weread));
 assert(ui_nav_hit(684,1200)==-1&&ui_nav_hit(0,UI_NAV_TOUCH_TOP-1)==-1&&app_nav_id(5)==-1&&app_nav_position(-1)==-1);
 puts("PASS: five tabs/stable legacy file-settings IDs, every pixel boundary, main-key cycle, root admission and all three gray modes");}
'''
with tempfile.TemporaryDirectory(prefix='five-tabs-') as d:
 p=Path(d);(p/'test.c').write_text(unit)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(root/'main/ui'),str(p/'test.c'),'-o',str(p/'probe')],check=True)
 subprocess.run([str(p/'probe')],check=True)
