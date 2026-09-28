# SPDX-License-Identifier: Apache-2.0
#
# E54 visual demo: media-transparent caustic chains, 1280x720, AgX
# punch. A fan of narrow lasers fires through three glass spheres into
# homogeneous fog - light->specular->medium->surface paths are the
# caustic class the chains now own on both sides of the partition.
#
#   python3.13 dev-tools/e54_visual_demo.py [gpu|cpu]

import os
import sys
import time
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..",
                                "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

REPO = Path(__file__).resolve().parent.parent
OUTDIR = Path("/tmp/e54_media_caustic"); OUTDIR.mkdir(exist_ok=True)
W, H, SPP = 1280, 720, 512
OCIO = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/colormanagement/config.ocio"
ENGINE = "PATHOCL" if len(sys.argv) < 2 or sys.argv[1] == "gpu" else "PATHCPU"
SCENE = REPO / "scenes/cornell/cornell-vol-caustic-show.scn"


def device_mask(want="METAL_GPU"):
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    m = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        m += "1" if t == want else "0"
        i += 1
    return m or None


def main():
    os.chdir(str(REPO))
    props = pysuperluxcore.Properties(str(SCENE))
    sc = pysuperluxcore.Scene()
    sc.Parse(props)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = {ENGINE}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 7
opencl.task.count = 65536
path.hybridbackforward.enable = 1
path.hybridbackforward.partition = 0.8
path.hybridbackforward.adaptivecaustic = 1
path.lighttracing.enable = 1
path.lighttracing.taskfraction = 0.25
film.imagepipelines.0.0.type = TONEMAP_OPENCOLORIO
film.imagepipelines.0.0.mode = DISPLAY_CONVERSION
film.imagepipelines.0.0.config = {OCIO}
film.imagepipelines.0.0.src = "Linear Rec.709"
film.imagepipelines.0.0.display = sRGB
film.imagepipelines.0.0.view = AgX
film.imagepipelines.0.0.look = "AgX - Punchy"
film.outputs.beauty.type = RGB_IMAGEPIPELINE
film.outputs.beauty.index = 0
film.outputs.beauty.filename = {OUTDIR}/vol_caustic_agx_{ENGINE.lower()}.png
""")
    if ENGINE == "PATHOCL":
        sel = device_mask()
        if sel:
            cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + 1800
    while True:
        ses.UpdateStats()
        p = ses.GetStats().Get("stats.renderengine.pass").GetInt()
        if p >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"stalled at {p}/{SPP}")
        time.sleep(1.0)
    ses.GetFilm().SaveOutputs()
    ses.Stop()
    print(f"saved {OUTDIR}/vol_caustic_agx_{ENGINE.lower()}.png")


if __name__ == "__main__":
    main()
