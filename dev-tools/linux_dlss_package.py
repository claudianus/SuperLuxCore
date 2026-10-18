"""Stage and verify the experimental Linux DLSS wheel (no GPU claim)."""
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


def elf_gate(path):
    import re
    header = subprocess.check_output(['readelf', '-h', str(path)], text=True)
    assert 'ELF64' in header and 'Advanced Micro Devices X86-64' in header, header
    versions = subprocess.check_output(['readelf', '--version-info', str(path)], text=True)
    glibc = [tuple(map(int,v.split('.'))) for v in re.findall(r'Name: GLIBC_([0-9.]+)',versions)]
    assert all(v <= (2,28) for v in glibc), (path,glibc)
    # Helper links its C++ support statically; no undeclared libstdc++ sidecar.
    dynamic = subprocess.check_output(['readelf', '-d', str(path)], text=True)
    needed = re.findall(r'Shared library: \[(.*?)\]', dynamic)
    assert set(needed) <= {'libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1','ld-linux-x86-64.so.2'}, needed
    print('PASS ELF x64/glibc <=2.28 and system dependency gate:',path,needed)


def stage(args):
    wheels = list(args.wheels.glob('*.whl'))
    assert len(wheels) == 1, wheels
    original = wheels[0]
    assert original.name.endswith('cp313-cp313-manylinux_2_28_x86_64.whl'), original
    with tempfile.TemporaryDirectory(prefix='slc-dlss-') as work:
        work = Path(work)
        subprocess.run([os.sys.executable, '-m', 'wheel', 'unpack', str(original), '-d', str(work)], check=True)
        unpacked, = work.iterdir()
        package = unpacked / 'pysuperluxcore'
        library = args.helper / 'libsuperluxcore_dlss.so'
        assert library.is_file()
        shutil.copy2(library, package / library.name)
        runtime = package / 'dlss-runtime'
        runtime.mkdir()
        dll = args.sdk / 'lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.16'
        assert dll.stat().st_size == 48238232
        assert dll.read_bytes()[:4] == b'\x7fELF', 'Missing real NVIDIA release ELF'
        elf_gate(library)
        elf_gate(dll)
        shutil.copy2(dll, runtime / dll.name)
        shutil.copy2(args.sdk / 'LICENSE.txt', runtime / 'NVIDIA-DLSS-LICENSE.txt')
        shutil.copy2(args.helper / 'CUEW-LICENSE.txt', package / 'CUEW-LICENSE.txt')
        provenance = {
            'classification': 'experimental Linux CUDA Ray Reconstruction integration',
            'engine_commit': os.environ['GITHUB_SHA'], 'sdk_commit': SDK_COMMIT,
            'sdk_repository': 'https://github.com/NVIDIA/DLSS',
            'helper_sha256': sha(library), 'nvidia_release_runtime_sha256': sha(dll),
            'GPU_evaluation_in_CI': False, 'RTX_quality_and_performance_accepted': False,
            'NVIDIA_license': 'dlss-runtime/NVIDIA-DLSS-LICENSE.txt',
            'renderer_source_baseline': 'Phase70; Phase71 progressive data-coherence WIP excluded',
        }
        (package / 'dlss-provenance.json').write_text(json.dumps(provenance, indent=2) + '\n')
        destination = work / 'packed'
        destination.mkdir()
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
        for name in ('libsuperluxcore_dlss.so', 'dlss-runtime/libnvidia-ngx-dlssd.so.310.9.16',
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
    print('PASS Linux DLSS payload and wheel RECORD:', wheel)


def abi(args):
    import pysuperluxcore
    root = Path(pysuperluxcore.__file__).parent
    lib = ctypes.CDLL(str(root / 'libsuperluxcore_dlss.so'))
    lib.slc_dlss_abi.restype = ctypes.c_uint32
    assert lib.slc_dlss_abi() == 1
    for name in ('slc_dlss_create', 'slc_dlss_process', 'slc_dlss_stats', 'slc_dlss_destroy', 'slc_dlss_set_preset'):
        getattr(lib, name)
    print('PASS installed Linux renderer import and DLSS ABI; GPU evaluation not performed')


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['stage', 'abi'])
    parser.add_argument('--wheels', type=Path)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--helper', type=Path)
    args = parser.parse_args()
    (stage if args.action == 'stage' else abi)(args)
