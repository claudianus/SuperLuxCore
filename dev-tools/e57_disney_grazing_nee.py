# SPDX-License-Identifier: Apache-2.0
#
# E57: Disney BSDF under grazing light-sampled (NEE) illumination.
#
# Two planes seen straight down by an orthographic camera, lit only by a
# sharpdistant (delta) light 10 degrees above the horizon, so every pixel
# is a pure Evaluate() connection:
#   left : disney, basecolor .5, specular 0, roughness 1 (Burley diffuse)
#   right: matte, kd .5 (Lambert reference)
# With the view on the normal (Fv = 0) the Burley diffuse over Lambert ratio
# is 1 + (Fd90 - 1) * (1 - cos(theta_l))^5, Fd90 = .5 + 2 r cos^2(theta_d):
# 1.260 here. Upstream LuxCoreRender 2.11 multiplied DisneyEvaluate() (which
# already carries cos(theta_l)) by cos(theta_l) a second time, so its NEE
# came out x cos(80 deg) = .17 too dark (.22) while BSDF-sampled hits were
# right - the grazing-lit Disney walls of a scene go dark upstream.
#
# Run from the repo root:
#   python3.13 dev-tools/e57_disney_grazing_nee.py [module_dir module_name]
# (default: the Release build of pysuperluxcore; pass e.g.
#  ../test-scenes/showcase-2026-10-12/_libs/up pyluxcore for upstream)

import importlib
import math
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
if len(sys.argv) == 3:
    sys.path.insert(0, str(Path(sys.argv[1]).resolve()))
    lux = importlib.import_module(sys.argv[2])
    TEST = False
else:
    sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
    import pysuperluxcore as lux
    TEST = True

W, H = 128, 64
ELEV = math.radians(10.0)
ROUGHNESS = 1.0


def expected_ratio():
    cl = math.sin(ELEV)                       # cos(theta_l)
    l = np.array([math.cos(ELEV), 0.0, math.sin(ELEV)])
    v = np.array([0.0, 0.0, 1.0])
    h = (l + v) / np.linalg.norm(l + v)
    ldoth = float(l @ h)
    fd90 = .5 + 2.0 * ROUGHNESS * ldoth * ldoth
    return 1.0 + (fd90 - 1.0) * (1.0 - cl) ** 5


def quad(name, x0, x1, mat):
    return f"""
scene.objects.{name}.material = {mat}
scene.objects.{name}.vertices = {x0} -0.5 0 {x1} -0.5 0 {x1} 0.5 0 {x0} 0.5 0
scene.objects.{name}.faces = 0 1 2 0 2 3
"""


def render():
    lux.Init()
    scn = lux.Properties()
    scn.SetFromString(f"""
scene.camera.type = orthographic
scene.camera.lookat.orig = 0 0 5
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 1 0
scene.camera.screenwindow = -1.1 1.1 -0.55 0.55
scene.lights.sun.type = sharpdistant
scene.lights.sun.direction = {-math.cos(ELEV)} 0 {-math.sin(ELEV)}
scene.lights.sun.color = 1 1 1
scene.lights.sun.gain = 1 1 1
scene.materials.disney.type = disney
scene.materials.disney.basecolor = 0.5 0.5 0.5
scene.materials.disney.specular = 0
scene.materials.disney.roughness = {ROUGHNESS}
scene.materials.matte.type = matte
scene.materials.matte.kd = 0.5 0.5 0.5
""" + quad("dplane", -1.05, -0.05, "disney") + quad("mplane", 0.05, 1.05, "matte"))
    scene = lux.Scene()
    scene.Parse(scn)
    cfg = lux.Properties()
    cfg.SetFromString(f"""
renderengine.type = PATHCPU
sampler.type = SOBOL
film.width = {W}
film.height = {H}
film.imagepipelines.0.0.type = NOP
path.pathdepth.total = 1
path.pathdepth.diffuse = 1
path.pathdepth.glossy = 1
path.pathdepth.specular = 1
batch.haltspp = 64
""")
    ses = lux.RenderSession(lux.RenderConfig(cfg, scene))
    ses.Start()
    t0 = time.monotonic()
    while not ses.HasDone():
        if time.monotonic() - t0 > 120:
            ses.Stop()
            raise TimeoutError("render stalled")
        time.sleep(0.1)
        ses.UpdateStats()
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(lux.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    ses.Stop()
    img = rgb.reshape(H, W, 3)
    # centre patches of each plane (screen x -1.1..1.1 -> 0..W)
    def patch(xc):
        c = int((xc + 1.1) / 2.2 * W)
        return float(img[H // 4: 3 * H // 4, c - W // 10: c + W // 10].mean())
    return patch(-0.55), patch(0.55)


def main():
    d, m = render()
    r, e = d / m, expected_ratio()
    print(f"{lux.__name__} {lux.Version()}: disney {d:.5f} matte {m:.5f} ratio {r:.4f} "
          f"(Burley {e:.4f}, x cos(theta_l) = {e * math.sin(ELEV):.4f})")
    if not TEST:
        return
    if abs(r / e - 1.0) > 0.02:
        print("FAIL: Disney NEE does not match the Burley diffuse at grazing light")
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
