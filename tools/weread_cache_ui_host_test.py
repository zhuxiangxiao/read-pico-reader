#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
from pathlib import Path
import os,re,subprocess
root=Path(__file__).resolve().parents[1]; out=root/'build/weread-tests/cache-ui'
out.mkdir(parents=True,exist_ok=True)
def function(path,name):
    source=(root/path).read_text()
    m=re.search(r'^.*\b'+re.escape(name)+r'\([^;]*?\)\s*\{',source,re.M)
    assert m,name
    start=m.start(); pos=m.end(); depth=1; quote=None
    while depth:
        c=source[pos]
        if quote:
            if c=='\\':pos+=2;continue
            if c==quote:quote=None
        elif source.startswith('//',pos):pos=source.index('\n',pos);continue
        elif source.startswith('/*',pos):pos=source.index('*/',pos+2)+2;continue
        elif c in '\"\'':quote=c
        elif c=='{':depth+=1
        elif c=='}':depth-=1
        pos+=1
    return source[start:pos]+'\n'
source=(root/'main/apps/app_weread.c').read_text()
production_slice=source[source.index('#define ROWS'):source.index('const app_desc_t app_weread')]
production_slice=re.sub(r'^\s*#include\s+"book_toc\.h"\s*$','',production_slice,flags=re.M)
ui_icons=(root/'boards/metalio47/src/main/assets/ui_icons.h').read_text()
m_icon=re.search(r'typedef enum\s*\{.*?\}\s*ui_icon_t;',ui_icons,re.S)
assert m_icon,'ui_icon_t'
toc_jump=''.join(function(Path('main/apps/book_toc.c'),n) for n in ['book_toc_jump_hit','book_toc_jump_percent','book_toc_page_from_percent'])
shared_stubs=f'''
#ifndef EPD_DRAW_ALIGN_RIGHT
#define EPD_DRAW_ALIGN_RIGHT 2
#endif
#ifndef UI_GRAY_BLACK
#define UI_GRAY_BLACK 0x00
#endif
#ifndef UI_GRAY_LIGHT
#define UI_GRAY_LIGHT 0x90
#endif
#ifndef UI_PX_BODY
#define UI_PX_BODY 38
#endif
#ifndef UI_SYSTEM_KEYBOARD_TOP
#define UI_SYSTEM_KEYBOARD_TOP 560
#endif
#ifndef UI_KEY_LEFT
#define UI_KEY_LEFT UI_KEY_1
#endif
#ifndef UI_KEY_CENTER
#define UI_KEY_CENTER UI_KEY_2
#endif
#ifndef UI_KEY_RIGHT
#define UI_KEY_RIGHT UI_KEY_3
#endif
#ifndef UI_KEY_BACK
#define UI_KEY_BACK 3
#endif
#ifndef UI_KEY_UP
#define UI_KEY_UP 4
#endif
#ifndef UI_KEY_DOWN
#define UI_KEY_DOWN 5
#endif
#ifndef BOOK_TOC_ROWS
#define BOOK_TOC_ROWS 7
#define BOOK_TOC_BACK (-2)
#define BOOK_TOC_PREV (-3)
#define BOOK_TOC_NEXT (-4)
#define BOOK_TOC_SKIP_PREV (-5)
#define BOOK_TOC_SKIP_NEXT (-6)
#define BOOK_TOC_JUMP (-7)
#define BOOK_TOC_JUMP_CANCEL (-8)
#define BOOK_TOC_JUMP_CONFIRM (-9)
#define BOOK_TOC_JUMP_TRACK (-10)
#define BOOK_TOC_SELECT_VOLUME (-11)
#endif
{m_icon.group(0)}
static bool book_path_is_weread(const char *p){{return p&&strncmp(p,"weread://",9)==0;}}
static int ui_text_effective_px(int px){{return px;}}
static void ui_hairline(uint8_t *f,int y,int x,int w,uint8_t g){{(void)f;(void)y;(void)x;(void)w;(void)g;}}
static void ui_keyboard_hold_start(int x,int y,int t,int64_t n){{(void)x;(void)y;(void)t;(void)n;}}
static void ui_keyboard_hold_cancel(void){{}}
static ui_keyboard_result_t ui_keyboard_hold_tick(bool h,int x,int y,int64_t n){{(void)h;(void)x;(void)y;(void)n;return UI_KEYBOARD_NONE;}}
static ui_keyboard_result_t ui_keyboard_idle_tick(bool t,int64_t n){{(void)t;(void)n;return UI_KEYBOARD_NONE;}}
static bool ui_keyboard_page(int d){{(void)d;++mock_keyboard_page_calls;return true;}}
static bool ui_keyboard_pending(void){{return mock_keyboard_pending_val;}}
static void epd_fill_circle(int x,int y,int r,uint8_t g,uint8_t *f){{(void)f;if(mock_circle_count<64)mock_circles[mock_circle_count++]=(mock_circle_call_t){{x,y,r,g}};}}
static void epd_draw_circle(int x,int y,int r,uint8_t g,uint8_t *f){{(void)x;(void)y;(void)r;(void)g;(void)f;}}
static void ui_draw_round_rect(uint8_t *f,EpdRect r,int rad,uint8_t g){{(void)f;(void)r;(void)rad;(void)g;}}
static void ui_text_fixed_vc(uint8_t *f,int x,int y,int px,const char *s,int a,bool inv){{(void)f;(void)x;(void)y;(void)px;(void)a;(void)inv;remember_text(s);}}
static void ui_text_fixed(uint8_t *f,int x,int y,int px,const char *s,int a,bool inv){{(void)f;(void)x;(void)y;(void)px;(void)a;(void)inv;remember_text(s);}}
static void ui_text_fixed_ink_vc(uint8_t *f,int x,int y,int px,const char *s,int a,uint8_t g){{(void)f;(void)x;(void)y;(void)px;(void)a;(void)g;remember_text(s);}}
static void ui_draw_icon(uint8_t *f,int cx,int cy,int sz,ui_icon_t icon,uint8_t g){{(void)f;(void)cx;(void)cy;(void)sz;(void)icon;(void)g;}}
{toc_jump}
bool weread_auth_is_expired(void){{return mock_auth_expired;}}
bool weread_runtime_probe_shelf(const char *const *ids,size_t n){{
    ++mock_probe_calls;
    if(!mock_probe_enabled||!ids||!n)return false;
    char buf[600]="";size_t used=0;
    for(size_t i=0;i<n;++i){{
        if(!ids[i])continue;
        size_t len=strlen(ids[i]);
        if(used+len+2>=sizeof(buf))break;
        if(i)buf[used++]=',';
        memcpy(buf+used,ids[i],len);used+=len;buf[used]=0;
    }}
    return weread_runtime_start(WR_JOB_PROBE_SHELF,NULL,buf);
}}
bool weread_runtime_shelf_cache_status(weread_shelf_cache_status_t *out){{if(out)*out=mock_shelf_cache_status;return mock_shelf_cache_status.active;}}
bool weread_runtime_cache_stats(weread_runtime_cache_stats_t *out){{if(out)*out=mock_cache_stats;return mock_cache_stats.valid;}}
'''
(out/'shared.inc').write_text(shared_stubs+''.join(function(Path('main/ui/ui_shelf.c'),n) for n in ['ui_shelf_rows','ui_shelf_entry','ui_shelf_cover','ui_shelf_order_mode','ui_shelf_order_desc','ui_shelf_order_value','ui_shelf_order_rect','ui_shelf_order_select','shelf_pager_group_rect','ui_shelf_search_rect','ui_shelf_page_label_rect']))
(out/'production.inc').write_text(production_slice+'\n')
(out/'cover.inc').write_text(''.join(function(Path('main/weread/weread_shelf.c'),n) for n in ['weread_cover_key','weread_cover_get_url','weread_cover_retry_url','weread_cover_retry_result']))
args=[os.environ.get('CC','cc'),'-pthread','-std=c11','-D_DEFAULT_SOURCE','-DWEREAD_CACHE_UI_HOST_STUBS','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs','-I'+str(out),str(root/'tools/test_weread_cache_ui.c'),'-o',str(out/'probe')]
subprocess.run(args,check=True,cwd=root)
subprocess.run([str(out/'probe')],check=True,cwd=root)
