#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: $0 [--fast]" >&2
  exit 2
}

FAST=0
case "${1:-}" in
  "") ;;
  --fast) FAST=1 ;;
  *) usage ;;
esac

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

if [ -z "${IDF_PATH:-}" ] || [ ! -f "$IDF_PATH/components/http_parser/http_parser.c" ]; then
  if [ -f "$HOME/esp/esp-idf/components/http_parser/http_parser.c" ]; then
    export IDF_PATH="$HOME/esp/esp-idf"
  else
    echo "ERROR: ESP-IDF components/http_parser/http_parser.c not found. Set IDF_PATH first." >&2
    exit 1
  fi
fi

echo "==> [1/4] Fork OTA & board consistency host tests"
bash tools/run_fork_ota_host_tests.sh

echo "==> [2/4] WeRead host tests (IDF_PATH=$IDF_PATH)"
IDF_PATH="$IDF_PATH" bash tools/run_weread_host_tests.sh

if [ "$FAST" -eq 1 ]; then
  echo "PASS: fast host verification succeeded."
  exit 0
fi

echo "==> [3/4] Cross-board root host tests"
python3 tools/reader_fullscreen_host_test.py
python3 tools/reader_origin_host_test.py
python3 tools/normal_book_compat_host_test.py
python3 tools/pico_home_progress_sync_host_test.py
python3 tools/home_initial_empty_host_test.py
python3 tools/reader_resume_durability_host_test.py
python3 tools/book_library_ui_host_test.py
python3 tools/s31_rc110_ripple_route_host_test.py
python3 tools/metalio_text_ripple_routes_host_test.py
python3 tools/license_notices_host_test.py

TMP_BIN_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_BIN_DIR"' EXIT
cc -std=c11 -D_DEFAULT_SOURCE -DKIIKO_BOARD_METALIO47=1 -Wall -Wextra -Werror \
  -fsanitize=address,undefined -Itools/settings_backup_stubs -Imain \
  tools/boot_state_host_test.c -o "$TMP_BIN_DIR/boot-state-test"
"$TMP_BIN_DIR/boot-state-test"

echo "==> [4/4] Materialized board-view host tests (pico & metalio47)"
for board in pico metalio47; do
  echo "--- Materializing build/host/$board ---"
  python3 tools/board_source_view.py --board "$board" --tests --output "build/host/$board"
  (
    cd "build/host/$board"
    sh tools/run_app_loop_host_tests.sh
    python3 tools/reader_fullscreen_host_test.py
    python3 tools/pager_host_test.py
    python3 tools/ui_pinpad_host_test.py
  )
done
(cd build/host/pico && python3 tools/quick_layer_raster_host_test.py)

echo "PASS: full upstream-sync host verification succeeded."
