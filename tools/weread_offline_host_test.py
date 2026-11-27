#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Production WeRead offline/content cases with encrypted archives and disk faults."""
from pathlib import Path
import json, os, runpy, subprocess, tempfile

root=Path(__file__).resolve().parents[1]
out=root/'build/night-optimization/offline';out.mkdir(parents=True,exist_ok=True)
fixture=runpy.run_path(str(root/'tools/fixtures/weread/generate_fixtures.py'))
contents={
    10:'<html><head><title>Empty</title></head><body> \n\t</body></html>',
    20:'<html><body style="background-image: url(\'../Images/bg.png\')"> </body></html>',
    30:'<html><body><p>甲<img src="../Images/a.png"/><img src="../Images/b.png"/>乙</p></body></html>',
    40:'<html><body><p>普通正文😀与脚注定位</p></body></html>',
    50:'',
    60:"<html><BODY STYLE='BACKGROUND: URL(\"../Images/bg.png\") center no-repeat'> </BODY></html>",
    70:'<html><body style="background-image:url(../Images/bg.png)"><p>装饰背景保留文字</p></body></html>',
    80:'<!-- <body style="background:url(../Images/bg.png)"> --><html><body> </body></html>',
    90:'<html><head><script>const x=\'<body style="background:url(../Images/bg.png)">\';</script></head><body> </body></html>',
    100:'<html><body style="background-image:url(https://example.com/p.png)"> </body></html>',
    110:'<html><body style="background-image:url(../../escape.png)"> </body></html>',
    120:'<html><body><img src="../Images/a.png"/></body></html>',
    130:'<html><body style="background:url(../Images/bg.png)"><p>'+('A'*1024*1024)+'</p></body></html>',
    140:'<html><body style="background:url(../Images/bg.png)"><svg/></body></html>',
    150:'<html><body style="background:url(../Images/bg.png)"><div><p> </p><br/></div></body></html>',
    160:'<html><body><p>'+('甲'*6000)+'</p></body></html>',
    170:'<html><body><p>'+('a甲'*6000)+'</p></body></html>',



}
for uid,html in contents.items():
    files={'info.txt':json.dumps({'bookId':'offline_book','bookVersion':100,'chapters':[{'chapterUid':uid,'files':['Text/main.xhtml']}]}).encode(),
           'Text/main.xhtml':html.encode(),**{f'Images/{name}.png':b'\x89PNG\r\n\x1a\n' for name in ('bg','a','b')}}
    (out/f'{uid}.zip').write_bytes(fixture['_make_zipcrypto_zip'](files,b'test_password_123'))
store=(root/'main/weread/weread_store.c').read_text().replace('esp_err_t wr_write_json(', 'esp_err_t actual_write_json(',1).replace('void *wr_alloc(', 'void *actual_wr_alloc(',1)
(out/'store.c').write_text(store)
html=(root/'main/book/html_text.c').read_text()
html=html.replace('esp_err_t html_to_blocks(', 'esp_err_t actual_html_to_blocks(',1).replace('esp_err_t html_to_blocks_mapped(', 'esp_err_t actual_html_to_blocks_mapped(',1)
html=html.replace('heap_caps_malloc(', 'offline_html_malloc(').replace('heap_caps_realloc(', 'offline_html_realloc(')
html='#include <stddef.h>\n#include <stdint.h>\nvoid *offline_html_malloc(size_t,unsigned);\nvoid *offline_html_realloc(void *,size_t,unsigned);\n'+html
(out/'html.c').write_text(html)
sources=['tools/test_weread_offline.c','main/book/book_weread_crypto.c','main/book/book_source_weread.c',str(out/'html.c'),
         *[f'main/weread/weread_{n}.c' for n in ('chapter','position','shelf','toc','auth','client')],str(out/'store.c'),'tools/weread_stubs/cJSON.c']
subprocess.run([os.environ.get('CC','/usr/bin/cc'),'-std=c11','-pthread','-D_DEFAULT_SOURCE','-Dstatvfs=weread_test_statvfs',
    '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
    '-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs',*sources,'-lz','-lm','-o',str(out/'test')],cwd=root,check=True)
with tempfile.TemporaryDirectory(prefix='weread-offline-content-') as cache:
    subprocess.run([str(out/'test'),cache,str(out)],cwd=root,check=True,
                   env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'))
