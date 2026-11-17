#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Production progress outbox: restart, delayed responses, conflicts and failed commits."""
from pathlib import Path
import os
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
build=root/'build/night-optimization/outbox-tests'
build.mkdir(parents=True,exist_ok=True)
source=(root/'main/weread/weread_store.c').read_text()
(build/'store.c').write_text(source.replace('esp_err_t wr_write_json(', 'esp_err_t actual_write_json(',1))
args=['/usr/bin/cc','-std=c11','-D_DEFAULT_SOURCE','-pthread','-Wall','-Wextra','-Werror',
      '-Wno-deprecated-declarations','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',
      '-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs',
      'tools/test_weread_outbox.c','main/weread/weread_progress.c','main/book/book_weread_crypto.c',
      'main/weread/weread_shelf.c',str(build/'store.c'),'main/weread/weread_auth.c',
      'main/weread/weread_client.c','tools/weread_stubs/cJSON.c','-lz','-lm','-o',str(build/'test')]
subprocess.run(args,cwd=root,check=True)
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
with tempfile.TemporaryDirectory(prefix='weread-outbox-restart-') as cache:
    for mode in ('produce','recover'):
        subprocess.run([str(build/'test'),cache,mode],env=env,check=True,timeout=15)
for mode in ('merge','fetch-race','session','owner','media','cancel','conflict','version','unknown-version','reselect','backoff-cap','backoff','ack-write','local-write','accounts','archive-write','active-write','unsafe-owner','incomplete-cloud','corrupt-json','corrupt-owned','corrupt-both','corrupt-foreign','corrupt-previous','corrupt-missing','bound-old','bound-zero','bound-new','legacy'):
    with tempfile.TemporaryDirectory(prefix=f'weread-outbox-{mode}-') as cache:
        subprocess.run([str(build/'test'),cache,mode],env=env,check=True,timeout=15)
for allocation in range(500):
    with tempfile.TemporaryDirectory(prefix='weread-outbox-oom-') as cache:
        run=subprocess.run([str(build/'test'),cache,f'oom-{allocation}'],env=env,text=True,capture_output=True,timeout=15)
        if run.returncode:
            print(run.stdout,run.stderr);raise RuntimeError(f'outbox OOM allocation {allocation}: {run.returncode}')
print('PASS: 500 JSON allocation faults retain the foreign owner pending record; rejected commits keep active bytes unchanged')
for allocation in range(250):
    with tempfile.TemporaryDirectory(prefix='weread-outbox-load-oom-') as cache:
        run=subprocess.run([str(build/'test'),cache,f'load-oom-{allocation}'],env=env,text=True,capture_output=True,timeout=15)
        if run.returncode:
            print(run.stdout,run.stderr);raise RuntimeError(f'outbox load OOM allocation {allocation}: {run.returncode}')
print('PASS: 250 recovery allocation faults return a complete owned backup or a zero output; never a rejected partial position')
print('PASS: production outbox restart, nonblocking local store, ACK merge, owner/session, conflict/version, retries and commit failures')
