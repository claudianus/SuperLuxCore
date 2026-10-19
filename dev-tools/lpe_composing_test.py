"""Real film splat regression; optionally reproduce the old LPE-only failure."""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--before', type=Path)
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
                           if '-c '+str(root/'src/slg/film/filmaddsample.cpp') in line))
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


results = {}
with tempfile.TemporaryDirectory(prefix='lpe-composing-') as scratch:
    temp = Path(scratch)
    obj = str(temp/'probe.o')
    command = [str(root/'dev-tools/lpe_composing_probe.cpp') if token == source else
               obj if token == object_name else obj+'.d' if token == object_name+'.d' else token
               for token in compile_template] + includes
    execute(command)
    labels = ['after', 'before'] if args.before else ['after']
    for label in labels:
        objects = [obj]
        if label == 'before':
            before_object = str(temp/'before.o')
            old_object = internal[internal.index('-o')+1]
            old_source = str(root/'src/slg/film/filmaddsample.cpp')
            command = [str(args.before.resolve()) if token == old_source else
                       before_object if token == old_object else
                       before_object+'.d' if token == old_object+'.d' else token for token in internal]
            execute(command)
            objects.append(before_object)
        binary = str(temp/label)
        command = []
        for token in link_template:
            command.extend(objects if token == object_name else [binary if token == binary_name else token])
        execute(command)
        results[label] = json.loads(execute([binary]+(['legacy'] if label == 'before' else [])))
args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text(json.dumps(results, indent=2))
print(json.dumps(results), flush=True)
