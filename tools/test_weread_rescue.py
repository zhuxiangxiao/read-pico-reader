#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Real production progress, independent durable emergency backend and cold processes."""
from pathlib import Path
import os,subprocess,tempfile
root=Path(__file__).resolve().parents[1]
build=Path(os.environ.get('WEREAD_TEST_BUILD_DIR',root/'build/weread-tests')).resolve()/'rescue';build.mkdir(parents=True,exist_ok=True)
store=(root/'main/weread/weread_store.c').read_text()
(build/'store.c').write_text(store.replace('esp_err_t wr_write_json(','esp_err_t actual_write_json(',1).replace('esp_err_t wr_read_json_primary(','esp_err_t actual_read_json_primary(',1))
args=[os.environ.get('CC','cc'),'-std=c11','-D_DEFAULT_SOURCE','-pthread','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
      '-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs',
      'tools/test_weread_rescue.c','main/weread/weread_progress.c','main/book/book_weread_crypto.c','main/weread/weread_shelf.c',
      str(build/'store.c'),'main/weread/weread_auth.c','main/weread/weread_client.c','tools/weread_stubs/cJSON.c','-lz','-lm','-o',str(build/'test')]
subprocess.run(args,cwd=root,check=True)
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
with tempfile.TemporaryDirectory(prefix='wr-rescue-card-') as card,tempfile.TemporaryDirectory(prefix='wr-rescue-nvs-') as nvs:
    for mode in ('produce','recover'):subprocess.run([str(build/'test'),card,nvs,mode],cwd=root,env=env,check=True,timeout=15)
for mode in ('ordinary','missing-read','internal-fail','ack-race','legacy-rescue','coalesce','blocked-report','foreign','different-card','different-root','full','changed-primary','choice','erase-fail','crc','version','restore-oom','restore-io','restore-timeout','restore-corrupt','restore-oom-once','restore-io-once','cid-get','cid-post'):
    with tempfile.TemporaryDirectory(prefix='wr-rescue-card-') as card,tempfile.TemporaryDirectory(prefix='wr-rescue-nvs-') as nvs:
        print(mode,flush=True);subprocess.run([str(build/'test'),card,nvs,mode],cwd=root,env=env,check=True,timeout=15)
print('PASS: emergency durability, cold process, bounded slots, identity/card, ACK race, primary CAS and TF/internal failures')
