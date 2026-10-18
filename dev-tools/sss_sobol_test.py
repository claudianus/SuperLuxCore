"""Compare real eager/lazy Sobol code in all scramble modes; remove scratch."""
import argparse
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

ap=argparse.ArgumentParser()
ap.add_argument('--before',type=Path,required=True)
ap.add_argument('--out',type=Path,required=True)
a=ap.parse_args()
root=Path(__file__).resolve().parents[1]
commands=json.loads((root/'out/build/compile_commands.json').read_text())
args=shlex.split(next(c['command'] for c in commands if c['file'].endswith('/sobolsequence.cpp')))
includes=[]
for i,arg in enumerate(args):
    if arg.startswith('-I'):includes.append(arg)
    elif arg=='-isystem':includes.extend((arg,args[i+1]))
data=root/'out/build/src/slg/CMakeFiles/slg-core.dir/Release/samplers/soboldata.cpp.o'
assert data.is_file(),data
a.out.mkdir(parents=True,exist_ok=True)
rows={}
with tempfile.TemporaryDirectory(prefix='slc-sss-sobol-') as scratch:
    temp=Path(scratch)
    for label,source in [('before',a.before.resolve()),('after',root/'src/slg/samplers/sobolsequence.cpp')]:
        binary=temp/label
        command=['clang++','-std=c++20','-O2',*includes,str(root/'dev-tools/sss_sobol_probe.cpp'),
                 str(source),str(data),'-o',str(binary)]
        built=subprocess.run(command,capture_output=True,text=True,timeout=90)
        assert built.returncode==0,built.stderr
        output=temp/(label+'.bin')
        ran=subprocess.run([str(binary),str(output)],capture_output=True,text=True,timeout=30)
        assert ran.returncode==0,ran.stderr
        rows[label]={**json.loads(ran.stdout),'sha256':hashlib.sha256(output.read_bytes()).hexdigest()}
    assert (temp/'before.bin').read_bytes()==(temp/'after.bin').read_bytes(),rows
    # AddressSanitizer exercises repeated RequestSamples ownership on the candidate.
    command=['clang++','-std=c++20','-O1','-g','-fsanitize=address',*includes,
             str(root/'dev-tools/sss_sobol_probe.cpp'),str(root/'src/slg/samplers/sobolsequence.cpp'),
             str(data),'-o',str(temp/'asan')]
    built=subprocess.run(command,capture_output=True,text=True,timeout=90)
    assert built.returncode==0,built.stderr
    ran=subprocess.run([str(temp/'asan'),str(temp/'asan.bin')],capture_output=True,text=True,timeout=30)
    assert ran.returncode==0,ran.stderr
    rows['asan']='passed'
(a.out/'result.json').write_text(json.dumps(rows,indent=2))
print(json.dumps(rows),flush=True)
