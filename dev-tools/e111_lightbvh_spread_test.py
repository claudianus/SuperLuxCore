#!/usr/bin/env python3
"""Light BVH on spread-out many lights: 300 small emitters 4 cm above a 10 x 10 m
matte floor, top-down camera, direct light only (pathdepth 2), 16 spp.

Gate:
  * LIGHT_BVH noise (two-seed median relative difference) well below UNIFORM
    for both point lights and tiny emissive quads (mesh lights)
  * unbiased: LIGHT_BVH 64 spp mean matches a 256 spp UNIFORM reference within 2%
    (UNIFORM's own 16 spp mean is ~2% low on the quads: heavy tail)
  * PATHOCL (Metal) LIGHT_BVH mean matches PATHCPU within 2%

Regression for the box-distance importance: sibling boxes touching at the
receiver tied at every level above the leaves, so LIGHT_BVH was only ~30%
better than UNIFORM here (2026-10-12: point 1.055 -> 0.774, quad 1.740 -> 1.181).
Centre distance clamped to the cluster radius: point 0.527 (x0.50), quad 0.733 (x0.43).
"""
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore as lux

N = 300
RES = 300


def scene_text(kind):
    rng = np.random.default_rng(3)
    P = rng.uniform(-5, 5, (N, 2))
    s = ['scene.camera.lookat.orig = 0 0 14', 'scene.camera.lookat.target = 0 0 0', 'scene.camera.up = 0 1 0',
         'scene.camera.fieldofview = 42',
         'scene.materials.floor.type = matte', 'scene.materials.floor.kd = .7 .7 .7',
         'scene.objects.floor.material = floor', 'scene.objects.floor.vertices = -6 -6 0 6 -6 0 6 6 0 -6 6 0',
         'scene.objects.floor.faces = 0 1 2 0 2 3',
         'scene.materials.bulb.type = matte', 'scene.materials.bulb.kd = 0 0 0', 'scene.materials.bulb.emission = 50 50 50']
    for i, (x, y) in enumerate(P):
        if kind == 'point':
            s += [f'scene.lights.p{i}.type = point', f'scene.lights.p{i}.position = {x} {y} .04',
                  f'scene.lights.p{i}.gain = .05 .05 .05']
        else:  # tiny quad facing down
            r = .01
            s += [f'scene.objects.b{i}.material = bulb',
                  f'scene.objects.b{i}.vertices = {x-r} {y-r} .04 {x+r} {y-r} .04 {x+r} {y+r} .04 {x-r} {y+r} .04',
                  f'scene.objects.b{i}.faces = 0 2 1 0 3 2']
    return '\n'.join(s)


def render(kind, strategy, seed, engine='PATHCPU', spp=16):
    sc = lux.Scene(); p = lux.Properties(); p.SetFromString(scene_text(kind)); sc.Parse(p)
    c = lux.Properties(); c.SetFromString(f'''renderengine.type = {engine}
renderengine.seed = {seed}
film.width = {RES}
film.height = {RES}
film.imagepipelines.0.0.type = NOP
batch.haltspp = {spp}
sampler.type = RANDOM
path.pathdepth.total = 2
path.lighttracing.enable = 0
path.mnee.enable = 0
opencl.devices.select = 010
opencl.native.threads.count = 0
lightstrategy.type = {strategy}''')
    ses = lux.RenderSession(lux.RenderConfig(c, sc)); ses.Start()
    while not ses.HasDone():
        time.sleep(.05); ses.UpdateStats()
    ses.Stop()
    a = np.empty(RES * RES * 3, np.float32)
    ses.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE, a, 0, True)
    return a.reshape(RES, RES, 3).mean(-1)


def noise(kind, strategy):
    a, b = render(kind, strategy, 1), render(kind, strategy, 2)
    m = (a + b) / 2; mk = m > 1e-4
    return float(np.median(np.abs(a - b)[mk] / m[mk]))


def main():
    lux.Init(lambda s: None)
    ok = True
    for kind in ('point', 'mesh'):
        nu = noise(kind, 'UNIFORM')
        nb = noise(kind, 'LIGHT_BVH')
        ratio = nb / nu
        good = ratio < .6
        ok &= good
        print(f'[{"PASS" if good else "FAIL"}] {kind}: noise UNIFORM {nu:.3f} LIGHT_BVH {nb:.3f} (x{ratio:.2f})', flush=True)
        ref = render(kind, 'UNIFORM', 5, spp=256).mean()
        c = render(kind, 'LIGHT_BVH', 1, 'PATHCPU', 64).mean()
        g = render(kind, 'LIGHT_BVH', 1, 'PATHOCL', 64).mean()
        good = abs(c / ref - 1) < .02
        ok &= good
        print(f'[{"PASS" if good else "FAIL"}] {kind}: LIGHT_BVH mean {c:.4f} vs UNIFORM reference {ref:.4f}', flush=True)
        good = abs(g / c - 1) < .02
        ok &= good
        print(f'[{"PASS" if good else "FAIL"}] {kind}: Metal mean {g:.4f} vs CPU {c:.4f}', flush=True)
    print('PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
