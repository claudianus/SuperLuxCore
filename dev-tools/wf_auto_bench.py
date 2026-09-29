# SPDX-License-Identifier: Apache-2.0
#
# Wavefront auto-on regression bench: PATHOCL walltime A/B of
# pathocl.wavefront=off vs =auto (Metal GPU auto-enables) across
# scenes. Reports Ms/s so a default-on regression is caught before it
# ships silently.
#
# Run: python3.13 dev-tools/wf_auto_bench.py [seconds_per_render]
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

SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
TASKS = int(sys.argv[2]) if len(sys.argv) > 2 else 262144
ALL = {
    "classroom-hdr": "scenes/classroom/classroom-hdr.scn",
    "kitchen": "scenes/kitchen/kitchen.scn",
    "cornell": "scenes/cornell/cornell.scn",
    "pg-indirect": "scenes/cornell/pg-indirect.scn",
    "focused-ring": "scenes/caustics/focused-caustic-ring.scn",
    "vol-caustic-deep": "scenes/caustics/vol-caustic-deep.scn",
    "prism": "scenes/gauntlet/prism-conservatory.scn",
}
SCENES = [(k, v) for k, v in ALL.items()
          if len(sys.argv) <= 3 or k in sys.argv[3].split(",")]


def device_mask(want_type):
    pysuperluxcore.Init()
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    mask, i = "", 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        mask += "1" if t == want_type else "0"
        i += 1
    return mask or None


def run(scene_rel, extra_props):
    props = pysuperluxcore.Properties(str(REPO / scene_rel))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = 1280
film.height = 720
renderengine.type = PATHOCL
sampler.type = SOBOL
opencl.task.count = {TASKS}
""")
    sel = device_mask("METAL_GPU")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    for k, v in extra_props.items():
        cfg.Set(pysuperluxcore.Property(k, v))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    t0 = time.monotonic()
    while time.monotonic() - t0 < SECS:
        ses.UpdateStats()
        time.sleep(0.5)
    stats = ses.GetStats()
    spp = stats.Get("stats.renderengine.pass").GetInt()
    # samples/sec from engine stat if present, else derive
    try:
        sps = stats.Get("stats.renderengine.total.samplesec").GetFloat()
    except Exception:
        sps = 0.0
    ses.Stop()
    return spp, sps


def main():
    print(f"wavefront auto-on A/B ({SECS:.0f}s/render, PATHOCL Metal)\n")
    for tag, rel in SCENES:
        spp_off, sps_off = run(rel, {"pathocl.wavefront": "off"})
        spp_auto, sps_auto = run(rel, {})  # auto -> on for GPU
        ratio = (sps_auto / sps_off) if (sps_off > 0 and sps_auto > 0) else float("nan")
        print(f"{tag}: off={spp_off}spp {sps_off/1e6:.2f}Ms/s  "
              f"auto={spp_auto}spp {sps_auto/1e6:.2f}Ms/s  "
              f"ratio={ratio:.3f}", flush=True)


if __name__ == "__main__":
    main()
