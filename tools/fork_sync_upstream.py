#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Inspect and safely merge upstream read_pico_firmware updates into the fork."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys

from fork_ota_config import BOARDS, ROOT, load_config

OTA_TOUCHPOINTS = (
    "CMakeLists.txt",
    "main/CMakeLists.txt",
    "main/ota_online.h",
    "main/ota_release.c",
    "boards/pico/src/main/ota_release.c",
    "boards/pico/src/main/ota_online.c",
    "boards/metalio47/src/main/ota_online.c",
    "tools/build_firmware.py",
    ".github/workflows/pages.yml",
)


def _git(repo_dir: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["git", "-C", str(repo_dir), *args],
        capture_output=True,
        text=True,
        check=check,
    )


def _board_version_at(repo_dir: Path, ref: str, board: str) -> str | None:
    proc = _git(repo_dir, "show", f"{ref}:boards/{board}/board.json", check=False)
    if proc.returncode != 0:
        return None
    try:
        return json.loads(proc.stdout).get("version")
    except json.JSONDecodeError:
        return None


def inspect_upstream(
    repo_dir: Path = ROOT,
    add_remote: bool = False,
    fetch: bool = True,
) -> dict:
    repo_dir = Path(repo_dir).resolve()
    cfg = load_config(repo_dir)
    remote = cfg["upstream_remote"]
    branch = cfg["upstream_branch"]
    upstream_url = f"https://github.com/{cfg['upstream_repo']}.git"

    remote_check = _git(repo_dir, "remote", "get-url", remote, check=False)
    if remote_check.returncode != 0:
        if not add_remote:
            raise RuntimeError(
                f"Git remote {remote!r} is not configured. Re-run with --add-remote "
                f"or run: git remote add {remote} {upstream_url}"
            )
        _git(repo_dir, "remote", "add", remote, upstream_url)

    if fetch:
        _git(repo_dir, "fetch", remote, "--tags")

    target_ref = f"{remote}/{branch}"
    if _git(repo_dir, "rev-parse", "--verify", target_ref, check=False).returncode != 0:
        raise RuntimeError(f"Upstream ref {target_ref!r} not found (try without --no-fetch)")

    head_sha = _git(repo_dir, "rev-parse", "--short", "HEAD").stdout.strip()
    target_sha = _git(repo_dir, "rev-parse", "--short", target_ref).stdout.strip()
    merge_base = _git(repo_dir, "merge-base", "HEAD", target_ref).stdout.strip()
    ahead_count = int(_git(repo_dir, "rev-list", "--count", f"HEAD..{target_ref}").stdout.strip())
    subjects_raw = _git(repo_dir, "log", "--oneline", "-n", "15", f"HEAD..{target_ref}").stdout.strip()
    subjects = [line for line in subjects_raw.splitlines() if line.strip()]

    upstream_changed = {
        line.strip()
        for line in _git(repo_dir, "diff", "--name-only", merge_base, target_ref).stdout.splitlines()
        if line.strip()
    }
    local_changed = {
        line.strip()
        for line in _git(repo_dir, "diff", "--name-only", merge_base, "HEAD").stdout.splitlines()
        if line.strip()
    }

    versions = {}
    for board in BOARDS:
        versions[board] = {
            "current": _board_version_at(repo_dir, "HEAD", board),
            "upstream": _board_version_at(repo_dir, target_ref, board),
        }

    touchpoint_set = set(OTA_TOUCHPOINTS)
    ota_touchpoint_updates = sorted(upstream_changed & touchpoint_set)
    both_modified_other = sorted((upstream_changed & local_changed) - touchpoint_set)

    return {
        "remote": remote,
        "branch": branch,
        "target_ref": target_ref,
        "head": head_sha,
        "upstream_head": target_sha,
        "merge_base": merge_base[:12],
        "new_commits": ahead_count,
        "recent_commits": subjects,
        "versions": versions,
        "ota_touchpoint_updates": ota_touchpoint_updates,
        "both_modified_other": both_modified_other,
    }


def merge_upstream(
    repo_dir: Path = ROOT,
    add_remote: bool = False,
    fetch: bool = True,
) -> tuple[int, dict]:
    repo_dir = Path(repo_dir).resolve()
    status = _git(repo_dir, "status", "--porcelain").stdout.strip()
    if status:
        raise RuntimeError("Working tree is not clean; commit or stash changes before merging upstream")

    report = inspect_upstream(repo_dir=repo_dir, add_remote=add_remote, fetch=fetch)
    if report["new_commits"] == 0:
        return 0, {**report, "merged": False, "up_to_date": True, "conflicts": []}

    proc = _git(repo_dir, "merge", "--no-ff", report["target_ref"], check=False)
    if proc.returncode != 0:
        conflicts = [
            line.strip()
            for line in _git(repo_dir, "diff", "--name-only", "--diff-filter=U", check=False).stdout.splitlines()
            if line.strip()
        ]
        return 2, {**report, "merged": False, "up_to_date": False, "conflicts": conflicts}

    return 0, {**report, "merged": True, "up_to_date": False, "conflicts": []}


def _print_report(report: dict) -> None:
    print(f"Upstream target: {report['target_ref']} ({report['upstream_head']}) vs HEAD ({report['head']})")
    print(f"New upstream commits: {report['new_commits']}")
    for board, ver in report["versions"].items():
        changed = " (UPDATED)" if ver["current"] != ver["upstream"] else ""
        print(f"  {board}: {ver['current']} -> {ver['upstream']}{changed}")
    if report["recent_commits"]:
        print("Recent upstream commits:")
        for line in report["recent_commits"]:
            print(f"  {line}")
    if report["ota_touchpoint_updates"]:
        print("Warning: upstream modified fork OTA touchpoint file(s):")
        for path in report["ota_touchpoint_updates"]:
            print(f"  - {path}")
    if report["both_modified_other"]:
        print("Warning: both local branch and upstream modified:")
        for path in report["both_modified_other"]:
            print(f"  - {path}")
    if not report["ota_touchpoint_updates"] and not report["both_modified_other"]:
        print("Conflict pre-check: no overlapping edits detected.")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true", help="Inspect upstream changes without merging (default)")
    mode.add_argument("--merge", action="store_true", help="Merge upstream into the current branch with --no-ff")
    parser.add_argument("--add-remote", action="store_true", help="Add upstream remote if missing")
    parser.add_argument("--no-fetch", action="store_true", help="Do not run git fetch before inspecting/merging")
    parser.add_argument("--repo-dir", type=Path, default=ROOT, help="Git repository path")
    parser.add_argument("--json", action="store_true", help="Output JSON report")
    args = parser.parse_args(argv)

    try:
        if args.merge:
            code, result = merge_upstream(
                repo_dir=args.repo_dir,
                add_remote=args.add_remote,
                fetch=not args.no_fetch,
            )
        else:
            code = 0
            result = inspect_upstream(
                repo_dir=args.repo_dir,
                add_remote=args.add_remote,
                fetch=not args.no_fetch,
            )
    except Exception as err:
        print(f"ERROR: {err}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return code

    _print_report(result)
    if args.merge:
        if result.get("up_to_date"):
            print("Already up to date with upstream.")
        elif result.get("merged"):
            print("Merged cleanly. Next steps:")
            print("  1. Run: bash tools/run_fork_ota_host_tests.sh")
            print("  2. Push your branch and trigger the 'Fork OTA Release' GitHub Actions workflow.")
        elif result.get("conflicts"):
            print("Merge stopped with conflicts in:")
            for path in result["conflicts"]:
                print(f"  * {path}")
            print("Resolution tips: keep upstream flash/ and boards/*/board.json; preserve KIIKO_OTA_ORIGIN guards in OTA files.")
    return code


if __name__ == "__main__":
    sys.exit(main())
