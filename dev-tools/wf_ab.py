# SPDX-License-Identifier: Apache-2.0
# Interleaved wavefront A/B: pathocl.wavefront=off vs on, min-of-N.
# Usage: python3.13 dev-tools/wf_ab.py <secs> <reps> <scenes,csv>
import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
REPS = int(sys.argv[2]) if len(sys.argv) > 2 else 2
WANT = sys.argv[3].split(",") if len(sys.argv) > 3 else None

ALL = {
    "cornell": "scenes/cornell/cornell.scn",
    "classroom": "scenes/classroom/classroom-hdr.scn",
    "luxball": "scenes/luxball/lightball.scn",
    "focused-ring": "scenes/caustics/focused-caustic-ring.scn",
    "vol-deep": "scenes/caustics/vol-caustic-deep.scn",
    "portal-int": "scenes/gauntlet/portal-interior.scn",
    "kitchen": "scenes/kitchen/kitchen.scn",
    "prism": "scenes/gauntlet/prism-conservatory.scn",
}

import pysuperluxcore  # noqa: E402


def device_mask():
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    m, i = "", 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        m += "1" if t == "METAL_GPU" else "0"
        i += 1
    return m or None


def run(rel, wf):
    props = pysuperluxcore.Properties(str(REPO / rel))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = 1280
film.height = 720
renderengine.type = PATHOCL
sampler.type = SOBOL
renderengine.seed = 17
opencl.task.count = 262144
opencl.native.threads.count = 0
pathocl.wavefront = {wf}
""")
    sel = device_mask()
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
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
    ses.Stop()
    return spp, sps


for tag, rel in ALL.items():
    if WANT and tag not in WANT:
        continue
    offs, ons = [], []
    for r in range(REPS):
        # interleave arms to decorrelate machine load
        for arm, acc in (("off", offs), ("on", ons)):
            spp, sps = run(rel, arm)
            acc.append(sps)
            print(f"  {tag} rep{r} {arm}: {spp}spp "
                  f"{sps/1e6:.2f}Ms/s", flush=True)
    mo, mn = min(offs), min(ons)
    print(f"{tag}: off={mo/1e6:.2f} on={mn/1e6:.2f} "
          f"ratio={mn/mo:.3f} (min-of-{REPS})", flush=True)
