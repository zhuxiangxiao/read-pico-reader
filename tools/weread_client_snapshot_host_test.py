#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Production-client snapshots release the service lock and reject stale credentials."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
out = root / "build/night-optimization/client-snapshot"
out.mkdir(parents=True, exist_ok=True)
binary = out / "probe"
subprocess.run([
    os.environ.get("CC", "/usr/bin/cc"), "-std=c11", "-pthread", "-D_DEFAULT_SOURCE",
    "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-declarations", "-g",
    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
    "-Imain/weread", "-Itools/weread_stubs", "tools/test_weread_client_snapshot.c",
    *[f"main/weread/weread_{name}.c" for name in ("auth", "store", "client")],
    "tools/weread_stubs/cJSON.c", "-lm", "-o", str(binary),
], cwd=root, check=True)
for mode in range(10):
    with tempfile.TemporaryDirectory(prefix="weread-client-snapshot-") as cache:
        subprocess.run([str(binary), cache, str(mode)], check=True, timeout=8,
                       env=dict(os.environ, ASAN_OPTIONS="abort_on_error=1:halt_on_error=1",
                                UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"))
print("PASS: concurrent local persistence,401 retention,token/account/media changes and cancellation")
