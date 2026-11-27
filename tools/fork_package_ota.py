#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Package and verify a fork OTA release bundle from a built board package."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

from fork_ota_config import (
    BOARDS,
    BOARD_ALIASES,
    ROOT,
    board_spec,
    canonical_board,
    parse_version_tuple,
    resolve_ota_origin,
    validate_feed_dict,
    validate_firmware_bytes,
    validate_ota_origin,
)


def _git_cmd(args: list[str], repo_dir: Path) -> subprocess.CompletedProcess[str]:
    resolved = Path(repo_dir).resolve()
    return subprocess.run(
        ["git", "-c", f"safe.directory={resolved}", "-C", str(resolved), *args],
        capture_output=True,
        text=True,
        timeout=30,
        check=False,
    )


def extract_annotated_tag_notes(tag: str, repo_dir: Path = ROOT, fetch: bool = False) -> str:
    tag_name = (tag or "").strip()
    if not tag_name:
        return ""
    ref = f"refs/tags/{tag_name}"
    type_proc = _git_cmd(["cat-file", "-t", ref], repo_dir)
    obj_type = type_proc.stdout.strip() if type_proc.returncode == 0 else ""
    if obj_type != "tag" and fetch:
        _git_cmd(["fetch", "--no-tags", "--depth=1", "--force", "origin", f"{ref}:{ref}"], repo_dir)
        type_proc = _git_cmd(["cat-file", "-t", ref], repo_dir)
        obj_type = type_proc.stdout.strip() if type_proc.returncode == 0 else ""
    if obj_type != "tag":
        return ""
    msg_proc = _git_cmd(["for-each-ref", "--format=%(contents)", ref], repo_dir)
    if msg_proc.returncode != 0:
        return ""
    text = msg_proc.stdout.replace("\r\n", "\n").replace("\r", "\n")
    for marker in ("\n-----BEGIN PGP SIGNATURE-----", "\n-----BEGIN SSH SIGNATURE-----"):
        idx = text.find(marker)
        if idx >= 0:
            text = text[:idx]
            break
    return text.strip()


def normalize_notes(notes: str | None, base_version: str, version: str) -> str:
    text = (notes or "").replace("\r\n", "\n").replace("\r", "\n").strip()
    if not text:
        text = f"基于上游 {base_version} 的自建 OTA 版本 ({version})"
    if len(text.encode("utf-8")) >= 384:
        ellipsis = "…"
        budget = 383 - len(ellipsis.encode("utf-8"))
        trimmed = text.encode("utf-8")[:budget].decode("utf-8", errors="ignore").rstrip()
        text = f"{trimmed}{ellipsis}" if trimmed else version
    return text


def package_ota(
    board: str,
    build_dir: Path,
    version: str,
    output: Path,
    ota_origin: str | None = None,
    notes: str = "",
    root: Path = ROOT,
    tag: str = "",
    fetch_tag: bool = False,
    repo_dir: Path | None = None,
) -> dict:
    board = canonical_board(board)
    spec = board_spec(board, root=root)
    parse_version_tuple(version)
    origin = validate_ota_origin(
        resolve_ota_origin(explicit=ota_origin, root=root, require_fork=True),
        require_fork=True,
    )

    build_dir = Path(build_dir).resolve()
    packages_dir = build_dir / "packages"
    board_pkg = packages_dir / board
    firmware_path = board_pkg / "firmware.bin"
    if not firmware_path.is_file():
        raise FileNotFoundError(f"Built firmware not found: {firmware_path}")

    sdkconfig_h = build_dir / board / "build" / "config" / "sdkconfig.h"
    if sdkconfig_h.is_file():
        cfg_text = sdkconfig_h.read_text(encoding="utf-8", errors="replace")
        if "#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1" not in cfg_text:
            raise ValueError(f"{board} build does not enable CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE")

    data = firmware_path.read_bytes()
    validate_firmware_bytes(
        data,
        board,
        expected_version=version,
        ota_origin=origin,
        check_no_upstream=True,
        root=root,
    )

    sha256 = hashlib.sha256(data).hexdigest()
    size = len(data)
    raw_notes = (notes or "").strip()
    if not raw_notes and tag:
        raw_notes = extract_annotated_tag_notes(tag, repo_dir=repo_dir or root, fetch=fetch_tag)
    clean_notes = normalize_notes(raw_notes, spec["base_version"], version)

    bin_filename = f"{spec['update_prefix']}-{version}.bin"
    feed_filename = spec["feed_filename"]
    tf_zip_filename = f"kiikoread-{board}-{version}-TF.zip"

    feed = {
        "schema": 1,
        "version": version,
        "project": spec["project"],
        "board": spec["board_id"],
        "layout": spec["layout"],
        "minimum_base_version": spec["minimum_base_version"],
        "url": f"{origin}{bin_filename}",
        "sha256": sha256,
        "notes": clean_notes,
        "size": size,
    }
    validate_feed_dict(feed, board, origin, expected_version=version, root=root)

    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    bin_path = output / bin_filename
    feed_path = output / feed_filename
    bin_path.write_bytes(data)
    feed_path.write_text(json.dumps(feed, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    tf_zip_path = output / tf_zip_filename
    tf_readme = (
        f"{spec['name']} ({board}) TF 卡升级包 {version}\n"
        f"1. 将包内 {spec['tf_filename']} 复制到 TF 卡根目录。\n"
        f"2. 在设备「设置 -> 升级和恢复 / 系统升级」中选择 TF 卡升级。\n"
        f"3. 从官方固件首次切换后，后续可通过设备联网 OTA ({origin}) 直接升级。\n"
        f"4. 本文件为应用镜像，切勿直接烧录到 Flash 地址 0。\n"
    )
    with zipfile.ZipFile(tf_zip_path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(spec["tf_filename"], data)
        archive.writestr("SHA256SUMS.txt", f"{sha256}  {spec['tf_filename']}\n")
        archive.writestr("README.txt", tf_readme)

    usb_zip_name = f"kiikoread-{board}-{version}-USB.zip"
    usb_src = packages_dir / usb_zip_name
    copied_usb = None
    if usb_src.is_file():
        shutil.copy2(usb_src, output / usb_zip_name)
        copied_usb = usb_zip_name

    free_app_bytes = spec["slot_size"] - size
    verification_path = board_pkg / "verification.json"
    if verification_path.is_file():
        verification = json.loads(verification_path.read_text(encoding="utf-8"))
        if isinstance(verification.get("free_app_bytes"), int):
            free_app_bytes = verification["free_app_bytes"]

    metadata = {
        "schema": 1,
        "board": board,
        "name": spec["name"],
        "project": spec["project"],
        "version": version,
        "base_version": spec["base_version"],
        "tag": f"{spec['tag_prefix']}{version}",
        "ota_origin": origin,
        "feed_filename": feed_filename,
        "bin_filename": bin_filename,
        "tf_zip_filename": tf_zip_filename,
        "usb_zip_filename": copied_usb,
        "size": size,
        "sha256": sha256,
        "free_app_bytes": free_app_bytes,
        "notes": clean_notes,
    }
    (output / "ota-metadata.json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    # Re-verify written feed and binary together.
    written_feed = json.loads(feed_path.read_text(encoding="utf-8"))
    validate_feed_dict(written_feed, board, origin, expected_version=version, root=root)
    written_bin = bin_path.read_bytes()
    if len(written_bin) != written_feed["size"] or hashlib.sha256(written_bin).hexdigest() != written_feed["sha256"]:
        raise ValueError("Packaged OTA feed and binary integrity mismatch")
    validate_firmware_bytes(written_bin, board, expected_version=version, ota_origin=origin, check_no_upstream=True, root=root)
    return metadata


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--board", choices=(*BOARDS, *BOARD_ALIASES), required=True)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/boards")
    parser.add_argument("--version", required=True)
    parser.add_argument("--ota-origin", default=None)
    parser.add_argument("--notes", default="")
    parser.add_argument("--tag", default="")
    parser.add_argument("--fetch-tag", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args(argv)

    meta = package_ota(
        board=args.board,
        build_dir=args.build_dir,
        version=args.version,
        output=args.output,
        ota_origin=args.ota_origin,
        notes=args.notes,
        tag=args.tag,
        fetch_tag=args.fetch_tag,
    )
    print(json.dumps(meta, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
