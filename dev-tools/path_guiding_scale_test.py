"""Model selection must not depend on unclipped training radiance units."""
import argparse,hashlib,json,shlex,subprocess,tempfile
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
root=Path(__file__).resolve().parents[1];build=root/'out/build';source=Path(__file__).with_name('path_guiding_scale_probe.cpp')
commands=subprocess.check_output(['ninja','-C',str(build),'-f','build-Release.ninja','-t','commands','luxcoreconsole'],text=True).splitlines()
oldsrc=str(root/'samples/luxcoreconsole/luxcoreconsole.cpp');oldobj='samples/luxcoreconsole/CMakeFiles/luxcoreconsole.dir/Release/luxcoreconsole.cpp.o';oldbin='samples/luxcoreconsole/Release/luxcoreconsole'
compile=shlex.split(next(x for x in commands if '-c '+oldsrc in x));link=shlex.split(next(x for x in commands if '-o '+oldbin+' ' in x))
while link[0] in (':','&&'):link.pop(0)
if '&&' in link:link=link[:link.index('&&')]
internal=shlex.split(next(x for x in commands if '-c '+str(root/'src/slg/materials/openpbr.cpp') in x));extra=[]
for i,x in enumerate(internal):
    if x in ('-I','-isystem'):extra.extend([x,internal[i+1]])
    elif x.startswith('-I') and len(x)>2:extra.append(x)
with tempfile.TemporaryDirectory(prefix='slc-guide-scale-') as tmp:
    obj=str(Path(tmp)/'probe.o');binary=str(Path(tmp)/'probe')
    compile=[str(source) if x==oldsrc else obj if x==oldobj else obj+'.d' if x==oldobj+'.d' else x for x in compile]+extra
    link=[obj if x==oldobj else binary if x==oldbin else x for x in link]
    with (a.out/'build.log').open('w') as log:
        for cmd in (compile,link):subprocess.run(cmd,cwd=build,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    legacy=subprocess.run([binary,'legacy'],capture_output=True,text=True,timeout=60)
    (a.out/'legacy.log').write_text(legacy.stdout+legacy.stderr)
    assert legacy.returncode!=0,'The negative control did not detect the original unit dependence'
    result=subprocess.run([binary],capture_output=True,text=True,timeout=60)
    (a.out/'corrected.log').write_text(result.stdout+result.stderr)
    assert result.returncode==0,result.stderr
    record=json.loads(result.stdout)
    record.update(legacy_rejected=True,source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                  fitter_sha256=hashlib.sha256((root/'src/slg/engines/pathguiding.cpp').read_bytes()).hexdigest(),
                  scope='Unclipped known anisotropic input at three radiance scales; model/PDF invariant, not render-performance proof')
    (a.out/'result.json').write_text(json.dumps(record,indent=2));print(json.dumps(record),flush=True)
