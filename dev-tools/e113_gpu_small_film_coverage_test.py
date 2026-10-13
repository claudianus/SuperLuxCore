#!/usr/bin/env python3
"""PATHOCL small-film pixel coverage: every pixel of a film filled by a unit
emitter must read 1 after 1 and 16 spp, for RANDOM, SOBOL and PMJ02, with the
default path depth (the paths continue after the hit).

Regression (2026-10-12): the staggered bucket sweep started each task at
gid % bucketSize, but bucket tickets follow gid order and bucketCount is a
multiple of bucketSize, so every task sharing a bucket started on the same
pixel. When the render halts inside the first wave (64x64 / 128x128 films)
25% of the pixels were never sampled: Metal read 0.75 on a constant emitter,
0.25x on an area-lit floor (camera-volume-regression.py failed the same way).
"""
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore as lux

SCENE = """
scene.camera.lookat.orig = 0 0 1
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 30
scene.materials.emitter.type = matte
scene.materials.emitter.kd = .5 .5 .5
scene.materials.emitter.emission = 1 1 1
scene.objects.emitter.material = emitter
scene.objects.emitter.vertices = -2 -2 0 2 -2 0 2 2 0 -2 2 0
scene.objects.emitter.faces = 0 1 2 0 2 3
"""


def render(sampler, res, spp):
    sc = lux.Scene(); p = lux.Properties(); p.SetFromString(SCENE); sc.Parse(p)
    c = lux.Properties(); c.SetFromString(f"""
renderengine.type = PATHOCL
film.width = {res}
film.height = {res}
film.filter.type = NONE
film.imagepipelines.0.0.type = NOP
batch.haltspp = {spp}
sampler.type = {sampler}
opencl.devices.select = 010
opencl.native.threads.count = 0
""")
    ses = lux.RenderSession(lux.RenderConfig(c, sc)); ses.Start()
    deadline = time.monotonic() + 120
    while not ses.HasDone():
        if time.monotonic() > deadline:
            ses.Stop(); raise TimeoutError(sampler)
        time.sleep(.05); ses.UpdateStats()
    ses.Stop()
    a = np.empty(res * res * 3, np.float32)
    ses.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE, a, 0, True)
    return a.reshape(res, res, 3).mean(-1)


def main():
    lux.Init(lambda s: None)
    fails = 0
    for sampler in ("RANDOM", "SOBOL", "PMJ02SAMPLER"):
        for res in (32, 64, 128):
            for spp in (1, 16):
                img = render(sampler, res, spp)
                dark = int((img < .99).sum())
                ok = dark == 0 and abs(img.mean() - 1) < .005
                fails += not ok
                print(f"[{'PASS' if ok else 'FAIL'}] {sampler:12s} {res:3d}px {spp:2d}spp: "
                      f"mean {img.mean():.4f}, unsampled pixels {dark}", flush=True)
    print(f"{fails} failure(s)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
