"""Check exact spectral blackbody results against a loaded macOS Release module.

Run with the same Python ABI used to build the module (Blender 5.2 uses 3.13).
Uses that module's actual C++ compile flags. The temporary bridge is removed
on success and failure; this checks numerical identity, not throughput.
"""
from pathlib import Path
import argparse
import ctypes
import hashlib
import json
import shlex
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-dir', type=Path, default=repo / 'out/build')
parser.add_argument('--module-dir', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
assert sys.platform == 'darwin', 'This bridge uses the macOS dynamic library linker.'
build = args.build_dir.resolve()
commands = json.loads((build / 'compile_commands.json').read_text())
command = next(row for row in commands
               if row['file'].endswith('/textures/blackbody.cpp')
               and '-DCMAKE_INTDIR="Release"' in shlex.split(row['command']))
argv = shlex.split(command['command'])
flags = []
i = 1
while i < len(argv):
    if argv[i] in ['-o', '-c', '-MT', '-MF']:
        i += 2
    elif argv[i] in ['-MD', '-MMD']:
        i += 1
    else:
        flags.append(argv[i])
        i += 1
source = repo / 'dev-tools/blackbody_exact_cache_contract.cpp'
args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='slc-blackbody-contract-') as temporary:
    bridge_path = Path(temporary) / 'contract.dylib'
    with args.output.with_suffix('.build.log').open('w') as log:
        subprocess.run([argv[0], *flags, '-dynamiclib', '-undefined', 'dynamic_lookup',
                        str(source), '-o', str(bridge_path)],
                       cwd=command['directory'], stdout=log,
                       stderr=subprocess.STDOUT, check=True)
    sys.path.insert(0, str(args.module_dir or build / 'src/pysuperluxcore/Release'))
    import pysuperluxcore as lux
    native = ctypes.CDLL(lux.__file__, mode=ctypes.RTLD_GLOBAL)
    bridge = ctypes.CDLL(str(bridge_path))
    bridge.slc_blackbody_exact_cache_contract.restype = ctypes.c_int
    assert bridge.slc_blackbody_exact_cache_contract() == 1
    proof = {'passed': True, 'exact_float_bits': True, 'workers': 4,
             'evaluations': 6912, 'bounded_entries_per_worker': 1,
             'native_sha256': hashlib.sha256(Path(lux.__file__).read_bytes()).hexdigest(),
             'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
proof['temporary_bridge_removed'] = not Path(temporary).exists()
args.output.write_text(json.dumps(proof, indent=2) + '\n')
print('BLACKBODY_EXACT_CACHE_CONTRACT_COMPLETE', flush=True)
