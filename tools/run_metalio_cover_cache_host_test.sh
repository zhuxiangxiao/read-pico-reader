#!/bin/sh
# 中文：S31 与 Pico 两种板级预算各编译跑一遍共享封面缓存测试。
# English: Run the shared cover-cache suite once per board budget (S31 then Pico).
set -eu
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -DKIIKO_BOARD_METALIO47=1 -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread \
  -Itools/metalio_cache_host_stubs -Imain/book \
  tools/metalio_cover_cache_host_test.c main/book/metalio_cover_cache.c -o "$out/test_s31"
"$out/test_s31"
cc -DKIIKO_BOARD_METALIO47=0 -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -pthread \
  -Itools/metalio_cache_host_stubs -Imain/book \
  tools/metalio_cover_cache_host_test.c main/book/metalio_cover_cache.c -o "$out/test_pico"
"$out/test_pico"
