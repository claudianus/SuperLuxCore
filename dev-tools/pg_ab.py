# SPDX-License-Identifier: Apache-2.0
# Path-guiding cost/benefit A/B: samples/s + image noise vs off,
# CPU engine (the promotion candidate's worst case is diffuse scenes).
# Usage: python3.13 dev-tools/pg_ab.py <secs> <reps> <scenes,csv>
import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0
REPS = int(sys.argv[2]) if len(sys.argv) > 2 else 2
WANT = sys.argv[3].split(",") if len(sys.argv) > 3 else None

ALL = {
    "cornell": "scenes/cornell/cornell.scn",
    "pg-indirect": "scenes/cornell/pg-indirect.scn",
    "pg-gallery": "scenes/cornell/pg-gallery.scn",
    "pg-glossy": "scenes/cornell/pg-glossy-indirect.scn",
    "classroom": "scenes/classroom/classroom-hdr.scn",
}

import numpy as np  # noqa: E402
import pysuperluxcore  # noqa: E402


def run(rel, onoff):
    props = pysuperluxcore.Properties(str(REPO / rel))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = 640
film.height = 480
renderengine.type = PATHCPU
sampler.type = SOBOL
renderengine.seed = 17
path.guiding.enable = {onoff}
""")
    ses = pysuperluxcore.RenderSession(
        pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    t0 = time.monotonic()
    while time.monotonic() - t0 < SECS:
        time.sleep(0.5)
    ses.UpdateStats()
    stats = ses.GetStats()
    spp = stats.Get("stats.renderengine.pass").GetInt()
    try:
        sps = stats.Get("stats.renderengine.total.samplesec").GetFloat()
    except Exception:
        sps = 0.0
    rgb = np.empty(640 * 480 * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB, rgb, 0, True)
    ses.Stop()
    return spp, sps, rgb.reshape(480, 640, 3)


def relerr(img):
    lum = img.mean(axis=2)
    nz = lum[lum > 1e-4]
    return float(lum.std() / (nz.mean() + 1e-9)) if nz.size else 0.0


for tag, rel in ALL.items():
    if WANT and tag not in WANT:
        continue
    offs, ons = [], []
    for r in range(REPS):
        for arm, acc in ((0, offs), (1, ons)):
            spp, sps, img = run(rel, arm)
            acc.append((sps, spp, img))
            print(f"  {tag} rep{r} guiding={arm}: {spp}spp "
                  f"{sps/1e3:.0f}Ks/s", flush=True)
    mo = min(o[0] for o in offs)
    mn = min(o[0] for o in ons)
    # noise proxy: relative pixel stddev of the last image
    no, nn = relerr(offs[-1][2]), relerr(ons[-1][2])
    print(f"{tag}: off={mo/1e3:.0f}Ks/s σ={no:.4f} "
          f"on={mn/1e3:.0f}Ks/s σ={nn:.4f} "
          f"sps-ratio={mn/mo:.3f}", flush=True)
