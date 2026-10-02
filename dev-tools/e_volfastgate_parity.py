# SPDX-License-Identifier: Apache-2.0
#
# Throwaway A/B: PATHCPU vs PATHOCL(Metal) on volume-bearing scenes to
# smoke-test the PathVolumeInfo fast-gates (BSDF::Init needsVolumes gate,
# Update idle/intVol gates, ContinueToTrace early-out + GPU NULLMAT fix).
#
# Scenes: volumeinfo-test (heterogeneous fire volume inside glass),
# media (homogeneous), luxball-vol (clear volume + priority), juice
# (nested priority volumes), cornell (no-volume control).
#
# Gate: per-scene CPU mean within 15%, no NaN/Inf, GPU/CPU relmean <= 0.20
# (64spp MC noise floor is scene dependent - this catches blackout-class
# regressions, not sampler noise).

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
SCENES = [
    ("volumeinfo", "scenes/cornell/volumeinfo-test.scn"),
    ("media", "scenes/media/media.scn"),
    ("luxball-vol", "scenes/luxball/luxball-vol.scn"),
    ("juice", "scenes/juice/scene.scn"),
    ("cornell", "scenes/cornell/cornell.scn"),
]


def parse_scene(rel):
    try:
        props = pysuperluxcore.Properties(str(REPO / rel))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    except Exception:
        cwd = os.getcwd()
        os.chdir(str(REPO / Path(rel).parent))
        try:
            props = pysuperluxcore.Properties(str(Path(rel).name))
            scene = pysuperluxcore.Scene()
            scene.Parse(props)
            return scene
        finally:
            os.chdir(cwd)


def render(scene, engine, extra=""):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 17
{extra}
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + 480
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(engine)
        time.sleep(0.25)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def rel_stats(a, b):
    denom = np.maximum(np.abs(b), 1e-4)
    rel = np.abs(a - b) / denom
    return rel.mean(), np.percentile(rel, 95)


def main():
    only = sys.argv[1:] if len(sys.argv) > 1 else [s[0] for s in SCENES]
    fails = 0
    for tag, rel in SCENES:
        if tag not in only:
            continue
        try:
            scene = parse_scene(rel)
        except Exception as e:
            print(f"[SKIP] {tag}: parse {e}", flush=True)
            continue
        try:
            cpu = render(scene, "PATHCPU")
            gpu = render(scene, "PATHOCL")
        except Exception as e:
            print(f"[FAIL] {tag}: render {e}", flush=True)
            fails += 1
            continue
        for name, img in (("cpu", cpu), ("gpu", gpu)):
            if not np.isfinite(img).all():
                print(f"[FAIL] {tag}: {name} non-finite", flush=True)
                fails += 1
        mean_cpu, mean_gpu = cpu.mean(), gpu.mean()
        rel_mean, rel_p95 = rel_stats(gpu, cpu)
        ok = (mean_cpu > 0 and abs(mean_gpu / mean_cpu - 1) < 0.15
              and rel_mean < 0.20)
        if not ok:
            fails += 1
        print(f"[{'PASS' if ok else 'FAIL'}] {tag}: cpu={mean_cpu:.4f} "
              f"gpu={mean_gpu:.4f} relmean={rel_mean:.3f} p95={rel_p95:.3f}",
              flush=True)
    print(f"{fails} failure(s)")
    sys.exit(1 if fails else 0)


main()
