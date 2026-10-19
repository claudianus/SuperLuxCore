"""Check real OpenPBR boundary transport with disposable native probe binaries.

Requires the current Release libraries. Tests physical values, conditional PDF
consistency and unbiased sample moments; the legacy proposal includes the fixed
TIR endpoint density. Failed historical receipts remain separate evidence.
"""
import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

p=argparse.ArgumentParser()
p.add_argument('--out',type=Path,required=True)
p.add_argument('--samples',type=int,default=262144)
a=p.parse_args();assert a.samples>=65536
root=Path(__file__).resolve().parents[1];build=root/'out/build';a.out.mkdir(parents=True,exist_ok=True)
commands=subprocess.check_output(['ninja','-C',str(build),'-f','build-Release.ninja','-t','commands','luxcoreconsole'],text=True).splitlines()
oldsrc=str(root/'samples/luxcoreconsole/luxcoreconsole.cpp')
oldobj='samples/luxcoreconsole/CMakeFiles/luxcoreconsole.dir/Release/luxcoreconsole.cpp.o'
oldbin='samples/luxcoreconsole/Release/luxcoreconsole'
compile=shlex.split(next(x for x in commands if '-c '+oldsrc in x))
link=shlex.split(next(x for x in commands if '-o '+oldbin+' ' in x))
while link[0] in (':','&&'):link.pop(0)
if '&&' in link:link=link[:link.index('&&')]
internal=shlex.split(next(x for x in commands if '-c '+str(root/'src/slg/materials/openpbr.cpp') in x))
extra=[]
for i,x in enumerate(internal):
    if x in ('-I','-isystem'):extra.extend([x,internal[i+1]])
    elif x.startswith('-I') and len(x)>2:extra.append(x)
source=root/'dev-tools/sss_boundary_probe.cpp'
ident=dict(source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),libraries={x:hashlib.file_digest((build/x).open('rb'),'sha256').hexdigest() for x in link if x.endswith('.a') and x.startswith('src/')})
rows={};processes=[]
with tempfile.TemporaryDirectory(prefix='slc-sss-boundary-') as tmp:
    scratch=Path(tmp);obj=str(scratch/'probe.o');binary=str(scratch/'probe')
    compile=[str(source) if x==oldsrc else obj if x==oldobj else obj+'.d' if x==oldobj+'.d' else x for x in compile]+extra
    link=[obj if x==oldobj else binary if x==oldbin else x for x in link]
    with (a.out/'build.log').open('w') as log:
        for cmd in (compile,link):subprocess.run(cmd,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    for mode in ('legacy','coupled'):
        env=os.environ.copy();env.pop('SUPERLUXCORE_SSS_LEGACY_BOUNDARY',None)
        if mode=='legacy':env['SUPERLUXCORE_SSS_LEGACY_BOUNDARY']='1'
        output=(a.out/(mode+'.csv')).resolve()
        with (a.out/(mode+'.log')).open('w') as log:
            child=subprocess.Popen([binary,str(output),str(a.samples)],env=env,stdout=log,stderr=subprocess.STDOUT)
            try:code=child.wait(timeout=180)
            finally:
                if child.poll() is None:child.kill();child.wait(timeout=10)
        processes.append(dict(mode=mode,pid=child.pid,reaped=True,returncode=code));assert code==0
        rows[mode]=list(csv.DictReader(output.open()))
assert len(rows['legacy'])==len(rows['coupled'])==48
scores=[]
for x,y in zip(rows['legacy'],rows['coupled']):
    assert [x[k] for k in ('rough','side','cosine','from_light')]==[y[k] for k in ('rough','side','cosine','from_light')]
    assert x['physical_sum']==y['physical_sum'],'Scattering law changed'
    for row in (x,y):
        assert float(row['pdf_error'])<1e-6,row
        assert float(row['reverse_error'])<1e-6,row
    score=abs(float(x['mean'])-float(y['mean']))/max(1e-12,math.hypot(float(x['stderr']),float(y['stderr'])))
    scores.append(score)
result=dict(passed=max(scores)<5.5,cases=48,samples_each=a.samples,physical_values_exact=True,max_mean_difference_combined_stderr=max(scores),pdf_error=0,reverse_error=0,processes=processes,identity=ident,scope='Finite IID moment check with estimated standard errors, not a proof of all material combinations')
(a.out/'result.json').write_text(json.dumps(result,indent=2));assert result['passed'],result
print(json.dumps(result),flush=True)
