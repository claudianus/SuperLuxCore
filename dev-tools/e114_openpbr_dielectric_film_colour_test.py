#!/usr/bin/env python3
"""OpenPBR thin film over a dielectric base keeps its interference colour on
the CPU, and the CPU matches the GPU kernel.

Regression (2026-10-12): the CPU specular lobe reduced the film's
per-wavelength reflectance to one number (Spectrum::Filter()), so oil on
water, soap films and filmed plastics rendered grey on the CPU while the
GPU kernel (Lerp3 with the float3 film Fresnel) kept the colour. Metal
substrates were unaffected (their lobe already used the Spectrum).

A smooth dark dielectric (n 1.33) with a 300 nm film (n 1.45) under a uniform
white sky: the reflection must be clearly tinted (RGB spread / mean > 0.25)
and CPU and Metal mean RGB must agree within 3%, in RGB and spectral mode.
"""
import sys
import time
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore as lux

SCENE = """
scene.camera.lookat.orig = 0 -1 .6
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 0 1
scene.camera.fieldofview = 20
scene.lights.sky.type = constantinfinite
scene.lights.sky.color = 1 1 1
scene.materials.oil.type = openpbr
scene.materials.oil.basecolor = 0 0 0
scene.materials.oil.basemetalness = 0
scene.materials.oil.specularroughness = 0
scene.materials.oil.specularior = 1.33
scene.materials.oil.filmweight = 1
scene.materials.oil.filmthickness = .3
scene.materials.oil.filmior = 1.45
scene.objects.plane.material = oil
scene.objects.plane.vertices = -5 -5 0 5 -5 0 5 5 0 -5 5 0
scene.objects.plane.faces = 0 1 2 0 2 3
"""


def render(engine, spectral):
    sc = lux.Scene(); p = lux.Properties(); p.SetFromString(SCENE); sc.Parse(p)
    c = lux.Properties(); c.SetFromString(f"""
renderengine.type = {engine}
film.width = 64
film.height = 64
film.imagepipelines.0.0.type = NOP
batch.haltspp = 64
path.spectral.enable = {int(spectral)}
path.lighttracing.enable = 0
opencl.devices.select = 010
opencl.native.threads.count = 0
""")
    ses = lux.RenderSession(lux.RenderConfig(c, sc)); ses.Start()
    deadline = time.monotonic() + 120
    while not ses.HasDone():
        if time.monotonic() > deadline:
            ses.Stop(); raise TimeoutError(engine)
        time.sleep(.05); ses.UpdateStats()
    ses.Stop()
    a = np.empty(64 * 64 * 3, np.float32)
    ses.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE, a, 0, True)
    return a.reshape(-1, 3).mean(0)


def main():
    lux.Init(lambda s: None)
    fails = 0
    for spectral in (False, True):
        cpu, gpu = render("PATHCPU", spectral), render("PATHOCL", spectral)
        tint = (cpu.max() - cpu.min()) / cpu.mean()
        diff = np.abs(cpu - gpu).max() / gpu.mean()
        ok = tint > .25 and diff < .03
        fails += not ok
        print(f"[{'PASS' if ok else 'FAIL'}] {'spectral' if spectral else 'rgb':8s}: CPU {np.round(cpu, 4)} "
              f"Metal {np.round(gpu, 4)} tint {tint:.2f} CPU/Metal diff {diff:.3f}", flush=True)
    print(f"{fails} failure(s)")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
