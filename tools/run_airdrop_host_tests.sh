#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 中文：隔空投送组件主机回归。包含仓库内路由/流式 CPIO 测试，以及从
# s-iwaki-d/ESP32Drop（ISC，Copyright (c) 2026 MIRO）导入并适配 include 路径的
# AWDL/AirDrop 解析器与状态机测试。
# English: AirDrop component host regressions: the in-tree routing/streaming CPIO test
# plus the AWDL/AirDrop parser and state-machine suites imported from
# s-iwaki-d/ESP32Drop (ISC, Copyright (c) 2026 MIRO) with include paths adapted.
#
# 只在仓库根运行；产物在 build/airdrop-tests（被忽略）。
# Run from the repository root; artifacts stay in the ignored build/airdrop-tests.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
OUT=${AIRDROP_TEST_OUT:-"$ROOT/build/airdrop-tests"}
CC=${CC:-cc}
SAN_FLAGS=(-fsanitize=address,undefined -fno-omit-frame-pointer)
BASE_FLAGS=(-std=c11 -O2 -Wall -Wextra -Wno-unused-function)
INC=(-I"$ROOT/tools/airdrop/tests"
     -I"$ROOT/components/read_pico_airdrop/awdl/core"
     -I"$ROOT/components/read_pico_airdrop/airdrop/core"
     -I"$ROOT/components/read_pico_airdrop")

mkdir -p "$OUT"
failed=0

build() {
    local name=$1 src=$2
    shift 2
    if ! "$CC" "${BASE_FLAGS[@]}" "$@" "${INC[@]}" "$src" -lm -o "$OUT/$name" \
        >"$OUT/$name.build.log" 2>&1; then
        echo "FAIL $name (compile)"
        tail -20 "$OUT/$name.build.log"
        failed=1
        return 1
    fi
}

run() {
    local name=$1
    if ! "$OUT/$name" >>"$OUT/$name.log" 2>&1; then
        echo "FAIL $name (run)"
        tail -20 "$OUT/$name.log"
        failed=1
    else
        echo "PASS $name"
    fi
}

# 仓库内测试 / In-tree test.
if build airdrop_route "$ROOT/tools/airdrop_file_route_host_test.c" "${SAN_FLAGS[@]}"; then
    run airdrop_route
fi

# 导入测试：上游标注使用 sanitizer 的保持 sanitizer。/ Imported tests keep their upstream sanitizers.
san_tests=(bplist cpio dvzip pack uti zlib http_fix)
plain_tests=(census dnssd elect frame gauge http peer peertab ring serve src status tick
             transfer_trace window awdl_gauge_fix awdl_status_ceiling)

for t in "${san_tests[@]}"; do
    if build "$t" "$ROOT/tools/airdrop/tests/${t}_test.c" "${SAN_FLAGS[@]}"; then
        run "$t"
    fi
done

for t in "${plain_tests[@]}"; do
    if build "$t" "$ROOT/tools/airdrop/tests/${t}_test.c"; then
        if [ "$t" = gauge ]; then
            # gauge 回放录制 trace；输出较长，只保留日志。
            # gauge replays a recorded trace; its frame-by-frame output stays in the log.
            if ! "$OUT/gauge" "$ROOT/tools/airdrop/testdata/trace3.txt" \
                >>"$OUT/gauge.log" 2>&1; then
                echo "FAIL gauge (run)"
                tail -20 "$OUT/gauge.log"
                failed=1
            else
                echo "PASS gauge"
            fi
        else
            run "$t"
        fi
    fi
done

# 生命周期测试：真实组件源码 + 平台桩（C++）。/ Lifecycle tests: real component code + platform stubs.
CXX=${CXX:-g++}
if [ "$(uname -s)" = Darwin ]; then
    GC_FLAG=(-Wl,-dead_strip)
else
    GC_FLAG=(-Wl,--gc-sections)
fi
CXX_FLAGS=(-std=c++17 -O1 -g "${SAN_FLAGS[@]}" -Wno-deprecated-declarations
           -Wno-unused-function -Wno-comment -Wno-missing-field-initializers
           -Wno-unused-variable)

build_cpp() {
    local name=$1 src=$2
    shift 2
    if ! "$CXX" "${CXX_FLAGS[@]}" "$@" "$src" -o "$OUT/$name" "${GC_FLAG[@]}" \
        >"$OUT/$name.build.log" 2>&1; then
        echo "FAIL $name (compile)"
        tail -20 "$OUT/$name.build.log"
        failed=1
        return 1
    fi
}

LIFE="$ROOT/tools/airdrop/lifecycle"
if build_cpp ring_recycle_real_host_test "$LIFE/ring_recycle_real_host_test.cpp" \
    -I"$LIFE/stubs" -I"$ROOT/components/read_pico_airdrop/airdrop/port"; then
    run ring_recycle_real_host_test
fi
if build_cpp airdrop_lifecycle_host_test "$LIFE/airdrop_lifecycle_host_test.cpp" \
    -I"$LIFE/stubs" -I"$ROOT/components/read_pico_airdrop" \
    -I"$ROOT/components/read_pico_airdrop/include" \
    -I"$ROOT/components/ble_page_turner/include"; then
    run airdrop_lifecycle_host_test
fi

if [ "$failed" -ne 0 ]; then
    echo "airdrop host tests: FAILED"
    exit 1
fi
echo "airdrop host tests: PASS"
