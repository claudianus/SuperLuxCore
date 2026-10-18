"""Stage and verify the experimental Windows DLSS wheel (no GPU claim)."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile

SDK_COMMIT = '9981c6350dc58c0ce21f024fd65e31e851c31689'


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def stage(args):
    wheels = list(args.wheels.glob('*.whl'))
    assert len(wheels) == 1, wheels
    original = wheels[0]
    assert original.name.endswith('cp313-cp313-win_amd64.whl'), original
    with tempfile.TemporaryDirectory(prefix='slc-dlss-') as work:
        work = Path(work)
        subprocess.run([os.sys.executable, '-m', 'wheel', 'unpack', str(original), '-d', str(work)], check=True)
        unpacked, = work.iterdir()
        package = unpacked / 'pysuperluxcore'
        library = args.helper / 'superluxcore_dlss.dll'
        assert library.is_file()
        shutil.copy2(library, package / library.name)
        runtime = package / 'dlss-runtime'
        runtime.mkdir()
        dll = args.sdk / 'lib/Windows_x86_64/rel/nvngx_dlssd.dll'
        assert dll.stat().st_size == 48343664
        assert dll.read_bytes()[:2] == b'MZ', 'Missing real NVIDIA release DLL'
        shutil.copy2(dll, runtime / dll.name)
        shutil.copy2(args.sdk / 'LICENSE.txt', runtime / 'NVIDIA-DLSS-LICENSE.txt')
        shutil.copy2(args.helper / 'CUEW-LICENSE.txt', package / 'CUEW-LICENSE.txt')
        provenance = {
            'classification': 'experimental Windows CUDA Ray Reconstruction integration',
            'engine_commit': os.environ['GITHUB_SHA'], 'sdk_commit': SDK_COMMIT,
            'sdk_repository': 'https://github.com/NVIDIA/DLSS',
            'helper_sha256': sha(library), 'nvidia_release_runtime_sha256': sha(dll),
            'GPU_evaluation_in_CI': False, 'RTX_quality_and_performance_accepted': False,
            'NVIDIA_license': 'dlss-runtime/NVIDIA-DLSS-LICENSE.txt',
            'renderer_source_baseline': 'Phase70; Phase71 progressive data-coherence WIP excluded',
        }
        (package / 'dlss-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
        destination = work / 'packed'
        subprocess.run([os.sys.executable, '-m', 'wheel', 'pack', str(unpacked), '-d', str(destination)], check=True)
        packed, = destination.glob('*.whl')
        # Replace only after wheel pack has regenerated RECORD successfully.
        shutil.copy2(packed, original)
    provenance['wheel_sha256'] = sha(original)
    (args.wheels / 'dlss-build-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
    verify(original)


def verify(wheel):
    with zipfile.ZipFile(wheel) as archive:
        assert archive.testzip() is None
        for name in ('superluxcore_dlss.dll', 'dlss-runtime/nvngx_dlssd.dll',
                     'dlss-runtime/NVIDIA-DLSS-LICENSE.txt', 'dlss-provenance.json'):
            assert 'pysuperluxcore/' + name in archive.namelist(), name
        # Verify every RECORD digest, including the newly added DLLs.
        import base64
        import csv
        import io
        record, = [n for n in archive.namelist() if n.endswith('.dist-info/RECORD')]
        for name, digest, size in csv.reader(io.StringIO(archive.read(record).decode())):
            if digest:
                data = archive.read(name)
                expected = 'sha256=' + base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b'=').decode()
                assert digest == expected and int(size) == len(data), name
    print('PASS Windows DLSS payload and wheel RECORD:', wheel)


def abi(args):
    import pysuperluxcore
    root = Path(pysuperluxcore.__file__).parent
    lib = ctypes.CDLL(str(root / 'superluxcore_dlss.dll'))
    lib.slc_dlss_abi.restype = ctypes.c_uint32
    assert lib.slc_dlss_abi() == 1
    for name in ('slc_dlss_create', 'slc_dlss_process', 'slc_dlss_stats', 'slc_dlss_destroy', 'slc_dlss_set_preset'):
        getattr(lib, name)
    print('PASS installed Windows renderer import and DLSS ABI; GPU evaluation not performed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['stage', 'abi'])
    parser.add_argument('--wheels', type=Path)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--helper', type=Path)
    args = parser.parse_args()
    (stage if args.action == 'stage' else abi)(args)
