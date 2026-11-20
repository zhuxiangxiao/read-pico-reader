#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Assemble a clean GitHub Pages OTA site from this fork's GitHub Releases."""
from __future__ import annotations

import argparse
import hashlib
import html
import json
from pathlib import Path
import sys
import time
from urllib.request import Request, urlopen

from fork_ota_config import (
    BOARDS,
    ROOT,
    UPSTREAM_REPO,
    board_spec,
    fetch_github_releases,
    load_config,
    parse_version_tuple,
    resolve_github_repo,
    resolve_ota_origin,
    validate_feed_dict,
    validate_firmware_bytes,
    validate_ota_origin,
)


def _download_bytes(url: str, max_bytes: int) -> bytes:
    for attempt in range(3):
        try:
            req = Request(url, headers={"User-Agent": "kiikoread-fork-ota"})
            with urlopen(req, timeout=60) as resp:
                data = resp.read(max_bytes + 1)
            if len(data) > max_bytes:
                raise ValueError(f"Downloaded payload exceeds limit {max_bytes} bytes: {url}")
            return data
        except Exception:
            if attempt == 2:
                raise
            time.sleep(3)
    raise RuntimeError(f"Failed to download {url}")


def _read_asset_bytes(
    release: dict,
    asset_name: str,
    max_bytes: int,
    local_dir: Path | None = None,
) -> bytes:
    assets = {item.get("name"): item for item in release.get("assets", []) if isinstance(item, dict)}
    if asset_name not in assets:
        raise FileNotFoundError(f"Release {release.get('tag_name')} missing asset {asset_name!r}")
    asset = assets[asset_name]
    if local_dir is not None:
        tag = str(release.get("tag_name", ""))
        candidates = [
            local_dir / "assets" / tag / asset_name,
            local_dir / tag / asset_name,
            local_dir / asset_name,
        ]
        if asset.get("local_path"):
            candidates.insert(0, Path(local_dir) / asset["local_path"])
        for candidate in candidates:
            if candidate.is_file():
                data = candidate.read_bytes()
                if len(data) > max_bytes:
                    raise ValueError(f"Local asset {candidate} exceeds {max_bytes} bytes")
                return data
        raise FileNotFoundError(f"Local asset not found for {tag}/{asset_name} under {local_dir}")

    download_url = asset.get("browser_download_url")
    if not download_url:
        raise ValueError(f"Release {release.get('tag_name')} asset {asset_name!r} has no download URL")
    return _download_bytes(download_url, max_bytes)


def _render_index_html(
    ota_origin: str,
    repo: str | None,
    staged: dict[str, dict],
) -> str:
    cards = []
    for board in BOARDS:
        if board not in staged:
            continue
        info = staged[board]
        feed = info["latest_feed"]
        spec = info["spec"]
        bin_name = feed["url"].rsplit("/", 1)[-1]
        retained_links = ", ".join(
            f'<a href="./{html.escape(name)}">{html.escape(ver)}</a>'
            for ver, name in info["retained"]
        )
        notes_html = "<br>".join(html.escape(line) for line in feed["notes"].splitlines())
        cards.append(
            f"""      <section class="card">
        <div class="card-head">
          <span class="badge">{html.escape(board)} · {html.escape(spec['idf_target'].upper())}</span>
          <h2>{html.escape(spec['name'])}</h2>
        </div>
        <dl class="meta">
          <div><dt>最新版本</dt><dd><code>{html.escape(feed['version'])}</code></dd></div>
          <div><dt>固件大小</dt><dd>{feed['size']:,} B</dd></div>
          <div><dt>SHA-256</dt><dd><code class="sha">{html.escape(feed['sha256'])}</code></dd></div>
          <div><dt>OTA 清单</dt><dd><a href="./{html.escape(spec['feed_filename'])}">{html.escape(spec['feed_filename'])}</a></dd></div>
          <div><dt>直链固件</dt><dd><a href="./{html.escape(bin_name)}">{html.escape(bin_name)}</a></dd></div>
          <div><dt>已保留版本</dt><dd>{retained_links}</dd></div>
        </dl>
        <div class="notes"><strong>更新说明</strong><p>{notes_html}</p></div>
      </section>"""
        )

    repo_link = (
        f'<a href="https://github.com/{html.escape(repo)}/releases">GitHub Releases（含 USB / TF 完整包）↗</a>'
        if repo
        else ""
    )
    return f"""<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1">
  <title>kiikoread Fork OTA Feed</title>
  <style>
    :root {{ color-scheme: light; --bg: #f6f6f3; --fg: #18181b; --muted: #52525b; --card: #ffffff; --line: #e4e4e7; }}
    * {{ box-sizing: border-box; }}
    body {{ margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif; background: var(--bg); color: var(--fg); line-height: 1.6; }}
    main {{ max-width: 920px; margin: 0 auto; padding: 32px 20px 56px; }}
    header {{ margin-bottom: 24px; }}
    h1 {{ margin: 0 0 6px; font-size: 1.75rem; }}
    .sub {{ color: var(--muted); margin: 0; font-size: 0.95rem; }}
    .grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(340px, 1fr)); gap: 16px; margin: 20px 0; }}
    .card {{ background: var(--card); border: 1px solid var(--line); border-radius: 12px; padding: 18px; }}
    .card-head {{ margin-bottom: 12px; }}
    .badge {{ display: inline-block; font-size: 0.75rem; padding: 2px 8px; border-radius: 999px; background: #18181b; color: #fff; }}
    h2 {{ margin: 8px 0 0; font-size: 1.2rem; }}
    .meta {{ display: grid; grid-template-columns: 92px 1fr; gap: 6px 10px; margin: 0 0 12px; font-size: 0.9rem; }}
    .meta div {{ display: contents; }}
    dt {{ color: var(--muted); }}
    dd {{ margin: 0; overflow-wrap: anywhere; }}
    code {{ font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 0.85em; }}
    .sha {{ word-break: break-all; }}
    .notes {{ border-top: 1px solid var(--line); padding-top: 10px; font-size: 0.9rem; }}
    .notes p {{ margin: 6px 0 0; color: var(--muted); }}
    footer {{ margin-top: 24px; font-size: 0.88rem; color: var(--muted); }}
    a {{ color: #18181b; }}
  </style>
</head>
<body>
  <main>
    <header>
      <h1>kiikoread 自建 OTA 更新源</h1>
      <p class="sub">OTA 根地址：<code>{html.escape(ota_origin)}</code> {repo_link}</p>
    </header>
    <div class="grid">
{chr(10).join(cards)}
    </div>
    <footer>
      <p>首次从官方固件切换：请在 GitHub Releases 下载对应板型的 <code>-TF.zip</code>，解压后将 <code>Pico-update.bin</code> / <code>Metalio-S31-update.bin</code> 放入 TF 卡根目录，在设备「设置 → 升级和恢复」完成一次 TF 卡升级。此后设备端联网检查更新将自动连接本页的 OTA 清单。</p>
    </footer>
  </main>
</body>
</html>
"""


def stage_ota_pages(
    output: Path,
    repo: str | None = None,
    ota_origin: str | None = None,
    retain: int | None = None,
    local_releases_dir: Path | None = None,
    require_tags: list[str] | None = None,
    root: Path = ROOT,
) -> dict:
    cfg = load_config(root)
    retain_count = retain if retain is not None else cfg["retain_versions"]
    if retain_count < 1:
        raise ValueError("retain must be >= 1")

    resolved_repo = resolve_github_repo(explicit_repo=repo, root=root) if (repo or local_releases_dir is None) else None
    if resolved_repo and resolved_repo.lower() == UPSTREAM_REPO.lower():
        raise ValueError(f"Refusing to stage fork pages for upstream repository {resolved_repo!r}")

    resolved_origin = validate_ota_origin(
        resolve_ota_origin(explicit=ota_origin, repo=resolved_repo, root=root, require_fork=True),
        require_fork=True,
    )

    required = [tag.strip() for tag in (require_tags or []) if tag and tag.strip()]

    if local_releases_dir is not None:
        local_dir = Path(local_releases_dir).resolve()
        releases = json.loads((local_dir / "releases.json").read_text(encoding="utf-8"))
        releases = [item for item in releases if not item.get("draft")]
        present = {str(item.get("tag_name", "")) for item in releases}
        missing = [tag for tag in required if tag not in present]
        if missing:
            raise RuntimeError(f"Missing required release tag(s): {', '.join(missing)}")
    else:
        local_dir = None
        if not resolved_repo:
            raise ValueError("GitHub repository slug is required to fetch Releases")
        releases = []
        for attempt in range(6):
            releases = fetch_github_releases(resolved_repo)
            present = {str(item.get("tag_name", "")) for item in releases}
            missing = [tag for tag in required if tag not in present]
            if not missing:
                break
            if attempt == 5:
                raise RuntimeError(
                    f"Timed out waiting for GitHub Releases API to expose tag(s): {', '.join(missing)}"
                )
            time.sleep(3)

    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    staged: dict[str, dict] = {}

    for board in BOARDS:
        spec = board_spec(board, root=root)
        prefix = spec["tag_prefix"]
        candidates = []
        for rel in releases:
            tag = str(rel.get("tag_name", ""))
            if not tag.startswith(prefix):
                continue
            ver = tag[len(prefix):]
            try:
                ver_tuple = parse_version_tuple(ver)
            except ValueError:
                continue
            candidates.append((ver_tuple, ver, rel))

        if not candidates:
            continue

        candidates.sort(key=lambda item: item[0], reverse=True)
        selected = candidates[:retain_count]
        retained_items: list[tuple[str, str]] = []
        latest_feed: dict | None = None

        for idx, (_, ver, rel) in enumerate(selected):
            feed_raw = _read_asset_bytes(rel, spec["feed_filename"], 8192, local_dir=local_dir)
            feed = json.loads(feed_raw.decode("utf-8"))
            validate_feed_dict(feed, board, resolved_origin, expected_version=ver, root=root)

            bin_name = f"{spec['update_prefix']}-{ver}.bin"
            bin_bytes = _read_asset_bytes(rel, bin_name, spec["slot_size"], local_dir=local_dir)
            if len(bin_bytes) != feed["size"]:
                raise ValueError(f"{bin_name} size {len(bin_bytes)} != feed size {feed['size']}")
            actual_sha = hashlib.sha256(bin_bytes).hexdigest()
            if actual_sha != feed["sha256"]:
                raise ValueError(f"{bin_name} sha256 {actual_sha} != feed sha256 {feed['sha256']}")

            validate_firmware_bytes(
                bin_bytes,
                board,
                expected_version=ver,
                ota_origin=resolved_origin,
                check_no_upstream=True,
                root=root,
            )

            (output / bin_name).write_bytes(bin_bytes)
            retained_items.append((ver, bin_name))
            if idx == 0:
                latest_feed = feed
                (output / spec["feed_filename"]).write_text(
                    json.dumps(feed, ensure_ascii=False, indent=2) + "\n",
                    encoding="utf-8",
                )

        assert latest_feed is not None
        staged[board] = {
            "spec": spec,
            "latest_feed": latest_feed,
            "retained": retained_items,
        }

    if not staged:
        raise RuntimeError("No valid fork OTA releases found for either pico or metalio47")

    (output / ".nojekyll").touch()
    (output / "index.html").write_text(_render_index_html(resolved_origin, resolved_repo, staged), encoding="utf-8")

    # Final staged site integrity verification.
    summary = {"ota_origin": resolved_origin, "repo": resolved_repo, "boards": {}}
    for board, info in staged.items():
        spec = info["spec"]
        feed_file = output / spec["feed_filename"]
        feed = json.loads(feed_file.read_text(encoding="utf-8"))
        validate_feed_dict(feed, board, resolved_origin, root=root)
        bin_name = feed["url"].rsplit("/", 1)[-1]
        bin_file = output / bin_name
        if not bin_file.is_file():
            raise FileNotFoundError(f"Staged site missing referenced binary {bin_name}")
        bin_bytes = bin_file.read_bytes()
        if len(bin_bytes) != feed["size"] or hashlib.sha256(bin_bytes).hexdigest() != feed["sha256"]:
            raise ValueError(f"Staged binary {bin_name} failed final integrity check")
        summary["boards"][board] = {
            "latest_version": feed["version"],
            "feed_file": spec["feed_filename"],
            "retained_bins": [name for _, name in info["retained"]],
        }
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Output directory for GitHub Pages artifact")
    parser.add_argument("--repo", default=None, help="GitHub owner/repo slug")
    parser.add_argument("--ota-origin", default=None, help="Fork OTA HTTPS base URL")
    parser.add_argument("--retain", type=int, default=None, help="Number of recent .bin releases to keep per board")
    parser.add_argument("--local-releases-dir", type=Path, default=None, help="Offline mock releases directory for testing")
    parser.add_argument("--require-tag", action="append", default=[], help="Release tag(s) that must be present in fetched Releases")
    args = parser.parse_args(argv)

    summary = stage_ota_pages(
        output=args.output,
        repo=args.repo,
        ota_origin=args.ota_origin,
        retain=args.retain,
        local_releases_dir=args.local_releases_dir,
        require_tags=args.require_tag,
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
