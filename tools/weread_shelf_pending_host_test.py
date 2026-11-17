#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
from pathlib import Path
import subprocess
root=Path(__file__).resolve().parents[1]
out=root/'build/weread-tests/shelf-pending/test'
out.parent.mkdir(parents=True,exist_ok=True)
args=['/usr/bin/cc','-pthread','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Imain','-Imain/weread','-Itools/weread_stubs','tools/test_weread_shelf_pending.c','main/weread/weread_store.c','main/weread/weread_auth.c','main/weread/weread_client.c','tools/weread_stubs/cJSON.c','-lm','-o',str(out)]
subprocess.run(args,cwd=root,check=True)
subprocess.run([str(out)],cwd=root,check=True)
