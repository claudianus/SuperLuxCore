"""Run the real film edit reset contract with the loaded macOS Release module.

Use the same Python ABI as that module. The temporary C++ bridge is removed
after both a pass and a failure. A failed baseline is retained as JSON evidence.
"""
from pathlib import Path
import argparse, ctypes, hashlib, json, shlex, subprocess, sys, tempfile

repo = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--module-dir', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
assert sys.platform == 'darwin'
build = repo / 'out/build'
row = next(x for x in json.loads((build / 'compile_commands.json').read_text())
           if x['file'].endswith('/textures/blackbody.cpp')
           and '-DCMAKE_INTDIR="Release"' in shlex.split(x['command']))
argv = shlex.split(row['command'])
flags, i = [], 1
while i < len(argv):
    if argv[i] in ['-o', '-c', '-MT', '-MF']: i += 2
    elif argv[i] in ['-MD', '-MMD']: i += 1
    else: flags.append(argv[i]); i += 1
source = repo / 'dev-tools/film_edit_reset_contract.cpp'
args.output.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix='slc-film-reset-contract-') as temporary:
    bridge = Path(temporary) / 'contract.dylib'
    with args.output.with_suffix('.build.log').open('w') as log:
        subprocess.run([argv[0], *flags, '-dynamiclib', '-undefined', 'dynamic_lookup',
                        str(source), '-o', str(bridge)], cwd=row['directory'],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    sys.path.insert(0, str(args.module_dir))
    import pysuperluxcore as lux
    native_path = getattr(lux, 'pysuperluxcore', lux).__file__
    native = ctypes.CDLL(native_path, mode=ctypes.RTLD_GLOBAL)
    contract = ctypes.CDLL(str(bridge))
    contract.slc_film_edit_reset_contract.restype = ctypes.c_int
    passed = contract.slc_film_edit_reset_contract() == 1
    proof = {'passed': passed, 'native_sha256': hashlib.sha256(Path(native_path).read_bytes()).hexdigest(),
             'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest()}
proof['temporary_bridge_removed'] = not Path(temporary).exists()
args.output.write_text(json.dumps(proof, indent=2) + '\n')
sys.exit(0 if passed else 1)
