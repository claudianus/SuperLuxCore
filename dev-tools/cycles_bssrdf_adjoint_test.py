# SPDX-License-Identifier: Apache-2.0
"""Independent LIGHTCPU/eye transport and surface-identity contracts.

Use Blender Python with the complete isolated wheel on PYTHONPATH and the
existing identity/audit environment variables. This runs the 50 existing CPU
contracts first. Adjoint transport remains an explicit pure-LIGHTCPU diagnostic
for uniform coefficients and rough camera boundaries, not production acceptance.
"""
import ast
import json
import os
from pathlib import Path
import runpy
import struct
import time

import numpy as np

ns = runpy.run_path(str(Path(__file__).with_name('cycles_bssrdf_transport_test.py')))
slc, props, scene = ns['slc'], ns['properties'], ns['scene']
W, H, center = ns['W'], ns['H'], ns['center']
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])
records = []

# Reuse the independent Cryptomatte-name reference without executing its suite.
hash_ns = {'struct': struct}
tree = ast.parse(Path(__file__).with_name('e40_cryptomatte_test.py').read_text())
nodes = [n for n in tree.body if isinstance(n, ast.FunctionDef) and
         n.name in ('murmur3_32', 'hash_to_float', 'name_id')]
exec(compile(ast.Module(body=nodes, type_ignores=[]), '<crypto-name-reference>', 'exec'), hash_ns)


def record(case, **data):
    records.append({'case': case, 'passed': True, **data})
    (folder / 'adjoint-metrics.json').write_text(json.dumps({
        'native_sha256': ns['identity']['native_sha256'], 'cpu_contracts': len(ns['records']),
        'production_acceptance': False, 'hybrid_or_mis_implemented': False,
        'complete': False, 'records': records}, indent=2) + '\n')
    print('BSSRDF_ADJOINT_PASS', case, data, flush=True)


def body(values=None, *, partitioned=False, nested_mix=False):
    scn = scene(grouped=True, partitioned=partitioned, nested_mix=nested_mix)
    values = values or {}
    scn.Parse(props({'scene.materials.body.type': 'cyclesbssrdf',
                     'scene.materials.body.kd': values.get('color', (.45, .45, .45)),
                     'scene.materials.body.radius': values.get('radius', (.3, .3, .3)),
                     'scene.materials.body.scale': 1.,
                     'scene.materials.body.ior': values.get('ior', 1.4),
                     'scene.materials.body.roughness': values.get('roughness', .4),
                     'scene.materials.body.anisotropy': values.get('g', 0.),
                     'scene.materials.body.id': 991}))
    return scn


def config(engine, *, spectral=False, spp=None):
    return ns['BASE'] | {'renderengine.type': engine,
                         'path.cyclesbssrdf.experimental.adjoint.enable': engine == 'LIGHTCPU',
                         'path.spectral.enable': spectral,
                         # The eye tracer's terminal non-primary vertex
                         # performs no NEE. LIGHTCPU connects its last one.
                         # Match three physical scattering vertices.
                         'path.pathdepth.total': 3 if engine == 'LIGHTCPU' else 4,
                         # LIGHTCPU has no zero-bounce environment background.
                         # Isolate transport from background filter spill in
                         # these small fixtures; 720p visual tests keep filters.
                         'film.filter.type': 'NONE',
                         'batch.haltspp': spp or (1024 if engine == 'PATHCPU' else 8192),
                         'film.outputs.8.type': 'CRYPTOMATTE_MATERIAL',
                         'film.outputs.8.filename': 'unused-material-crypto.exr',
                         'film.outputs.9.type': 'CRYPTOMATTE_OBJECT',
                         'film.outputs.9.filename': 'unused-object-crypto.exr'}


def session(scn, engine='LIGHTCPU', **overrides):
    return slc.RenderSession(slc.RenderConfig(props(config(engine) | overrides), scn))


def reject(case, scn, overrides, phrase):
    try:
        session(scn, **overrides)
    except RuntimeError as error:
        assert phrase in str(error), (case, str(error))
        record(case, message=str(error))
    else:
        raise AssertionError(case + ' must reject before render workers')


reject('light-opt-in-required', body(), {'path.cyclesbssrdf.experimental.adjoint.enable': False}, 'Experimental cyclesbssrdf')
reject('sharp-boundary-rejected', body({'roughness': 0.}), {}, 'internal-vertex/manifold')
reject('mixed-kernel-rejected', body(nested_mix=True), {}, 'adjoint mixed')
reject('partitioned-kernel-rejected', body(partitioned=True), {}, 'partitioned groups')
bumped = body()
bumped.Parse(props({'scene.materials.body.type': 'cyclesbssrdf', 'scene.materials.body.roughness': .4,
                    'scene.materials.body.bumptex': .1}))
reject('normal-bump-kernel-rejected', bumped, {}, 'Normal/Bump')
textured = body()
textured.Parse(props({'scene.textures.checker.type': 'checkerboard3d',
                      'scene.textures.checker.texture1': .45, 'scene.textures.checker.texture2': .65,
                      'scene.materials.body.type': 'cyclesbssrdf', 'scene.materials.body.roughness': .4,
                      'scene.materials.body.kd': 'checker'}))
reject('textured-kernel-rejected', textured, {}, 'textured coefficients')
reject('cpu-vertex-connect-bypass-rejected', body(),
       {'engine': 'PATHCPU', 'path.vertexconnect.enable': True}, 'vertex connections')
reject('adjoint-vertex-connect-rejected', body(), {'path.vertexconnect.enable': True}, 'vertex connections')


def render(scn, engine, spectral=False, *, spp=None, seed=131):
    values = config(engine, spectral=spectral, spp=spp) | {'renderengine.seed': seed}
    ses = slc.RenderSession(slc.RenderConfig(props(values), scn))
    ses.Start()
    start = time.monotonic()
    try:
        while True:
            ses.UpdateStats()
            passes = ses.GetStats().Get('stats.renderengine.pass').GetInt()
            if passes >= values['batch.haltspp']:
                break
            assert time.monotonic() - start < 180, (engine, passes)
            time.sleep(.05)
    finally:
        ses.Stop()
    result = {'passes': passes}
    for kind, components in (('RGB', 3), ('CRYPTOMATTE_MATERIAL', 12), ('CRYPTOMATTE_OBJECT', 12)):
        data = np.empty(W * H * components, dtype=np.float32)
        ses.GetFilm().GetOutputFloat(getattr(slc.FilmOutputType, kind), data, 0, True)
        assert np.isfinite(data).all(), (engine, kind)
        result[kind] = data.reshape(H, W, components)
    ids = np.empty(W * H, np.uint32)
    ses.GetFilm().GetOutputUInt(slc.FilmOutputType.MATERIAL_ID, ids, 0, True)
    result['MATERIAL_ID'] = ids.reshape(H, W)
    return result


def identities(image, material_names=('body',), object_names=('body',)):
    for kind, names in (('CRYPTOMATTE_MATERIAL', material_names), ('CRYPTOMATTE_OBJECT', object_names)):
        ids = image[kind][..., 0::2]
        seen = set(ids[ids != 0.].tolist())
        expected = {hash_ns['name_id'](name) for name in names}
        assert seen and seen <= expected, (kind, seen, expected)


for name, values, spectral in (
        ('gray', {}, False), ('low-ior', {'ior': 1.1, 'roughness': .4}, False),
        ('high-ior', {'ior': 3.8, 'roughness': .8}, False),
        ('forward-phase', {'g': .5, 'roughness': .6}, False),
        ('backward-phase', {'g': -.5, 'roughness': .6}, False),
        ('colored-rgb', {'color': (.55, .2, .08)}, False),
        ('partial-radius-rgb', {'radius': (0., .3, .3)}, False),
        ('all-local', {'radius': (0., 0., 0.), 'roughness': 0.}, False),
        ('colored-spectral', {'color': (.55, .2, .08)}, True),
        ('all-local-spectral', {'radius': (0., 0., 0.), 'roughness': 0.}, True)):
    scn = body(values)
    # At IOR close to one, the inverse camera lobe has very narrow angular
    # support. Use the independently checked larger photon budget, while
    # retaining the same 3% energy gate.
    eye = render(scn, 'PATHCPU', spectral, spp=4096 if name == 'low-ior' else None)
    light = render(scn, 'LIGHTCPU', spectral, spp=65536 if name == 'low-ior' else None)
    for engine, data in (('eye', eye), ('light', light)):
        np.savez_compressed(folder / (name + '-' + engine + '.npz'), **data)
    identities(eye)
    identities(light)
    a, b = [p['RGB'][center].mean(axis=(0, 1)).astype(np.float64) for p in (eye, light)]
    error = float(np.max(np.abs(a - b) / np.maximum(a, .02)))
    assert error < .03, (name, a, b, error)
    record(name + '-independent-transport-and-surface-identity', spectral=spectral,
           eye_rgb_mean=a.tolist(), light_rgb_mean=b.tolist(), max_relative_channel_error=error,
           eye_pass=eye['passes'], light_pass=light['passes'])

black = render(body({'color': (0., 0., 0.)}), 'LIGHTCPU')
assert np.max(np.abs(black['RGB'])) == 0.
record('black-nonlocal-closure-has-no-light-splat')

# A continuation beyond the inverse boundary: light -> SSS -> ordinary floor
# -> camera, compared with camera -> floor -> SSS -> light. The floor ROI is
# defined by the independent eye path's first-hit material ID.
scn = body()
scn.Parse(props({'scene.materials.floor.type': 'matte', 'scene.materials.floor.kd': .6,
                 'scene.materials.floor.id': 123, 'scene.objects.floor.material': 'floor',
                 'scene.objects.floor.vertices': (-4., -4., -1.2, 4., -4., -1.2,
                                                  4., 4., -1.2, -4., 4., -1.2),
                 'scene.objects.floor.faces': (0, 1, 2, 0, 2, 3),
                 'scene.camera.lookat.orig': (0., -4., 2.),
                 'scene.camera.lookat.target': (0., 0., -.5)}))
eye, light = render(scn, 'PATHCPU', spp=4096), render(scn, 'LIGHTCPU', spp=32768)
np.savez_compressed(folder / 'continuation-eye.npz', **eye)
np.savez_compressed(folder / 'continuation-light.npz', **light)
identities(eye, ('body', 'floor'), ('body', 'floor'))
identities(light, ('body', 'floor'), ('body', 'floor'))
for mat in (991, 123):
    mask = eye['MATERIAL_ID'] == mat
    # Avoid filter pixels crossing an object/background boundary.
    interior = mask.copy()
    for axis in (0, 1):
        interior &= np.roll(mask, 1, axis) & np.roll(mask, -1, axis)
    interior[[0, -1], :] = False
    interior[:, [0, -1]] = False
    assert interior.sum() > 100, (mat, interior.sum())
    a, b = [p['RGB'][interior].mean(axis=0).astype(np.float64) for p in (eye, light)]
    error = float(np.max(np.abs(a - b) / np.maximum(a, .02)))
    assert error < .03, (mat, a, b, error)
    record('sss-to-ordinary-surface-continuation-' + str(mat), eye_rgb_mean=a.tolist(),
           light_rgb_mean=b.tolist(), max_relative_channel_error=error, pixels=int(interior.sum()))

scn = body()
ses = session(scn)
ses.Start()
ses.BeginSceneEdit()
scn.Parse(props({'scene.materials.body.type': 'cyclesbssrdf', 'scene.materials.body.roughness': 0.}))
try:
    ses.EndSceneEdit()
except RuntimeError as error:
    assert 'internal-vertex/manifold' in str(error), str(error)
else:
    raise AssertionError('Live edits must preserve adjoint preflight')
scn.Parse(props({'scene.materials.body.type': 'cyclesbssrdf', 'scene.materials.body.roughness': .4}))
ses.EndSceneEdit()
ses.Stop()
record('live-adjoint-preflight-and-repair-recovery')

(folder / 'adjoint-metrics.json').write_text(json.dumps({
    'native_sha256': ns['identity']['native_sha256'], 'cpu_contracts': len(ns['records']),
    'production_acceptance': False, 'hybrid_or_mis_implemented': False, 'complete': True,
    'records': records}, indent=2) + '\n')
print('BSSRDF_ADJOINT_COMPLETE', len(records), flush=True)
