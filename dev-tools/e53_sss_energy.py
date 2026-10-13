# SPDX-License-Identifier: Apache-2.0
#
# E53: OpenPBR subsurface energy in a white furnace.
#
# An OpenPBR sphere with base = subsurface color A = .8, subsurface
# weight 1 and a radius 1/100 of the sphere size (the high-albedo
# diffusion regime of skin-like media) seen at normal incidence in a
# white furnace must return ~A minus the dielectric boundary loss
# (~.78), with the engine-default path depths. Verified:
#   1. CB15 albedo inversion: the old 6-step fixed point oscillated
#      (A=.8, eta=1.5 flipped between a=.9909/.99995) and lost ~20%
#      (.606), CPU and GPU landing on different iterates (.606/.690),
#   2. default path depths: the random walk counted against total 6 /
#      diffuse 4 and its boundary reflections against glossy 4 (.13);
#      path.pathdepth.volume.auto now gives it its own budget, and a
#      surface hit from inside an SSS medium is a walk step,
#   3. path.pathdepth.volume.auto = 0 still truncates (the scene is
#      sensitive to the depth budget),
#   4. CPU/GPU parity (pure GPU, 8192 tasks: the default 128K task
#      population keeps whole samples per pixel in flight on a small
#      film and the halt drops the long walks - a short-path bias).
#
# Run from the repo root:
#   python3.13 dev-tools/e53_sss_energy.py
#
# Env: SUPERLUXCORE_BACKENDS=cpu,opencl,metal (leg subset, default all);
#      E53_OCL_DEV overrides the PATHOCL opencl.devices.select mask.

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 32, 32
SPP = 1024
RENDER_TIMEOUT_S = 900
OCL_DEV = os.environ.get("E53_OCL_DEV", "")

BACKENDS = {b.strip() for b in
        os.environ.get("SUPERLUXCORE_BACKENDS", "cpu,opencl,metal").split(",")
        if b.strip()}

# sphere-mid.ply: centre (-2.776, 2.760, 3.279), radius .8537; a narrow
# camera keeps the whole film near normal incidence
SCENE = """
scene.camera.lookat.orig = -2.776 1.0 3.279
scene.camera.lookat.target = -2.776 2.76 3.279
scene.camera.fieldofview = 2
scene.lights.env.type = constantinfinite
scene.lights.env.color = 1.0 1.0 1.0
scene.lights.env.gain = 1.0 1.0 1.0
scene.materials.ball.type = openpbr
scene.materials.ball.basecolor = 0.8 0.8 0.8
scene.materials.ball.subsurfacecolor = 0.8 0.8 0.8
scene.materials.ball.subsurfaceweight = 1.0
scene.materials.ball.subsurfaceradius = 0.0085 0.0085 0.0085
scene.objects.ball.material = ball
scene.objects.ball.ply = scenes/cornell/sphere-mid.ply
"""


def parse():
    props = pysuperluxcore.Properties()
    props.SetFromString(SCENE)
    os.chdir(str(REPO))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return scene


def render(engine, sel=None, extra=""):
    cfg = pysuperluxcore.Properties()
    lines = [f"film.width = {WIDTH}", f"film.height = {HEIGHT}",
             "film.imagepipelines.0.0.type = NOP",
             f"renderengine.type = {engine}", "sampler.type = SOBOL",
             f"batch.haltspp = {SPP}", "renderengine.seed = 17"]
    if engine.endswith("OCL"):
        dev = sel if sel is not None else OCL_DEV
        if dev:
            lines.append(f'opencl.devices.select = "{dev}"')
        lines += ["opencl.native.threads.count = 0",
                  "opencl.task.count = 8192"]
    # No path.pathdepth.*: the engine defaults are under test
    cfg.SetFromString("\n".join(lines) + "\n" + extra + "\n")
    ses = pysuperluxcore.RenderSession(
        pysuperluxcore.RenderConfig(cfg, parse()))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while not ses.HasDone():
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled ({engine})")
        time.sleep(0.2)
        ses.UpdateStats()
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    ses.Stop()
    img = rgb.reshape(HEIGHT, WIDTH, 3)
    if not np.isfinite(img).all():
        print(f"FAIL: NaN/inf ({engine})")
        sys.exit(1)
    return float(img.mean())


def main():
    engines = []
    if "cpu" in BACKENDS:
        engines.append(("cpu", "PATHCPU", None))
    for name, dtype in (("opencl", "OPENCL_GPU"), ("metal", "METAL_GPU")):
        if name in BACKENDS:
            pysuperluxcore.Init()
            descs = pysuperluxcore.GetOpenCLDeviceDescs()
            mask, i = "", 0
            while True:
                try:
                    t = descs.Get(f"opencl.device.{i}.type").GetString()
                except Exception:
                    break
                mask += "1" if t == dtype else "0"
                i += 1
            if "1" in mask:
                engines.append((name, "PATHOCL", mask))
    if not engines:
        print("SKIP: no backends requested/available")
        return

    ok = True
    ref = None
    for name, engine, sel in engines:
        v = render(engine, sel)
        print(f"{name}: furnace SSS albedo {v:.4f} (expected ~.78)")
        if not (0.75 < v < 0.81):
            print(f"FAIL: {name} SSS energy out of range")
            ok = False
        if ref is None:
            ref = (name, v)
        elif abs(v - ref[1]) / ref[1] > 0.02:
            print(f"FAIL: {name}/{ref[0]} parity {v:.4f} vs {ref[1]:.4f}")
            ok = False

    name, engine, sel = engines[0]
    v = render(engine, sel, "path.pathdepth.volume.auto = 0")
    print(f"{name}: volume.auto = 0 -> {v:.4f} (truncated, expected < .3)")
    if not v < 0.3:
        print("FAIL: the furnace is not sensitive to the depth budget")
        ok = False

    if not ok:
        sys.exit(1)
    print("PASS")


if __name__ == "__main__":
    main()
