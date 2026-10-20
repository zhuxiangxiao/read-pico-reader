#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 中文：IDF 构建后的门禁：应用镜像必须放进 ota_0，且 AES 链接包装必须真的进镜像。
# English: Post-build gate: the app image must fit ota_0, and the AES linker wrappers
# must really be linked into the image.
#
# 用法 / Usage: python3 tools/check_airdrop_build.py [--build-dir build]
# 依赖 / Requires: xtensa-esp32s3-elf-nm on PATH (after exporting ESP-IDF), or set NM.
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WRAP_SYMBOLS = ("__wrap_esp_aes_crypt_cbc", "__wrap_esp_aes_crypt_ctr", "__wrap_esp_aes_crypt_ecb")


def partition_size(partitions_csv: Path, name: str) -> int:
    for raw in partitions_csv.read_text().splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) >= 5 and fields[0] == name:
            return int(fields[4], 16)
    raise SystemExit(f"partition {name!r} not found in {partitions_csv}")


def find_nm() -> str:
    nm = os.environ.get("NM") or shutil.which("xtensa-esp32s3-elf-nm")
    if not nm:
        raise SystemExit("xtensa-esp32s3-elf-nm not found; source export.sh or set NM")
    return nm


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", default=str(ROOT / "build"))
    parser.add_argument("--partition", default="ota_0")
    args = parser.parse_args()
    build = Path(args.build_dir)
    bin_path = build / "Read_Pico.bin"
    elf_path = build / "Read_Pico.elf"
    for path in (bin_path, elf_path):
        if not path.is_file():
            print(f"FAIL: missing build artifact {path}")
            return 1

    ota_size = partition_size(ROOT / "partitions_16M.csv", args.partition)
    app_size = bin_path.stat().st_size
    print(f"app image: {app_size} bytes; {args.partition}: {ota_size} bytes; "
          f"headroom: {ota_size - app_size} bytes")
    failed = False
    if app_size > ota_size:
        print(f"FAIL: app image exceeds {args.partition} by {app_size - ota_size} bytes")
        failed = True
    else:
        print(f"PASS: app image fits {args.partition}")

    nm = find_nm()
    out = subprocess.run([nm, str(elf_path)], check=False, capture_output=True, text=True)
    if out.returncode != 0:
        print(f"FAIL: nm failed: {out.stderr.strip()}")
        return 1
    missing = [s for s in WRAP_SYMBOLS if s not in out.stdout]
    if missing:
        print("FAIL: AES linker wrappers missing from the image: " + ", ".join(missing))
        failed = True
    else:
        print("PASS: AES linker wrappers present")

    print("airdrop build gate: " + ("FAILED" if failed else "PASS"))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
