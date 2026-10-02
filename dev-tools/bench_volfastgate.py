# SPDX-License-Identifier: Apache-2.0
# A/B bench for the PathVolumeInfo fast-gates: PATHCPU samples/sec on a
# no-volume scene (cornell) and a volume scene (media) for a fixed wall
# window. Run under `new`/`old` module dirs (the caller copies the right
# .so into place before each rep).

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 15.0
SCENES = [
    ("cornell", "scenes/cornell/cornell.scn"),
    ("media", "scenes/media/media.scn"),
]


def parse(rel):
    props = pysuperluxcore.Properties(str(REPO / rel))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    return scene


def bench(rel):
    scene = parse(rel)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString("""
film.width = 640
film.height = 360
renderengine.type = PATHCPU
sampler.type = SOBOL
renderengine.seed = 17
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    time.sleep(SECS)
    ses.UpdateStats()
    stats = ses.GetStats()
    perf = stats.Get("stats.renderengine.total.samplesec").GetFloat()
    ses.Stop()
    return perf

def main():
    for tag, rel in SCENES:
        p = bench(rel)
        print(f"{tag}: {p/1e6:.4f} Ms/s", flush=True)


main()
