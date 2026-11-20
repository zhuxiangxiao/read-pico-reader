#!/usr/bin/env bash
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
# Host test runner for fork OTA parsing, download isolation, packaging, staging, and board checks.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT_DIR}"

if [[ "$(uname -s)" == "Linux" ]]; then
    export ASAN_OPTIONS="detect_leaks=1:abort_on_error=1:halt_on_error=1"
else
    export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1"
fi
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

CC="${CC:-cc}"
FORK_ORIGIN='"https://example-fork.github.io/read-pico-reader/"'

for board in pico metalio47; do
    view="build/host/${board}"
    python3 tools/board_source_view.py --board "${board}" --tests --output "${view}"
    mkdir -p "${view}/build/ota-tests"

    board_flag="-DKIIKO_BOARD_METALIO47=0"
    if [[ "${board}" == "metalio47" ]]; then
        board_flag="-DKIIKO_BOARD_METALIO47=1"
    fi

    common_flags=(
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
        "${board_flag}"
        "-I${view}/tools/ota_stubs"
        "-I${view}/tools/weread_stubs"
        "-I${view}/main"
    )

    for mode in default fork; do
        extra_defs=()
        if [[ "${mode}" == "fork" ]]; then
            extra_defs+=("-DKIIKO_OTA_ORIGIN=${FORK_ORIGIN}")
        fi

        echo "Running ota_release_host_test (${board}, ${mode})..."
        "${CC}" "${common_flags[@]}" ${extra_defs[@]+"${extra_defs[@]}"} \
            "${view}/tools/ota_release_host_test.c" \
            "${view}/main/ota_release.c" \
            "${view}/tools/weread_stubs/cJSON.c" \
            -o "${view}/build/ota-tests/ota_release_${mode}"
        "${view}/build/ota-tests/ota_release_${mode}"

        echo "Running ota_online_host_test (${board}, ${mode})..."
        "${CC}" "${common_flags[@]}" ${extra_defs[@]+"${extra_defs[@]}"} \
            "${view}/tools/ota_online_host_test.c" \
            "${view}/main/ota_release.c" \
            "${view}/tools/weread_stubs/cJSON.c" \
            -o "${view}/build/ota-tests/ota_online_${mode}"
        "${view}/build/ota-tests/ota_online_${mode}"
    done
done

echo "Running Fork OTA tooling host tests..."
python3 tools/fork_ota_host_test.py

echo "Running board layer and selection consistency tests..."
python3 tools/board_layer_host_test.py
python3 tools/board_selection_host_test.py

echo "All fork OTA host tests passed cleanly!"
