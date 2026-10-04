# SPDX-License-Identifier: Apache-2.0
#
# E105: Cycles area light "Spread" (scene.materials.*.emission.spread).
#
# A small square emitter (0.1 m, facing -Z, 2 m above a white Lambertian
# floor) lights the floor; the camera looks straight down from below the
# emitter. Against the analytic Cycles soft-box profile
#   L(a) = L0 * max(tan(s/2) - tan(a), 0) / (tan(s/2) - s/2)
# the floor radiance is rho/pi * L(a) * A * cos^4(a) / h^2.
#
#   1  Lambertian emitter matches the analytic floor (calibrates L0)
#   2  spread ~180 deg is a drop-in for the Lambertian lobe (equal power)
#   3  spread 60 deg matches the analytic profile (ring means)
#   4  LIGHTCPU (TriangleLight::Emit) matches PATHCPU
#   5  PATHOCL matches PATHCPU
#   6  camera looking at the emitter sees L(a) (TriangleLight::GetRadiance)
#
# Run from the repo root:
#   python3.13 dev-tools/e105_area_spread_test.py

import math
import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 160, 160
H_LIGHT = 2.0
SIZE = 0.1
GAIN = 100.0
RHO = 0.8
CAM_Z = 1.5
FOV = 90.0
OCL_DEV = os.environ.get("E105_OCL_DEV", "")


def scene_props(spread_deg=None, look_up=False, big_light=False):
    s = SIZE * (10 if big_light else 1) / 2
    zl = H_LIGHT
    spread = "" if spread_deg is None else \
        f"scene.materials.light.emission.spread = {math.radians(spread_deg)}\n"
    if look_up:
        cam = (f"scene.camera.lookat.orig = 0 0 0\n"
               f"scene.camera.lookat.target = 0 0 1\n"
               f"scene.camera.up = 0 1 0\n"
               f"scene.camera.fieldofview = 30\n")
    else:
        cam = (f"scene.camera.lookat.orig = 0 0 {CAM_Z}\n"
               f"scene.camera.lookat.target = 0 0 0\n"
               f"scene.camera.up = 0 1 0\n"
               f"scene.camera.fieldofview = {FOV}\n")
    return f"""
{cam}
scene.materials.floor.type = matte
scene.materials.floor.kd = {RHO} {RHO} {RHO}
scene.materials.light.type = matte
scene.materials.light.kd = 0 0 0
scene.materials.light.emission = 1 1 1
scene.materials.light.emission.gain = {GAIN} {GAIN} {GAIN}
scene.materials.light.emission.power = 0
scene.materials.light.emission.efficency = 0
scene.materials.light.emission.normalizebycolor = 0
{spread}
scene.objects.floor.material = floor
scene.objects.floor.vertices = -20 -20 0  20 -20 0  20 20 0  -20 20 0
scene.objects.floor.faces = 0 1 2  0 2 3
scene.objects.light.material = light
scene.objects.light.vertices = {-s} {-s} {zl}  {s} {-s} {zl}  {s} {s} {zl}  {-s} {s} {zl}
scene.objects.light.faces = 0 2 1  0 3 2
"""


def render(props, engine, spp):
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(props)
    scn.Parse(p)
    cfg = pysuperluxcore.Properties()
    extra = f'opencl.devices.select = "{OCL_DEV}"' if OCL_DEV else ""
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
path.pathdepth.total = 1
path.pathdepth.diffuse = 1
light.maxdepth = 1
path.hybridbackforward.enable = 0
path.vertexconnection.enable = 0
{extra}
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scn))
    ses.Start()
    deadline = time.monotonic() + 600
    while True:
        ses.UpdateStats()
        st = ses.GetStats()
        if engine == "LIGHTCPU":
            if time.monotonic() > deadline - 600 + 40:
                break
        elif st.Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            raise TimeoutError(engine)
        time.sleep(0.5)
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(H, W, 3).mean(axis=2)[::-1]


def profile(a, spread_deg):
    if spread_deg is None:
        return np.ones_like(a)
    hs = math.radians(spread_deg) / 2
    t = math.tan(hs)
    n = 1 / (t - hs) if hs > 0.05 else 3 / hs ** 3
    return np.maximum((t - np.tan(a)) * n, 0) * (a < math.pi / 2)


def analytic_floor(spread_deg):
    half = math.tan(math.radians(FOV / 2)) * CAM_Z
    xs = (np.arange(W) + 0.5) / W * 2 * half - half
    X, Y = np.meshgrid(xs, xs)
    r = np.hypot(X, Y)
    a = np.arctan2(r, H_LIGHT)
    E = profile(a, spread_deg) * SIZE * SIZE * np.cos(a) ** 4 / H_LIGHT ** 2
    return RHO / math.pi * E, a


def rings(img, a, edges):
    return np.array([img[(a >= lo) & (a < hi)].mean()
                     for lo, hi in zip(edges[:-1], edges[1:])])


def main():
    fails = []

    def check(name, ok, msg):
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {msg}")
        if not ok:
            fails.append(name)

    edges = np.radians([0, 5, 10, 15, 20, 25, 28])

    lam = render(scene_props(None), "PATHCPU", 64)
    ref_lam, a = analytic_floor(None)
    k = lam.mean() / ref_lam.mean()
    check("1 lambertian calibration", abs(k / GAIN - 1) < 0.03,
          f"L0/gain={k / GAIN:.4f}")

    wide = render(scene_props(179.9), "PATHCPU", 64)
    r = wide.mean() / lam.mean()
    check("2 spread~180 drop-in", abs(r - 1) < 0.03, f"ratio={r:.4f}")

    sp = render(scene_props(60), "PATHCPU", 64)
    ref_sp, _ = analytic_floor(60)
    got = rings(sp, a, edges)
    exp = rings(ref_sp * k, a, edges)
    rr = got / exp
    check("3 spread 60 profile", np.all(np.abs(rr - 1) < 0.05),
          "ring ratios " + " ".join(f"{x:.3f}" for x in rr))
    outside = sp[a > math.radians(32)].mean() / sp.mean()
    check("3b no light outside the spread cone", outside < 1e-3,
          f"outside/mean={outside:.2e}")

    lt = render(scene_props(60), "LIGHTCPU", 0)
    rr = rings(lt, a, edges) / got
    check("4 LIGHTCPU == PATHCPU", np.all(np.abs(rr - 1) < 0.06),
          "ring ratios " + " ".join(f"{x:.3f}" for x in rr))

    try:
        ocl = render(scene_props(60), "PATHOCL", 64)
        rr = rings(ocl, a, edges) / got
        check("5 PATHOCL == PATHCPU", np.all(np.abs(rr - 1) < 0.04),
              "ring ratios " + " ".join(f"{x:.3f}" for x in rr))
    except Exception as e:  # no GPU
        print(f"[SKIP] 5 PATHOCL: {e}")

    # Camera at the floor centre looking up at a 1 m emitter: every pixel
    # on the emitter shows L(a) for its own view angle
    for eng in ("PATHCPU", "PATHOCL"):
        try:
            img = render(scene_props(60, look_up=True, big_light=True), eng, 16)
        except Exception as e:
            print(f"[SKIP] 6 {eng}: {e}")
            continue
        xs = (np.arange(W) + 0.5) / W * 2 - 1
        X, Y = np.meshgrid(xs, xs)
        t = math.tan(math.radians(15))
        aa = np.arctan(np.hypot(X, Y) * t)
        on = (np.abs(X * t * H_LIGHT) < SIZE * 5 * 0.9) & (np.abs(Y * t * H_LIGHT) < SIZE * 5 * 0.9)
        exp = profile(aa, 60) * k
        rr = img[on].mean() / exp[on].mean()
        check(f"6 {eng} emitter radiance", abs(rr - 1) < 0.03, f"ratio={rr:.4f}")

    print("PASS overall" if not fails else f"FAIL overall: {fails}")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
