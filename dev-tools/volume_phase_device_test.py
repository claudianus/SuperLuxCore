"""Actual phase-sampler transport-mode regression with an old-object control."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = root/'out/build'
commands = subprocess.check_output(['ninja', '-C', str(build), '-f', 'build-Release.ninja',
                                    '-t', 'commands', 'luxcoreconsole'], text=True).splitlines()
source = str(root/'samples/luxcoreconsole/luxcoreconsole.cpp')
object_name = 'samples/luxcoreconsole/CMakeFiles/luxcoreconsole.dir/Release/luxcoreconsole.cpp.o'
binary_name = 'samples/luxcoreconsole/Release/luxcoreconsole'
compile_template = shlex.split(next(line for line in commands if '-c '+source in line))
link_template = shlex.split(next(line for line in commands if '-o '+binary_name+' ' in line))
while link_template[0] in (':', '&&'):
    link_template.pop(0)
if '&&' in link_template:
    link_template = link_template[:link_template.index('&&')]
internal = shlex.split(next(line for line in commands
                           if '-c '+str(root/'src/slg/bsdf/bsdf.cpp') in line))
includes = []
for index, token in enumerate(internal):
    if token in ('-I', '-isystem'):
        includes.extend([token, internal[index+1]])
    elif token.startswith('-I'):
        includes.append(token)


def execute(command):
    result = subprocess.run(command, cwd=build, capture_output=True, text=True, timeout=90)
    if result.returncode:
        raise RuntimeError(result.stderr)
    return result.stdout


with tempfile.TemporaryDirectory(prefix='volume-phase-device-') as scratch:
    temp = Path(scratch);obj=str(temp/'probe.o');binary=str(temp/'probe')
    command=[str(root/'dev-tools/volume_phase_device_probe.cpp') if token==source else obj if token==object_name else obj+'.d' if token==object_name+'.d' else token for token in compile_template]+includes
    execute(command)
    command=[obj if token==object_name else binary if token==binary_name else token for token in link_template]
    execute(command)
    args.out.parent.mkdir(parents=True,exist_ok=True)
    result=subprocess.run([binary,str(args.out.resolve())],cwd=root,capture_output=True,text=True,timeout=180)
    if result.returncode:raise RuntimeError(result.stderr+'\n'+result.stdout)
    print(result.stdout,flush=True)
