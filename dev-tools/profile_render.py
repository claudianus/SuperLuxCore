# SPDX-License-Identifier: Apache-2.0
#
# CPU/GPU profiling driver: renders one scene for N seconds so an
# external sampler (`sample <pid>`) can attribute render-thread time.
# Usage:
#   python3.13 dev-tools/profile_render.py <scene.scn> [ENGINE] [SECONDS] [W H] [EXTRAPROPS]
# Prints "PROFILE_READY <pid>" once rendering has started.

import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

scene_path = sys.argv[1]
engine = sys.argv[2] if len(sys.argv) > 2 else "PATHCPU"
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 120
width = int(sys.argv[4]) if len(sys.argv) > 4 else 640
height = int(sys.argv[5]) if len(sys.argv) > 5 else 360
extra = sys.argv[6] if len(sys.argv) > 6 else ""

import pysuperluxcore  # noqa: E402

props = pysuperluxcore.Properties(str(scene_path))
scene = pysuperluxcore.Scene()
scene.Parse(props)

cfg = pysuperluxcore.Properties()
devsel = ""
if engine in ("PATHOCL", "TILEPATHOCL", "RTPATHOCL"):
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    m, i = "", 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        m += "1" if t == "METAL_GPU" else "0"
        i += 1
    if m:
        devsel = f"opencl.devices.select = {m}\n"

cfg.SetFromString(f"""
film.width = {width}
film.height = {height}
renderengine.type = {engine}
sampler.type = SOBOL
renderengine.seed = 17
opencl.task.count = 65536
opencl.native.threads.count = 0
{devsel}
{extra}
""")

ses = pysuperluxcore.RenderSession(
    pysuperluxcore.RenderConfig(cfg, scene))
ses.Start()
print(f"PROFILE_READY {os.getpid()}", flush=True)
time.sleep(seconds)
ses.UpdateStats()
st = ses.GetStats()
try:
    print("pass", st.Get("stats.renderengine.pass").GetInt(), flush=True)
    print("samplesec",
          st.Get("stats.renderengine.total.samplesec").GetFloat(),
          flush=True)
except Exception as e:
    print("stats err", e, flush=True)
ses.Stop()
