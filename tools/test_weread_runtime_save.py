#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Production runtime save failure/recovery and retained raw-coordinate identity."""
from pathlib import Path
import json,os,runpy,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
out=Path(os.environ.get('WEREAD_TEST_BUILD_DIR',root/'build/night-optimization')).resolve()/'runtime-save';out.mkdir(parents=True,exist_ok=True)
fixture=runpy.run_path(str(root/'tools/fixtures/weread/generate_fixtures.py'))
data={'info.txt':json.dumps({'bookId':'window_book','bookVersion':100,'chapters':[{'chapterUid':11,'files':['Text/main.xhtml']}]}).encode(),
      'Text/main.xhtml':b'<html><body><p>Chapter 11 fixture</p></body></html>'}
(out/'11-v100.zip').write_bytes(fixture['_make_zipcrypto_zip'](data,b'test_password_123'))
for uid in (27,50,103,404,777,900,1201):
    data={'info.txt':json.dumps({'bookId':'window_book','bookVersion':100,'chapters':[{'chapterUid':uid,'files':['Text/main.xhtml']}]}).encode(),
          'Text/main.xhtml':f'<html><body><p>Chapter {uid} fixture</p></body></html>'.encode()}
    (out/f'{uid}-v100.zip').write_bytes(fixture['_make_zipcrypto_zip'](data,b'test_password_123'))
large=out/'long';large.mkdir(exist_ok=True)
data={'info.txt':json.dumps({'bookId':'window_book','bookVersion':100,'chapters':[{'chapterUid':11,'files':['Text/main.xhtml']}]}).encode(),
      'Text/main.xhtml':b'<html><body><p>'+b'x'*10000+b'</p></body></html>'}
(large/'11-v100.zip').write_bytes(fixture['_make_zipcrypto_zip'](data,b'test_password_123'))
sources=['tools/test_weread_runtime_save.c','main/book/book_weread_crypto.c','main/book/book_source_weread.c','main/book/html_text.c',
         *[f'main/weread/weread_{n}.c' for n in ('runtime','chapter','position','progress','shelf','toc','store','auth','client')],'tools/weread_stubs/cJSON.c']
subprocess.run([os.environ.get('CC','/usr/bin/cc'),'-std=c11','-pthread','-D_DEFAULT_SOURCE','-Dstatvfs=window_statvfs','-DWEREAD_RUNTIME_TEST_FILE_IO','-Dfopen=runtime_test_fopen',
    '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
    '-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs',*sources,'-lz','-lm','-o',str(out/'test')],cwd=root,check=True)
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
for mode in ('recover','renew','owner','media','version','begin','rescue-leave','rescue-internal','rescue-eject','rescue-logout','rescue-wrong-card','rescue-eject-large','rescue-eject-large-newer','throttle-time','throttle-pages','throttle-conflict','throttle-chapter','throttle-closed'):
    with tempfile.TemporaryDirectory(prefix=f'weread-runtime-save-{mode}-') as cache,tempfile.TemporaryDirectory(prefix='wr-runtime-nvs-') as nvs:
        subprocess.run([str(out/'test'),cache,str(large if mode=='rescue-wrong-card' or mode.startswith('rescue-eject-large') else out),mode],cwd=root,env=dict(env,WEREAD_RESCUE_FIXTURE=nvs),check=True,timeout=15)
