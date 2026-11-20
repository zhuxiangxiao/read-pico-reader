#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Fork OTA configuration, version arithmetic, origin resolution, and release planning."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
from urllib.error import HTTPError
from urllib.parse import urlparse
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM_REPO = "wegooo-cell/read-pico-reader"
UPSTREAM_FORBIDDEN_HOSTS = ("wegooo-cell.github.io", "kiikoread.com")
BOARDS = ("pico", "metalio47")
BOARD_ALIASES = {"metalio_s31": "metalio47"}

DEFAULT_CONFIG = {
    "schema": 1,
    "github_repo": "",
    "ota_origin": "",
    "upstream_repo": UPSTREAM_REPO,
    "upstream_remote": "upstream",
    "upstream_branch": "main",
    "retain_versions": 3,
    "tag_prefix": {
        "pico": "ota-pico-v",
        "metalio47": "ota-s31-v",
    },
}

_ORIGIN_RE = re.compile(r"^https://[A-Za-z0-9.-]+(?:/[A-Za-z0-9._-]+)*/$")
_SLUG_RE = re.compile(r"^[A-Za-z0-9._-]+/[A-Za-z0-9._-]+$")
_VERSION_RE = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-rc(\d+)(?:\.(\d+))?|-r(\d+))?$")


def canonical_board(board: str) -> str:
    return BOARD_ALIASES.get(board, board)


def load_config(root: Path = ROOT) -> dict:
    config = json.loads(json.dumps(DEFAULT_CONFIG))
    path = Path(root) / "fork_ota.json"
    if not path.is_file():
        return config
    raw = json.loads(path.read_text(encoding="utf-8"))
    if raw.get("schema") != 1:
        raise ValueError("fork_ota.json schema must be 1")
    for key in ("github_repo", "ota_origin", "upstream_repo", "upstream_remote", "upstream_branch"):
        if key in raw and raw[key] is not None:
            if not isinstance(raw[key], str):
                raise ValueError(f"fork_ota.json {key} must be a string")
            config[key] = raw[key].strip()
    if "retain_versions" in raw:
        retain = raw["retain_versions"]
        if not isinstance(retain, int) or retain < 1 or retain > 20:
            raise ValueError("fork_ota.json retain_versions must be an integer between 1 and 20")
        config["retain_versions"] = retain
    if "tag_prefix" in raw:
        prefixes = raw["tag_prefix"]
        if not isinstance(prefixes, dict):
            raise ValueError("fork_ota.json tag_prefix must be an object")
        for board in BOARDS:
            if board in prefixes:
                value = str(prefixes[board]).strip()
                if not value or not re.fullmatch(r"[A-Za-z0-9._-]+", value):
                    raise ValueError(f"Invalid tag_prefix for {board}: {value!r}")
                config["tag_prefix"][board] = value
    if config["tag_prefix"]["pico"] == config["tag_prefix"]["metalio47"]:
        raise ValueError("pico and metalio47 tag_prefix must differ")
    return config


def parse_github_slug(remote_url: str) -> str | None:
    if not remote_url:
        return None
    text = remote_url.strip()
    if not text:
        return None
    slug = None
    if _SLUG_RE.fullmatch(text):
        slug = text
    else:
        ssh_match = re.fullmatch(r"(?:ssh://)?(?:[^@/\s]+@)?github\.com[:/]([^/\s]+/[^/\s]+?)(?:\.git)?/?", text, re.I)
        if ssh_match:
            slug = ssh_match.group(1)
        else:
            parsed = urlparse(text)
            if parsed.scheme in ("https", "http") and (parsed.hostname or "").lower() == "github.com":
                path = parsed.path.strip("/")
                if path.endswith(".git"):
                    path = path[:-4]
                slug = path
    if not slug or not _SLUG_RE.fullmatch(slug):
        return None
    owner, repo = slug.split("/", 1)
    if owner in (".", "..") or repo in (".", ".."):
        return None
    return f"{owner}/{repo}"


def pages_origin_for_repo(slug: str) -> str:
    parsed = parse_github_slug(slug)
    if not parsed:
        raise ValueError(f"Invalid GitHub repository slug: {slug!r}")
    owner, repo = parsed.split("/", 1)
    host = f"{owner.lower()}.github.io"
    if repo.lower() == host:
        return f"https://{host}/"
    return f"https://{host}/{repo}/"


def validate_ota_origin(origin: str, require_fork: bool = False) -> str:
    if not isinstance(origin, str):
        raise ValueError("OTA origin must be a string")
    value = origin.strip()
    if not value:
        raise ValueError("OTA origin is empty")
    if any(bad in value for bad in ("..", "@", "?", "#")):
        raise ValueError(f"Invalid OTA origin: {value!r}")
    if not value.endswith("/"):
        value += "/"
    if len(value) > 180 or not _ORIGIN_RE.fullmatch(value):
        raise ValueError(f"OTA origin must be an HTTPS base URL ending with '/': {value!r}")
    parsed = urlparse(value)
    host = (parsed.hostname or "").lower()
    if not host or parsed.port is not None or host.startswith(".") or host.endswith("."):
        raise ValueError(f"Invalid OTA origin host: {value!r}")
    if require_fork and host in UPSTREAM_FORBIDDEN_HOSTS:
        raise ValueError(f"Fork OTA origin cannot point to upstream host {host!r}")
    return value


def _git_origin_slug(root: Path) -> str | None:
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "remote", "get-url", "origin"],
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except OSError:
        return None
    if proc.returncode != 0:
        return None
    return parse_github_slug(proc.stdout)


def resolve_github_repo(explicit_repo: str | None = None, root: Path = ROOT) -> str | None:
    if explicit_repo is not None and explicit_repo.strip():
        slug = parse_github_slug(explicit_repo)
        if not slug:
            raise ValueError(f"Invalid GitHub repo: {explicit_repo!r}")
        return slug

    config = load_config(root)
    candidates = [
        config.get("github_repo", ""),
        os.environ.get("GITHUB_REPOSITORY", ""),
        _git_origin_slug(root) or "",
    ]
    for raw in candidates:
        if not raw or not raw.strip():
            continue
        slug = parse_github_slug(raw)
        if not slug:
            raise ValueError(f"Invalid configured GitHub repo: {raw!r}")
        if slug.lower() == UPSTREAM_REPO.lower():
            continue
        return slug
    return None


def resolve_ota_origin(
    explicit: str | None = None,
    repo: str | None = None,
    root: Path = ROOT,
    require_fork: bool = False,
) -> str | None:
    config = load_config(root)
    if explicit is not None and explicit.strip():
        return validate_ota_origin(explicit, require_fork=require_fork)
    env_origin = os.environ.get("KIIKO_OTA_ORIGIN", "").strip()
    if env_origin:
        return validate_ota_origin(env_origin, require_fork=require_fork)
    cfg_origin = config.get("ota_origin", "").strip()
    if cfg_origin:
        return validate_ota_origin(cfg_origin, require_fork=require_fork)
    slug = resolve_github_repo(explicit_repo=repo, root=root)
    if slug:
        if require_fork and slug.lower() == UPSTREAM_REPO.lower():
            raise ValueError("Cannot use upstream repository as fork OTA origin")
        return validate_ota_origin(pages_origin_for_repo(slug), require_fork=require_fork)
    if require_fork:
        raise ValueError(
            "Unable to determine fork OTA origin; set --ota-origin, KIIKO_OTA_ORIGIN, "
            "or fork_ota.json (github_repo/ota_origin)"
        )
    return None


def parse_version_tuple(version: str) -> tuple[int, int, int, int, int]:
    if not isinstance(version, str) or not version or len(version) > 32:
        raise ValueError(f"Invalid version string: {version!r}")
    match = _VERSION_RE.fullmatch(version)
    if not match:
        raise ValueError(f"Unsupported version format: {version!r}")
    major, minor, patch = (int(match.group(i)) for i in (1, 2, 3))
    if any(x > 65535 for x in (major, minor, patch)):
        raise ValueError(f"Version component exceeds 65535: {version!r}")
    rc_str, rc_rev_str, stable_rev_str = match.group(4), match.group(5), match.group(6)
    rc_part = 65536
    rev_part = 0
    if rc_str is not None:
        rc_part = int(rc_str)
        if rc_part > 65535:
            raise ValueError(f"RC number exceeds 65535: {version!r}")
        if rc_rev_str is not None:
            rev_part = int(rc_rev_str)
            if rev_part < 1 or rev_part > 65535:
                raise ValueError(f"Fork revision must be in 1..65535: {version!r}")
    elif stable_rev_str is not None:
        rev_part = int(stable_rev_str)
        if rev_part < 1 or rev_part > 65535:
            raise ValueError(f"Fork revision must be in 1..65535: {version!r}")
    return (major, minor, patch, rc_part, rev_part)


def format_fork_version(base_version: str, revision: int) -> str:
    parts = parse_version_tuple(base_version)
    if parts[4] != 0:
        raise ValueError(f"Base version already has a fork revision: {base_version!r}")
    if not isinstance(revision, int) or isinstance(revision, bool) or revision < 1 or revision > 65535:
        raise ValueError(f"Revision must be an integer in 1..65535: {revision!r}")
    candidate = f"{base_version}.{revision}" if parts[3] < 65536 else f"{base_version}-r{revision}"
    parse_version_tuple(candidate)
    return candidate


def next_revision_for_base(base_version: str, existing_versions: list[str]) -> int:
    base_prefix = parse_version_tuple(base_version)[:4]
    max_rev = 0
    for version in existing_versions:
        try:
            parsed = parse_version_tuple(version)
        except ValueError:
            continue
        if parsed[:4] == base_prefix and parsed[4] > max_rev:
            max_rev = parsed[4]
    nxt = max_rev + 1
    if nxt > 65535:
        raise ValueError(f"Revision overflow for base {base_version!r}")
    return nxt


def board_spec(board: str, root: Path = ROOT) -> dict:
    board = canonical_board(board)
    if board not in BOARDS:
        raise ValueError(f"Unknown board: {board!r}")
    info = json.loads((Path(root) / "boards" / board / "board.json").read_text(encoding="utf-8"))
    if info.get("id") != board:
        raise ValueError(f"Invalid board.json id for {board}")
    cfg = load_config(root)
    if board == "pico":
        extra = {
            "board_id": "RDP-G01-W",
            "layout": "pico-dual-4m-v1",
            "minimum_base_version": "0.3.3-rc72",
            "capability": "PICO_HTTPS_OTA_V1",
            "feed_filename": "update.json",
            "update_prefix": "Pico-update",
            "tf_filename": "Pico-update.bin",
            "slot_size": 0x400000,
        }
    else:
        extra = {
            "board_id": "Metalio-EINK4-Plus-S31",
            "layout": "metalio-s31-dual-5m-v1",
            "minimum_base_version": "0.3.3-rc98",
            "capability": "METALIO_S31_HTTPS_OTA_V1",
            "feed_filename": "metalio-s31-update.json",
            "update_prefix": "Metalio-S31-update",
            "tf_filename": "Metalio-S31-update.bin",
            "slot_size": 0x500000,
        }
    return {
        "board": board,
        "name": info["name"],
        "idf_target": info["idf_target"],
        "project": info["project"],
        "image_chip_id": info["image_chip_id"],
        "bootloader_offset": info["bootloader_offset"],
        "base_version": info["version"],
        "tag_prefix": cfg["tag_prefix"][board],
        **extra,
    }


def validate_firmware_bytes(
    data: bytes,
    board: str,
    expected_version: str | None = None,
    ota_origin: str | None = None,
    check_no_upstream: bool = False,
    root: Path = ROOT,
) -> dict:
    spec = board_spec(board, root=root)
    if len(data) < 288 or len(data) > spec["slot_size"]:
        raise ValueError(f"{board} firmware size {len(data)} out of range 288..{spec['slot_size']}")
    if data[0] != 0xE9 or struct.unpack_from("<I", data, 32)[0] != 0xABCD5432:
        raise ValueError(f"{board} firmware lacks valid ESP image/app_desc magic")
    chip_id = struct.unpack_from("<H", data, 12)[0]
    if chip_id != spec["image_chip_id"]:
        raise ValueError(f"{board} chip_id mismatch: got {chip_id}, expected {spec['image_chip_id']}")
    version = data[48:80].split(b"\0")[0].decode("ascii")
    project = data[80:112].split(b"\0")[0].decode("ascii")
    parse_version_tuple(version)
    if project != spec["project"]:
        raise ValueError(f"{board} project mismatch: got {project!r}, expected {spec['project']!r}")
    if expected_version is not None and version != expected_version:
        raise ValueError(f"{board} version mismatch: got {version!r}, expected {expected_version!r}")
    if ota_origin is not None:
        normalized_origin = validate_ota_origin(ota_origin, require_fork=True)
        if spec["capability"].encode("ascii") not in data:
            raise ValueError(f"{board} firmware missing capability {spec['capability']}")
        expected_feed = f"{normalized_origin}{spec['feed_filename']}".encode("ascii") + b"\0"
        if expected_feed not in data:
            raise ValueError(f"{board} firmware missing baked OTA feed URL {expected_feed[:-1].decode()}")
    if check_no_upstream:
        for forbidden in (
            b"https://wegooo-cell.github.io/read-pico-reader/",
            b"https://kiikoread.com/",
        ):
            if forbidden in data:
                raise ValueError(f"{board} firmware still contains upstream OTA origin {forbidden.decode()}")
    return {"board": spec["board"], "version": version, "project": project, "size": len(data)}


def validate_feed_dict(
    feed: dict,
    board: str,
    ota_origin: str,
    expected_version: str | None = None,
    root: Path = ROOT,
) -> dict:
    spec = board_spec(board, root=root)
    normalized_origin = validate_ota_origin(ota_origin, require_fork=True)
    if not isinstance(feed, dict):
        raise ValueError("OTA feed must be a JSON object")
    if feed.get("schema") != 1 or isinstance(feed.get("schema"), bool):
        raise ValueError("OTA feed schema must be 1")
    version = feed.get("version")
    parse_version_tuple(version)
    if expected_version is not None and version != expected_version:
        raise ValueError(f"OTA feed version mismatch: {version!r} != {expected_version!r}")
    if feed.get("project") != spec["project"] or len(feed["project"]) >= 33:
        raise ValueError(f"OTA feed project mismatch: {feed.get('project')!r}")
    if feed.get("board") != spec["board_id"] or len(feed["board"]) >= 24:
        raise ValueError(f"OTA feed board mismatch: {feed.get('board')!r}")
    if feed.get("layout") != spec["layout"] or len(feed["layout"]) >= 32:
        raise ValueError(f"OTA feed layout mismatch: {feed.get('layout')!r}")
    min_base = feed.get("minimum_base_version")
    parse_version_tuple(min_base)
    expected_bin = f"{spec['update_prefix']}-{version}.bin"
    expected_url = f"{normalized_origin}{expected_bin}"
    url = feed.get("url")
    if url != expected_url or len(url) >= 256:
        raise ValueError(f"OTA feed URL mismatch: {url!r} != {expected_url!r}")
    sha256 = feed.get("sha256")
    if not isinstance(sha256, str) or not re.fullmatch(r"[a-f0-9]{64}", sha256):
        raise ValueError(f"Invalid OTA feed sha256: {sha256!r}")
    notes = feed.get("notes")
    if not isinstance(notes, str) or not (0 < len(notes.encode("utf-8")) < 384):
        raise ValueError("OTA feed notes must be 1..383 UTF-8 bytes")
    size = feed.get("size")
    if not isinstance(size, int) or isinstance(size, bool) or not (288 <= size <= spec["slot_size"]):
        raise ValueError(f"Invalid OTA feed size: {size!r}")
    return feed


def fetch_github_releases(repo: str, token: str | None = None) -> list[dict]:
    slug = parse_github_slug(repo)
    if not slug:
        raise ValueError(f"Invalid GitHub repository: {repo!r}")
    auth_token = token if token is not None else (os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN"))
    releases: list[dict] = []
    url: str | None = f"https://api.github.com/repos/{slug}/releases?per_page=100"
    while url:
        headers = {
            "Accept": "application/vnd.github+json",
            "User-Agent": "kiikoread-fork-ota",
            "X-GitHub-Api-Version": "2022-11-28",
        }
        if auth_token:
            headers["Authorization"] = f"Bearer {auth_token}"
        req = Request(url, headers=headers)
        try:
            with urlopen(req, timeout=30) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                link_header = resp.headers.get("Link", "")
        except HTTPError as err:
            body = err.read().decode("utf-8", errors="replace")[:300]
            raise RuntimeError(f"GitHub API error {err.code} for {url}: {body}") from err
        if not isinstance(data, list):
            raise RuntimeError(f"Unexpected GitHub releases response for {url}")
        releases.extend(item for item in data if not item.get("draft"))
        next_match = re.search(r'<([^>]+)>;\s*rel="next"', link_header)
        url = next_match.group(1) if next_match else None
    return releases


def plan_release(
    board: str,
    revision: str = "",
    repo: str | None = None,
    ota_origin: str | None = None,
    releases: list[dict] | None = None,
    root: Path = ROOT,
) -> dict:
    selected_board = board.strip()
    if selected_board != "all":
        selected_board = canonical_board(selected_board)
        if selected_board not in BOARDS:
            raise ValueError(f"Unknown board: {board!r}")
        boards = [selected_board]
    else:
        boards = list(BOARDS)

    resolved_repo = resolve_github_repo(explicit_repo=repo, root=root)
    if not resolved_repo:
        raise ValueError("Unable to determine fork GitHub repository")
    if resolved_repo.lower() == UPSTREAM_REPO.lower():
        raise ValueError(f"Refusing to run fork OTA release on upstream repository {resolved_repo!r}")

    resolved_origin = resolve_ota_origin(explicit=ota_origin, repo=resolved_repo, root=root, require_fork=True)
    if releases is None:
        releases = fetch_github_releases(resolved_repo)

    existing_tags = {str(item.get("tag_name", "")) for item in releases if not item.get("draft")}

    explicit_rev: int | None = None
    if revision is not None and str(revision).strip():
        rev_text = str(revision).strip()
        if not re.fullmatch(r"\d+", rev_text):
            raise ValueError(f"Revision must be a positive integer: {rev_text!r}")
        explicit_rev = int(rev_text)
        if explicit_rev < 1 or explicit_rev > 65535:
            raise ValueError(f"Revision must be in 1..65535: {explicit_rev}")

    entries = []
    for item_board in boards:
        spec = board_spec(item_board, root=root)
        prefix = spec["tag_prefix"]
        existing_versions = [
            tag[len(prefix):]
            for tag in existing_tags
            if tag.startswith(prefix)
        ]
        rev = explicit_rev if explicit_rev is not None else next_revision_for_base(spec["base_version"], existing_versions)
        version = format_fork_version(spec["base_version"], rev)
        tag = f"{prefix}{version}"
        if tag in existing_tags:
            raise ValueError(f"Release tag already exists: {tag}")
        entries.append({
            "board": item_board,
            "idf_target": spec["idf_target"],
            "project": spec["project"],
            "base_version": spec["base_version"],
            "revision": rev,
            "version": version,
            "tag": tag,
            "feed_filename": spec["feed_filename"],
            "bin_filename": f"{spec['update_prefix']}-{version}.bin",
        })

    return {
        "repo": resolved_repo,
        "ota_origin": resolved_origin,
        "boards": entries,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_origin = sub.add_parser("resolve-origin", help="Resolve fork OTA HTTPS base origin")
    p_origin.add_argument("--ota-origin", default=None)
    p_origin.add_argument("--repo", default=None)
    p_origin.add_argument("--require-fork", action="store_true")

    p_plan = sub.add_parser("plan-release", help="Plan next fork OTA versions and matrix")
    p_plan.add_argument("--board", default="all", choices=("all", *BOARDS, *BOARD_ALIASES))
    p_plan.add_argument("--revision", default="")
    p_plan.add_argument("--repo", default=None)
    p_plan.add_argument("--ota-origin", default=None)
    p_plan.add_argument("--releases-json", type=Path, default=None)
    p_plan.add_argument("--github-output", type=Path, default=None)

    args = parser.parse_args(argv)
    if args.cmd == "resolve-origin":
        origin = resolve_ota_origin(explicit=args.ota_origin, repo=args.repo, require_fork=args.require_fork)
        print(origin or "")
        return 0

    if args.cmd == "plan-release":
        releases = None
        if args.releases_json is not None:
            releases = json.loads(args.releases_json.read_text(encoding="utf-8"))
        plan = plan_release(
            board=args.board,
            revision=args.revision,
            repo=args.repo,
            ota_origin=args.ota_origin,
            releases=releases,
        )
        if args.github_output is not None:
            lines = [
                f"ota_origin={plan['ota_origin']}",
                f"matrix={json.dumps(plan['boards'], separators=(',', ':'), ensure_ascii=False)}",
                f"boards_json={json.dumps(plan['boards'], separators=(',', ':'), ensure_ascii=False)}",
            ]
            with args.github_output.open("a", encoding="utf-8") as handle:
                handle.write("\n".join(lines) + "\n")
        print(json.dumps(plan, indent=2, ensure_ascii=False))
        return 0

    return 1


if __name__ == "__main__":
    sys.exit(main())
