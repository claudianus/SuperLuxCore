# SPDX-License-Identifier: Apache-2.0
"""Uniform rough SSS hybrid partition on actual Metal, with/without PSR.

The unsuppressed CPU eye walk is the independent reference. A CPU eye-only
hybrid partition must measurably omit caustics before device comparisons can
pass. Uses 65536 eye / 81920 light samples and the existing 3% region gate.
Small private fixtures do not establish production adapter/image acceptance.
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
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR']) / 'device-hybrid'
folder.mkdir(parents=True, exist_ok=True)
identity = json.loads(Path(os.environ['SUPERLUXCORE_BSSRDF_IDENTITY']).read_text())
for name, digest in identity['source_files'].items():
    assert hashlib.sha256(Path(name).read_bytes()).hexdigest() == digest, name
assert hashlib.sha256(Path(slc.pysuperluxcore.__file__).read_bytes()).hexdigest() == identity['native_sha256']
slc.Init()
W = H = 32
ns = {'slc': slc, 'np': np}
tree = ast.parse((repo / 'dev-tools/cycles_bssrdf_transport_test.py').read_text())
exec(compile(ast.Module(body=[n for n in tree.body if isinstance(n, ast.FunctionDef) and
                             n.name in ('properties', 'scene')], type_ignores=[]), '<scene>', 'exec'), ns)
props, scene = ns['properties'], ns['scene']
builder = ast.parse((repo / 'dev-tools/cycles_bssrdf_hybrid_test.py').read_text())
exec(compile(ast.Module(body=[n for n in builder.body if isinstance(n, ast.FunctionDef) and
                             n.name == 'body'], type_ignores=[]), '<hybrid-scene>', 'exec'), globals())
base = {'film.width': W, 'film.height': H, 'film.imagepipelines.0.0.type': 'NOP',
        'film.filter.type': 'NONE', 'sampler.type': 'SOBOL', 'native.threads.count': 8,
        # Hybrid reserves at least 8192 eye tasks. At exactly 8192 total
        # the engine has no light tail and safely disables GPU hybrid.
        # 16384 enables both actual device populations without CPU fallback.
        'opencl.task.count': 16384, 'opencl.native.threads.count': 0,
        'opencl.cpu.use': False, 'opencl.gpu.use': True,
        'renderengine.seed': 131, 'batch.haltspp': 0,
        'path.cyclesbssrdf.experimental.enable': True,
        'path.cyclesbssrdf.experimental.device.enable': True,
        'path.cyclesbssrdf.experimental.adjoint.enable': True,
        'path.hybridbackforward.adaptivecaustic': True,
        'path.lighttracing.auto': False, 'path.lighttracing.only': False,
        'path.mnee.enable': False, 'path.mnee.auto': False,
        'path.regularization.auto': False,
        'path.vertexconnection.enable': False,
        'path.photongi.caustic.enabled': False, 'path.photongi.indirect.enabled': False,
        'path.pathdepth.total': 4, 'path.forceblackbackground.enable': True,
        'film.outputs.0.type': 'RGB', 'film.outputs.0.filename': 'unused.exr',
        'film.outputs.1.type': 'MATERIAL_ID', 'film.outputs.1.filename': 'unused.png',
        'film.outputs.2.type': 'ALPHA', 'film.outputs.2.filename': 'unused-alpha.exr'}
records = []


def save(complete=False):
    (folder / 'metrics.json').write_text(json.dumps({
        'native_sha256': identity['native_sha256'],
        'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'production_acceptance': False, 'gate': .03, 'minimum_eye_samples': 65536,
        'minimum_light_samples': 81920, 'complete': complete, 'records': records}, indent=2) + '\n')


def render(scn, label, engine, *, hybrid=False, partition=.8, mode='off', psr=0., spectral=False):
    values = base | {'renderengine.type': engine, 'pathocl.wavefront': mode,
                     'path.hybridbackforward.enable': hybrid,
                     'path.hybridbackforward.partition': partition,
                     'path.lighttracing.enable': hybrid and engine == 'PATHOCL',
                     'path.regularization.sigma': psr, 'path.spectral.enable': spectral}
    (folder / (label + '-config.cfg')).write_text(props(values).ToString())
    ses = slc.RenderSession(slc.RenderConfig(props(values), scn))
    ses.Start()
    start, next_report = time.monotonic(), 20.
    try:
        while True:
            ses.UpdateStats()
            stats = ses.GetStats()
            eye = stats.Get('stats.renderengine.pass.eye').GetInt()
            light = stats.Get('stats.renderengine.pass.light').GetInt()
            elapsed = time.monotonic() - start
            if elapsed >= next_report:
                print('BSSRDF_DEVICE_HYBRID_PROGRESS', label, eye, light, round(elapsed, 2), flush=True)
                next_report += 20.
            if hybrid and partition < 1. and elapsed >= 20.:
                assert light > 0, ('configured hybrid must execute a light pass', label, eye, light)
            if eye >= 65536 and (not hybrid or partition == 1. or light >= 81920):
                break
            assert elapsed < 1200, (label, eye, light)
            time.sleep(.1)
    finally:
        ses.Stop()
    result = {}
    for kind, count in (('RGB', 3), ('ALPHA', 1)):
        data = np.empty(W * H * count, np.float32)
        ses.GetFilm().GetOutputFloat(getattr(slc.FilmOutputType, kind), data, 0, True)
        assert np.isfinite(data).all(), (label, kind)
        result[kind] = data.reshape(H, W, count)
    ids = np.empty(W * H, np.uint32)
    ses.GetFilm().GetOutputUInt(slc.FilmOutputType.MATERIAL_ID, ids, 0, True)
    result['MATERIAL_ID'] = ids.reshape(H, W)
    np.savez_compressed(folder / (label + '.npz'), **result)
    (folder / (label + '-render.json')).write_text(json.dumps({
        'eye': eye, 'light': light, 'seconds': time.monotonic() - start,
        'native_sha256': identity['native_sha256'], 'complete': True}, indent=2) + '\n')
    print('BSSRDF_DEVICE_HYBRID_RENDER', label, eye, light, flush=True)
    return result, {'eye': eye, 'light': light}


for case, parameters, psr, spectral in (
        ('mirror-rough', {'roughness': .4}, 0., False),
        ('mirror-rough-psr', {'roughness': .4}, .03, False),
        ('mirror-colored-spectral', {'roughness': .4, 'color': (.55, .2, .08),
                                    'radius': (1., .3, .2)}, 0., True)):
    scn = body(parameters)
    (folder / (case + '-scene.scn')).write_text(scn.ToProperties().ToString())
    eye, eye_counts = render(scn, case + '-cpu-eye', 'PATHCPU', psr=psr, spectral=spectral)
    mask = eye['MATERIAL_ID'] == 991
    interior = mask.copy()
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            interior &= np.roll(np.roll(mask, dy, 0), dx, 1)
    assert interior.sum() >= 100
    a = eye['RGB'][interior].mean(axis=0).astype(np.float64)
    omitted = None
    if case == 'mirror-rough':
        hole, _ = render(scn, case + '-cpu-eye-hole', 'PATHCPU', hybrid=True, partition=1.)
        omitted = 1. - float(hole['RGB'][interior].mean() / eye['RGB'][interior].mean())
        assert omitted > .05, ('fixture must contain meaningful caustics', omitted)
    for mode in ('off', 'on'):
        hybrid, counts = render(scn, case + '-' + mode, 'PATHOCL', hybrid=True,
                                mode=mode, psr=psr, spectral=spectral)
        b = hybrid['RGB'][interior].mean(axis=0).astype(np.float64)
        error = float(np.max(np.abs(a - b) / np.maximum(a, 1e-4)))
        alpha = hybrid['ALPHA'][interior]
        detail = {'case': case + '-' + mode, 'passed': error < .03,
                  'eye_rgb_mean': a.tolist(), 'hybrid_rgb_mean': b.tolist(),
                  'max_relative_channel_error': error, 'pixels': int(interior.sum()),
                  'psr': psr, 'spectral': spectral, 'eye_counts': eye_counts,
                  'hybrid_counts': counts, 'alpha_min': float(alpha.min()),
                  'alpha_max': float(alpha.max())}
        if omitted is not None:
            detail['verified_omitted_caustic_fraction'] = omitted
        records.append(detail)
        save()
        assert error < .03 and np.allclose(alpha, 1., atol=1e-5), detail
        print('BSSRDF_DEVICE_HYBRID_PASS', detail, flush=True)

save(complete=True)
print('BSSRDF_DEVICE_HYBRID_COMPLETE', len(records), flush=True)
