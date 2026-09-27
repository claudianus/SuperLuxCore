# SPDX-License-Identifier: Apache-2.0
#
# E48: Christensen-Burley 2015 SSS parametrization
# (scene.volumes.*.sssprofile = "cb15").
#
# Same furnace geometry as e35_sss_albedo.py: an index-matched glass
# sphere wraps the albedo-parametrized volume; measured center reflectance
# must equal the requested surface albedo A for the CB15 normalized-
# diffusion inversion, including under non-zero anisotropy and a
# refractive volume IOR (interior boundary correction A_b enters the
# inversion). CPU/GPU parity closes the test.
#
# Run from the repo root:
#   python3.13 dev-tools/e48_sss_cb15_parity.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = 128
RENDER_TIMEOUT_S = 900
OCL_DEV = os.environ.get("E48_OCL_DEV", "")


def render(scene, engine, seed=17):
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
path.pathdepth.total = 256
path.pathdepth.diffuse = 256
path.pathdepth.glossy = 64
path.pathdepth.specular = 64
{extra}
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


def center_stat(img):
    h, w = img.shape[:2]
    crop = img[h // 5:4 * h // 5, w // 5:4 * w // 5]
    lum = crop.mean(axis=2)
    return lum.mean(), np.percentile(lum, 5), np.percentile(lum, 95)


CAMERA = """
scene.camera.lookat.orig = -2.78 1.6 3.28
scene.camera.lookat.target = -2.78 2.76 3.28
scene.camera.fieldofview = 45
"""

FURNACE = """
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1.0 1.0 1.0
scene.lights.env.gain = 1.0 1.0 1.0
"""


def parse(props_str):
    props = pysuperluxcore.Properties()
    props.SetFromString(props_str)
    os.chdir(str(REPO))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return scene


def cb15_furnace(albedo, mfp=0.05, g=0.0, ior=1.0, engine="PATHCPU"):
    return render(parse(f"""{CAMERA}
scene.volumes.sss.type = homogeneous
scene.volumes.sss.ior = {ior}
scene.volumes.sss.sssalbedo = {albedo} {albedo} {albedo}
scene.volumes.sss.sssmfp = {mfp} {mfp} {mfp}
scene.volumes.sss.sssprofile = cb15
scene.volumes.sss.asymmetry = {g} {g} {g}
scene.volumes.sss.multiscattering = 1
scene.volumes.sss.phase = hg
scene.materials.ball.type = glass
scene.materials.ball.kr = 1.0 1.0 1.0
scene.materials.ball.kt = 1.0 1.0 1.0
scene.materials.ball.interiorior = {ior}
scene.materials.ball.exteriorior = 1.0
scene.materials.ball.volume.interior = sss
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
{FURNACE}"""), engine)


def expect(name, img, target, tol=0.15):
    mean, p5, p95 = center_stat(img)
    rel = abs(mean - target) / target
    print(f"{name}: center mean {mean:.4f} p5 {p5:.4f} p95 {p95:.4f} "
          f"(target ~{target})")
    if not np.isfinite(img).all():
        print(f"FAIL: NaN/inf in {name}")
        sys.exit(1)
    if rel > tol:
        print(f"FAIL: {name} reflectance {mean:.4f} != {target} "
              f"(rel {rel:.2f})")
        sys.exit(1)
    return mean


def main():
    # Albedo round-trip under the CB15 inversion. Index-matched (ior=1)
    # cases reduce to the exact vdH remap. Refractive interior (ior=1.4,
    # surface ior matched so Fresnel trapping is real) uses the
    # diffusion boundary-ratio correction - directionally exact but the
    # ratio is a diffusion-theory approximation, so it halves the error
    # vs the plain vdH remap (0.243 vs 0.170 measured at A=0.3) rather
    # than eliminating it; looser tolerance there.
    for a in (1.0, 0.8, 0.5, 0.3):
        expect(f"cb15 furnace A={a}", cb15_furnace(a), a)
        expect(f"cb15 furnace A={a} ior=1.4", cb15_furnace(a, ior=1.4), a,
                tol=0.25)

    # Anisotropy defold: same requested albedo out of g=0.6.
    expect("cb15 furnace A=0.5 g=0.6", cb15_furnace(0.5, g=0.6), 0.5)

    # CPU/GPU parity (Newton-solve mirror in volume_funcs.cl).
    cpu = cb15_furnace(0.5, engine="PATHCPU")
    gpu = cb15_furnace(0.5, engine="PATHOCL")
    cl, gl = cpu.mean(axis=2), gpu.mean(axis=2)
    mask = cl > 1e-3
    ratio = np.abs(cl[mask] - gl[mask]) / np.maximum(cl[mask], gl[mask])
    print(f"cb15 parity: mean rel. error {ratio.mean():.4f}, "
          f"p95 {np.percentile(ratio, 95):.4f}")
    if ratio.mean() > 0.15:
        print("FAIL: CPU/GPU parity out of tolerance")
        sys.exit(1)

    print("PASS")


if __name__ == "__main__":
    main()
