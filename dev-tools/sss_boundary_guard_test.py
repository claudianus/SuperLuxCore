"""Exercise the real SSS guard and its grazing negative control; delete scratch."""
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
commands = json.loads((root/'out/build/compile_commands.json').read_text())
arguments = shlex.split(next(item['command'] for item in commands
                            if item['file'].endswith('/volumes/homogenous.cpp')))
includes = []
for index, argument in enumerate(arguments):
    if argument.startswith('-I'):
        includes.append(argument)
    elif argument == '-isystem':
        includes.extend((argument, arguments[index+1]))
with tempfile.TemporaryDirectory(prefix='sss-boundary-guard-') as scratch:
    binary = Path(scratch) / 'probe'
    compiled = subprocess.run(['clang++', '-std=c++20', '-O2', *includes,
                    str(root/'dev-tools/sss_boundary_guard_probe.cpp'), '-o', str(binary)],
                   capture_output=True, text=True, timeout=60)
    if compiled.returncode:
        raise RuntimeError(compiled.stderr)
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=30)
    data = json.loads(result.stdout)
args.out.parent.mkdir(parents=True, exist_ok=True)
args.out.write_text(json.dumps(data, indent=2))
print(json.dumps(data), flush=True)
