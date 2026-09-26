# SPDX-License-Identifier: Apache-2.0
#
# E31: OpenPBR material CPU/GPU parity + white-furnace energy test.
#
# Renders scenes/openpbr/openpbr-lobes.scn on PATHCPU and PATHOCL and compares
# per-pixel luminance. Then renders a white-furnace scene (white openpbr
# diffuse sphere + walls under a constant infinite light) and verifies the
# mean albedo stays physically plausible (<= ~1.0, no energy gain).
#
# Run from the repo root:
#   python3.13 dev-tools/e31_openpbr_parity.py

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


def render(scene, engine, seed=17, extra_cfg=""):
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.pathdepth.total = 8
{extra_cfg}
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


def main():
    scene = parse_scene("scenes/openpbr/openpbr-lobes.scn")

    print("== PATHCPU ==")
    cpu = render(scene, "PATHCPU")
    print("== PATHOCL ==")
    gpu = render(scene, "PATHOCL")

    lum = lambda a: a.mean(axis=2)
    cpu_l, gpu_l = lum(cpu), lum(gpu)
    mask = cpu_l > 1e-3
    ratio = np.abs(cpu_l[mask] - gpu_l[mask]) / np.maximum(cpu_l[mask], gpu_l[mask])
    print(f"parity: mean rel. error {ratio.mean():.4f}, "
          f"p95 {np.percentile(ratio, 95):.4f}, max {ratio.max():.4f}")

    # White-furnace energy check: uniform env + diffuse openpbr floor.
    props = pyluxcore.Properties()
    props.SetFromString("""
scene.camera.lookat.orig = 0 0 3
scene.camera.lookat.target = 0 0 0
scene.camera.fieldofview = 30
scene.materials.white.type = openpbr
scene.materials.white.basecolor = 1.0 1.0 1.0
scene.materials.white.specularweight = 0.0
scene.materials.white.basediffuseroughness = 0.0
scene.materials.white-metal.type = openpbr
scene.materials.white-metal.basecolor = 1.0 1.0 1.0
scene.materials.white-metal.basemetalness = 1.0
scene.materials.white-metal.specularroughness = 0.0
scene.objects.floor.material = white
scene.objects.floor.ply = scenes/cornell/box.ply
scene.objects.ball.material = white-metal
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
scene.objects.ball.transformation = 1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1.0 1.0 1.0
scene.lights.env.gain = 1.0 1.0 1.0
""")
    os.chdir(str(REPO))
    fscene = pyluxcore.Scene()
    fscene.Parse(props)
    fur = render(fscene, "PATHCPU")
    fl = fur.mean(axis=2)
    print(f"furnace: mean {fl.mean():.4f} max {fl.max():.4f} "
          f"(ideal diffuse floor ~1.0, metal ball ~1.0)")
    if fl.max() > 1.15:
        print("FAIL: energy gain detected in white furnace")
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
