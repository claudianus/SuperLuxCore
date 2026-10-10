# SPDX-License-Identifier: Apache-2.0
"""Dependency-order and live-edit material regressions with an exact staged wheel.

An existing parent is rewired to a subtree created later. Replacing a leaf in
that subtree must render like a fresh scene, including nonlocal and null flags.
Small renders verify update contracts, not general production acceptance.
"""
import hashlib
import json
import os
from pathlib import Path
import time

import numpy as np
import pysuperluxcore as slc

identity = json.loads(Path(os.environ['SUPERLUXCORE_BSSRDF_IDENTITY']).read_text())
assert hashlib.sha256(Path(slc.pysuperluxcore.__file__).read_bytes()).hexdigest() == identity['native_sha256']
for filename, digest in identity.get('source_files', {}).items():
    assert hashlib.sha256(Path(filename).read_bytes()).hexdigest() == digest, filename
slc.Init()
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])
folder.mkdir(parents=True, exist_ok=True)
W = H = 64
center = np.s_[18:46, 18:46]
records = []
metal_mask = ''


def props(values):
    result = slc.Properties()
    for name, value in values.items():
        result.Set(slc.Property(name, list(value) if isinstance(value, tuple) else value))
    return result


def leaf(kind):
    result = {'scene.materials.late.type': kind,
              'scene.materials.late.kd': .45,
              'scene.materials.late.radius': (.3, .3, .3),
              'scene.materials.late.scale': 1.,
              'scene.materials.late.ior': 1.4}
    if kind == 'emissive':
        result.update({'scene.materials.late.type': 'matte',
                       'scene.materials.late.kd': 0., 'scene.materials.late.emission': 1.})
    return result


def blend(name, first, second, amount=0., additive=False):
    return {f'scene.materials.{name}.type': 'mix',
            f'scene.materials.{name}.material1': first,
            f'scene.materials.{name}.material2': second,
            f'scene.materials.{name}.amount': amount,
            f'scene.materials.{name}.additive': additive}


def scene(kind='matte', wrapper='mix'):
    result = slc.Scene()
    result.Parse(props({'scene.materials.bridge.type': 'matte', 'scene.materials.bridge.kd': .45,
                        'scene.materials.flat.type': 'matte', 'scene.materials.flat.kd': .2,
                        'scene.materials.black.type': 'matte', 'scene.materials.black.kd': 0.}))
    result.Parse(props(blend('root', 'bridge', 'black', additive=wrapper == 'add')))
    if wrapper == 'twosided':
        result.Parse(props({'scene.materials.wrap.type': 'twosided',
                            'scene.materials.wrap.frontmaterial': 'root',
                            'scene.materials.wrap.backmaterial': 'root'}))
    # This subtree did not exist when the parent materials were constructed.
    result.Parse(props(leaf(kind)))
    result.Parse(props(blend('late_mix', 'late', 'flat', .35)))
    result.Parse(props(blend('bridge', 'late_mix', 'black')))
    result.Parse(props({
        'scene.camera.type': 'orthographic',
        'scene.camera.lookat.orig': (0., -4., 0.), 'scene.camera.lookat.target': (0., 0., 0.),
        'scene.camera.up': (0., 0., 1.), 'scene.camera.screenwindow': (-1.3, 1.3, -1.3, 1.3),
        'scene.objects.body.material': 'wrap' if wrapper == 'twosided' else 'root',
        'scene.objects.body.vertices': (-1., -1., -1., 1., -1., -1., 1., 1., -1., -1., 1., -1.,
                                       -1., -1., 1., 1., -1., 1., 1., 1., 1., -1., 1., 1.),
        'scene.objects.body.faces': (0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
                                    0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
                                    2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7),
        'scene.lights.env.type': 'constantinfinite', 'scene.lights.env.color': (1., 1., 1.)}))
    return result


def session(scn, device, overrides=None):
    config = {'film.width': W, 'film.height': H, 'film.imagepipelines.0.0.type': 'NOP',
              'film.outputs.0.type': 'RGB', 'film.outputs.0.filename': 'unused.exr',
              'film.outputs.1.type': 'ALPHA', 'film.outputs.1.filename': 'unused-alpha.exr',
              'renderengine.type': 'PATHCPU' if device == 'CPU' else 'PATHOCL',
              'sampler.type': 'SOBOL', 'batch.haltspp': 1024, 'native.threads.count': 1,
              'renderengine.seed': 131, 'path.pathdepth.total': 4,
              'path.spectral.enable': False, 'path.hybridbackforward.enable': False,
              'path.lighttracing.enable': False, 'path.lighttracing.auto': False,
              'path.mnee.enable': False, 'path.photongi.caustic.enabled': False,
              'path.photongi.indirect.enabled': False, 'path.cyclesbssrdf.experimental.enable': True,
              'path.cyclesbssrdf.experimental.device.enable': device != 'CPU',
              'opencl.cpu.use': False, 'opencl.gpu.use': True, 'opencl.task.count': 8192,
              'pathocl.wavefront': 'on' if device == 'METAL_ON' else 'off'}
    if device != 'CPU':
        config['opencl.devices.select'] = metal_mask
    config.update(overrides or {})
    return slc.RenderSession(slc.RenderConfig(props(config), scn))


def pixels(ses):
    start = time.monotonic()
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get('stats.renderengine.pass').GetInt() >= 1024:
            break
        assert time.monotonic() - start < 120, 'material edit timed out'
        time.sleep(.05)
    result = {}
    for kind, channels in (('RGB', 3), ('ALPHA', 1)):
        data = np.empty(W * H * channels, np.float32)
        ses.GetFilm().GetOutputFloat(getattr(slc.FilmOutputType, kind), data, 0, True)
        result[kind] = data.reshape(H, W, channels)
        assert np.isfinite(data).all(), kind
    return result


def render(scn, device):
    ses = session(scn, device)
    try:
        ses.Start()
        return pixels(ses)
    finally:
        ses.Stop()


def compare(case, measured, expected):
    rgb = measured['RGB'][center].mean(axis=(0, 1))
    reference = expected['RGB'][center].mean(axis=(0, 1))
    rgb_error = float(np.abs(rgb - reference).max())
    alpha = float(measured['ALPHA'][center].mean())
    reference_alpha = float(expected['ALPHA'][center].mean())
    alpha_error = abs(alpha - reference_alpha)
    passed = rgb_error < .015 and alpha_error < .008
    record = {'case': case, 'passed': passed, 'rgb_mean': rgb.tolist(),
              'fresh_rgb_mean': reference.tolist(), 'max_rgb_mean_error': rgb_error,
              'alpha_mean': alpha, 'fresh_alpha_mean': reference_alpha,
              'alpha_mean_error': alpha_error}
    records.append(record)
    save()
    print('MATERIAL_GRAPH_EDIT', record, flush=True)
    assert passed, record


def save():
    (folder / 'material-edit-metrics.json').write_text(json.dumps({
        'native_sha256': identity['native_sha256'], 'experimental': True,
        'production_acceptance': False, 'records': records}, indent=2) + '\n')


for device in os.environ.get('SUPERLUXCORE_GRAPH_EDIT_DEVICES', 'CPU,METAL_OFF,METAL_ON').split(','):
    if device != 'CPU':
        devices = slc.GetOpenCLDeviceDescs()
        names = devices.GetAllNames('opencl.device')
        indices = sorted({int(name.split('.')[2]) for name in names})
        metal_mask = ''.join('1' if devices.Get(f'opencl.device.{index}.type').GetString() == 'METAL_GPU'
                             else '0' for index in indices)
        assert '1' in metal_mask, str(devices)
        print('MATERIAL_GRAPH_EDIT_DEVICES', str(devices), 'selected', metal_mask, flush=True)
    for wrapper in ('mix', 'add', 'twosided'):
        edited = scene(wrapper=wrapper)
        for kind in ('cyclesbssrdf', 'null', 'matte', 'cyclesbssrdf', 'emissive', 'matte'):
            edited.Parse(props(leaf(kind)))
            compare(device + '-offline-' + wrapper + '-' + kind,
                    render(edited, device), render(scene(kind, wrapper), device))
    edited = scene(wrapper='twosided')
    ses = session(edited, device)
    try:
        ses.Start()
        pixels(ses)
        for kind in ('cyclesbssrdf', 'null', 'matte', 'cyclesbssrdf'):
            ses.BeginSceneEdit()
            edited.Parse(props(leaf(kind)))
            ses.EndSceneEdit()
            compare(device + '-live-twosided-' + kind, pixels(ses), render(scene(kind, 'twosided'), device))
    finally:
        ses.Stop()
    # A live edit must preserve exactly the same preflight boundary as a
    # new session. Repair the rejected edit while workers are still stopped.
    for name, overrides in (('opt-in', {'path.cyclesbssrdf.experimental.enable': False}),
                             ('hybrid', {'path.hybridbackforward.enable': True})):
        edited = scene()
        ses = session(edited, device, overrides)
        try:
            ses.Start()
            ses.BeginSceneEdit()
            edited.Parse(props(leaf('cyclesbssrdf')))
            try:
                ses.EndSceneEdit()
            except RuntimeError as error:
                assert 'Experimental cyclesbssrdf' in str(error), str(error)
                records.append({'case': device + '-live-preflight-' + name, 'passed': True,
                                'message': str(error)})
                save()
                edited.Parse(props(leaf('matte')))
                ses.EndSceneEdit()
                pixels(ses)
            else:
                raise AssertionError('live edit bypassed ' + name + ' preflight')
        finally:
            ses.Stop()
print('MATERIAL_GRAPH_EDIT_COMPLETE', len(records), flush=True)
