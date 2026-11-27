---
name: upstream-sync-release
description: >-
  Sync and merge upstream wegooo-cell/read-pico-reader updates into zhuxiangxiao/read-pico-reader,
  resolve Fork OTA and WeRead conflicts cleanly, verify host regressions, and publish single-board
  or dual-board Fork OTA releases via GitHub Actions and the gh CLI. Use whenever the user asks
  to sync upstream, merge upstream changes, resolve upstream conflicts, or build/publish a new OTA
  version (e.g. "合并上游并发布新版本", "同步上游代码发版", "发布 Pico/S31 OTA").
---

# Upstream Sync & Fork OTA Release

Automates syncing `upstream` (`wegooo-cell/read-pico-reader`) into `origin` (`zhuxiangxiao/read-pico-reader`), resolving recurring conflicts while keeping WeRead and Fork OTA isolation intact, running host verification suites, and publishing verified single- or dual-board OTA updates.

## Prerequisites & Hard Rules

1. **Target board**: Confirm whether the user wants `all` (dual-board default), `pico` (ESP32-S3), or `metalio47` (Metalio S31 / ESP32-S31).
2. **Auth & remotes**: Verify `gh auth status` succeeds (SSH protocol, user `zhuxiangxiao`) and `git remote -v` points `origin` to `git@github.com:zhuxiangxiao/read-pico-reader.git` and `upstream` to `https://github.com/wegooo-cell/read-pico-reader.git`.
3. **Never violate**:
   - Never push, tag, or create releases on `upstream`.
   - Never touch other local worktrees (e.g. `/Users/skyzhu/Documents/read-pico-reader`).
   - Never alter partition tables, upstream `boards/*/board.json` versions, VCOM calibration, eFuse/OTP state, or hardware power sequencing.

## Step 1: Fetch & Upstream Pre-check

```bash
git fetch origin --tags
git fetch upstream --tags
python3 tools/fork_sync_upstream.py --check --add-remote
```

- Ensure the active branch incorporates `origin/main` first (if behind, fast-forward `git merge --ff-only origin/main`). Abort and ask the user if `git status --short` has unexpected uncommitted work.
- If `new_commits == 0` and `HEAD == origin/main` with no unreleased changes, stop and confirm with the user before triggering a duplicate release.

## Step 2: Merge Upstream & Resolve Conflicts

```bash
git merge --no-ff --no-commit upstream/main
```

- Consult [references/conflict-resolution.md](./references/conflict-resolution.md) for all conflict patterns, WeRead preservation guards, `main/CMakeLists.txt` flash-budget alignment, and `.github/workflows/fork-ota-release.yml` vs `.github/workflows/build.yml` flag parity.
- Remove obsolete files cleanly with `git rm` if upstream replaced mechanisms (such as RLE vs deflate UI icons).
- Before moving on, confirm zero conflict markers exist:

```bash
! git grep -n '^<<<<<<< \|^=======$\|^>>>>>>> '
```

## Step 3: Local Host Verification & Release Preview

Run the host verification script covering Fork OTA isolation, WeRead tests (with `IDF_PATH` `http_parser`), cross-board tests, and materialized `pico`/`metalio47` views:

```bash
bash .agents/skills/upstream-sync-release/scripts/verify_host_tests.sh
```

Preview the derived release version(s) and tags:

```bash
GITHUB_TOKEN="$(gh auth token)" python3 tools/fork_ota_config.py plan-release \
  --board <all|pico|metalio47> \
  --repo zhuxiangxiao/read-pico-reader
```

- Verify `pico` computes `0.3.3-rc<N>.1` (`ota-pico-v...`) and/or `metalio47` computes `0.3.3-rc<M>.1` (`ota-s31-v...`) with `ota_origin` = `https://zhuxiangxiao.github.io/read-pico-reader/`.

## Step 4: Commit, Push & Trigger Release

1. Commit the 2-parent merge commit and verify parent #2 matches `upstream/main`:

```bash
git add -A
git commit -m "Merge upstream/main (Pico <pico_ver> / S31 <s31_ver>) into $(git branch --show-current)"
test "$(git rev-parse HEAD^2)" = "$(git rev-parse upstream/main)"
```

2. Push the current branch and fast-forward `origin/main`:

```bash
git push origin HEAD
git push origin HEAD:main
```

3. Choose a non-colliding release tag for the requested scope and keep the annotated tag message concise in Chinese (< 384 UTF-8 bytes so it fits device OTA prompts). The device OTA confirmation screen (`draw_upgrade_notes` in `app_device_settings.c`) splits on `\n` / `;` / `；`, wraps at `540 px` (~19–20 Chinese chars/line), and renders at most 6 lines — format annotated tag messages as concise numbered Chinese lines (`1. ...\n2. ...`, `<= 6` short lines):
   - Dual-board (`all`): `release-YYYYMMDD` (append `-2`, `-3`, ... if already exists)
   - Pico only (`pico`): `release-pico-YYYYMMDD`
   - Metalio S31 only (`metalio47`): `release-s31-YYYYMMDD`

```bash
git tag -a "<tag>" -m "<concise Chinese release summary>"
git push origin "<tag>"
```

*(Alternative for re-runs / dry-runs without creating a tag: `gh workflow run "Fork OTA Release" -f board=<all|pico|metalio47> -f dry_run=<false|true>`.)*

## Step 5: Watch Workflow & Verify Releases / Pages

1. Find the triggered run and watch it in the background (wait reactively for completion; do not poll in a loop):

```bash
gh run list --workflow "Fork OTA Release" -L 3 --json databaseId,headBranch,status,conclusion,url
gh run watch <run_id> --exit-status
```

2. If any job fails (`host-tests`, `build (pico)`, `build (metalio47)`, `publish`, or `deploy`), inspect `gh run view <run_id> --log-failed` before patching.
3. Once `gh run watch` succeeds, run the post-release verifier to confirm firmware sizes, `free_app_bytes`, Release assets, live GitHub Pages JSON feeds, and HTTP 200 downloads:

```bash
bash .agents/skills/upstream-sync-release/scripts/verify_release.sh <run_id> <all|pico|metalio47>
```

4. Confirm `git status -sb` is clean and report results in Chinese with clickable links to the workflow run, GitHub Release(s), live `update.json` / `metalio-s31-update.json`, exact sizes and remaining `free_app_bytes`.
