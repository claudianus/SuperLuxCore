# SPDX-License-Identifier: Apache-2.0
#
# Quick wall-clock bench for the GPU guide-record drain cost:
#   PATHOCL on scenes/cornell/pg-indirect.scn with path.guiding.enable=1,
#   reports samples/sec over a fixed window.
#
# Run from the repo root:
#   python3.13 dev-tools/guide_drain_bench.py [seconds]
import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
for _cfg in ("Release", "Debug"):
    _lib = REPO / "out/build/src/pysuperluxcore" / _cfg
    if _lib.exists():
        sys.path.insert(0, str(_lib))
        os.environ["DYLD_LIBRARY_PATH"] = str(REPO / "out/build/src/luxcore" / _cfg)
        break

import pysuperluxcore

SCENE = REPO / "scenes/cornell/pg-indirect.scn"
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 20.0


def device_mask(want_type):
    pysuperluxcore.Init()
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


def main():
    props = pysuperluxcore.Properties(str(SCENE))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString("""
film.width = 640
film.height = 480
renderengine.type = PATHOCL
sampler.type = SOBOL
opencl.task.count = 262144
path.guiding.enable = 1
""")
    sel = device_mask("METAL_GPU")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    t0 = time.monotonic()
    while time.monotonic() - t0 < SECS:
        time.sleep(0.5)
    ses.UpdateStats()
    stats = ses.GetStats()
    spp = stats.Get("stats.renderengine.pass").GetInt()
    elapsed = time.monotonic() - t0
    ses.Stop()
    rate = spp * 640 * 480 / elapsed / 1e6
    print(f"RESULT spp={spp} elapsed={elapsed:.1f}s rate={rate:.3f} Ms/s")


if __name__ == "__main__":
    main()
