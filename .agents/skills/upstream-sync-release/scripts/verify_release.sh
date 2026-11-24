#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "Usage: $0 <run_id> [all|pico|metalio47]" >&2
  exit 2
fi

RUN_ID="$1"
TARGET_BOARD="${2:-all}"

case "$TARGET_BOARD" in
  all|pico|metalio47) ;;
  *) echo "ERROR: board must be all, pico, or metalio47 (got '$TARGET_BOARD')" >&2; exit 2 ;;
esac

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

RUN_JSON="$(gh run view "$RUN_ID" --json status,conclusion,url,headBranch)"
python3 - "$RUN_JSON" <<'PY'
import json, sys
run = json.loads(sys.argv[1])
if run.get("status") != "completed" or run.get("conclusion") != "success":
    print(f"ERROR: run is not successful: status={run.get('status')} conclusion={run.get('conclusion')} url={run.get('url')}", file=sys.stderr)
    sys.exit(1)
print(f"Workflow Run: {run.get('url')} (ref={run.get('headBranch')}, conclusion={run.get('conclusion')})")
PY

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT
LOG_FILE="$TMP_DIR/run.log"
META_FILE="$TMP_DIR/metadata.json"

gh run view "$RUN_ID" --log > "$LOG_FILE"

python3 - "$LOG_FILE" "$TARGET_BOARD" "$META_FILE" <<'PY'
import json, sys

log_path, target_board, out_path = sys.argv[1], sys.argv[2], sys.argv[3]
expected = ["pico", "metalio47"] if target_board == "all" else [target_board]
found = {}

with open(log_path, encoding="utf-8", errors="replace") as fh:
    for line in fh:
        idx = line.find("{")
        if idx < 0:
            continue
        candidate = line[idx:].strip()
        try:
            obj = json.loads(candidate)
        except json.JSONDecodeError:
            continue
        if isinstance(obj, dict) and obj.get("schema") == 1 and "free_app_bytes" in obj and "feed_filename" in obj:
            board = obj.get("board")
            if board in expected:
                found[board] = obj

missing = [b for b in expected if b not in found]
if missing:
    print(f"ERROR: missing packaged ota-metadata.json in run logs for board(s): {', '.join(missing)}", file=sys.stderr)
    sys.exit(1)

with open(out_path, "w", encoding="utf-8") as fh:
    json.dump([found[b] for b in expected], fh, ensure_ascii=False, indent=2)
PY

python3 - "$META_FILE" <<'PY'
import json, subprocess, sys, time, urllib.parse

with open(sys.argv[1], encoding="utf-8") as fh:
    boards = json.load(fh)

for meta in boards:
    board = meta["board"]
    tag = meta["tag"]
    version = meta["version"]
    base_version = meta["base_version"]
    expected_size = int(meta["size"])
    expected_sha = meta["sha256"]
    free_bytes = int(meta["free_app_bytes"])
    origin = meta["ota_origin"]
    if not origin.endswith("/"):
        origin += "/"
    feed_url = urllib.parse.urljoin(origin, meta["feed_filename"])

    rel_raw = subprocess.check_output(
        ["gh", "release", "view", tag, "--json", "tagName,name,url,assets"],
        text=True,
    )
    rel = json.loads(rel_raw)
    assets_by_name = {a["name"]: a for a in rel.get("assets", [])}
    required_assets = [
        meta["bin_filename"],
        meta["feed_filename"],
        meta["tf_zip_filename"],
    ]
    if meta.get("usb_zip_filename"):
        required_assets.append(meta["usb_zip_filename"])

    missing_assets = [name for name in required_assets if name not in assets_by_name]
    if missing_assets:
        raise SystemExit(f"ERROR [{board}]: release {tag} missing assets: {missing_assets}")

    bin_asset_size = int(assets_by_name[meta["bin_filename"]]["size"])
    if bin_asset_size != expected_size:
        raise SystemExit(f"ERROR [{board}]: release bin size {bin_asset_size} != expected {expected_size}")

    feed = None
    for attempt in range(2):
        feed_raw = subprocess.check_output(["curl", "-fsSL", feed_url], text=True)
        parsed = json.loads(feed_raw)
        if parsed.get("version") == version:
            feed = parsed
            break
        if attempt == 0:
            time.sleep(5)
    if feed is None:
        raise SystemExit(f"ERROR [{board}]: {feed_url} returned version {parsed.get('version')!r}, expected {version!r}")
    if feed.get("sha256") != expected_sha or int(feed.get("size", -1)) != expected_size:
        raise SystemExit(f"ERROR [{board}]: feed metadata mismatch in {feed_url}: {feed}")

    bin_url = feed["url"]
    head_out = subprocess.check_output(["curl", "-fsSI", "-L", bin_url], text=True)
    blocks = [b.strip() for b in head_out.split("\r\n\r\n") if b.strip()]
    if not blocks:
        blocks = [head_out.strip()]
    last_headers = blocks[-1].splitlines()
    status_line = last_headers[0] if last_headers else ""
    if " 200" not in status_line:
        raise SystemExit(f"ERROR [{board}]: expected HTTP 200 for {bin_url}, got {status_line!r}")
    cl = None
    for h in last_headers[1:]:
        if h.lower().startswith("content-length:"):
            cl = int(h.split(":", 1)[1].strip())
    if cl != expected_size:
        raise SystemExit(f"ERROR [{board}]: content-length {cl} != expected {expected_size} for {bin_url}")

    print(f"[{board}] VERIFIED:")
    print(f"  version:        {version} (upstream base {base_version})")
    print(f"  binary size:    {expected_size} B (free_app_bytes={free_bytes} B)")
    print(f"  release:        {rel.get('url')} ({len(assets_by_name)} assets)")
    print(f"  pages manifest: {feed_url}")
    print(f"  bin url:        {bin_url} ({status_line}, content-length={cl})")
PY
