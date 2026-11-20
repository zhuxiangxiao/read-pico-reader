#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Host regression tests for fork OTA config, CMake header generation, packaging, Pages staging, and upstream sync."""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
import fork_ota_config as cfg
from fork_package_ota import normalize_notes, package_ota
from fork_stage_ota_pages import stage_ota_pages
from fork_sync_upstream import inspect_upstream, merge_upstream

ROOT = Path(__file__).resolve().parents[1]
FORK_ORIGIN = "https://example-fork.github.io/read-pico-reader/"


def make_fake_firmware(
    board: str,
    version: str,
    ota_origin: str,
    extra_bytes: bytes = b"",
    size_override: int | None = None,
) -> bytes:
    spec = cfg.board_spec(board, root=ROOT)
    buf = bytearray(max(4096, size_override or 4096))
    buf[0] = 0xE9
    struct.pack_into("<H", buf, 12, spec["image_chip_id"])
    struct.pack_into("<I", buf, 32, 0xABCD5432)
    ver_bytes = version.encode("ascii")
    proj_bytes = spec["project"].encode("ascii")
    buf[48:48 + len(ver_bytes)] = ver_bytes
    buf[80:80 + len(proj_bytes)] = proj_bytes
    trailer = (
        spec["capability"].encode("ascii")
        + b"\0"
        + f"{ota_origin}{spec['feed_filename']}".encode("ascii")
        + b"\0"
        + extra_bytes
    )
    assert 300 + len(trailer) <= len(buf)
    buf[300:300 + len(trailer)] = trailer
    return bytes(buf)


def test_config_and_versions() -> None:
    assert cfg.parse_github_slug("https://github.com/Alice/read-pico-reader.git") == "Alice/read-pico-reader"
    assert cfg.parse_github_slug("git@github.com:Alice/read-pico-reader.git") == "Alice/read-pico-reader"
    assert cfg.parse_github_slug("ssh://git@github.com/Alice/read-pico-reader/") == "Alice/read-pico-reader"
    assert cfg.parse_github_slug("https://gitlab.com/Alice/repo.git") is None

    assert cfg.pages_origin_for_repo("Alice/read-pico-reader") == "https://alice.github.io/read-pico-reader/"
    assert cfg.pages_origin_for_repo("Alice/alice.github.io") == "https://alice.github.io/"

    assert cfg.validate_ota_origin("https://alice.github.io/read-pico-reader") == "https://alice.github.io/read-pico-reader/"
    for bad in (
        "http://alice.github.io/read-pico-reader/",
        "https://alice.github.io/read-pico-reader/../evil/",
        "https://alice.github.io/read-pico-reader/?a=1",
        "https://user@alice.github.io/read-pico-reader/",
        "https://alice.github.io:8443/read-pico-reader/",
    ):
        try:
            cfg.validate_ota_origin(bad, require_fork=True)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Accepted invalid origin: {bad}")

    for upstream in ("https://wegooo-cell.github.io/read-pico-reader/", "https://kiikoread.com/"):
        try:
            cfg.validate_ota_origin(upstream, require_fork=True)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Accepted upstream origin in fork mode: {upstream}")

    ordered = [
        "0.3.3-rc108",
        "0.3.3-rc108.1",
        "0.3.3-rc108.2",
        "0.3.3-rc108.10",
        "0.3.3-rc109",
        "0.3.3-rc109.1",
        "0.3.3",
        "0.3.3-r1",
        "0.3.3-r2",
        "0.3.4-rc1",
    ]
    tuples = [cfg.parse_version_tuple(v) for v in ordered]
    assert tuples == sorted(tuples) and len(set(tuples)) == len(tuples)

    for bad_ver in ("0.3.3-rc108.0", "0.3.3-r0", "0.3.3-rc108.", "0.3.3-foo", "0.3.3-rc108.70000", "0.3.3-r70000"):
        try:
            cfg.parse_version_tuple(bad_ver)
        except ValueError:
            pass
        else:
            raise AssertionError(f"Accepted invalid version: {bad_ver}")

    assert cfg.format_fork_version("0.3.3-rc108", 1) == "0.3.3-rc108.1"
    assert cfg.format_fork_version("0.3.3", 2) == "0.3.3-r2"
    assert cfg.next_revision_for_base("0.3.3-rc108", ["0.3.3-rc107.4", "0.3.3-rc108.1", "0.3.3-rc108.2"]) == 3
    assert cfg.next_revision_for_base("0.3.3-rc109", ["0.3.3-rc108.1", "0.3.3-rc108.2"]) == 1
    assert cfg.next_revision_for_base("0.3.3", ["0.3.3-rc109.2", "0.3.3-r1"]) == 2

    plan = cfg.plan_release(
        board="all",
        repo="example-fork/read-pico-reader",
        releases=[
            {"tag_name": "ota-pico-v0.3.3-rc108.1", "draft": False},
            {"tag_name": "ota-s31-v0.3.3-rc114.2", "draft": False},
        ],
    )
    assert plan["ota_origin"] == FORK_ORIGIN
    by_board = {entry["board"]: entry for entry in plan["boards"]}
    assert by_board["pico"]["version"] == "0.3.3-rc108.2"
    assert by_board["metalio47"]["version"] == "0.3.3-rc114.3"

    try:
        cfg.plan_release(
            board="pico",
            revision="1",
            repo="example-fork/read-pico-reader",
            releases=[{"tag_name": "ota-pico-v0.3.3-rc108.1", "draft": False}],
        )
    except ValueError:
        pass
    else:
        raise AssertionError("Allowed duplicate release tag")


def test_cmake_header_generation() -> None:
    cmake_exe = shutil.which("cmake")
    cc_exe = shutil.which("cc")
    assert cmake_exe and cc_exe, "cmake and cc are required"
    cmake_text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    begin = cmake_text.index("set(KIIKO_EFFECTIVE_OTA_ORIGIN")
    end = cmake_text.index("project(${KIIKO_PROJECT_NAME})")
    snippet = cmake_text[begin:end]

    with tempfile.TemporaryDirectory(prefix="kiiko-cmake-ota-") as td:
        tmp = Path(td)
        script = tmp / "check.cmake"
        build_dir = tmp / "build"
        script.write_text(f'set(CMAKE_BINARY_DIR "{build_dir.as_posix()}")\n' + snippet, encoding="utf-8")

        subprocess.run([cmake_exe, f"-DKIIKO_OTA_ORIGIN={FORK_ORIGIN}", "-P", str(script)], check=True)
        header = build_dir / "kiiko_fork_inc/kiiko_ota_origin_gen.h"
        assert header.is_file()
        assert f'#define KIIKO_OTA_ORIGIN "{FORK_ORIGIN}"' in header.read_text(encoding="utf-8")

        c_src = tmp / "check_header.c"
        c_src.write_text(
            '#include <assert.h>\n#include <string.h>\n#define KIIKO_BOARD_METALIO47 0\n'
            '#include "ota_online.h"\n'
            'int main(void){assert(!strcmp(PICO_OTA_FEED_URL, "' + FORK_ORIGIN + 'update.json"));'
            'assert(!strcmp(PICO_OTA_FEED_FALLBACK_URL, ""));return 0;}\n',
            encoding="utf-8",
        )
        c_bin = tmp / "check_header"
        subprocess.run(
            [cc_exe, "-Wall", "-Wextra", "-Werror", "-I", str(build_dir / "kiiko_fork_inc"),
             "-I", str(ROOT / "main"), "-I", str(ROOT / "tools/ota_stubs"), str(c_src), "-o", str(c_bin)],
            check=True,
        )
        subprocess.run([str(c_bin)], check=True)

        # Clearing KIIKO_OTA_ORIGIN removes stale generated header.
        env = dict(os.environ)
        env.pop("KIIKO_OTA_ORIGIN", None)
        subprocess.run([cmake_exe, "-P", str(script)], env=env, check=True)
        assert not header.exists()

        # Invalid origins fail CMake configure.
        for bad in ("http://example.com/", "https://example.com/../a/", "https://user@example.com/"):
            proc = subprocess.run(
                [cmake_exe, f"-DKIIKO_OTA_ORIGIN={bad}", "-P", str(script)],
                capture_output=True,
                text=True,
            )
            assert proc.returncode != 0, f"CMake accepted invalid origin: {bad}"


def test_package_and_pages_staging() -> None:
    with tempfile.TemporaryDirectory(prefix="kiiko-fork-pkg-") as td:
        tmp = Path(td)
        build_dir = tmp / "build/boards"
        for board, ver in (("pico", "0.3.3-rc108.1"), ("metalio47", "0.3.3-rc114.1")):
            pkg_dir = build_dir / "packages" / board
            pkg_dir.mkdir(parents=True)
            (build_dir / board / "build/config").mkdir(parents=True)
            (build_dir / board / "build/config/sdkconfig.h").write_text(
                "#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1\n", encoding="utf-8"
            )
            fw = make_fake_firmware(board, ver, FORK_ORIGIN)
            (pkg_dir / "firmware.bin").write_bytes(fw)
            (pkg_dir / "verification.json").write_text(json.dumps({"free_app_bytes": 12345}), encoding="utf-8")
            with zipfile.ZipFile(build_dir / "packages" / f"kiikoread-{board}-{ver}-USB.zip", "w") as zf:
                zf.writestr("firmware.bin", fw)

            out_dir = tmp / "dist" / board
            long_notes = "测试中文更新日志：" + ("微信读书优化与修复；" * 30)
            meta = package_ota(board, build_dir, ver, out_dir, ota_origin=FORK_ORIGIN, notes=long_notes, root=ROOT)
            assert len(meta["notes"].encode("utf-8")) < 384
            assert meta["notes"].endswith("…")
            assert (out_dir / meta["bin_filename"]).is_file()
            assert (out_dir / meta["feed_filename"]).is_file()
            assert (out_dir / meta["tf_zip_filename"]).is_file()
            assert (out_dir / meta["usb_zip_filename"]).is_file()

        # Verify UTF-8 note boundary helper directly.
        bounded = normalize_notes("汉" * 200, "0.3.3-rc108", "0.3.3-rc108.1")
        assert 0 < len(bounded.encode("utf-8")) < 384

        # Reject firmware containing upstream URL.
        bad_pkg = tmp / "bad/build/boards"
        (bad_pkg / "packages/pico").mkdir(parents=True)
        (bad_pkg / "packages/pico/firmware.bin").write_bytes(
            make_fake_firmware("pico", "0.3.3-rc108.1", FORK_ORIGIN, extra_bytes=b"https://kiikoread.com/update.json\0")
        )
        try:
            package_ota("pico", bad_pkg, "0.3.3-rc108.1", tmp / "bad-out", ota_origin=FORK_ORIGIN, root=ROOT)
        except ValueError:
            pass
        else:
            raise AssertionError("Packager accepted firmware containing upstream URL")

        # Reject missing rollback flag.
        no_rb = tmp / "norb/build/boards"
        (no_rb / "packages/pico").mkdir(parents=True)
        (no_rb / "pico/build/config").mkdir(parents=True)
        (no_rb / "pico/build/config/sdkconfig.h").write_text("#define CONFIG_OTHER 1\n", encoding="utf-8")
        (no_rb / "packages/pico/firmware.bin").write_bytes(make_fake_firmware("pico", "0.3.3-rc108.1", FORK_ORIGIN))
        try:
            package_ota("pico", no_rb, "0.3.3-rc108.1", tmp / "norb-out", ota_origin=FORK_ORIGIN, root=ROOT)
        except ValueError:
            pass
        else:
            raise AssertionError("Packager accepted build without rollback enabled")

        # Stage Pages from local mock releases (4 Pico + 2 Metalio -> retain 3 Pico + 2 Metalio).
        releases_root = tmp / "mock-releases"
        releases_list = []
        for board, versions in (
            ("pico", ["0.3.3-rc108.1", "0.3.3-rc108.2", "0.3.3-rc108.3", "0.3.3-rc108.4"]),
            ("metalio47", ["0.3.3-rc114.1", "0.3.3-rc114.2"]),
        ):
            spec = cfg.board_spec(board, root=ROOT)
            for ver in versions:
                tag = spec["tag_prefix"] + ver
                bdir = tmp / f"rel-{board}-{ver}/build/boards"
                (bdir / "packages" / board).mkdir(parents=True)
                (bdir / "packages" / board / "firmware.bin").write_bytes(make_fake_firmware(board, ver, FORK_ORIGIN))
                asset_dir = releases_root / "assets" / tag
                meta = package_ota(board, bdir, ver, asset_dir, ota_origin=FORK_ORIGIN, notes=f"Release {ver}", root=ROOT)
                releases_list.append({
                    "tag_name": tag,
                    "draft": False,
                    "assets": [
                        {"name": meta["bin_filename"]},
                        {"name": meta["feed_filename"]},
                    ],
                })
        (releases_root / "releases.json").write_text(json.dumps(releases_list), encoding="utf-8")

        site = tmp / "_site"
        summary = stage_ota_pages(
            site,
            ota_origin=FORK_ORIGIN,
            retain=3,
            local_releases_dir=releases_root,
            require_tags=["ota-pico-v0.3.3-rc108.4", "ota-s31-v0.3.3-rc114.2"],
            root=ROOT,
        )
        assert summary["boards"]["pico"]["latest_version"] == "0.3.3-rc108.4"
        assert summary["boards"]["metalio47"]["latest_version"] == "0.3.3-rc114.2"
        assert not (site / "Pico-update-0.3.3-rc108.1.bin").exists()
        for retained in ("0.3.3-rc108.2", "0.3.3-rc108.3", "0.3.3-rc108.4"):
            assert (site / f"Pico-update-{retained}.bin").is_file()
        for retained in ("0.3.3-rc114.1", "0.3.3-rc114.2"):
            assert (site / f"Metalio-S31-update-{retained}.bin").is_file()
        assert (site / ".nojekyll").is_file()
        assert "0.3.3-rc108.4" in (site / "index.html").read_text(encoding="utf-8")

        try:
            stage_ota_pages(
                tmp / "_site_missing_tag",
                ota_origin=FORK_ORIGIN,
                retain=3,
                local_releases_dir=releases_root,
                require_tags=["ota-pico-v0.3.3-rc999.1"],
                root=ROOT,
            )
        except RuntimeError:
            pass
        else:
            raise AssertionError("Pages staging did not reject missing required tag")

        # Single-board staging still works when only one board has Releases.
        pico_only = tmp / "mock-pico-only"
        shutil.copytree(releases_root / "assets", pico_only / "assets")
        (pico_only / "releases.json").write_text(
            json.dumps([r for r in releases_list if r["tag_name"].startswith("ota-pico-v")]),
            encoding="utf-8",
        )
        site_pico = tmp / "_site_pico"
        summary_pico = stage_ota_pages(site_pico, ota_origin=FORK_ORIGIN, retain=2, local_releases_dir=pico_only, root=ROOT)
        assert list(summary_pico["boards"]) == ["pico"]

        # Tampered binary fails staging checksum check.
        tamper_bin = releases_root / "assets/ota-pico-v0.3.3-rc108.4/Pico-update-0.3.3-rc108.4.bin"
        raw_bin = bytearray(tamper_bin.read_bytes())
        raw_bin[-1] ^= 0xFF
        tamper_bin.write_bytes(bytes(raw_bin))
        try:
            stage_ota_pages(tmp / "_site_bad", ota_origin=FORK_ORIGIN, retain=3, local_releases_dir=releases_root, root=ROOT)
        except ValueError:
            pass
        else:
            raise AssertionError("Pages staging accepted tampered binary")


def test_sync_upstream_git() -> None:
    with tempfile.TemporaryDirectory(prefix="kiiko-sync-test-") as td:
        tmp = Path(td)
        upstream_bare = tmp / "upstream.git"
        work_up = tmp / "work-up"
        fork_repo = tmp / "fork"

        subprocess.run(["git", "init", "--bare", "-b", "main", str(upstream_bare)], check=True, capture_output=True)
        subprocess.run(["git", "clone", str(upstream_bare), str(work_up)], check=True, capture_output=True)
        for repo_path in (work_up,):
            subprocess.run(["git", "-C", str(repo_path), "config", "user.name", "Test"], check=True)
            subprocess.run(["git", "-C", str(repo_path), "config", "user.email", "test@example.com"], check=True)

        (work_up / "boards/pico").mkdir(parents=True)
        (work_up / "boards/metalio47").mkdir(parents=True)
        (work_up / "main").mkdir(parents=True)
        (work_up / "boards/pico/board.json").write_text(json.dumps({"id": "pico", "version": "0.3.3-rc108"}), encoding="utf-8")
        (work_up / "boards/metalio47/board.json").write_text(json.dumps({"id": "metalio47", "version": "0.3.3-rc114"}), encoding="utf-8")
        (work_up / "main/ota_online.h").write_text("// upstream v1\n", encoding="utf-8")
        (work_up / "fork_ota.json").write_text(json.dumps({
            "schema": 1,
            "upstream_repo": "wegooo-cell/read-pico-reader",
            "upstream_remote": "upstream",
            "upstream_branch": "main",
        }), encoding="utf-8")
        subprocess.run(["git", "-C", str(work_up), "add", "."], check=True)
        subprocess.run(["git", "-C", str(work_up), "commit", "-m", "init"], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(work_up), "push", "origin", "main"], check=True, capture_output=True)

        subprocess.run(["git", "clone", str(upstream_bare), str(fork_repo)], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(fork_repo), "config", "user.name", "Fork"], check=True)
        subprocess.run(["git", "-C", str(fork_repo), "config", "user.email", "fork@example.com"], check=True)
        subprocess.run(["git", "-C", str(fork_repo), "remote", "add", "upstream", str(upstream_bare)], check=True)
        (fork_repo / "README.fork.md").write_text("fork\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(fork_repo), "add", "."], check=True)
        subprocess.run(["git", "-C", str(fork_repo), "commit", "-m", "fork change"], check=True, capture_output=True)

        # Advance upstream with a board version bump and a touchpoint change.
        (work_up / "boards/pico/board.json").write_text(json.dumps({"id": "pico", "version": "0.3.3-rc109"}), encoding="utf-8")
        (work_up / "main/ota_online.h").write_text("// upstream v2\n", encoding="utf-8")
        subprocess.run(["git", "-C", str(work_up), "add", "."], check=True)
        subprocess.run(["git", "-C", str(work_up), "commit", "-m", "upstream rc109"], check=True, capture_output=True)
        subprocess.run(["git", "-C", str(work_up), "push", "origin", "main"], check=True, capture_output=True)

        report = inspect_upstream(repo_dir=fork_repo, fetch=True)
        assert report["new_commits"] == 1
        assert report["versions"]["pico"] == {"current": "0.3.3-rc108", "upstream": "0.3.3-rc109"}
        assert "main/ota_online.h" in report["ota_touchpoint_updates"]

        code, merged = merge_upstream(repo_dir=fork_repo, fetch=False)
        assert code == 0 and merged["merged"] is True
        assert json.loads((fork_repo / "boards/pico/board.json").read_text(encoding="utf-8"))["version"] == "0.3.3-rc109"


def test_build_firmware_cli_dry_run() -> None:
    with tempfile.TemporaryDirectory(prefix="kiiko-bf-dry-") as td:
        sdk = Path(td) / "idf"
        (sdk / "tools").mkdir(parents=True)
        (sdk / "tools/idf.py").write_text("")
        (sdk / "components/soc/esp32s3").mkdir(parents=True)
        ok = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/build_firmware.py"),
                "--board", "pico",
                "--idf-pico", str(sdk),
                "--version", "0.3.3-rc108.1",
                "--ota-origin", FORK_ORIGIN,
                "--require-fork-ota",
                "--dry-run",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        data = json.loads(ok.stdout)
        assert data["version"] == "0.3.3-rc108.1"
        assert data["ota_origin"] == FORK_ORIGIN

        bad = subprocess.run(
            [
                sys.executable,
                str(ROOT / "tools/build_firmware.py"),
                "--board", "pico",
                "--idf-pico", str(sdk),
                "--ota-origin", "https://wegooo-cell.github.io/read-pico-reader/",
                "--require-fork-ota",
                "--dry-run",
            ],
            capture_output=True,
            text=True,
        )
        assert bad.returncode != 0


def main() -> int:
    test_config_and_versions()
    test_cmake_header_generation()
    test_package_and_pages_staging()
    test_sync_upstream_git()
    test_build_firmware_cli_dry_run()
    print("PASS: fork OTA config, CMake origin header, packaging, Pages staging, upstream sync, and build CLI")
    return 0


if __name__ == "__main__":
    sys.exit(main())
