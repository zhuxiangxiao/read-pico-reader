#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 kiikoread
# SPDX-License-Identifier: Apache-2.0
#
# Master test runner for Kiikoread WeChat Read (微信读书) E2E Test Suite.
# Manages loopback mock HTTP server lifecycle and executes test suites.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

PORT="${WEREAD_MOCK_PORT:-8989}"
TIER="${TIER:-all}"
VERBOSE="${VERBOSE:-0}"
SMOKE_ONLY="${SMOKE_ONLY:-0}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            PORT="$2"
            shift 2
            ;;
        --tier)
            TIER="$2"
            shift 2
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --smoke-only)
            SMOKE_ONLY=1
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            echo "Usage: $0 [--tier 1|2|3|4|all] [--smoke-only] [--verbose] [--port <int>]" >&2
            exit 1
            ;;
    esac
done

export WEREAD_MOCK_PORT="$PORT"
if [ "$VERBOSE" -eq 1 ]; then
    export WEREAD_MOCK_VERBOSE=1
fi

MOCK_PID=""

cleanup() {
    local exit_code=$?
    if [ -n "$MOCK_PID" ] && kill -0 "$MOCK_PID" 2>/dev/null; then
        echo "[runner] Stopping WeRead mock server (PID: ${MOCK_PID})..."
        kill "$MOCK_PID" 2>/dev/null || true
        wait "$MOCK_PID" 2>/dev/null || true
    fi
    exit $exit_code
}
trap cleanup EXIT INT TERM

echo "=========================================================="
echo " Starting Kiikoread WeRead E2E Test Runner"
echo " Workspace: ${WORKSPACE_ROOT}"
echo " Port:      ${PORT}"
echo " Tier:      ${TIER}"
echo "=========================================================="

# 1. Start mock server
MOCK_LOG="/tmp/weread_mock_server_${PORT}.log"
READY_FILE="/tmp/weread_mock_${PORT}.ready"
rm -f "$READY_FILE"

python3 -u "${SCRIPT_DIR}/weread_mock_server.py" --port "$PORT" > "$MOCK_LOG" 2>&1 &
MOCK_PID=$!
echo "[runner] WeRead mock server launched in background (PID: ${MOCK_PID}, log: ${MOCK_LOG})"

# 2. Wait for server readiness
echo -n "[runner] Waiting for mock server to become ready..."
READY=0
for i in {1..40}; do
    if [ -f "$READY_FILE" ]; then
        if curl --connect-timeout 1 -s -f "http://127.0.0.1:${PORT}/mock/control/health" > /dev/null 2>&1; then
            READY=1
            break
        fi
    fi
    sleep 0.2
    echo -n "."
done
echo ""

if [ "$READY" -ne 1 ]; then
    echo "[runner] ERROR: Mock server failed to start within timeout. Server log:" >&2
    cat "$MOCK_LOG" >&2
    exit 1
fi
echo "[runner] Mock server is healthy and responding."

# 3. Execute test suites
PASSED=0
FAILED=0

run_test_cmd() {
    local test_name="$1"
    shift
    echo "----------------------------------------------------------"
    echo "[RUN] ${test_name}"
    echo "----------------------------------------------------------"
    if "$@"; then
        echo "[PASS] ${test_name}"
        PASSED=$((PASSED + 1))
    else
        echo "[FAIL] ${test_name} (exit code: $?)" >&2
        FAILED=$((FAILED + 1))
    fi
}

# Always run the mock infra smoke suite first
run_test_cmd "Production C HTTP E2E (ASan/UBSan)" python3 "${SCRIPT_DIR}/test_weread_native_e2e.py"
run_test_cmd "WeRead Mock Infra & C Stub Smoke Suite" python3 "${SCRIPT_DIR}/test_weread_mock_infra.py"

# If not smoke-only, run tier tests if present
if [ "$SMOKE_ONLY" -eq 0 ]; then
    TESTS_DIR="${SCRIPT_DIR}/weread_tests"
    if [ -d "$TESTS_DIR" ]; then
        case "$TIER" in
            1|tier1)
                TIER_DIRS=("${TESTS_DIR}/tier1_features")
                ;;
            2|tier2)
                TIER_DIRS=("${TESTS_DIR}/tier2_boundaries")
                ;;
            3|tier3)
                TIER_DIRS=("${TESTS_DIR}/tier3_combinations")
                ;;
            4|tier4)
                TIER_DIRS=("${TESTS_DIR}/tier4_scenarios")
                ;;
            all|*)
                TIER_DIRS=(
                    "${TESTS_DIR}/tier1_features"
                    "${TESTS_DIR}/tier2_boundaries"
                    "${TESTS_DIR}/tier3_combinations"
                    "${TESTS_DIR}/tier4_scenarios"
                )
                ;;
        esac

        for tdir in "${TIER_DIRS[@]}"; do
            if [ -d "$tdir" ]; then
                for test_file in "$tdir"/test_*.py; do
                    if [ -f "$test_file" ]; then
                        run_test_cmd "$(basename "$test_file")" python3 "$test_file"
                    fi
                done
                for test_file in "$tdir"/test_*.sh; do
                    if [ -f "$test_file" ]; then
                        run_test_cmd "$(basename "$test_file")" bash "$test_file"
                    fi
                done
            fi
        done
    fi
fi

echo "=========================================================="
echo " Kiikoread WeRead E2E Test Summary"
echo " Passed: ${PASSED}"
echo " Failed: ${FAILED}"
echo " Total:  $((PASSED + FAILED))"
echo "=========================================================="

if [ "$FAILED" -gt 0 ]; then
    echo "[runner] STATUS: TEST SUITE FAILED" >&2
    exit 1
else
    echo "[runner] STATUS: ALL TESTS PASSED"
    exit 0
fi
