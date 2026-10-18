"""Exercise real wheel CLI staging/reversion; fixtures are not GPU binaries."""
import base64
import csv
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

scripts = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='slc-wheel-regression-') as temp:
    root = Path(temp)
    wheels = root / 'addon/wheels'
    wheels.mkdir(parents=True)
    info = 'pysuperluxcore-2.11.34.dist-info/'
    payload = {
        'pysuperluxcore/native-payload': b'PACKAGING FIXTURE ONLY: NO RENDERER OR GPU CODE',
        info+'METADATA': b'Metadata-Version: 2.1\nName: pysuperluxcore\nVersion: 2.11.34\n',
        info+'WHEEL': b'Wheel-Version: 1.0\nGenerator: packaging-fixture\nRoot-Is-Purelib: false\nTag: cp313-cp313-win_amd64\n',
    }
    record = io.StringIO()
    writer = csv.writer(record)
    for name, data in payload.items():
        writer.writerow([name, 'sha256='+base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b'=').decode(), len(data)])
    writer.writerow([info+'RECORD', '', ''])
    payload[info+'RECORD'] = record.getvalue().encode()
    with zipfile.ZipFile(wheels/'pysuperluxcore-2.11.34-cp313-cp313-win_amd64.whl', 'w') as archive:
        for name, data in payload.items():archive.writestr(name, data)
    helper = root/'helper'
    helper.mkdir()
    (helper/'superluxcore_dlss.dll').write_bytes(b'MZ PACKAGING FIXTURE ONLY')
    (helper/'CUEW-LICENSE.txt').write_text('PACKAGING FIXTURE ONLY')
    sdk = root/'sdk'
    runtime = sdk/'lib/Windows_x86_64/rel/nvngx_dlssd.dll'
    runtime.parent.mkdir(parents=True)
    with runtime.open('wb') as stream:
        stream.write(b'MZ PACKAGING FIXTURE ONLY: NOT NVIDIA CODE')
        stream.truncate(48343664)
    (sdk/'LICENSE.txt').write_text('PACKAGING FIXTURE ONLY')
    env = {**os.environ, 'GITHUB_SHA': 'fixture', 'SLC_NATIVE_COMMIT': 'fixture'}
    subprocess.run([sys.executable, str(scripts/'windows_dlss_package.py'), 'stage',
                    '--wheels', str(wheels), '--helper', str(helper), '--sdk', str(sdk)], env=env, check=True)
    subprocess.run([sys.executable, str(scripts/'reversion_windows_wheel.py')], cwd=root, env=env, check=True)
    wheel, = wheels.glob('*.whl')
    assert '+dlss.exp20261019' in wheel.name
    with zipfile.ZipFile(wheel) as archive:
        assert archive.read('pysuperluxcore/native-payload') == payload['pysuperluxcore/native-payload']
        assert archive.read('pysuperluxcore/superluxcore_dlss.dll') == (helper/'superluxcore_dlss.dll').read_bytes()
        assert b'Version: 2.11.34+dlss.exp20261019' in archive.read('pysuperluxcore-2.11.34+dlss.exp20261019.dist-info/METADATA')
        with zipfile.ZipFile(wheel) as z:
            for name, digest, size in csv.reader(io.StringIO(z.read('pysuperluxcore-2.11.34+dlss.exp20261019.dist-info/RECORD').decode())):
                if digest:
                    data=z.read(name)
                    assert digest=='sha256='+base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b'=').decode()
                    assert len(data)==int(size)
    print('PASS actual wheel CLI staging/reversion, payload preservation, unique cache identity and complete RECORD')
    print('Packaging fixtures only; no renderer, SDK, GPU, image-quality or performance acceptance')
