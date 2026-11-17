# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
from pathlib import Path
import subprocess
root=next(p for p in Path(__file__).resolve().parents if (p/'main/weread').is_dir() and (p/'tools/weread_stubs').is_dir())
source=Path(__file__).with_name('test_weread_shelf_noop.c');binary=root/'build/weread-tests/shelf-noop/test';binary.parent.mkdir(parents=True,exist_ok=True)
args=['/usr/bin/cc','-pthread','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Imain','-Imain/weread','-Itools/weread_stubs',str(source),'main/weread/weread_auth.c','main/weread/weread_client.c','tools/weread_stubs/cJSON.c','-lm','-o',str(binary)]
subprocess.run(args,cwd=root,check=True);subprocess.run([str(binary)],cwd=root,check=True)
