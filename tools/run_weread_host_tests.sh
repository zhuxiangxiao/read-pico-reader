#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 kiikoread
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
# WeRead host test runner.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

mkdir -p build/weread-tests

if [[ "$(uname -s)" == "Linux" ]]; then
    export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1"
else
    export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1"
fi
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

CC="${CC:-/usr/bin/cc}"

flags=(
    -pthread
    -std=c11
    -D_DEFAULT_SOURCE
    -Wall
    -Wextra
    -Werror
    -Wno-deprecated-declarations
    -g
    -fsanitize=address,undefined
    -fno-omit-frame-pointer
    -Imain
    -Imain/book
    -Imain/weread
    -Itools/weread_stubs
)

echo "Compiling WeRead crypto host tests..."
"${CC}" "${flags[@]}" \
    tools/test_weread_crypto.c \
    main/book/book_weread_crypto.c \
    -lz \
    -o build/weread-tests/test_weread_crypto
python3 tools/weread_platform_crypto_host_test.py

echo "Running WeRead crypto host tests under ASan/UBSan..."
build/weread-tests/test_weread_crypto

echo "Compiling WeRead auth & security client host tests..."
"${CC}" "${flags[@]}" \
    tools/test_weread_auth.c \
    main/weread/weread_auth.c main/weread/weread_store.c \
    main/weread/weread_client.c \
    tools/weread_stubs/cJSON.c \
    -lz -lm \
    -o build/weread-tests/test_weread_auth

echo "Running WeRead auth & security client host tests under ASan/UBSan..."
build/weread-tests/test_weread_auth

echo "Compiling and running production WeRead shelf/TOC tests..."
"${CC}" "${flags[@]}" \
    -DWEREAD_TEST_FAT_ROOT -Dmkdir=weread_test_mkdir tools/test_weread_shelf.c main/weread/weread_shelf.c main/weread/weread_toc.c \
    main/weread/weread_store.c main/weread/weread_auth.c main/weread/weread_client.c \
    tools/weread_stubs/cJSON.c -lm -o build/weread-tests/test_weread_shelf
build/weread-tests/test_weread_shelf

"${CC}" "${flags[@]}" -Dtime=resume_test_time tools/test_weread_shelf_resume.c \
    main/weread/weread_{shelf,store,auth,client}.c tools/weread_stubs/cJSON.c \
    -lm -o build/weread-tests/test_weread_shelf_resume
build/weread-tests/test_weread_shelf_resume

"${CC}" "${flags[@]}" -Dtime=toc_test_time tools/test_weread_toc_policy.c \
    main/weread/weread_{shelf,toc,store,auth,client}.c \
    tools/weread_stubs/cJSON.c -lm -o build/weread-tests/test_weread_toc_policy
python3 - <<'PY'
import subprocess,tempfile
with tempfile.TemporaryDirectory(prefix='weread-toc-policy-') as cache:
    subprocess.run(['build/weread-tests/test_weread_toc_policy',cache],check=True)
PY

python3 - <<'PY'
import zipfile,random,runpy,json
with zipfile.ZipFile('build/weread-tests/large.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('large.txt', b'x' * (2 * 1024 * 1024))
with zipfile.ZipFile('build/weread-tests/multichunk.zip','w',zipfile.ZIP_DEFLATED) as archive:
    archive.writestr('random.bin',random.Random(21).randbytes(300000))
fixture=runpy.run_path('tools/fixtures/weread/generate_fixtures.py')
with open('build/weread-tests/no-version.zip','wb') as output:
    output.write(fixture['_make_zipcrypto_zip']({
        'info.txt':json.dumps({'bookId':'test_book_101','synckey':100,'chapterUpdateTime':0,'chapters':[{'chapterUid':2,'files':['Text/live.xhtml']}]}).encode(),
        'Text/live.xhtml':'<html><body><p>真实清单兼容，无版本字段</p></body></html>'.encode(),
    },b'test_password_123'))
with open('build/weread-tests/notes.zip','wb') as output:
    output.write(fixture['_make_zipcrypto_zip']({
        'info.txt':json.dumps({'bookId':'test_book_101','chapters':[{'chapterUid':3,'files':['Text/one.xhtml','Text/two.xhtml','Text/three.xhtml']}]}).encode(),
        'Text/one.xhtml':('<html><head><!-- metadata --><style>.unused:before {content:"<";}</style><title>T</title></head><body>'+''.join(f'<p id="long_unreferenced_paragraph_id_{i:02d}">普通段落</p>' for i in range(16))+'<p id="noteref-a">甲😀<a href=\'#note\'>[1]</a>&amp;乙<a href="two.xhtml#note">[2]</a><a href="../Other/missing.xhtml#note">[9]</a><img src="../Images/picture.png"/></p><p id="note">第一注<a href="#noteref-a">返回</a></p></body></html>').encode(),
        'Text/two.xhtml':'<html><body><p id="noteref-b">丙<a href="#note">[3]</a></p><p id=note>第二注<a href="#noteref-b">返回</a></p></body></html>'.encode(),
        'Text/three.xhtml':'<html><body><p>丁<a href="#abcdefghijklmnopqrstuvw">[4]</a></p><p id="abcdefghijklmnopqrstuvw">第三注</p></body></html>'.encode(),
        'Styles/notes.css':b'#note {text-align:right} #abcdefghijklmnopqrstuvw {text-align:center}',
    },b'test_password_123'))
PY
echo "Compiling and running production WeRead streamed chapter tests..."
"${CC}" "${flags[@]}" \
    tools/test_weread_chapter.c main/book/book_weread_crypto.c main/book/book_source_weread.c main/book/html_text.c \
    main/weread/weread_chapter.c main/weread/weread_position.c main/weread/weread_shelf.c main/weread/weread_toc.c \
    main/weread/weread_store.c main/weread/weread_auth.c main/weread/weread_client.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_chapter
build/weread-tests/test_weread_chapter

echo "Compiling and running new-open version binding tests..."
"${CC}" "${flags[@]}" tools/test_weread_open_version.c \
    main/book/book_weread_crypto.c main/book/book_source_weread.c main/book/html_text.c \
    main/weread/weread_{runtime,chapter,position,progress,shelf,toc,store,auth,client}.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_open_version
python3 - <<'PY'
import subprocess,tempfile
for mode in ('updated','selfheal','moved','corrupt','bad-offset','bad-index','protection'):
    with tempfile.TemporaryDirectory(prefix='weread-open-version-') as cache:
        subprocess.run(['build/weread-tests/test_weread_open_version',cache,mode],check=True)
PY

"${CC}" "${flags[@]}" tools/test_weread_open_cache.c \
    main/book/book_weread_crypto.c main/book/book_source_weread.c main/book/html_text.c \
    main/weread/weread_{runtime,chapter,position,progress,shelf,toc,store,auth,client}.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_open_cache
python3 - <<'PY'
import subprocess,tempfile
for mode in ('warm','missing-uid','fresh-version','split-progress','split-toc','known-no-fresh','selfheal-split','append-same-version','reordered-same-version','stale-progress-version','hint-empty-unread'):
    with tempfile.TemporaryDirectory(prefix='weread-open-cache-') as cache:
        subprocess.run(['build/weread-tests/test_weread_open_cache',cache,mode],check=True)
PY

echo "Compiling and running production WeRead progress tests..."
"${CC}" "${flags[@]}" \
    tools/test_weread_progress.c main/weread/weread_progress.c main/book/book_weread_crypto.c \
    main/weread/weread_shelf.c main/weread/weread_store.c \
    main/weread/weread_auth.c main/weread/weread_client.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_progress
build/weread-tests/test_weread_progress

python3 - <<'PY'
from pathlib import Path
s=Path('main/weread/weread_store.c').read_text()
Path('build/weread-tests/store-transaction.c').write_text(s.replace('esp_err_t wr_write_json(', 'esp_err_t actual_write_json(',1))
PY
"${CC}" "${flags[@]}" tools/test_weread_progress_transaction.c \
    main/weread/weread_progress.c main/book/book_weread_crypto.c main/weread/weread_shelf.c \
    build/weread-tests/store-transaction.c main/weread/weread_auth.c main/weread/weread_client.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_progress_transaction
python3 - <<'PY'
import subprocess,tempfile
for mode in range(1,6):
    with tempfile.TemporaryDirectory(prefix='weread-progress-transaction-') as cache:
        subprocess.run(['build/weread-tests/test_weread_progress_transaction',cache,str(mode)],check=True)
PY

echo "WeRead host tests passed cleanly!"
echo "Compiling and running constrained WeRead memory-pool tests..."
"${CC}" "${flags[@]}" -DWEREAD_TEST_MEMORY \
    -Dmalloc=weread_test_malloc -Dcalloc=weread_test_calloc -Drealloc=weread_test_realloc -Dfree=weread_test_free \
    tools/test_weread_memory.c tools/weread_stubs/heap_caps_memory_mock.c main/weread/weread_store.c main/weread/weread_auth.c \
    main/weread/weread_client.c main/weread/weread_shelf.c main/weread/weread_toc.c \
    tools/weread_stubs/cJSON.c -lm -o build/weread-tests/test_weread_memory
build/weread-tests/test_weread_memory

echo "Compiling and running bounded cache lifecycle tests..."
"${CC}" "${flags[@]}" -DWEREAD_TEST_MEMORY -Dtime=cache_test_time \
    -Dmalloc=weread_test_malloc -Dcalloc=weread_test_calloc -Drealloc=weread_test_realloc -Dfree=weread_test_free \
    tools/test_weread_cache.c tools/weread_stubs/heap_caps_memory_mock.c \
    main/book/book_weread_crypto.c main/book/book_source_weread.c main/book/html_text.c \
    main/weread/weread_{chapter,position,progress,shelf,toc,store,auth,client}.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_cache
build/weread-tests/test_weread_cache
python3 tools/weread_cache_ui_host_test.py
python3 tools/weread_reader_input_host_test.py
python3 tools/weread_chapter_transition_host_test.py
python3 tools/weread_client_snapshot_host_test.py
python3 tools/weread_client_cancel_cleanup_host_test.py
python3 tools/test_weread_outbox.py
python3 tools/weread_offline_host_test.py
python3 tools/test_weread_window.py
python3 tools/test_weread_runtime_save.py
python3 tools/weread_shelf_idle_host_test.py
python3 tools/weread_cover_cache_host_test.py
python3 tools/weread_cache_fault_host_test.py
python3 tools/weread_shelf_noop_host_test.py
python3 tools/weread_shelf_ids_host_test.py
python3 tools/weread_io_cache_host_test.py

python3 tools/weread_time_cancel_host_test.py
