#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Compare isolated old/current production readers under one controlled pre-socket wait model."""
from pathlib import Path
import hashlib,json,math,os,runpy,subprocess,tempfile

root=Path(__file__).resolve().parents[1]
out=root/'build/night-optimization/cached-prepare-benchmark';out.mkdir(parents=True,exist_ok=True)
baseline='68b378c73fae1000f52b6ee6344a06d228ab5a99'
sources=['main/book/book_weread_crypto.c','main/book/book_source_weread.c','main/book/html_text.c',
         *[f'main/weread/weread_{n}.c' for n in ('runtime','chapter','position','progress','shelf','toc','store','auth','client')],
         'tools/weread_stubs/cJSON.c']
headers=subprocess.check_output(['git','ls-files','main/book/*.h','main/weread/*.h','tools/weread_stubs/*.h'],cwd=root,text=True).splitlines()
paths=sorted(set(sources+headers))
harness=(root/'tools/test_weread_cached_prepare_benchmark.c').read_bytes()
current={name:(root/name).read_bytes() for name in paths}
assert all((root/name).read_bytes()==data for name,data in current.items()),'sources changed during snapshot'
fixtures=out/'fixtures';fixtures.mkdir(exist_ok=True)
generator=runpy.run_path(str(root/'tools/fixtures/weread/generate_fixtures.py'))
for uid in (11,27):
    content={'info.txt':json.dumps({'bookId':'bench_book','bookVersion':100,'chapters':[{'chapterUid':uid,'files':['Text/main.xhtml']}]}).encode(),
             'Text/main.xhtml':f'<html><body><p>Benchmark chapter {uid} content</p></body></html>'.encode()}
    (fixtures/f'{uid}.zip').write_bytes(generator['_make_zipcrypto_zip'](content,b'test_password_123'))
report={'baseline_commit':baseline,'current_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),
        'model':'Same production reader_prepare API on an already READY other chapter during NOTE POST. Transport deliberately ignores cancellation for1000ms before returning; no real socket/TLS or ink display is measured.',
        'compiler':subprocess.check_output(['cc','--version'],text=True).splitlines()[0],
        'instrumentation':'ASan/UBSan, pthread production worker, real filesystem commits, real encrypted chapter fixtures, n=10 each',
        'harness_sha256':hashlib.sha256(harness).hexdigest(),'variants':{}}
env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1:halt_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
for label in ('baseline','current'):
    folder=out/label;folder.mkdir(exist_ok=True);digest=hashlib.sha256();file_hashes={}
    for name in paths:
        data=current[name] if label=='current' else subprocess.check_output(['git','show',f'{baseline}:{name}'],cwd=root)
        target=folder/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
        digest.update(name.encode()+b'\0'+data);file_hashes[name]=hashlib.sha256(data).hexdigest()
    (folder/'benchmark.c').write_bytes(harness)
    flags=['-DWEREAD_BENCH_CURRENT'] if label=='current' else []
    subprocess.run(['cc','-std=c11','-pthread','-D_DEFAULT_SOURCE','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
        '-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*flags,
        '-Imain','-Imain/book','-Imain/weread','-Itools/weread_stubs','benchmark.c',*sources,'-lz','-lm','-o','benchmark'],cwd=folder,check=True)
    with tempfile.TemporaryDirectory(prefix=f'weread-benchmark-{label}-') as cache:
        result=subprocess.run([str(folder/'benchmark'),cache,str(fixtures)],cwd=folder,env=env,text=True,capture_output=True,timeout=45)
    (folder/'run.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0,(label,result.stdout,result.stderr)
    rows=[]
    for line in result.stdout.splitlines():
        if line.startswith('SAMPLE '):
            _,sample,microseconds,status,valid,raw=line.split()
            rows.append({'sample':int(sample),'microseconds':int(microseconds),'status':int(status),'valid_position':bool(int(valid)),'raw_offset':int(raw)})
    assert len(rows)==10 and all(r['status']==0 and r['valid_position'] for r in rows)
    values=sorted(r['microseconds']/1000 for r in rows)
    report['variants'][label]={'source_snapshot_sha256':digest.hexdigest(),'source_files':file_hashes,'samples':rows,
        'n':len(rows),'failures':sum(r['status']!=0 or not r['valid_position'] for r in rows),
        'p50_ms':(values[4]+values[5])/2,'p95_ms':values[math.ceil(.95*len(values))-1]}
assert [r['raw_offset'] for r in report['variants']['baseline']['samples']]==[r['raw_offset'] for r in report['variants']['current']['samples']]
(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
for label,value in report['variants'].items():print(f'{label}: n={value["n"]} failures={value["failures"]} p50={value["p50_ms"]:.3f}ms p95={value["p95_ms"]:.3f}ms')
print('Controlled host pre-socket model only; not ESP hardware, real network, display time, or a claim of1000ms device improvement.')
