#!/usr/bin/env python3
"""One fresh-process CUDA renderer case for compiler/cache regression checks.

Run under a bounded supervisor with a private SUPERLUXCORE_CACHE_DIR. Repeat
with the same and different LUX_CUDA_OFAST/LUX_CUDA_SPLIT settings; inspect
native Program cached/compiled logs and cache-file inventories externally.
This fixture verifies execution and cache selection, not image quality/speed.
"""
from pathlib import Path
import argparse
import hashlib
import json
import os
import sys
import time

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--site-packages', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
sys.path.insert(0, str(args.site_packages))
import numpy as np
import pysuperluxcore as s


def properties(text):
    result = s.Properties()
    result.SetFromString(text)
    return result


s.Init()
scene = s.Scene()
scene.DefineMesh('wall', [(-100, 2, -100), (100, 2, -100),
                          (100, 2, 100), (-100, 2, 100)], [(0, 1, 2), (0, 2, 3)])
scene.Parse(properties('''scene.camera.lookat.orig = 0 -3 1
scene.camera.lookat.target = 0 2 1
scene.camera.up = 0 0 1
scene.camera.fieldofview = 45
scene.materials.wall.type = matte
scene.materials.wall.kd = 0.6 0.6 0.6
scene.objects.wall.shape = wall
scene.objects.wall.material = wall
scene.lights.environment.type = constantinfinite
scene.lights.environment.color = 1 1 1
'''))
devices = s.GetOpenCLDeviceDescs()
mask = ''.join('1' if devices.Get(name + '.type').GetString() == 'CUDA_GPU' else '0'
               for name in devices.GetAllUniqueSubNames('opencl.device'))
assert mask.count('1') == 1, 'Exactly one CUDA device required'
config = properties(f'''renderengine.type = RTPATHOCL
renderengine.seed = 42
sampler.type = TILEPATHSAMPLER
opencl.devices.select = {mask}
opencl.native.threads.count = 0
opencl.task.count = 8192
pathocl.wavefront = off
film.width = 96
film.height = 64
film.filter.type = NONE
film.imagepipelines.0.0.type = NOP
film.denoisingguides.enable = 1
rtpath.denoisingframes.enable = 1
rtpath.denoisingframes.lossless = 1
rtpath.denoisingframes.haltspp = 1
scene.epsilon.min = 0.00001
scene.epsilon.max = 0.1
path.shadowterminator = chiang
path.pathdepth.total = 4
path.lighttracing.auto = 0
path.lighttracing.enable = 0
path.vertexconnection.enable = 0
path.mnee.auto = 0
path.mnee.enable = 0
path.guiding.enable = 0
path.photongi.indirect.enabled = 0
path.photongi.caustic.enabled = 0
path.spectral.enable = 0
''')
session = s.RenderSession(s.RenderConfig(config, scene))
packet = None
try:
    session.Start()
    deadline = time.monotonic() + 20
    while packet is None:
        assert time.monotonic() < deadline, 'Tiny native frame deadline'
        packet = session.GetDenoisingFrame(0, 100)
    assert packet['complete'] and np.all(packet['sample_count'] == 1)
    assert np.isfinite(packet['color']).all()
    args.output.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output / 'frame.npz', color=packet['color'],
                        guides=packet['guides'], sample_count=packet['sample_count'])
    result = {
        'requested_ofast': os.environ.get('LUX_CUDA_OFAST'),
        'requested_split': os.environ.get('LUX_CUDA_SPLIT'),
        'frame_id': packet['frame_id'], 'complete': True,
        'sample_counts_one': True, 'finite': True,
        'native_sha256': hashlib.file_digest(Path(s.pysuperluxcore.__file__).open('rb'), 'sha256').hexdigest(),
        'scope': 'Actual native cache-selection regression; no general quality or speed claim.',
    }
finally:
    if session.IsStarted():
        session.Stop()
    packet = None
(args.output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
