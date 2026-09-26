# SPDX-License-Identifier: Apache-2.0
#
# Shared CPU/GPU parity-test infrastructure for the dev-tools/parity
# matrix runner. Every parity scene renders the same film config
# (1280x720, linear tonemap + gamma) so results are comparable.

import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pyluxcore/Debug"))
sys.path.insert(0, str(REPO / "dev-tools"))
import pyluxcore

WIDTH, HEIGHT = 1280, 720
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 300

BASE_CFG = f"""
film.width = {WIDTH}
film.height = {HEIGHT}
batch.haltspp = {{spp}}
renderengine.seed = {{seed}}
opencl.task.count = {TASK_COUNT}
film.imagepipelines.0.0.type = NOP
film.imagepipelines.0.1.type = TONEMAP_LINEAR
film.imagepipelines.0.1.scale = 1
film.imagepipelines.0.2.type = GAMMA_CORRECTION
film.imagepipelines.0.2.value = 2.2
film.outputs.0.type = RGB_IMAGEPIPELINE
film.outputs.0.index = 0
"""


def device_mask(want_type):
    pyluxcore.Init()  # required: device enumeration is empty before Init
    descs = pyluxcore.GetOpenCLDeviceDescs()
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


def load_scene(spec):
    """spec.props: inline Properties text; spec.props_file: repo-relative
    .scn path (parsed with cwd at the repo root - the shipped scenes
    reference assets as 'scenes/<dir>/<file>'); spec.builder: zero-arg
    callable returning a Scene for API-built geometry (meshes, vertex
    motion)."""
    if spec.get("builder"):
        return spec["builder"]()
    scene = pyluxcore.Scene()
    if spec.get("props_file"):
        import os
        rel = REPO / spec["props_file"]
        cwd = os.getcwd()
        os.chdir(str(REPO))
        try:
            scene.Parse(pyluxcore.Properties(str(rel)))
        finally:
            os.chdir(cwd)
    else:
        props = pyluxcore.Properties()
        props.SetFromString(spec["props"])
        scene.Parse(props)
    return scene


def render(scene, spec, sel=None, seed=17, spp=None):
    cfg_body = BASE_CFG.format(spp=spp or spec["spp"], seed=seed)
    cfg_body += f"renderengine.type = {spec['engine']}\n"
    cfg_body += f"sampler.type = {spec['sampler']}\n"
    cfg_body += spec.get("cfg_extra", "")
    cfg = pyluxcore.Properties()
    cfg.SetFromString(cfg_body)
    if sel:
        cfg.Set(pyluxcore.Property("opencl.devices.select", sel))
    # Per-scene env overrides (e.g. LUXRAYS_METAL_CURVES): the accel reads
    # them during session Start() on the render thread, so they must stay
    # set for the whole render.
    import os
    old_env = {}
    for k, v in spec.get("env", {}).items():
        old_env[k] = os.environ.get(k)
        os.environ[k] = v
    try:
        ses = pyluxcore.RenderSession(pyluxcore.RenderConfig(cfg, scene))
        ses.Start()
        deadline = time.monotonic() + (spec.get("timeout") or RENDER_TIMEOUT_S)
        while time.monotonic() < deadline:
            time.sleep(0.5)
            # Halt conditions (batch.haltspp) are evaluated inside
            # Film::RunTests which only runs during the stats/film update
            # cycle - polling HasDone() alone never fires the halt
            ses.UpdateStats()
            if ses.HasDone():
                break
        ses.Stop()
    finally:
        for k, v in old_env.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v
    film = ses.GetFilm()
    buf = np.zeros((HEIGHT, WIDTH, 3), dtype=np.float32)
    film.GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf, 0)
    return buf


def luminance(img):
    return 0.2126 * img[..., 0] + 0.7152 * img[..., 1] + 0.0722 * img[..., 2]


def metrics(ref, img):
    """Parity metrics vs the CPU reference. black_frac is the share of
    pixels that are ~zero in img but lit in ref (structural artifact
    detector: dead tiles, missing splats)."""
    refL, L = luminance(ref), luminance(img)
    lit = refL > 0.02
    return {
        "ratio": float(L.mean() / max(refL.mean(), 1e-9)),
        "rmse": float(np.sqrt(((L - refL) ** 2).mean())),
        "nans": int(np.isnan(img).sum()),
        "black_frac": float(((L < 0.002) & lit).mean()),
    }


def save_ppm(img, path):
    path.parent.mkdir(parents=True, exist_ok=True)
    arr = (np.clip(img, 0, 1) * 255).astype(np.uint8)
    with open(str(path.with_suffix(".ppm")), "wb") as f:
        f.write(f"P6\n{arr.shape[1]} {arr.shape[0]}\n255\n".encode())
        f.write(arr.tobytes())
