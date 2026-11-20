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
state=source[source.index('#define ROWS'):source.index('bool app_weread_request_open')]
funcs=['message','release_covers','stop_job','compare_match','filter_page','filter','row_book','covers_checked','covers_waiting','page_signature','adopt_books','row_rect','more_rect','more_draw','wifi_close','wifi_open','start','open_id','enter','leave','media_lost','before_lock','media_ready','render','keyboard_update','gesture','tick','key','main_page','main_key','navigation']
(out/'shared.inc').write_text(''.join(function(Path('main/ui/ui_shelf.c'),n) for n in ['ui_shelf_rows','ui_shelf_entry','ui_shelf_cover','ui_shelf_order_mode','ui_shelf_order_desc','ui_shelf_order_value','ui_shelf_order_rect','ui_shelf_order_select','shelf_pager_group_rect','ui_shelf_search_rect']))
(out/'production.inc').write_text(state+'\n'+''.join(function(Path('main/apps/app_weread.c'),n) for n in funcs))
(out/'cover.inc').write_text(''.join(function(Path('main/weread/weread_shelf.c'),n) for n in ['weread_cover_key','weread_cover_get_url','weread_cover_retry_url','weread_cover_retry_result']))
args=[os.environ.get('CC','cc'),'-pthread','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs','-I'+str(out),str(root/'tools/test_weread_cache_ui.c'),'-o',str(out/'probe')]
subprocess.run(args,check=True,cwd=root)
subprocess.run([str(out/'probe')],check=True,cwd=root)
