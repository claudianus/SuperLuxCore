# SPDX-License-Identifier: Apache-2.0
"""Experimental CPU BSSRDF transport boundaries, absorption and entry AOVs.

Use Blender's bundled Python with the complete isolated wheel on PYTHONPATH.
SUPERLUXCORE_BSSRDF_IDENTITY identifies the exact tested package; this script
does not accept an older released library as evidence for this implementation.
Small native fixtures exercise contracts, not production/visual acceptance.
"""
import hashlib
import json
import os
from pathlib import Path
import time

import numpy as np
import pysuperluxcore as slc

identity = json.loads(Path(os.environ['SUPERLUXCORE_BSSRDF_IDENTITY']).read_text())
for filename, digest in identity.get('source_files', {}).items():
    assert hashlib.sha256(Path(filename).read_bytes()).hexdigest() == digest, filename
assert hashlib.sha256(Path(slc.pysuperluxcore.__file__).read_bytes()).hexdigest() == identity['native_sha256']
slc.Init()
W = H = 64
BASE = {'film.width': W, 'film.height': H,
        'film.imagepipelines.0.0.type': 'NOP',
        'renderengine.type': 'PATHCPU', 'sampler.type': 'SOBOL',
        'batch.haltspp': 256, 'native.threads.count': 8,
        'path.hybridbackforward.enable': False,
        'path.lighttracing.enable': False, 'path.lighttracing.auto': False,
        'path.mnee.enable': False, 'path.mnee.auto': False,
        'path.photongi.caustic.enabled': False,
        'path.photongi.indirect.enabled': False,
        'path.cyclesbssrdf.experimental.enable': True,
        'path.spectral.enable': False, 'path.pathdepth.total': 4}
for index, kind in enumerate(('RGB', 'ALPHA', 'DEPTH', 'POSITION', 'GEOMETRY_NORMAL',
                              'MATERIAL_ID', 'OBJECT_ID', 'ALBEDO')):
    BASE[f'film.outputs.{index}.type'] = kind
    suffix = '.png' if kind in ('MATERIAL_ID', 'OBJECT_ID') else '.exr'
    BASE[f'film.outputs.{index}.filename'] = 'unused-' + kind + suffix


def properties(values):
    props = slc.Properties()
    for name, value in values.items():
        if value is not None:
            if isinstance(value, tuple):
                value = list(value)
            props.Set(slc.Property(name, value))
    return props


def scene(kind='cyclesbssrdf', color=(.45, .45, .45), radius=(.3, .3, .3),
          nested_mix=False, embedded=False, unused=False):
    props = properties({
        'scene.camera.type': 'orthographic',
        'scene.camera.lookat.orig': (0., -4., 0.),
        'scene.camera.lookat.target': (0., 0., 0.),
        'scene.camera.up': (0., 0., 1.), 'scene.camera.screenwindow': (-1.3, 1.3, -1.3, 1.3),
        'scene.materials.body.type': kind,
        'scene.materials.body.kd': color,
        'scene.materials.body.radius': radius,
        'scene.materials.body.scale': 1.,
        'scene.materials.body.ior': 1.4,
        'scene.materials.body.roughness': 0.,
        'scene.materials.body.anisotropy': 0.,
        'scene.materials.body.id': 991,
        'scene.objects.body.material': 'body', 'scene.objects.body.id': 432,
        'scene.objects.body.vertices': (-1., -1., -1., 1., -1., -1.,
                                       1., 1., -1., -1., 1., -1.,
                                       -1., -1., 1., 1., -1., 1.,
                                       1., 1., 1., -1., 1., 1.),
        'scene.objects.body.faces': (0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7,
                                    0, 1, 5, 0, 5, 4, 1, 2, 6, 1, 6, 5,
                                    2, 3, 7, 2, 7, 6, 3, 0, 4, 3, 4, 7),
        'scene.lights.env.type': 'constantinfinite',
        'scene.lights.env.color': (1., 1., 1.),
    })
    if nested_mix:
        props.Set(properties({'scene.materials.other.type': 'matte',
                              'scene.materials.other.kd': .5,
                              'scene.materials.mix.type': 'mix',
                              'scene.materials.mix.material1': 'body',
                              'scene.materials.mix.material2': 'other',
                              'scene.materials.wrap.type': 'twosided',
                              'scene.materials.wrap.frontmaterial': 'mix',
                              'scene.materials.wrap.backmaterial': 'body',
                              'scene.objects.body.material': 'wrap'}))
    if embedded:
        props.Set(properties({'scene.materials.other.type': 'matte',
                              'scene.materials.other.kd': 0.,
                              'scene.objects.foreign.material': 'other',
                              'scene.objects.foreign.vertices': (-.9, 0., -.9, .9, 0., -.9,
                                                                 .9, 0., .9, -.9, 0., .9),
                              'scene.objects.foreign.faces': (0, 1, 2, 0, 2, 3)}))
    if unused:
        props.Set(properties({'scene.materials.ordinary.type': 'matte',
                              'scene.materials.ordinary.kd': .5,
                              'scene.objects.body.material': 'ordinary'}))
    result = slc.Scene()
    result.Parse(props)
    return result


def session(scn, overrides=None):
    config = BASE | (overrides or {})
    return slc.RenderSession(slc.RenderConfig(properties(config), scn))


def render(scn, overrides=None):
    ses = session(scn, overrides)
    ses.Start()
    start = time.monotonic()
    try:
        while True:
            ses.UpdateStats()
            if ses.GetStats().Get('stats.renderengine.pass').GetInt() >= 256:
                break
            assert time.monotonic() - start < 120, 'BSSRDF fixture timed out'
            time.sleep(.05)
    finally:
        ses.Stop()
    result = {}
    for kind, channels in (('RGB', 3), ('ALPHA', 1), ('DEPTH', 1), ('POSITION', 3),
                           ('GEOMETRY_NORMAL', 3), ('ALBEDO', 3),
                           ('MATERIAL_ID', 1), ('OBJECT_ID', 1)):
        output_type = getattr(slc.FilmOutputType, kind)
        integers = kind in ('MATERIAL_ID', 'OBJECT_ID')
        pixels = np.empty(W * H * channels, np.uint32 if integers else np.float32)
        if integers:
            ses.GetFilm().GetOutputUInt(output_type, pixels, 0, True)
        else:
            ses.GetFilm().GetOutputFloat(output_type, pixels, 0, True)
        result[kind] = pixels.reshape(H, W, channels)
    return result


records = []


def record(name, detail):
    records.append({'case': name, 'passed': True, **detail})
    print('BSSRDF_TRANSPORT_PASS', name, detail, flush=True)


for name, overrides in (
        ('opt-in-required', {'path.cyclesbssrdf.experimental.enable': None}),
        ('hybrid-default-rejected', {'path.hybridbackforward.enable': None}),
        ('metal-rejected', {'renderengine.type': 'PATHOCL'}),
        ('bidir-rejected', {'renderengine.type': 'BIDIRCPU'}),
        ('lightcpu-rejected', {'renderengine.type': 'LIGHTCPU'}),
        ('light-tracing-rejected', {'path.lighttracing.enable': True}),
        ('hybrid-rejected', {'path.hybridbackforward.enable': True}),
        ('caustic-cache-rejected', {'path.photongi.caustic.enabled': True}),
        ('indirect-cache-rejected', {'path.photongi.indirect.enabled': True}),
        ('restir-gi-rejected', {'path.restir.gi.enable': True}),
        ('restir-pt-rejected', {'path.restir.pt.enable': True})):
    try:
        session(scene(), overrides)
    except RuntimeError as error:
        assert 'Experimental cyclesbssrdf' in str(error), (name, str(error))
        record(name, {'message': str(error)})
    else:
        raise AssertionError(name + ' must reject unsupported nonlocal transport')

try:
    session(scene(nested_mix=True))
except RuntimeError as error:
    assert 'mixed closures' in str(error), str(error)
    record('nested-twosided-mix-rejected', {'message': str(error)})
else:
    raise AssertionError('Nested mix must not silently become a local closure')

session(scene(unused=True), {'path.cyclesbssrdf.experimental.enable': None})
record('unused-experimental-material-does-not-block-ordinary-scene', {})
serialized = scene().ToProperties()
assert serialized.Get('scene.materials.body.type').GetString() == 'cyclesbssrdf'
assert serialized.Get('scene.materials.body.ior').GetFloat() == np.float32(1.4)
record('native-property-roundtrip', {'type': 'cyclesbssrdf', 'ior': 1.4})

try:
    session(scene(radius=(0., .3, .3)), {'path.spectral.enable': True})
except RuntimeError as error:
    assert 'partial-radius spectral closure mapping' in str(error), str(error)
    record('partial-spectral-rejected-before-render-workers', {'message': str(error)})
else:
    raise AssertionError('Partial spectral radii must be rejected before workers start')

session(scene(radius=(0., 0., 0.)), {'path.spectral.enable': True})
record('all-local-spectral-limit-allowed', {})

def dynamic_scale_scene():
    scn = scene()
    scn.Parse(properties({'scene.textures.generated.type': 'hitpoint',
                          'scene.textures.generated.channel': 'generated',
                          'scene.textures.scale.type': 'splitfloat3',
                          'scene.textures.scale.texture': 'generated',
                          'scene.textures.scale.channel': 1,
                          'scene.materials.body.type': 'cyclesbssrdf',
                          'scene.materials.body.kd': [.45, .45, .45],
                          'scene.materials.body.radius': [.3, .3, .3],
                          'scene.materials.body.scale': 'scale',
                          'scene.materials.body.id': 991}))
    return scn

try:
    session(dynamic_scale_scene(), {'path.spectral.enable': True})
except RuntimeError as error:
    assert 'dynamic spectral Radius/Scale' in str(error), str(error)
    record('dynamic-spectral-scale-rejected-before-render-workers', {'message': str(error)})
else:
    raise AssertionError('Unimplemented dynamic spectral radius mapping must be rejected')

center = np.s_[24:40, 24:40]


def check_entry_aovs(pixels, name):
    assert np.all(pixels['MATERIAL_ID'][center] == 991), pixels['MATERIAL_ID'][center]
    assert np.all(pixels['OBJECT_ID'][center] == 432)
    assert np.allclose(pixels['POSITION'][center][:, :, 1], -1., atol=1e-5)
    assert np.allclose(pixels['GEOMETRY_NORMAL'][center], (0., -1., 0.), atol=1e-5)
    # Native depth is measured from the ray origin on its default .001
    # near plane, so the unchanged orthographic fixture records 2.999.
    assert np.allclose(pixels['DEPTH'][center], 2.999, atol=1e-5)
    assert np.allclose(pixels['ALPHA'][center], 1., atol=1e-5)
    record(name, {'camera_visible_entry_preserved': True,
                  'material_id': 991, 'object_id': 432, 'depth': 2.999})


black = render(scene(color=(0., 0., 0.)))
assert np.max(np.abs(black['RGB'][center])) < 1e-6, {
    'max_rgb': float(np.abs(black['RGB'][center]).max()),
    'mean_rgb': black['RGB'][center].mean(axis=(0, 1)).tolist(),
    'alpha_range': [float(black['ALPHA'][center].min()), float(black['ALPHA'][center].max())],
    'material_ids': np.unique(black['MATERIAL_ID'][center]).tolist(),
    'depth_range': [float(black['DEPTH'][center].min()), float(black['DEPTH'][center].max())]}
check_entry_aovs(black, 'absorbed-path-is-opaque-entry-not-miss')
positive = render(scene())
check_entry_aovs(positive, 'positive-sss-entry-position-depth-normal-ids-alpha')
assert np.allclose(positive['ALBEDO'][center], .45, atol=1e-5)
record('entry-albedo-not-white-exit-closure', {'albedo': .45})
overlap = render(scene(embedded=True))
assert np.isfinite(overlap['RGB']).all()
delta = float(np.abs(positive['RGB'][center] - overlap['RGB'][center]).mean())
assert delta < .005, delta
record('foreign-interior-mesh-does-not-become-bssrdf-boundary', {'pixel_mae': delta})
local = render(scene(radius=(0., 0., 0.)))
matte = render(scene(kind='matte'))
delta = float(np.abs(local['RGB'] - matte['RGB']).mean())
assert delta < .002, delta
record('zero-radius-local-limit', {'pixel_mae_vs_matte': delta})
dynamic = render(dynamic_scale_scene())
delta = float(np.abs(dynamic['RGB'][center] - matte['RGB'][center]).mean())
assert delta < .002, delta
check_entry_aovs(dynamic, 'dynamic-rgb-scale-zero-entry-aovs')
record('dynamic-rgb-scale-local-limit', {'pixel_mae_vs_matte': delta})
partial = render(scene(radius=(0., .3, .3)))
assert np.isfinite(partial['RGB']).all()
means = partial['RGB'][center].mean(axis=(0, 1))
reference_red = float(local['RGB'][center][:, :, 0].mean())
assert abs(float(means[0]) - reference_red) < .02, (means, reference_red)
assert np.all(means > .2) and np.all(means < .7), means
check_entry_aovs(partial, 'partial-radius-rgb-entry-aovs')
record('partial-radius-rgb-local-channel-weight',
       {'rgb_mean': means.tolist(), 'local_reference_red': reference_red})

folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])
folder.mkdir(parents=True, exist_ok=True)
(folder / 'transport-metrics.json').write_text(json.dumps({
    'native_sha256': identity['native_sha256'], 'experimental': True,
    'production_acceptance': False, 'records': records}, indent=2) + '\n')
print('BSSRDF_TRANSPORT_COMPLETE', len(records), flush=True)
