# SPDX-License-Identifier: Apache-2.0
"""Private device random-walk contracts, preceded by current CPU regressions.

Run with Blender Python and the complete isolated wheel on PYTHONPATH.
The explicit device opt-in retains all unsupported transport preflight gates.
Small native fixtures are contract evidence, not production image acceptance.
"""
import json
import os
from pathlib import Path
import runpy

import numpy as np

ns = runpy.run_path(str(Path(__file__).with_name('cycles_bssrdf_transport_test.py')))
scene, render, props = ns['scene'], ns['render'], ns['properties']
center = ns['center']
identity = ns['identity']
reference = ns['positive']['RGB'][center].mean(axis=(0, 1))
ns['BASE'].update({'renderengine.type': 'PATHOCL',
                   'path.cyclesbssrdf.experimental.device.enable': True,
                   'opencl.cpu.use': False, 'opencl.gpu.use': True,
                   # Two in-flight paths/pixel for this 64px contract. AUTO's
                   # 131072 tasks are 32/pixel and stopping at 256spp leaves
                   # too many unfinished random walks for a tight mean check.
                   'opencl.task.count': 8192, 'batch.haltspp': 1024})
records = []


def record(case, **data):
    records.append({'case': case, 'passed': True, **data})
    print('BSSRDF_DEVICE_PASS', case, data, flush=True)


def entry(pixels):
    assert np.all(pixels['MATERIAL_ID'][center] == 991)
    assert np.all(pixels['OBJECT_ID'][center] == 432)
    assert np.allclose(pixels['POSITION'][center][:, :, 1], -1., atol=1e-5)
    assert np.allclose(pixels['GEOMETRY_NORMAL'][center], (0., -1., 0.), atol=1e-5)
    assert np.allclose(pixels['DEPTH'][center], 2.999, atol=1e-5)
    assert np.allclose(pixels['ALPHA'][center], 1., atol=1e-5)
    assert np.allclose(pixels['ALBEDO'][center], .45, atol=1e-5)


for name, overrides, material in (
        ('vertex-connect-rejected', {'path.vertexconnection.enable': True}, None),
        ('entry-bump-rejected', {}, 'body'),
        ('grouped-exit-bump-rejected', {}, 'partition')):
    scn = scene(partitioned=material == 'partition')
    if material:
        # Parse redefines a material; an omitted type defaults to Matte.
        prefix = f'scene.materials.{material}.'
        kind = 'cyclesbssrdf' if material == 'body' else 'matte'
        scn.Parse(props({prefix + 'type': kind, prefix + 'bumptex': .1,
                         prefix + 'kd': .45 if material == 'body' else 0.,
                         prefix + 'radius': (.3, .3, .3), prefix + 'scale': 1.}))
        assert scn.ToProperties().Get(prefix + 'type').GetString() == kind
    try:
        ns['session'](scn, overrides)
    except RuntimeError as error:
        assert 'Experimental cyclesbssrdf' in str(error), str(error)
        record(name, message=str(error))
    else:
        raise AssertionError(name + ' must reject an unimplemented context')


for mode in ('off', 'on'):
    overrides = {'pathocl.wavefront': mode}
    positive = render(scene(), overrides)
    entry(positive)
    mean = positive['RGB'][center].mean(axis=(0, 1))
    assert np.isfinite(positive['RGB']).all() and np.max(np.abs(mean - reference)) < .008, (mean, reference)
    # The local Matte mean is .45; the nonlocal walk must actually run.
    assert np.all(mean > .48) and np.all(mean < .54), mean
    record(mode + '-nonlocal-walk-and-entry-aovs', rgb_mean=mean.tolist(), cpu_rgb_mean=reference.tolist())
    black = render(scene(color=(0., 0., 0.)), overrides)
    assert np.max(np.abs(black['RGB'][center])) < 1e-6
    assert np.allclose(black['ALPHA'][center], 1., atol=1e-5)
    record(mode + '-black-absorption-is-opaque-not-environment-miss')
    foreign = render(scene(embedded=True, foreign_shared_id=True), overrides)
    error = float(np.abs(foreign['RGB'][center].mean(axis=(0, 1)) - mean).max())
    assert error < .008, error
    record(mode + '-foreign-shared-id-boundary-skipped', max_channel_mean_error=error)
    partition = render(scene(partitioned=True), overrides)
    entry(partition)
    error = float(np.abs(partition['RGB'][center].mean(axis=(0, 1)) - mean).max())
    assert error < .008, error
    record(mode + '-material-partition-boundary-and-entry-coefficients', max_channel_mean_error=error)
    local = render(scene(radius=(0., 0., 0.)), overrides)
    entry(local)
    means = local['RGB'][center].mean(axis=(0, 1))
    assert np.allclose(means, .45, atol=.008), means
    record(mode + '-all-local-radius', rgb_mean=means.tolist())
    partial = render(scene(radius=(0., .3, .3)), overrides)
    entry(partial)
    means = partial['RGB'][center].mean(axis=(0, 1))
    assert abs(float(means[0]) - .45) < .02 and np.all(means[1:] > .48), means
    record(mode + '-partial-rgb-radius', rgb_mean=means.tolist())
    dynamic = render(ns['dynamic_scale_scene'](), overrides)
    entry(dynamic)
    means = dynamic['RGB'][center].mean(axis=(0, 1))
    assert np.allclose(means, .45, atol=.008), means
    record(mode + '-dynamic-zero-scale', rgb_mean=means.tolist())


    for amount, additive in ((0., False), (.25, False), (.75, False), (1., False), (.5, True)):
        mixed_scene = scene(nested_mix=True)
        mixed_scene.Parse(props({'scene.materials.mix.type': 'mix',
                                'scene.materials.mix.material1': 'body',
                                'scene.materials.mix.material2': 'other',
                                'scene.materials.mix.amount': amount,
                                'scene.materials.mix.additive': additive}))
        mixed = render(mixed_scene, overrides)
        measured = mixed['RGB'][center].mean(axis=(0, 1))
        expected = mean + .5 if additive else (1. - amount) * mean + amount * .5
        error = float(np.abs(measured - expected).max())
        assert error < .015, (amount, additive, measured, expected)
        record(mode + '-mixed-weight-' + str(amount) + '-add-' + str(additive),
               rgb_mean=measured.tolist(), expected=expected.tolist(), max_error=error)
    mixed_local = scene(nested_mix=True, radius=(0., 0., 0.))
    pixels = render(mixed_local, overrides)
    measured = pixels['RGB'][center].mean(axis=(0, 1))
    assert np.allclose(measured, .475, atol=.008), measured
    record(mode + '-mixed-local-radius-selected-leaf', rgb_mean=measured.tolist())


    for amount, additive, nested in ((0., False, False), (.5, False, False), (1., False, False),
                                     (.5, True, False), (.4, False, True)):
        label = 'null-mix-' + str(amount) + '-add-' + str(additive) + '-nested-' + str(nested)
        pixels = render(ns['transparent_scene'](amount=amount, additive=additive, nested=nested), overrides)
        measured = pixels['RGB'][center].mean(axis=(0, 1))
        expected = ns['transparent_references'][label]
        error = float(np.abs(measured - expected).max())
        assert error < .02, (label, measured, expected)
        record(mode + '-' + label + '-nonlocal', rgb_mean=measured.tolist(), cpu_rgb_mean=expected.tolist(), max_error=error)
        local = render(ns['transparent_scene'](radius=(0., 0., 0.), amount=amount, additive=additive, nested=nested), overrides)
        ordinary = render(ns['transparent_scene'](kind='matte', amount=amount, additive=additive, nested=nested), overrides)
        error = float(np.abs(local['RGB'][center].mean(axis=(0, 1)) - ordinary['RGB'][center].mean(axis=(0, 1))).max())
        alpha_error = float(np.abs(local['ALPHA'][center].mean() - ordinary['ALPHA'][center].mean()))
        assert error < .015 and alpha_error < .015, (label, error, alpha_error)
        record(mode + '-' + label + '-local-vs-ordinary', max_error=error, alpha_mean_error=alpha_error)


    inside = render(ns['inside_scene'](), overrides)
    measured = inside['RGB'][center].mean(axis=(0, 1))
    expected = ns['inside_mean']
    error = float(np.abs(measured - expected).max())
    assert np.isfinite(inside['RGB']).all() and error < .02 and np.all(measured > .505), (measured, expected)
    record(mode + '-inside-back-facing-entry-random-walk', rgb_mean=measured.tolist(), cpu_rgb_mean=expected.tolist(), max_error=error)

folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])
(folder / 'device-metrics.json').write_text(json.dumps({
    'native_sha256': identity['native_sha256'], 'experimental': True,
    'production_acceptance': False, 'cpu_contracts': len(ns['records']),
    'records': records}, indent=2) + '\n')
print('BSSRDF_DEVICE_COMPLETE', len(records), flush=True)
