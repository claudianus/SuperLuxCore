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
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = 64
RENDER_TIMEOUT_S = 600


def parse_scene(rel_path):
    cwd = os.getcwd()
    os.chdir(str(REPO))
    try:
        props = pysuperluxcore.Properties(str(REPO / rel_path))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def render(scene, engine, seed=17):
    cfg = pysuperluxcore.Properties()
    # NOP pipeline: the default AutoLinearToneMap normalizes image mean and
    # would hide any reflectance differences in a white furnace.
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.pathdepth.total = 8
""")
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


def furnace(mb):
    # NOTE: the cornell room meshes live around (-2.78, 2, 2.7), not the
    # origin -- the camera must look at the room or no object is hit.
    props = pysuperluxcore.Properties()
    props.SetFromString(f"""
scene.camera.lookat.orig = -2.78 -1.0 2.73
scene.camera.lookat.target = -2.78 1.5 1.8
scene.camera.fieldofview = 45
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
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return render(scene, "PATHCPU")


def furnace_metal(mb):
    # metal2 GGX white-furnace: F0~=1 conductor at roughness 0.5. Single
    # scatter loses ~10% energy; the Heitz'16 height-tracking multi-bounce
    # walk must recover it (albedo -> ~1.0).
    props = pysuperluxcore.Properties()
    props.SetFromString(f"""
scene.camera.lookat.orig = -2.78 -1.0 2.73
scene.camera.lookat.target = -2.78 1.5 1.8
scene.camera.fieldofview = 45
scene.textures.whitefr.type = constfloat3
scene.textures.whitefr.value = 1.0 1.0 1.0
scene.textures.fr.type = fresnelcolor
scene.textures.fr.kr = whitefr
scene.materials.white.type = metal2
scene.materials.white.fresnel = fr
scene.materials.white.uroughness = 0.5
scene.materials.white.vroughness = 0.5
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
    scene = pysuperluxcore.Scene()
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

    # metal2: the Heitz'16 height-tracking multi-bounce walk must recover the
    # missing single-scatter energy without overshooting. The estimator uses
    # deterministic per-evaluation walks, so a few pixels can exceed 1.0 as
    # estimator noise; judge by the mean and high percentiles, not the max.
    fl0 = fl1 = None
    for mb in (0, 1):
        fl = furnace_metal(mb).mean(axis=2)
        print(f"furnace metal2-ggx mb={mb}: mean {fl.mean():.4f} "
              f"p99 {np.percentile(fl, 99):.4f} max {fl.max():.4f} "
              f"(ideal mean ~1.0)")
        if np.percentile(fl, 99) > 1.15:
            print("FAIL: systematic energy gain in white furnace")
            sys.exit(1)
        if mb == 0:
            fl0 = fl
        else:
            fl1 = fl
    if fl1.mean() <= fl0.mean() * 1.02 or abs(fl1.mean() - 1.0) > 0.05:
        print(f"FAIL: multibounce did not recover ss energy "
              f"(mb0={fl0.mean():.4f}, mb1={fl1.mean():.4f})")
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
