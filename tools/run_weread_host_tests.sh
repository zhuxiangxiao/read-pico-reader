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
PY
echo "Compiling and running production WeRead streamed chapter tests..."
"${CC}" "${flags[@]}" \
    tools/test_weread_chapter.c main/book/book_weread_crypto.c main/book/book_source_weread.c main/book/html_text.c \
    main/weread/weread_chapter.c main/weread/weread_position.c main/weread/weread_shelf.c main/weread/weread_toc.c \
    main/weread/weread_store.c main/weread/weread_auth.c main/weread/weread_client.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_chapter
build/weread-tests/test_weread_chapter

echo "Compiling and running production WeRead progress tests..."
"${CC}" "${flags[@]}" \
    tools/test_weread_progress.c main/weread/weread_progress.c main/book/book_weread_crypto.c \
    main/weread/weread_shelf.c main/weread/weread_store.c \
    main/weread/weread_auth.c main/weread/weread_client.c \
    tools/weread_stubs/cJSON.c -lz -lm -o build/weread-tests/test_weread_progress
build/weread-tests/test_weread_progress

echo "WeRead host tests passed cleanly!"
echo "Compiling and running constrained WeRead memory-pool tests..."
"${CC}" "${flags[@]}" -DWEREAD_TEST_MEMORY \
    -Dmalloc=weread_test_malloc -Dcalloc=weread_test_calloc -Drealloc=weread_test_realloc -Dfree=weread_test_free \
    tools/test_weread_memory.c tools/weread_stubs/heap_caps_memory_mock.c main/weread/weread_store.c main/weread/weread_auth.c \
    main/weread/weread_client.c main/weread/weread_shelf.c main/weread/weread_toc.c \
    tools/weread_stubs/cJSON.c -lm -o build/weread-tests/test_weread_memory
build/weread-tests/test_weread_memory
