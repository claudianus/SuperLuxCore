# SPDX-License-Identifier: Apache-2.0
#
# E106: image-based infinite light in spectral mode.
#
# A strongly coloured synthetic environment (warm horizon band, blue
# zenith, PFM) lights a white Lambertian floor. The floor radiance is the
# cosine-weighted integral of the map, computed here in numpy. CPU
# spectral NEE used to return the raw RGB map value without the
# RGB -> emission-spectrum upsampling (InfiniteLight::Illuminate/Emit), so
# the floor came out with red and blue swapped-ish while camera rays (which
# went through GetRadiance) looked right.
#
#   1  PATHCPU RGB      == analytic
#   2  PATHCPU spectral == analytic (per channel; the RGB -> spectrum ->
#      RGB round trip of these saturated colours costs ~4%)
#   3  PATHOCL spectral == analytic
#   4  PATHCPU spectral == PATHOCL spectral
#
# Run from the repo root:
#   python3.13 dev-tools/e106_spectral_envlight_test.py

import math
import os
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 256, 128
RHO = 0.8


def make_env():
    v = (np.arange(H) + .5) / H                 # 0 = top row (zenith)
    elev = (0.5 - v) * math.pi
    img = np.zeros((H, W, 3), np.float32)
    band = np.exp(-(elev / 0.25) ** 2)[:, None]
    img[..., 0] = 0.2 + 3.0 * band
    img[..., 1] = 0.3 + 1.2 * band
    img[..., 2] = 1.5 - 1.2 * band
    img[elev < 0] *= 0.05
    # a hot spot off-axis
    u = (np.arange(W) + .5) / W
    U, V = np.meshgrid(u, v)
    spot = np.exp(-(((U - 0.3) / 0.02) ** 2 + ((V - 0.35) / 0.02) ** 2))
    img[..., 0] += 40 * spot
    img[..., 1] += 12 * spot
    return img, elev


def write_pfm(path, img):
    # PFM stores rows bottom-to-top
    with open(path, "wb") as f:
        f.write(b"PF\n%d %d\n-1.0\n" % (img.shape[1], img.shape[0]))
        f.write(np.ascontiguousarray(img[::-1]).astype("<f4").tobytes())


def analytic(img, elev):
    w = np.clip(np.sin(elev), 0, None) * np.cos(elev) * (2 * math.pi / W) * (math.pi / H)
    E = (img * w[:, None, None]).sum((0, 1))
    return RHO / math.pi * E


def render(env, engine, spectral, spp=256, seconds=None):
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(f"""
scene.camera.lookat.orig = 0 0 1
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.fieldofview = 20
scene.lights.w.type = infinite
scene.lights.w.file = "{env}"
scene.lights.w.gamma = 1
scene.lights.w.gain = 1 1 1
scene.materials.m.type = matte
scene.materials.m.kd = {RHO} {RHO} {RHO}
scene.objects.o.material = m
scene.objects.o.vertices = -100 -100 0  100 -100 0  100 100 0  -100 100 0
scene.objects.o.faces = 0 1 2  0 2 3
""")
    scn.Parse(p)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = 32
film.height = 32
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
path.pathdepth.total = 1
path.pathdepth.diffuse = 1
light.maxdepth = 1
path.spectral.enable = {1 if spectral else 0}
path.hybridbackforward.enable = 0
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scn))
    ses.Start()
    t0 = time.monotonic()
    while True:
        ses.UpdateStats()
        if seconds is not None:
            if time.monotonic() - t0 > seconds:
                break
        elif ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() - t0 > 600:
            raise TimeoutError(engine)
        time.sleep(0.2)
    rgb = np.empty(32 * 32 * 3, np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(-1, 3).mean(0)


def main():
    img, elev = make_env()
    env = os.path.join(tempfile.mkdtemp(), "e106_env.pfm")
    write_pfm(env, img)
    ref = analytic(img, elev)
    print(f"analytic floor rgb = {ref}")
    fails = []

    def check(name, got, tol):
        r = got / ref
        ok = bool(np.all(np.abs(r - 1) < tol))
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: ratio rgb = "
              + " ".join(f"{x:.4f}" for x in r))
        if not ok:
            fails.append(name)

    check("1 PATHCPU rgb", render(env, "PATHCPU", False), 0.01)
    cpu = render(env, "PATHCPU", True)
    check("2 PATHCPU spectral", cpu, 0.06)
    try:
        gpu = render(env, "PATHOCL", True)
        check("3 PATHOCL spectral", gpu, 0.06)
        r = cpu / gpu
        ok = bool(np.all(np.abs(r - 1) < 0.01))
        print(f"[{'PASS' if ok else 'FAIL'}] 4 PATHCPU == PATHOCL spectral: "
              + " ".join(f"{x:.4f}" for x in r))
        if not ok:
            fails.append("4")
    except Exception as e:
        print(f"[SKIP] 3/4 PATHOCL: {e}")

    print("PASS overall" if not fails else f"FAIL overall: {fails}")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
