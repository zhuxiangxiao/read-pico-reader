#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise ESP cover failure metadata with real production functions."""
from pathlib import Path
import subprocess
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
out=root/'build/weread-tests/cover-cache'
out.mkdir(parents=True,exist_ok=True)
source=root/'main/weread/weread_runtime.c'
(out/'weread-cover-production.inc').write_text('\n'.join(function(name,source) for name in ('cover_job_impl','cover_job')))
binary=out/'test'
subprocess.run(['cc','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(out),str(root/'tools/test_weread_cover_cache.c'),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
