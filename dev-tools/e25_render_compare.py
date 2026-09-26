# SPDX-License-Identifier: Apache-2.0
# 720p visual validation for the adaptive caustic partition: renders
# caustic-roughglass.scn with the adaptive classifier OFF and ON
# (PATHCPU hybrid) and saves tonemapped PNGs + raw HDR stats.

import os
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Release"))
import pyluxcore

WIDTH, HEIGHT = 1280, 720
SPP = int(sys.argv[1]) if len(sys.argv) > 1 else 64
RENDER_TIMEOUT_S = 900
SCENE = REPO / "scenes/cornell/caustic-roughglass.scn"
OUT = REPO / "out/e25"


def parse_scene():
    cwd = os.getcwd()
    os.chdir(str(REPO))
    try:
        props = pyluxcore.Properties(str(SCENE))
        scene = pyluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def render(scene, tag, extra):
    cfg = pyluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHCPU
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 17
opencl.native.threads.count = 0
path.hybridbackforward.enable = 1
path.hybridbackforward.partition = 0.8
{extra}
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
            raise TimeoutError("render stalled")
        time.sleep(1.0)
    # Stop() performs the final UpdateFilmLockLess() - thread/task film
    # merges (incl. light-pass splats) only happen then.
    ses.Stop()
    OUT.mkdir(parents=True, exist_ok=True)
    tonemapped = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 tonemapped, 0, True)
    img8 = (np.clip(tonemapped, 0, 1).reshape(HEIGHT, WIDTH, 3) * 255).astype(np.uint8)
    Image.fromarray(img8).save(OUT / f"{tag}.png")

    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB, rgb, 0, True)
    lum = rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)
    # Caustic patch below the sphere (720p: sphere ~ (640,400))
    patch = lum[360:600, 480:820]
    print(f"{tag}: patch mean={patch.mean():.4f} p99={np.percentile(patch, 99):.3f} "
          f"max={patch.max():.3f}", flush=True)
    return lum


def main():
    scene = parse_scene()
    off = render(scene, "adaptive_off",
                 "path.hybridbackforward.adaptivecaustic = 0\n")
    on = render(scene, "adaptive_on",
                "path.hybridbackforward.adaptivecaustic = 1\n")
    diff = np.abs(on - off)
    print(f"diff: mean={diff.mean():.5f} max={diff.max():.3f}", flush=True)


if __name__ == "__main__":
    main()
