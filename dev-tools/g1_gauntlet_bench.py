# SPDX-License-Identifier: Apache-2.0
#
# G1: gauntlet benchmark harness - fixed-walltime renders over the
# scenes/gauntlet suite, recording convergence metrics for regression
# and A/B comparisons during optimization work.
#
# Modes:
#   --quick              480x270 sanity render of every scene (PATHOCL)
#   --bench SECONDS      1280x720 fixed-walltime on PATHOCL + PATHCPU
#   --hero SECONDS       single-scene 720p+ ACES 2.0 output (promo shot)
#   --scene NAME         restrict to one scene
#   --compare A.json B.json   diff two result files
#
# Every render runs in its own subprocess. Metrics per render:
#   spp reached in the walltime budget, samples/sec (engine stat),
#   mean/median luminance of the LINEAR output, %finite pixels.
# With --ref <dir>: MSE/RMSE against a stored reference .exr.
#
# Images go to <repo>/renders/gauntlet/<scene>_<engine>_<wallclock>.png
# (unique per run) and results JSON to dev-tools/gauntlet/results/.
#
# Run from repo root:
#   python3.13 dev-tools/g1_gauntlet_bench.py --bench 45
#   python3.13 dev-tools/g1_gauntlet_bench.py --hero 120 --scene prism-conservatory

import argparse
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

OCIO = "/Applications/Blender.app/Contents/Resources/5.2/datafiles/" \
       "colormanagement/config.ocio"
RESULTS_DIR = Path(__file__).resolve().parent / "gauntlet/results"
IMG_DIR = REPO / "renders/gauntlet"

TASK_COUNT = 65536
CHILD_TIMEOUT_S = 7200

# (tag, scene file, hero-worthy?)
SCENES = [
    ("prism-conservatory",
     REPO / "scenes/gauntlet/prism-conservatory.scn", True),
    # Reuse proven stress scenes as extra rows once the hero lands.
    ("vol-caustic-deep",
     REPO / "scenes/caustics/vol-caustic-deep.scn", False),
    ("focused-ring",
     REPO / "scenes/caustics/focused-caustic-ring.scn", False),
]

# Default hard-path-friendly engine props applied on top of every cfg.
# Mirrors the "just press render" target configuration.
DEFAULT_PROPS = """
path.hybridbackforward.enable = 1
path.hybridbackforward.partition = 0.8
path.hybridbackforward.adaptivecaustic = 1
path.lighttracing.enable = 1
path.lighttracing.taskfraction = 0.25
path.photongi.caustic.enabled = 1
path.photongi.caustic.updatespp = 4
path.photongi.caustic.volumebeams = 1
"""


def device_mask(want="METAL_GPU"):
    import pysuperluxcore
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    m, i = "", 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        m += "1" if t == want else "0"
        i += 1
    return m or None


def child_render(scene_path, engine, seconds, width, height, out_png,
                 ref_exr, extra):
    """Child mode: render `seconds` walltime, emit RESULT json + PNG."""
    import numpy as np
    import pysuperluxcore

    os.chdir(str(REPO))
    props = pysuperluxcore.Properties(str(scene_path))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)

    cfg = pysuperluxcore.Properties()
    aces = ""
    if out_png:
        aces = f"""
film.imagepipelines.0.0.type = TONEMAP_OPENCOLORIO
film.imagepipelines.0.0.mode = DISPLAY_CONVERSION
film.imagepipelines.0.0.config = {OCIO}
film.imagepipelines.0.0.src = "Linear Rec.709"
film.imagepipelines.0.0.display = sRGB
film.imagepipelines.0.0.view = "ACES 2.0"
film.outputs.beauty.type = RGB_IMAGEPIPELINE
film.outputs.beauty.index = 0
film.outputs.beauty.filename = {out_png}
"""
    cfg.SetFromString(f"""
film.width = {width}
film.height = {height}
renderengine.type = {engine}
sampler.type = SOBOL
renderengine.seed = 17
opencl.task.count = {TASK_COUNT}
opencl.native.threads.count = 0
{aces}
{extra}
""")
    if engine == "PATHOCL":
        sel = device_mask()
        if sel:
            cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    t0 = time.monotonic()
    ses.Start()
    while time.monotonic() - t0 < seconds:
        time.sleep(0.5)
    ses.UpdateStats()
    stats = ses.GetStats()
    spp = stats.Get("stats.renderengine.pass").GetInt()

    def stat(name, default=0.0):
        try:
            return stats.Get(name).GetFloat()
        except RuntimeError:
            return default

    sample_sec = stat("stats.renderengine.total.samplesec")
    conv = stat("stats.renderengine.convergence")
    if out_png:
        ses.GetFilm().SaveOutputs()
    ses.Stop()
    elapsed = time.monotonic() - t0

    rgb = np.empty(width * height * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB,
                                 rgb, 0, True)
    img = rgb.reshape(height, width, 3)

    res = {"spp": spp, "seconds": elapsed, "mean": float(img.mean()),
           "median": float(np.median(img)),
           "finite": bool(np.isfinite(img).all()),
           "convergence": conv,
           "samples_per_s": float(sample_sec)}
    print("RESULT " + json.dumps(res), flush=True)


def render(scene_path, engine, seconds, width, height, out_png=None,
           extra=""):
    args = [sys.executable, str(Path(__file__).resolve()), "--render",
            str(scene_path), engine, f"{seconds}", f"{width}", f"{height}",
            out_png or "", "", extra]
    try:
        r = subprocess.run(args, capture_output=True, text=True,
                           timeout=CHILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        return {"error": f"timeout after {CHILD_TIMEOUT_S}s"}
    for line in r.stdout.splitlines():
        if line.startswith("RESULT "):
            res = json.loads(line[7:])
            if r.returncode != 0:
                res["error"] = f"rc={r.returncode}"
            return res
    tail = (r.stderr or r.stdout or "")[-400:].strip()
    return {"error": f"no RESULT (rc={r.returncode}): {tail!r}"}


def cmd_compare(a_path, b_path):
    a = json.load(open(a_path)); b = json.load(open(b_path))
    print(f"{'scene':22s} {'engine':8s} {'sppA':>6s} {'sppB':>6s} "
          f"{'spsA':>10s} {'spsB':>10s} {'sps Δ':>8s}")
    for (scene, engine), ra in sorted(flatten(a).items()):
        rb = flatten(b).get((scene, engine))
        if not rb or "error" in ra or "error" in rb:
            continue
        d = ra["samples_per_s"] and \
            rb["samples_per_s"] / ra["samples_per_s"] - 1
        print(f"{scene:22s} {engine:8s} {ra['spp']:6d} {rb['spp']:6d} "
              f"{ra['samples_per_s']:10.0f} {rb['samples_per_s']:10.0f} "
              f"{d:8.1%}")


def flatten(res):
    out = {}
    for scene, row in res.get("renders", {}).items():
        for engine, r in row.items():
            out[(scene, engine)] = r
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--render", nargs=8, help=argparse.SUPPRESS)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--bench", type=float, metavar="SECONDS")
    ap.add_argument("--hero", type=float, metavar="SECONDS")
    ap.add_argument("--scene")
    ap.add_argument("--extra", default="")
    ap.add_argument("--engines", default="PATHOCL,PATHCPU")
    ap.add_argument("--compare", nargs=2)
    ap.add_argument("--label", default=None)
    args = ap.parse_args()

    if args.render:
        (scene, engine, seconds, w, h, out_png, ref_exr, extra) = args.render
        child_render(scene, engine, float(seconds), int(w), int(h),
                     out_png or None, ref_exr or None, extra)
        return

    if args.compare:
        cmd_compare(args.compare[0], args.compare[1])
        return

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    IMG_DIR.mkdir(parents=True, exist_ok=True)

    stamp = time.strftime("%Y%m%d-%H%M%S")
    label = args.label or stamp
    scenes = [s for s in SCENES if not args.scene or s[0] == args.scene]

    if args.quick:
        engines, seconds, (w, h), do_png = ["PATHOCL"], 20, (480, 270), True
    elif args.hero:
        engines, seconds, (w, h), do_png = ["PATHOCL"], args.hero, \
            (1920, 1080), True
    else:
        engines = args.engines.split(",")
        seconds, (w, h), do_png = args.bench, (1280, 720), True

    results = {"label": label, "stamp": stamp, "walltime": seconds,
               "renders": {}}
    for tag, scn, _hero in scenes:
        row = {}
        for eng in engines:
            png = str(IMG_DIR / f"{tag}_{eng}_{label}.png") if do_png else None
            t0 = time.monotonic()
            r = render(scn, eng, seconds, w, h, png,
                       extra=DEFAULT_PROPS + args.extra)
            dt = time.monotonic() - t0
            row[eng] = r
            if "error" in r:
                print(f"{tag:22s} {eng:8s} ERROR {r['error']}", flush=True)
            else:
                print(f"{tag:22s} {eng:8s} spp={r['spp']:5d} "
                      f"{r['samples_per_s']/1e6:6.2f}Ms/s "
                      f"mean={r['mean']:.4f} finite={r['finite']} "
                      f"({dt:.0f}s)", flush=True)
        results["renders"][tag] = row

    jf = RESULTS_DIR / f"gauntlet-{label}.json"
    jf.write_text(json.dumps(results, indent=1))
    print(f"\nresults -> {jf}")
    print(f"images  -> {IMG_DIR}")


if __name__ == "__main__":
    main()
