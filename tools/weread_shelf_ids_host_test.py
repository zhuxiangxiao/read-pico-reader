#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
from pathlib import Path
import json,os,subprocess
root=Path(__file__).resolve().parents[1];out=root/'build/weread-tests/shelf-ids'
out.mkdir(parents=True,exist_ok=True)
s=(root/'main/weread/weread_shelf.c').read_text().replace('while(seen[slot]&&strcmp(seen[slot],id))','while((++probes,seen[slot])&&strcmp(seen[slot],id))')
assert '++probes' in s
(out/'shelf_under_test.inc').write_text(s)
collisions=[]
for i in range(2000000):
    key=f'collision_{i}';h=2166136261
    for ch in key.encode():h=((h^ch)*16777619)&0xffffffff
    if h&2047==2047:collisions.append(key)
    if len(collisions)==80:break
assert len(collisions)==80
(out/'collisions.json').write_text(json.dumps({'synckey':5,'bookIds':collisions+collisions[::-1]}))
args=[os.environ.get('CC','cc'),'-pthread','-std=c11','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Imain','-Imain/weread','-Itools/weread_stubs','-I'+str(out),str(root/'tools/test_weread_shelf_ids.c'),'main/weread/weread_store.c','main/weread/weread_auth.c','main/weread/weread_client.c','tools/weread_stubs/cJSON.c','-lm','-o',str(out/'hash_probe')]
subprocess.run(args,cwd=root,check=True);subprocess.run([str(out/'hash_probe')],cwd=root,check=True)
