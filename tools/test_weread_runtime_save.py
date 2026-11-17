#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Production runtime save failure/recovery and retained raw-coordinate identity."""
from pathlib import Path
import json,os,runpy,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
out=root/'build/night-optimization/runtime-save';out.mkdir(parents=True,exist_ok=True)
fixture=runpy.run_path(str(root/'tools/fixtures/weread/generate_fixtures.py'))
data={'info.txt':json.dumps({'bookId':'window_book','bookVersion':100,'chapters':[{'chapterUid':11,'files':['Text/main.xhtml']}]}).encode(),
      'Text/main.xhtml':b'<html><body><p>Chapter 11 fixture</p></body></html>'}
(out/'11-v100.zip').write_bytes(fixture['_make_zipcrypto_zip'](data,b'test_password_123'))
sources=['tools/test_weread_runtime_save.c','main/book/book_weread_crypto.c','main/book/book_source_weread.c','main/book/html_text.c',
         *[f'main/weread/weread_{n}.c' for n in ('runtime','chapter','position','progress','shelf','toc','store','auth','client')],'tools/weread_stubs/cJSON.c']
subprocess.run([os.environ.get('CC','/usr/bin/cc'),'-std=c11','-pthread','-D_DEFAULT_SOURCE','-Dstatvfs=window_statvfs',
    '-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
    '-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs',*sources,'-lz','-lm','-o',str(out/'test')],cwd=root,check=True)
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
for mode in ('recover','renew','owner','media','version','begin','rescue-leave','rescue-internal','rescue-eject'):
    with tempfile.TemporaryDirectory(prefix=f'weread-runtime-save-{mode}-') as cache,tempfile.TemporaryDirectory(prefix='wr-runtime-nvs-') as nvs:
        subprocess.run([str(out/'test'),cache,str(out),mode],cwd=root,env=dict(env,WEREAD_RESCUE_FIXTURE=nvs),check=True,timeout=15)
