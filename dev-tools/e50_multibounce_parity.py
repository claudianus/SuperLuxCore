# SPDX-License-Identifier: Apache-2.0
#
# E50: multi-scattering (Turquin / coating-style) compensation on
# roughglass, carpaint, disney specular lobes.
#
# Checks per material:
#   1. multibounce=1 yields >= multibounce=0 energy on a white furnace
#      (scattering adds back lost energy; a ratio well above 1 for rough
#      lobes, near 1 for smooth ones).
#   2. CPU/GPU parity.
#
# Run from the repo root:
#   python3.13 dev-tools/e50_multibounce_parity.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = 96
RENDER_TIMEOUT_S = 900
OCL_DEV = os.environ.get("E50_OCL_DEV", "")

CAMERA = """
scene.camera.lookat.orig = -2.78 1.6 3.28
scene.camera.lookat.target = -2.78 2.76 3.28
scene.camera.fieldofview = 45
"""

ENV = """
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1.0 1.0 1.0
scene.lights.env.gain = 1.0 1.0 1.0
"""


def render(props_str, engine, seed=17):
    cfg = pysuperluxcore.Properties()
    extra = f'opencl.devices.select = "{OCL_DEV}"' if OCL_DEV else ""
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
{extra}
""")
    props = pysuperluxcore.Properties()
    props.SetFromString(props_str)
    os.chdir(str(REPO))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled ({engine})")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def sphere_scene(mat_props):
    return f"""{CAMERA}
{mat_props}
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
{ENV}"""


MATS = {
    "roughglass-ggx": """
scene.materials.ball.type = roughglass
scene.materials.ball.kr = 1 1 1
scene.materials.ball.kt = 0 0 0
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.interiorior = 1.5
scene.materials.ball.uroughness = 0.4
scene.materials.ball.vroughness = 0.4
scene.materials.ball.distribution = ggx
scene.materials.ball.multibounce = {mb}
""",
    "roughglass-schlick": """
scene.materials.ball.type = roughglass
scene.materials.ball.kr = 1 1 1
scene.materials.ball.kt = 0 0 0
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.interiorior = 1.5
scene.materials.ball.uroughness = 0.4
scene.materials.ball.vroughness = 0.4
scene.materials.ball.multibounce = {mb}
""",
    "carpaint-ggx": """
scene.materials.ball.type = carpaint
scene.materials.ball.kd = 0.1 0.1 0.1
scene.materials.ball.ks1 = 0.5 0.5 0.5
scene.materials.ball.ks2 = 0.3 0.3 0.3
scene.materials.ball.ks3 = 0.2 0.2 0.2
scene.materials.ball.m1 = 0.5
scene.materials.ball.m2 = 0.3
scene.materials.ball.m3 = 0.15
scene.materials.ball.distribution = ggx
scene.materials.ball.multibounce = {mb}
""",
    "disney": """
scene.materials.ball.type = disney
scene.materials.ball.basecolor = 0.7 0.7 0.7
scene.materials.ball.metallic = 0.9
scene.materials.ball.roughness = 0.55
scene.materials.ball.specular = 1.0
scene.materials.ball.multibounce = {mb}
""",
}


def check(name):
    off = render(sphere_scene(MATS[name].format(mb=0)), "PATHCPU")
    on = render(sphere_scene(MATS[name].format(mb=1)), "PATHCPU")
    gpu = render(sphere_scene(MATS[name].format(mb=1)), "PATHOCL")
    for tag, img in (("off", off), ("on", on), ("gpu", gpu)):
        if not np.isfinite(img).all():
            print(f"FAIL: NaN/inf in {name} {tag}")
            return False
    mo, mn, mg = off.mean(), on.mean(), gpu.mean()
    if mo < 1e-3 or mn < 1e-3:
        print(f"FAIL: {name} black (off={mo:.4f} on={mn:.4f})")
        return False
    ratio = mn / mo
    parity = abs(mg - mn) / max(mn, 1e-9)
    ok_energy = ratio >= 0.98  # MS must not lose energy
    ok_parity = parity <= 0.20
    print(f"{name}: off={mo:.4f} on={mn:.4f} gain={ratio:.3f} "
          f"gpu={mg:.4f} parity={parity:.3f} "
          f"[{'PASS' if ok_energy and ok_parity else 'FAIL'}]")
    return ok_energy and ok_parity


def main():
    ok = True
    for name in MATS:
        ok &= check(name)
    print("PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
