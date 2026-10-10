#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile a contract against the actual Release library; remove the binary."""
import json
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[1]
# pysuperluxcore links the static core and does not refresh libluxcore.dylib.
# This contract links the shared core, so build that Release target explicitly.
subprocess.run(['ninja', '-C', str(repo / 'out/build'), '-f',
                'build-Release.ninja', 'luxcore'], check=True)
commands = json.loads((repo / 'out/build/compile_commands.json').read_text())
entry = next(e for e in commands if e['file'].endswith('/samplers/metropolis.cpp')
             and '-DNDEBUG' in e['command'])
parts = shlex.split(entry['command'])
includes = []
i = 1
while i < len(parts):
    if parts[i] == '-isystem':
        includes += parts[i:i + 2]
        i += 2
        continue
    if parts[i].startswith('-I'):
        includes.append(parts[i])
    i += 1
library_dir = repo / 'out/build/src/luxcore/Release'
source = repo / 'dev-tools/metropolis_sparse_contract.cpp'
if len(sys.argv) > 1:
    assert sys.argv[1:] == ['--startup'], 'only --startup is supported'
    source = repo / 'dev-tools/metropolis_startup_contract.cpp'
with tempfile.TemporaryDirectory(prefix='superlux-metropolis-contract-') as temp:
    binary = Path(temp) / 'contract'
    subprocess.run(['/usr/bin/c++', '-std=c++20', '-O2', *includes,
                    str(source),
                    str(library_dir / 'libluxcore.dylib'),
                    '-Wl,-rpath,' + str(library_dir), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
