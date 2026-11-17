#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise the production worker/window with real encrypted, non-contiguous chapters."""
from pathlib import Path
import json,os,runpy,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
out=root/'build/night-optimization/window';out.mkdir(parents=True,exist_ok=True)
fixture=runpy.run_path(str(root/'tools/fixtures/weread/generate_fixtures.py'))
for version in (100,101,0):
    for uid in (11,27,50,103,404,777,900,1201):
        data={'info.txt':json.dumps({'bookId':'window_book','bookVersion':version,'chapters':[{'chapterUid':uid,'files':['Text/main.xhtml']}]}).encode(),
              'Text/main.xhtml':f'<html><body><p>Chapter {uid} 正文 version{version}</p><img src="../Images/p.png"/></body></html>'.encode(), 'Images/p.png':b'\x89PNG\r\n\x1a\n'+f'version{version}'.encode()}
        (out/f'{uid}-v{version}.zip').write_bytes(fixture['_make_zipcrypto_zip'](data,b'test_password_123'))
sources=['tools/test_weread_window.c','main/book/book_weread_crypto.c','main/book/book_source_weread.c','main/book/html_text.c',
         *[f'main/weread/weread_{n}.c' for n in ('runtime','chapter','position','progress','shelf','toc','store','auth','client')],'tools/weread_stubs/cJSON.c']
subprocess.run([os.environ.get('CC','/usr/bin/cc'),'-std=c11','-pthread','-D_DEFAULT_SOURCE','-Dstatvfs=window_statvfs',
    '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
    '-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs',*sources,'-lz','-lm','-o',str(out/'test')],cwd=root,check=True)
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
for mode in ('binding','version-zero','window','space','fail','cancel','jump'):
    with tempfile.TemporaryDirectory(prefix=f'weread-window-{mode}-') as cache:
        subprocess.run([str(out/'test'),cache,str(out),mode],cwd=root,env=env,check=True,timeout=15)
