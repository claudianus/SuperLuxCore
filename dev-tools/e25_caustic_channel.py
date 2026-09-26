# SPDX-License-Identifier: Apache-2.0
# CAUSTIC channel (RADIANCE_PER_SCREEN_NORMALIZED) view of the adaptive
# partition: shows exactly which contributions the light pass owns.
# adaptive ON should hand the rough-glass caustic to LT -> brighter
# CAUSTIC image under the sphere than adaptive OFF.

import os
import sys
import time
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Release"))
import pyluxcore

WIDTH, HEIGHT = 640, 360
SPP = int(sys.argv[1]) if len(sys.argv) > 1 else 96
RENDER_TIMEOUT_S = 600
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
        time.sleep(0.5)

    ses.Stop()
    OUT.mkdir(parents=True, exist_ok=True)
    film = ses.GetFilm()
    try:
        caustic = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
        film.GetOutputFloat(pyluxcore.FilmOutputType.CAUSTIC, caustic, 0, True)
        caustic = caustic.reshape(HEIGHT, WIDTH, 3).mean(axis=2)
    except Exception as e:
        print(f"{tag}: CAUSTIC channel unavailable: {e}", flush=True)
        caustic = np.zeros((HEIGHT, WIDTH), dtype=np.float32)

    # Normalize for display
    scale = 255.0 / max(caustic.max(), 1e-9)
    Image.fromarray(np.clip(caustic * scale, 0, 255).astype(np.uint8)).save(
        OUT / f"caustic_{tag}.png")
    print(f"{tag}: caustic sum={caustic.sum():.3f} max={caustic.max():.3f} "
          f"nonzero={np.count_nonzero(caustic > 1e-4)}", flush=True)
    return caustic


def main():
    scene = parse_scene()
    off = render(scene, "off", "path.hybridbackforward.adaptivecaustic = 0\n")
    on = render(scene, "on", "path.hybridbackforward.adaptivecaustic = 1\n")
    print(f"caustic coverage on/off = {on.sum() / max(off.sum(), 1e-9):.2f}x",
          flush=True)


if __name__ == "__main__":
    main()
