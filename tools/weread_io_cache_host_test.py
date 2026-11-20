#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise production extract and worker cover-routing boundaries under sanitizers."""
from pathlib import Path
import os
import re
import subprocess

root = Path(__file__).resolve().parents[1]
out = root / 'build/weread-tests/io-cache'
out.mkdir(parents=True, exist_ok=True)
flags = [os.environ.get('CC', 'cc'), '-pthread', '-std=c11', '-D_DEFAULT_SOURCE', '-Wall',
         '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-g',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-Imain',
         '-Imain/book', '-Imain/weread', '-Itools/weread_stubs', '-I'+str(out)]

def function(source, name):
    match = re.search(r'^.*\b'+re.escape(name)+r'\([^;]*?\)\s*\{', source, re.M)
    assert match, name
    pos, depth, quote = match.end(), 1, None
    while depth:
        char = source[pos]
        if quote:
            if char == '\\':
                pos += 2
                continue
            if char == quote:
                quote = None
        elif source.startswith('//', pos):
            pos = source.index('\n', pos)
            continue
        elif source.startswith('/*', pos):
            pos = source.index('*/', pos+2)+2
            continue
        elif char in '\"\'':
            quote = char
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
        pos += 1
    return source[match.start():pos]+'\n'

shelf = (root/'main/weread/weread_shelf.c').read_text()
runtime = (root/'main/weread/weread_runtime.c').read_text()
case = runtime[runtime.index('        case WR_JOB_COVER:'):runtime.index('        case WR_JOB_CLEAR:')]
(out/'cover-routing.inc').write_text(''.join(function(shelf, name) for name in (
    'weread_cover_get_url', 'weread_cover_retry_url', 'weread_cover_retry_result'))+
    'static esp_err_t dispatch(void){esp_err_t err=ESP_OK;switch(WR_JOB_COVER){\n'+
    case+'default:assert(false);\n}return err;}\n')
subprocess.run(flags+['tools/test_weread_cover_routing.c', '-o', str(out/'cover')], cwd=root, check=True)
subprocess.run([str(out/'cover')], cwd=root, check=True)

sources = ['tools/test_weread_extract_cache.c', 'main/book/book_weread_crypto.c',
           'main/book/book_source_weread.c', 'main/book/html_text.c']
sources += ['main/weread/weread_'+name+'.c' for name in ('position', 'shelf', 'toc', 'store', 'auth', 'client')]
subprocess.run(flags+sources+['tools/weread_stubs/cJSON.c', '-lz', '-lm', '-o', str(out/'extract')], cwd=root, check=True)
subprocess.run([str(out/'extract')], cwd=root, check=True)
