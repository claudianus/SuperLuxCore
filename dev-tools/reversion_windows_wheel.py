"""Give the experimental distribution a unique wheel cache identity."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

VERSION = '2.11.34+dlss.exp20261019.fix2'
wheels = Path('addon/wheels')
original, = wheels.glob('pysuperluxcore-*.whl')
with tempfile.TemporaryDirectory(prefix='slc-variant-') as directory:
    work = Path(directory)
    subprocess.run([os.sys.executable, '-m', 'wheel', 'unpack', str(original), '-d', str(work)], check=True)
    unpacked, = work.iterdir()
    info, = unpacked.glob('*.dist-info')
    metadata = info / 'METADATA'
    text = metadata.read_text()
    assert text.count('\nVersion: 2.11.34\n') == 1
    metadata.write_text(text.replace('\nVersion: 2.11.34\n', '\nVersion: '+VERSION+'\n'))
    info.rename(unpacked / ('pysuperluxcore-'+VERSION+'.dist-info'))
    provenance_path = unpacked / 'pysuperluxcore/dlss-provenance.json'
    provenance = json.loads(provenance_path.read_text())
    assert provenance['engine_commit'] == os.environ['SLC_NATIVE_COMMIT']
    provenance['distribution_version'] = VERSION
    provenance['package_commit'] = os.environ['GITHUB_SHA']
    provenance['native_version'] = '2.11.34'
    provenance_path.write_text(json.dumps(provenance, indent=2)+'\n')
    data_dir = unpacked / 'pysuperluxcore-2.11.34.data'
    if data_dir.exists():data_dir.rename(unpacked / ('pysuperluxcore-'+VERSION+'.data'))
    variant = unpacked.with_name('pysuperluxcore-'+VERSION)
    unpacked.rename(variant)
    packed = work / 'packed'
    packed.mkdir()
    subprocess.run([os.sys.executable, '-m', 'wheel', 'pack', str(variant), '-d', str(packed)], check=True)
    wheel, = packed.glob('*.whl')
    target = wheels / wheel.name
    shutil.copy2(wheel, target)
    original.unlink()
print('Experimental distribution:', target)
