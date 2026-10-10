# SPDX-License-Identifier: Apache-2.0
"""Ordinary matte/mirror energy with PSR in actual CPU and Metal hybrid paths.

The eye reference retains the same regularized transport, without suppressing
caustics. No imported SSS or private production adapter is needed by this test.
"""
import ast
import hashlib
import json
import os
from pathlib import Path
import time

import numpy as np
import pysuperluxcore as slc

repo = Path(__file__).resolve().parents[1]
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])
folder.mkdir(parents=True, exist_ok=True)
identity = json.loads(Path(os.environ['SUPERLUXCORE_BSSRDF_IDENTITY']).read_text())
for name, digest in identity['source_files'].items():
    assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == digest, name
assert hashlib.sha256(Path(slc.pysuperluxcore.__file__).read_bytes()).hexdigest() == identity['native_sha256']
slc.Init()
W = H = 32
namespace = {'slc': slc, 'np': np, 'W': W, 'H': H}
tree = ast.parse((repo / 'dev-tools/cycles_bssrdf_transport_test.py').read_text())
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef)
             and n.name in ('properties', 'scene')]
exec(compile(ast.Module(body=functions, type_ignores=[]), '<frozen-builder>', 'exec'), namespace)
props = namespace['properties']
scene = namespace['scene'](kind='matte')
scene.Parse(props({'scene.materials.mirror.type': 'mirror',
                   'scene.materials.mirror.kr': 1.,
                   'scene.objects.mirror.material': 'mirror',
                   # A side mirror is visible to the ordinary matte face's
                   # hemisphere, while staying outside the camera frustum.
                   'scene.objects.mirror.vertices': (2., -8., -8., 2., 8., -8.,
                                                     2., 8., 8., 2., -8., 8.),
                   'scene.objects.mirror.faces': (0, 1, 2, 0, 2, 3),
                   'scene.objects.mirror.id': 999}))
(folder / 'scene.scn').write_text(scene.ToProperties().ToString())
devices = slc.GetOpenCLDeviceDescs()
mask = ''
index = 0
while devices.IsDefined('opencl.device.' + str(index) + '.type'):
    mask += '1' if devices.Get('opencl.device.' + str(index) + '.type').GetString() == 'METAL_GPU' else '0'
    index += 1
assert '1' in mask, 'This contract requires actual Metal execution; no skip acceptance'

base = {'film.width': W, 'film.height': H, 'film.imagepipelines.0.0.type': 'NOP',
        'film.filter.type': 'NONE', 'sampler.type': 'SOBOL', 'batch.haltspp': 0,
        'native.threads.count': 8, 'opencl.task.count': 8192,
        'opencl.devices.select': mask, 'opencl.cpu.use': False, 'opencl.gpu.use': True,
        'renderengine.seed': 817, 'path.hybridbackforward.partition': .8,
        'path.hybridbackforward.adaptivecaustic': True,
        'path.lighttracing.auto': False, 'path.lighttracing.only': False,
        'path.regularization.auto': False, 'path.regularization.sigma': .03,
        'path.mnee.enable': False, 'path.mnee.auto': False,
        'path.vertexconnection.enable': False, 'path.photongi.caustic.enabled': False,
        'path.photongi.indirect.enabled': False, 'path.pathdepth.total': 4,
        'path.forceblackbackground.enable': True, 'film.outputs.0.type': 'RGB',
        'film.outputs.0.filename': 'unused.exr', 'film.outputs.1.type': 'MATERIAL_ID',
        'film.outputs.1.filename': 'unused.png'}
records = []
for engine, mode in (('PATHCPU', 'cpu'), ('PATHOCL', 'off'), ('PATHOCL', 'on')):
    results = {}
    variants = [('eye', False, .8), ('hybrid', True, .8)]
    if engine == 'PATHCPU':
        variants.insert(1, ('eye-hole', True, 1.))
    for variant, hybrid, partition in variants:
        label = mode + '-' + variant
        values = base | {'renderengine.type': engine, 'pathocl.wavefront': mode if engine == 'PATHOCL' else 'off',
                         'path.hybridbackforward.enable': hybrid,
                         'path.hybridbackforward.partition': partition,
                         'path.lighttracing.enable': hybrid and engine == 'PATHOCL'}
        (folder / (label + '.cfg')).write_text(props(values).ToString())
        session = slc.RenderSession(slc.RenderConfig(props(values), scene))
        session.Start()
        start = time.monotonic()
        try:
            while True:
                session.UpdateStats()
                stats = session.GetStats()
                eye = stats.Get('stats.renderengine.pass.eye').GetInt()
                light = stats.Get('stats.renderengine.pass.light').GetInt()
                if eye >= 16384 and (not hybrid or partition == 1. or light >= 20480):
                    break
                assert time.monotonic() - start < 600, (label, eye, light)
                time.sleep(.1)
        finally:
            session.Stop()
        rgb = np.empty(W * H * 3, np.float32)
        ids = np.empty(W * H, np.uint32)
        session.GetFilm().GetOutputFloat(slc.FilmOutputType.RGB, rgb, 0, True)
        session.GetFilm().GetOutputUInt(slc.FilmOutputType.MATERIAL_ID, ids, 0, True)
        rgb, ids = rgb.reshape(H, W, 3), ids.reshape(H, W)
        assert np.isfinite(rgb).all()
        np.savez_compressed(folder / (label + '.npz'), RGB=rgb, MATERIAL_ID=ids)
        results[variant] = (rgb, ids, eye, light)
    region = results['eye'][1] == 991
    interior = region.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            interior &= np.roll(np.roll(region, dy, 0), dx, 1)
    assert interior.sum() >= 100
    means = {variant: result[0][interior].mean(axis=0).tolist()
             for variant, result in results.items()}
    error = float(np.max(np.abs(np.array(means['hybrid']) - means['eye']) /
                         np.maximum(means['eye'], 1e-4)))
    record = {'mode': mode, 'means': means, 'max_relative_channel_error': error,
              'counts': {str(h): {'eye': r[2], 'light': r[3]} for h, r in results.items()},
              'passed': error < .03}
    records.append(record)
    if 'eye-hole' in means:
        loss = 1. - float(np.mean(means['eye-hole']) / np.mean(means['eye']))
        record['verified_omitted_caustic_fraction'] = loss
        assert loss > .05, ('fixture must have meaningful caustic support', means)
    (folder / 'metrics.json').write_text(json.dumps({'native_sha256': identity['native_sha256'],
                    'records': records, 'complete': False}, indent=2) + '\n')
    assert record['passed'], record
    print('PSR_PARTITION_PASS', record, flush=True)
(folder / 'metrics.json').write_text(json.dumps({'native_sha256': identity['native_sha256'],
                    'records': records, 'complete': True}, indent=2) + '\n')
print('PSR_PARTITION_COMPLETE', len(records), flush=True)
