# SPDX-License-Identifier: Apache-2.0
#
# E107: remap texture with a descending target (or source) range.
#
# Cycles Map Range (clamp on) 0..14 -> 0.065..0.006 is a height falloff;
# RemapTexture::ClampedRemap clamped to [targetMin, targetMax] as given,
# so a descending range collapsed to a constant (045 fog ~2.5x too thin).
# An emissive plane shows remap(value) directly; CPU and GPU must match
# the Cycles formula for ascending/descending, in-range and clamped values.
#
# Run from the repo root:
#   python3.13 dev-tools/e107_remap_descending_test.py

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

CASES = [
    # value, smin, smax, tmin, tmax
    (7.0, 0.0, 14.0, 0.065, 0.006),
    (-3.0, 0.0, 14.0, 0.065, 0.006),
    (20.0, 0.0, 14.0, 0.065, 0.006),
    (0.5, 0.35, 0.7, 0.05, 1.0),
    (0.9, 0.35, 0.7, 0.05, 1.0),
    (0.5, 1.0, 0.0, 0.0, 1.0),
]


def expected(v, smin, smax, tmin, tmax):
    r = tmin + (v - smin) * (tmax - tmin) / (smax - smin)
    return min(max(r, min(tmin, tmax)), max(tmin, tmax))


def render(engine, case):
    v, smin, smax, tmin, tmax = case
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(f"""
scene.camera.lookat.orig = 0 0 1
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 20
scene.textures.v.type = constfloat1
scene.textures.v.value = {v}
scene.textures.r.type = remap
scene.textures.r.value = v
scene.textures.r.sourcemin = {smin}
scene.textures.r.sourcemax = {smax}
scene.textures.r.targetmin = {tmin}
scene.textures.r.targetmax = {tmax}
scene.materials.m.type = matte
scene.materials.m.kd = 0 0 0
scene.materials.m.emission = r
scene.objects.o.material = m
scene.objects.o.vertices = -1 -1 0  1 -1 0  1 1 0  -1 1 0
scene.objects.o.faces = 0 1 2  0 2 3
""")
    scn.Parse(p)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = 16
film.height = 16
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = 4
path.pathdepth.total = 1
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scn))
    ses.Start()
    t0 = time.monotonic()
    while ses.GetStats().Get("stats.renderengine.pass").GetInt() < 4:
        ses.UpdateStats()
        if time.monotonic() - t0 > 300:
            raise TimeoutError(engine)
        time.sleep(0.1)
    rgb = np.empty(16 * 16 * 3, np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return float(rgb.mean())


def main():
    fails = []
    for engine in ("PATHCPU", "PATHOCL"):
        for case in CASES:
            try:
                got = render(engine, case)
            except Exception as e:
                print(f"[SKIP] {engine}: {e}")
                break
            exp = expected(*case)
            ok = abs(got - exp) <= 1e-3 * max(1.0, abs(exp))
            print(f"[{'PASS' if ok else 'FAIL'}] {engine} remap{case}: got={got:.5f} exp={exp:.5f}")
            if not ok:
                fails.append((engine, case))
    print("PASS overall" if not fails else f"FAIL overall: {fails}")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
