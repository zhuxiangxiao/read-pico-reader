#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Verify worker/foreground decoupling, non-blocking reader exit and whole-book idle prefetch."""
from pathlib import Path
import json
import os
import runpy
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools'))
from weread_mock_server import encrypt_encrypt_key

out = root / 'build/weread-tests/decoupling'
out.mkdir(parents=True, exist_ok=True)
fixture = runpy.run_path(str(root / 'tools/fixtures/weread/generate_fixtures.py'))

for idx in range(8):
    uid = 10 + idx
    data = {
        'info.txt': json.dumps({
            'bookId': 'decouple_book',
            'bookVersion': 100,
            'chapters': [{'chapterUid': uid, 'files': ['Text/main.xhtml']}],
        }).encode(),
        'Text/main.xhtml': f'<html><body><p>Chapter {idx} content</p><img src="../Images/p.png"/></body></html>'.encode(),
        'Images/p.png': b'\x89PNG\r\n\x1a\n' + f'img-{idx}'.encode(),
    }
    (out / f'{uid}.zip').write_bytes(fixture['_make_zipcrypto_zip'](data, b'test_password_123'))

sources = [
    'tools/test_weread_decoupling.c',
    'main/book/book_weread_crypto.c',
    'main/book/book_source_weread.c',
    'main/book/html_text.c',
    *[f'main/weread/weread_{n}.c' for n in ('runtime', 'chapter', 'position', 'progress', 'shelf', 'toc', 'store', 'auth', 'client')],
    'tools/weread_stubs/cJSON.c',
]
subprocess.run([
    os.environ.get('CC', '/usr/bin/cc'),
    '-std=c11', '-pthread', '-D_DEFAULT_SOURCE', '-Dstatvfs=decoupling_statvfs',
    '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-g',
    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
    '-Imain', '-Imain/book', '-Imain/weread', '-Itools/weread_stubs',
    *sources, '-lz', '-lm', '-o', str(out / 'test')
], cwd=root, check=True)

encrypt_key = encrypt_encrypt_key('test_password_123', '123')
env = dict(os.environ, ASAN_OPTIONS='abort_on_error=1:halt_on_error=1', UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
for mode in ('concurrent-io', 'nonblocking-exit', 'whole-book-prefetch', 'storage-cap'):
    with tempfile.TemporaryDirectory(prefix=f'weread-decoupling-{mode}-') as cache:
        subprocess.run([str(out / 'test'), cache, str(out), encrypt_key, mode], cwd=root, env=env, check=True, timeout=15)
