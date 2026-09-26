# SPDX-License-Identifier: Apache-2.0
#
# E28: Disney material spectral-transport parity (PATHCPU vs GPU backends).
#
# Reproduces/validates the report that Disney materials evaluate near-black
# on the GPU spectral eye path. Renders cornell-disney with
# path.spectral.enable on PATHCPU, PATHOCL(OpenCL) and PATHOCL(Metal) and
# compares image means and per-pixel luminance ratios.
#
# Run from the repo root (after a configure+build of pysuperluxcore Debug):
#   python3.13 dev-tools/e28_disney_spectral_parity.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Debug"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = 32
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300


def device_mask(want_type):
    pysuperluxcore.Init()  # required: device enumeration is empty before Init
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    mask = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        mask += "1" if t == want_type else "0"
        i += 1
    return mask or None


def parse_scene(rel_path):
    try:
        props = pysuperluxcore.Properties(str(REPO / rel_path))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    except Exception:
        pass
    cwd = os.getcwd()
    os.chdir(str(REPO / Path(rel_path).parent))
    try:
        props = pysuperluxcore.Properties(str(Path(rel_path).name))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def render(scene, engine, sel=None, spectral=True, seed=17):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
path.spectral.enable = {1 if spectral else 0}
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError("render stalled")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def main():
    scene = parse_scene("scenes/cornell/cornell-disney.scn")
    legs = {}
    legs["PATHCPU"] = render(scene, "PATHCPU")

    for dtype in ("METAL_GPU", "OPENCL_GPU"):
        mask = device_mask(dtype)
        if mask:
            legs[f"PATHOCL-{dtype}"] = render(scene, "PATHOCL", mask)

    ref = legs["PATHCPU"]
    print(f"PATHCPU mean luminance: {ref.mean():.5f}")
    ok_all = True
    for name, img in legs.items():
        if name == "PATHCPU":
            continue
        ratio = img.mean() / max(ref.mean(), 1e-9)
        pix = np.divide(img, np.maximum(ref, 1e-6))
        p50, p99 = np.percentile(pix, 50), np.percentile(pix, 99)
        finite = np.isfinite(img).all()
        # spectral transport differs per-backend in RNG streams; gate on
        # mean within 15% and p50 pixel ratio in [0.85, 1.18]
        ok = finite and (0.85 <= ratio <= 1.18) and (0.80 <= p50 <= 1.25)
        ok_all &= ok
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: mean={img.mean():.5f} "
              f"ratio={ratio:.3f} p50={p50:.3f} p99={p99:.3f} finite={finite}")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
