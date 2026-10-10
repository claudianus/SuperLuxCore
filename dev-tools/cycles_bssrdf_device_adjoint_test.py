# SPDX-License-Identifier: Apache-2.0
"""Independent device adjoint transport, identities and unsupported contexts.

Run with Blender Python, a complete isolated wheel on PYTHONPATH, and the
SUPERLUXCORE_BSSRDF_IDENTITY / SUPERLUXCORE_AUDIT_DIR variables. The existing
50 CPU contracts run first. Budgets, regions and the 3% mean gate are fixed
before rendering. This is private uniform-coefficient diagnostic coverage;
production adapters, sharp device connections and mixed reverse walks remain
separate work. No performance claim follows from these small fixtures.
"""
import ast
import hashlib
import json
import os
from pathlib import Path
import runpy
import struct
import time

import numpy as np

ns = runpy.run_path(str(Path(__file__).with_name('cycles_bssrdf_transport_test.py')))
slc, props, scene = ns['slc'], ns['properties'], ns['scene']
identity = ns['identity']
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR']) / 'device-adjoint'
folder.mkdir(parents=True, exist_ok=True)
W = H = 32
center = np.s_[6:26, 6:26]
records = []
GATE = .03
EYE_SPP, LIGHT_SPP = 8192, 65536

hash_ns = {'struct': struct}
tree = ast.parse(Path(__file__).with_name('e40_cryptomatte_test.py').read_text())
nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and
         n.name in ('murmur3_32', 'hash_to_float', 'name_id')]
exec(compile(ast.Module(body=nodes, type_ignores=[]), '<crypto-name-reference>', 'exec'), hash_ns)


def save(complete=False):
    (folder / 'metrics.json').write_text(json.dumps({
        'native_sha256': identity['native_sha256'],
        'test_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        'cpu_contracts': len(ns['records']), 'production_acceptance': False,
        'gate': GATE, 'minimum_eye_samples': EYE_SPP,
        'minimum_light_samples': LIGHT_SPP,
        'complete': complete, 'records': records}, indent=2) + '\n')


def record(case, **data):
    records.append({'case': case, 'passed': True, **data})
    save()
    print('BSSRDF_DEVICE_ADJOINT_PASS', case, data, flush=True)


def body(values=None, **geometry):
    values = values or {}
    scn = scene(**geometry)
    scn.Parse(props({'scene.materials.body.type': 'cyclesbssrdf',
                     'scene.materials.body.kd': values.get('color', (.45, .45, .45)),
                     'scene.materials.body.radius': values.get('radius', (.3, .3, .3)),
                     'scene.materials.body.scale': 1.,
                     'scene.materials.body.ior': values.get('ior', 1.4),
                     'scene.materials.body.roughness': values.get('roughness', .4),
                     'scene.materials.body.anisotropy': values.get('g', 0.),
                     'scene.materials.body.id': 991}))
    return scn


BASE = {
    'film.width': W, 'film.height': H,
    'film.imagepipelines.0.0.type': 'NOP', 'film.filter.type': 'NONE',
    'native.threads.count': 8, 'sampler.type': 'SOBOL',
    'opencl.cpu.use': False, 'opencl.gpu.use': True,
    'opencl.native.threads.count': 0, 'opencl.task.count': 2048,
    'path.cyclesbssrdf.experimental.enable': True,
    'path.cyclesbssrdf.experimental.device.enable': True,
    'path.lighttracing.auto': False, 'path.hybridbackforward.enable': False,
    'path.mnee.enable': False, 'path.mnee.auto': False,
    'path.regularization.auto': False, 'path.regularization.sigma': 0.,
    'path.vertexconnection.enable': False,
    'path.photongi.caustic.enabled': False, 'path.photongi.indirect.enabled': False,
    'path.forceblackbackground.enable': True,
}
for index, kind in enumerate(('RGB', 'ALPHA', 'MATERIAL_ID', 'CRYPTOMATTE_MATERIAL', 'CRYPTOMATTE_OBJECT')):
    BASE[f'film.outputs.{index}.type'] = kind
    BASE[f'film.outputs.{index}.filename'] = 'unused-' + kind + ('.png' if kind == 'MATERIAL_ID' else '.exr')


def config(engine='PATHOCL', *, mode='off', eye=False, spectral=False, seed=131, spp=None):
    is_eye = engine == 'PATHCPU' or eye
    return BASE | {
        'renderengine.type': engine, 'renderengine.seed': seed,
        'pathocl.wavefront': mode,
        'path.spectral.enable': spectral,
        'path.cyclesbssrdf.experimental.adjoint.enable': not is_eye,
        'path.lighttracing.enable': not is_eye,
        'path.lighttracing.only': not is_eye,
        # Match three physical scattering vertices. Eye's final
        # non-primary vertex performs no NEE, while light connects it.
        'path.pathdepth.total': 4 if is_eye else 3,
        'batch.haltspp': spp or (EYE_SPP if is_eye else LIGHT_SPP),
    }


def reject(case, scn, overrides, phrase):
    try:
        slc.RenderSession(slc.RenderConfig(props(config() | overrides), scn))
    except RuntimeError as error:
        assert phrase in str(error), (case, str(error))
        record(case, message=str(error))
    else:
        raise AssertionError(case + ' must reject before render workers')


reject('device-opt-in-required', body(), {'path.cyclesbssrdf.experimental.device.enable': False}, 'Experimental cyclesbssrdf')
reject('adjoint-opt-in-required', body(), {'path.cyclesbssrdf.experimental.adjoint.enable': False}, 'Experimental cyclesbssrdf')
reject('sharp-nonlocal-remains-gated', body({'roughness': 0.}), {}, 'sharp boundary camera connections')
reject('mixed-reverse-remains-gated', body(nested_mix=True), {}, 'adjoint mixed')
reject('partitioned-reverse-remains-gated', body(partitioned=True), {}, 'partitioned groups')
reject('vertex-connection-remains-gated', body(), {'path.vertexconnection.enable': True}, 'vertex connection')
reject('bidir-remains-gated', body(), {'renderengine.type': 'BIDIRCPU'}, 'Experimental cyclesbssrdf')
for kind in ('caustic', 'indirect'):
    reject(kind + '-cache-remains-gated', body(), {'path.photongi.' + kind + '.enabled': True}, 'caches')
textured = body()
textured.Parse(props({'scene.textures.pattern.type': 'checkerboard3d',
                      'scene.textures.pattern.texture1': .45, 'scene.textures.pattern.texture2': .65,
                      'scene.materials.body.type': 'cyclesbssrdf', 'scene.materials.body.roughness': .4,
                      'scene.materials.body.kd': 'pattern'}))
reject('textured-reverse-remains-gated', textured, {}, 'textured coefficients')
for field, value in (('bumptex', .1), ('transparency', .2)):
    scn = body()
    scn.Parse(props({'scene.materials.body.type': 'cyclesbssrdf',
                     'scene.materials.body.roughness': .4, 'scene.materials.body.' + field: value}))
    reject(field + '-reverse-remains-gated', scn, {}, 'adjoint mixed')


def render(scn, label, engine='PATHOCL', **options):
    values = config(engine, **options)
    (folder / (label + '-config.cfg')).write_text(props(values).ToString())
    ses = slc.RenderSession(slc.RenderConfig(props(values), scn))
    ses.Start()
    start = time.monotonic()
    next_report = 20.
    try:
        while True:
            ses.UpdateStats()
            passes = ses.GetStats().Get('stats.renderengine.pass').GetInt()
            elapsed = time.monotonic() - start
            if elapsed >= next_report:
                print('BSSRDF_DEVICE_ADJOINT_PROGRESS', label, passes, round(elapsed, 2), flush=True)
                next_report += 20.
            if passes >= values['batch.haltspp']:
                break
            assert elapsed < 600, (label, passes)
            time.sleep(.05)
    finally:
        ses.Stop()
    result = {}
    for kind, components in (('RGB', 3), ('ALPHA', 1), ('CRYPTOMATTE_MATERIAL', 12), ('CRYPTOMATTE_OBJECT', 12)):
        data = np.empty(W * H * components, np.float32)
        ses.GetFilm().GetOutputFloat(getattr(slc.FilmOutputType, kind), data, 0, True)
        assert np.isfinite(data).all(), (label, kind)
        result[kind] = data.reshape(H, W, components)
    ids = np.empty(W * H, np.uint32)
    ses.GetFilm().GetOutputUInt(slc.FilmOutputType.MATERIAL_ID, ids, 0, True)
    result['MATERIAL_ID'] = ids.reshape(H, W)
    assert result['ALPHA'].min() >= 0. and result['ALPHA'].max() <= 1.00001, label
    np.savez_compressed(folder / (label + '.npz'), **result)
    (folder / (label + '-render.json')).write_text(json.dumps({
        'passes': passes, 'seconds': time.monotonic() - start,
        'native_sha256': identity['native_sha256'], 'complete': True}, indent=2) + '\n')
    print('BSSRDF_DEVICE_ADJOINT_RENDER', label, passes, flush=True)
    return result, passes


def identities(image, names=('body',)):
    for kind in ('CRYPTOMATTE_MATERIAL', 'CRYPTOMATTE_OBJECT'):
        ids = image[kind][..., 0::2]
        seen = set(ids[ids != 0.].tolist())
        expected = {hash_ns['name_id'](name) for name in names}
        assert seen and seen <= expected, (kind, seen, expected)


def compare(case, eye, light, eye_pass, light_pass, mask=center):
    a, b = [p['RGB'][mask].mean(axis=0 if isinstance(mask, np.ndarray) else (0, 1)).astype(np.float64)
            for p in (eye, light)]
    error = float(np.max(np.abs(a - b) / np.maximum(a, 1e-4)))
    detail = {'eye_rgb_mean': a.tolist(), 'light_rgb_mean': b.tolist(),
              'max_relative_channel_error': error,
              'eye_pass': eye_pass, 'light_pass': light_pass}
    (folder / (case + '-comparison.json')).write_text(json.dumps(detail, indent=2) + '\n')
    assert error < GATE, (case, detail)
    record(case, **detail)


for name, values, spectral, seed in (
        ('gray', {}, False, 131), ('gray-independent-seed', {}, False, 817),
        ('low-ior', {'ior': 1.1}, False, 131),
        ('high-ior', {'ior': 3.8, 'roughness': .8}, False, 131),
        ('forward-phase', {'g': .5, 'roughness': .6}, False, 131),
        ('backward-phase', {'g': -.5, 'roughness': .6}, False, 131),
        ('low-albedo-compensation', {'color': (.015, .03, .05)}, False, 131),
        ('colored-rgb', {'color': (.55, .2, .08)}, False, 131),
        ('partial-radius-rgb', {'radius': (0., .3, .3)}, False, 131),
        ('all-local', {'radius': (0., 0., 0.), 'roughness': 0.}, False, 131),
        ('colored-spectral', {'color': (.55, .2, .08)}, True, 131),
        ('all-local-spectral', {'radius': (0., 0., 0.), 'roughness': 0.}, True, 131)):
    scn = body(values)
    (folder / (name + '-scene.scn')).write_text(scn.ToProperties().ToString())
    eye, eye_pass = render(scn, name + '-cpu-eye', 'PATHCPU', spectral=spectral, seed=seed)
    identities(eye)
    for mode in ('off', 'on'):
        light, light_pass = render(scn, name + '-' + mode, mode=mode, spectral=spectral, seed=seed)
        identities(light)
        compare(name + '-' + mode, eye, light, eye_pass, light_pass)

for mode in ('off', 'on'):
    black, passes = render(body({'color': (0., 0., 0.)}), 'black-' + mode,
                           mode=mode, spp=16384)
    assert np.max(np.abs(black['RGB'])) == 0.
    record('black-absorption-' + mode, passes=passes)

scn = body()
scn.Parse(props({'scene.materials.floor.type': 'matte', 'scene.materials.floor.kd': .6,
                 'scene.materials.floor.id': 123, 'scene.objects.floor.material': 'floor',
                 'scene.objects.floor.vertices': (-4., -4., -1.2, 4., -4., -1.2,
                                                  4., 4., -1.2, -4., 4., -1.2),
                 'scene.objects.floor.faces': (0, 1, 2, 0, 2, 3),
                 'scene.camera.lookat.orig': (0., -4., 2.),
                 'scene.camera.lookat.target': (0., 0., -.5)}))
(folder / 'continuation-scene.scn').write_text(scn.ToProperties().ToString())
eye, eye_pass = render(scn, 'continuation-cpu-eye', 'PATHCPU')
identities(eye, ('body', 'floor'))
for mode in ('off', 'on'):
    light, light_pass = render(scn, 'continuation-' + mode, mode=mode)
    identities(light, ('body', 'floor'))
    for mat in (991, 123):
        mask = eye['MATERIAL_ID'] == mat
        interior = mask.copy()
        for axis in (0, 1):
            interior &= np.roll(mask, 1, axis) & np.roll(mask, -1, axis)
        interior[[0, -1], :] = False
        interior[:, [0, -1]] = False
        assert interior.sum() > 20, (mat, interior.sum())
        compare('continuation-' + str(mat) + '-' + mode,
                eye, light, eye_pass, light_pass, mask=interior)

save(complete=True)
print('BSSRDF_DEVICE_ADJOINT_COMPLETE', len(records), flush=True)
