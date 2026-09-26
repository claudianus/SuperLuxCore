# SPDX-License-Identifier: Apache-2.0
# Fast debug harness for e35: isolates TILEPATHOCL eye-only vs
# eye+light on a small render.

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
import pyluxcore

sys.path.insert(0, str(REPO / "dev-tools"))
from e35_tilepath_lighttracing import (SCENE_PROPS, device_mask, parse_scene,
        save_png, luminance)

WIDTH, HEIGHT = 640, 360
SPP = 32
RENDER_TIMEOUT_S = 240
OUT_DIR = REPO / "dev-tools/out/e35dbg"


def render(scene, engine, lt, hybrid, sel=None, tile_size=None):
    extra = ""
    if tile_size:
        extra += f"tile.size.x = {tile_size[0]}\ntile.size.y = {tile_size[1]}\n"
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = {"TILEPATHSAMPLER" if "TILEPATH" in engine else "SOBOL"}
batch.haltspp = {SPP}
renderengine.seed = 17
path.hybridbackforward.enable = {hybrid}
path.lighttracing.enable = {lt}
path.lighttracing.taskfraction = 0.5
film.imagepipelines.0.0.type = NOP
film.imagepipelines.0.1.type = TONEMAP_LINEAR
film.imagepipelines.0.1.scale = 1
film.imagepipelines.0.2.type = GAMMA_CORRECTION
film.imagepipelines.0.2.value = 2.2
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.index = 0
{extra}""")
    if sel:
        cfg.Set(pyluxcore.Property("opencl.devices.select", sel))
    ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(0.5)
        ses.UpdateStats()
        if ses.HasDone():
            break
    ses.Stop()
    buf = np.zeros((HEIGHT, WIDTH, 3), dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
            pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf, 0)
    return buf


def report(name, img, ref):
    L, refL = luminance(img), luminance(ref)
    rmse = float(np.sqrt(((L - refL) ** 2).mean()))
    print(f"{name}: mean={L.mean():.5f} ratio={L.mean()/refL.mean():.4f} "
            f"rmse={rmse:.5f}", flush=True)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    scene = parse_scene()
    mask = device_mask("METAL_GPU")
    tile = (160, 90)

    print("=== CPU LT=0 hybrid=0 ===", flush=True)
    cpu0 = render(scene, "PATHCPU", 0, 0)
    save_png(cpu0, OUT_DIR / "cpu_lt0.png")

    print("=== TILE-Metal LT=0 hybrid=0 ===", flush=True)
    t0 = render(scene, "TILEPATHOCL", 0, 0, sel=mask, tile_size=tile)
    save_png(t0, OUT_DIR / "tile_lt0.png")
    report("tile_lt0", t0, cpu0)

    print("=== TILE-Metal LT=1 hybrid=1 ===", flush=True)
    t1 = render(scene, "TILEPATHOCL", 1, 1, sel=mask, tile_size=tile)
    save_png(t1, OUT_DIR / "tile_lt1.png")

    print("=== CPU LT=1 hybrid=1 ===", flush=True)
    cpu1 = render(scene, "PATHCPU", 1, 1)
    save_png(cpu1, OUT_DIR / "cpu_lt1.png")
    report("tile_lt1", t1, cpu1)

    # Light-channel-only delta: (LT1 - LT0) per backend
    d_cpu = np.clip(cpu1 - cpu0, 0, None)
    d_gpu = np.clip(t1 - t0, 0, None)
    report("delta_gpu", d_gpu, d_cpu)
    save_png(d_cpu * 4.0, OUT_DIR / "delta_cpu_x4.png")
    save_png(d_gpu * 4.0, OUT_DIR / "delta_gpu_x4.png")


if __name__ == "__main__":
    main()
