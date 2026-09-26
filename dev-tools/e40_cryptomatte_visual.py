# SPDX-License-Identifier: Apache-2.0
#
# E40 visual: 720p render + Cryptomatte matte decode visualization.
# Renders scenes/cornell with CRYPTOMATTE_OBJECT, then writes:
#   - beauty.png (tonemapped RGB)
#   - matte_<name>.png per object id (coverage as greyscale)
#   - matte_preview.png (top ids tinted distinct colors, coverage-weighted)
#
import sys
import tempfile
import time
from pathlib import Path

import numpy as np
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from e40_cryptomatte_test import (LEVELS, STRIDE_OUT, device_mask, id_hex,
                                murmur3_32, hash_to_float, name_id)

REPO = Path(__file__).resolve().parent.parent
for _variant in ("Release", "Debug"):
    _p = REPO / "out/build/src/pysuperluxcore" / _variant
    if any(_p.glob("pysuperluxcore*.so")):
        sys.path.insert(0, str(_p))
        break
import pysuperluxcore

W, H = 1280, 720
SPP = 256
OUT = REPO / "dev-tools/out/e40"
OUT.mkdir(parents=True, exist_ok=True)


def render(engine, sel=None):
    scn = pysuperluxcore.Properties(str(REPO / "scenes/cornell/cornell.scn"))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
opencl.task.count = {1 << 16}
film.imagepipelines.0.0.type = TONEMAP_LINEAR
film.imagepipelines.0.0.scale = 0.7
film.imagepipelines.0.1.type = GAMMA_CORRECTION
film.imagepipelines.0.1.value = 2.2
film.outputs.0.type = CRYPTOMATTE_OBJECT
film.outputs.0.filename = {OUT}/crypto.exr
film.outputs.1.type = RGB_IMAGEPIPELINE
film.outputs.1.index = 0
film.outputs.1.filename = {OUT}/beauty.png
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + 900
    while True:
        ses.UpdateStats()
        p = ses.GetStats().Get("stats.renderengine.pass").GetInt()
        if p >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError("stalled")
        time.sleep(1)
    film = ses.GetFilm()
    obj = np.empty(W * H * STRIDE_OUT, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.CRYPTOMATTE_OBJECT, obj)
    rgb = np.empty(W * H * 3, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb)
    film.SaveOutputs()
    ses.Stop()
    return (obj.reshape(H, W, LEVELS, 2),
            rgb.reshape(H, W, 3))


def save_png(path, arr):
    Image.fromarray((np.clip(arr, 0, 1) * 255).astype(np.uint8)).save(path)


NAMES = ["Khaki", "HalveRed", "DarkGreen", "Grey"]
PALETTE = {
    "Khaki": np.array([1.0, 0.85, 0.2]),
    "HalveRed": np.array([0.95, 0.1, 0.1]),
    "DarkGreen": np.array([0.1, 0.8, 0.2]),
    "Grey": np.array([0.3, 0.6, 1.0]),
}

crypto, rgb = render("PATHCPU")
save_png(OUT / "beauty.png", rgb)

preview = np.zeros((H, W, 3), dtype=np.float32)
for name in NAMES:
    fid = name_id(name)
    match = crypto[..., 0] == fid          # (H, W, LEVELS)
    cov = (crypto[..., 1] * match).sum(-1)  # (H, W)
    save_png(OUT / f"matte_{name}.png", np.dstack([cov] * 3))
    preview += cov[..., None] * PALETTE[name]
save_png(OUT / "matte_preview.png", preview)
print("wrote", OUT)
