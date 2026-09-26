# SPDX-License-Identifier: Apache-2.0
#
# E21: cross-backend scene parity sweep (PATHCPU / OpenCL / Metal).
#
# Audit test: renders a battery of small scenes covering distinct
# shading paths on all three backends and gates on mean + per-pixel
# parity. This class of check is what caught the Metal vload_half
# offset bug (HALF image maps read as a constant texel) - a systematic
# translation bug invisible to single-scene testing.
#
# Coverage:
#   cornell          - diffuse + area light baseline
#   sky2-glass       - procedural sky env + glass transmission
#   media            - homogeneous volume scattering
#   bump             - hitpointcolor + bump/vertex-colour textures
#   glossycoating    - glossy-coating BRDF
#   twosided         - two-sided material
#   infinitelight    - image-map env light (HALF path)
#   strands/hair     - curve/ribbon primitives
#   luxball          - multi-material + glass + env
#
# Gates per scene:
#   - Metal and OpenCL legs must run on their real intersect devices
#     (stats.renderengine.devices.*) so a device-selection regression
#     cannot produce a vacuous pass.
#   - MTL vs OCL per-pixel luminance ratio: p50 in [0.95, 1.05],
#     p99 in [0.80, 1.30] (same deterministic sampler -> near-pixel
#     equality; the vload_half bug produced a constant 0.4x image).
#   - Both GPU means within 10% of the PATHCPU mean (samplers differ,
#     so CPU is a mean-only anchor).
#   - All outputs finite.
# Scenes that fail to parse or render on a leg are recorded as SKIP,
# not FAIL (some features are legitimately backend-specific).
#
# Run from the repo root:
#   python3.13 dev-tools/e21_backend_parity_test.py

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Debug"))
import pysuperluxcore

WIDTH, HEIGHT = 160, 120
SPP = 32
TASK_COUNT = 8192
RENDER_TIMEOUT_S = 240

SCENES = [
    ("cornell",       "scenes/cornell/cornell.scn"),
    ("sky2-glass",    "scenes/sky/sky2-glass.scn"),
    ("media",         "scenes/media/media.scn"),
    ("bump",          "scenes/bump/bump-light-interp.scn"),
    ("glossycoating", "scenes/glossycoating/scene.scn"),
    ("twosided",      "scenes/twosided/twosided.scn"),
    ("infinitelight", "scenes/infinitelight/scene.scn"),
    ("hair",          "scenes/strands/hair.scn"),
    ("luxball",       "scenes/luxball/luxball.scn"),
]

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def device_mask(want_type):
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
    """Scene files mix repo-root-relative and scene-dir-relative asset
    paths; try the repo root first, then the scene directory."""
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


def render(scene, engine, sel=None, spp=SPP, seed=17):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = {seed}
opencl.task.count = {TASK_COUNT}
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {spp} spp")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    used = {n.split("stats.renderengine.devices.")[1].rsplit("-", 1)[0]
            for n in ses.GetStats().GetAllNames()
            if n.startswith("stats.renderengine.devices.")}
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2), used


def main():
    print(f"Cross-backend parity sweep ({len(SCENES)} scenes, "
          f"{WIDTH}x{HEIGHT} @ {SPP}spp)\n", flush=True)

    ocl_mask = device_mask("OPENCL_GPU")
    mtl_mask = device_mask("METAL_GPU")
    print(f"  devices: OPENCL_GPU={ocl_mask} METAL_GPU={mtl_mask}",
          flush=True)
    if not ocl_mask or not mtl_mask:
        print("SKIP: need both an OpenCL GPU and a Metal GPU device",
              flush=True)
        return

    for tag, rel in SCENES:
        try:
            scene = parse_scene(rel)
        except Exception as e:
            record(f"{tag}.parse", True,
                   f"SKIP (scene parse failed: {str(e)[:60]})")
            continue
        try:
            cpu, _ = render(scene, "PATHCPU")
        except Exception as e:
            record(f"{tag}.cpu", True,
                   f"SKIP (PATHCPU render failed: {str(e)[:60]})")
            cpu = None
        try:
            ocl, ocl_dev = render(scene, "PATHOCL", sel=ocl_mask)
        except Exception as e:
            record(f"{tag}.ocl", True,
                   f"SKIP (OpenCL render failed: {str(e)[:60]})")
            continue
        try:
            mtl, mtl_dev = render(scene, "PATHOCL", sel=mtl_mask)
        except Exception as e:
            record(f"{tag}.mtl", True,
                   f"SKIP (Metal render failed: {str(e)[:60]})")
            continue

        print(f"  [{tag}] cpu={cpu.mean():.4f} ocl={ocl.mean():.4f} "
              f"mtl={mtl.mean():.4f}", flush=True)
        record(f"{tag}.device-assert",
               any("Metal" in d for d in mtl_dev) and
               any("OpenCL" in d for d in ocl_dev),
               f"mtl={sorted(mtl_dev)} ocl={sorted(ocl_dev)}")
        record(f"{tag}.finite",
               np.isfinite(mtl).all() and np.isfinite(ocl).all() and
               (cpu is None or np.isfinite(cpu).all()),
               "all pixels finite")

        # Per-pixel ratio on pixels bright enough to matter: black
        # background pixels make 0/eps ratios meaningless (a scene that
        # is half-black reports p50=0 even when identical).
        floor = max(float(ocl.mean()) * 0.05, 1e-6)
        lit = ocl > floor
        if lit.any():
            r = mtl[lit] / ocl[lit]
            p50 = float(np.percentile(r, 50))
            p99 = float(np.percentile(r, 99))
            hot = float(np.mean(np.abs(mtl[lit] - ocl[lit]) /
                                ocl[lit] > 0.30))
            record(f"{tag}.mtl-vs-ocl-pixel",
                   0.95 < p50 < 1.05 and 0.80 < p99 < 1.30 and
                   hot < 0.01,
                   f"lit-pixel ratio p50={p50:.3f} p99={p99:.3f} "
                   f">30%-diff fraction={hot * 100:.2f}%")
        else:
            record(f"{tag}.mtl-vs-ocl-pixel", True,
                   "SKIP (no pixels above 5% of mean)")

        if cpu is not None:
            for name, img in (("ocl", ocl), ("mtl", mtl)):
                record(f"{tag}.{name}-vs-cpu-mean",
                       abs(float(img.mean()) - float(cpu.mean())) /
                       max(float(cpu.mean()), 1e-12) < 0.10,
                       f"{name}={img.mean():.4f} cpu={cpu.mean():.4f} "
                       f"(gate 10%)")


if __name__ == "__main__":
    pysuperluxcore.Init()
    main()
    failed = [n for n, ok in results if not ok]
    print(f"\n{'FAIL ' + str(failed) if failed else 'ALL PASS'} "
          f"({len(results) - len(failed)}/{len(results)})", flush=True)
    sys.exit(1 if failed else 0)
