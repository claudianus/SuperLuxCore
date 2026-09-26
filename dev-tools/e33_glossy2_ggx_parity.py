# SPDX-License-Identifier: Apache-2.0
#
# E33: opt-in GGX ("distribution = ggx") CPU/GPU parity +
# white-furnace energy test.
#
# Renders scenes/glossy2ggx/glossy2-ggx.scn (Schlick / GGX / GGX+multibounce
# spheres) and scenes/ggxoptin/ggx-optin-coating.scn (glossycoating /
# glossytranslucent Schlick vs GGX pairs) on PATHCPU and PATHOCL and
# compares per-pixel luminance. Then renders white-furnace scenes with a
# glossy2-ggx floor+ball and verifies reflectance stays <= ~1.0 (no energy
# gain) with and without multibounce.
#
# Run from the repo root:
#   python3.13 dev-tools/e33_glossy2_ggx_parity.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Release"))
import pyluxcore

WIDTH, HEIGHT = 320, 240
SPP = 64
RENDER_TIMEOUT_S = 600


def parse_scene(rel_path):
    cwd = os.getcwd()
    os.chdir(str(REPO))
    try:
        props = pyluxcore.Properties(str(REPO / rel_path))
        scene = pyluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def render(scene, engine, seed=17):
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.pathdepth.total = 8
""")
    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
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
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3)


def furnace(mb):
    props = pyluxcore.Properties()
    props.SetFromString(f"""
scene.camera.lookat.orig = 0 -2 3
scene.camera.lookat.target = 0 0 0
scene.camera.fieldofview = 30
scene.materials.white.type = glossy2
scene.materials.white.kd = 1.0 1.0 1.0
scene.materials.white.ks = 1.0 1.0 1.0
scene.materials.white.uroughness = 0.3
scene.materials.white.vroughness = 0.3
scene.materials.white.distribution = ggx
scene.materials.white.multibounce = {mb}
scene.objects.floor.material = white
scene.objects.floor.ply = scenes/cornell/box.ply
scene.objects.ball.material = white
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
scene.objects.ball.transformation = 1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1.0 1.0 1.0
scene.lights.env.gain = 1.0 1.0 1.0
""")
    os.chdir(str(REPO))
    scene = pyluxcore.Scene()
    scene.Parse(props)
    return render(scene, "PATHCPU")


def check_parity(scene_path):
    scene = parse_scene(scene_path)

    print(f"== {scene_path} PATHCPU ==")
    cpu = render(scene, "PATHCPU")
    print(f"== {scene_path} PATHOCL ==")
    gpu = render(scene, "PATHOCL")

    lum = lambda a: a.mean(axis=2)
    cpu_l, gpu_l = lum(cpu), lum(gpu)
    mask = cpu_l > 1e-3
    ratio = np.abs(cpu_l[mask] - gpu_l[mask]) / np.maximum(cpu_l[mask], gpu_l[mask])
    print(f"parity: mean rel. error {ratio.mean():.4f}, "
          f"p95 {np.percentile(ratio, 95):.4f}, max {ratio.max():.4f}")
    if not np.isfinite(cpu).all() or not np.isfinite(gpu).all():
        print("FAIL: NaN/inf in output")
        sys.exit(1)
    if ratio.mean() > 0.15:
        print("FAIL: CPU/GPU parity out of tolerance")
        sys.exit(1)


def main():
    check_parity("scenes/glossy2ggx/glossy2-ggx.scn")
    check_parity("scenes/ggxoptin/ggx-optin-coating.scn")
    check_parity("scenes/ggxoptin/ggx-optin-carpaint.scn")

    for mb in (0, 1):
        fl = furnace(mb).mean(axis=2)
        print(f"furnace ggx mb={mb}: mean {fl.mean():.4f} max {fl.max():.4f} "
              f"(ideal <= ~1.0)")
        if fl.max() > 1.15:
            print("FAIL: energy gain detected in white furnace")
            sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
