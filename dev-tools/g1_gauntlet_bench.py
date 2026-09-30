# SPDX-License-Identifier: Apache-2.0
#
# Gauntlet benchmark harness v2 - the unified measurement standard for
# SuperLuxCore (see dev-tools/megaplan-production-sota.md section 5).
#
# Scene coverage: multi-caustic chains, spectral dispersion, volume
# caustics, sealed-interior indirect (portal worst case), glossy-mixed
# caustics, many-light interiors, heterogeneous volumes, hair, instanced
# geometry, diffraction gratings, production material stacks.
#
# Modes:
#   --quick              480x270 sanity render of every scene (PATHOCL)
#   --bench SECONDS      1280x720 fixed-walltime on PATHOCL + PATHCPU
#   --fixedspp N         fixed-sample-count renders (image-parity mode):
#                        wall time to reach N spp is the metric
#   --refgen SPP         render high-spp PATHCPU references ->
#                        renders/gauntlet/ref/<tag>.npy (linear float32)
#   --parity             PATHCPU vs PATHOCL at --fixedspp: RMSE + mean
#                        ratio per scene (CPU/GPU equivalence gate)
#   --hero SECONDS       single-scene 720p+ ACES 2.0 output (promo shot)
#   --scene NAME         restrict to one scene
#   --compare A.json B.json   diff two result files
#   --require-idle       refuse to run wall benchmarks while the machine
#                        is loaded (shared-machine measurement hygiene)
#
# Every render runs in its own subprocess. Metrics per render:
#   spp reached, samples/sec (engine stat), wall seconds,
#   mean/median luminance of the LINEAR output, %finite pixels,
#   peak host RSS, (t,spp) convergence curve. With a stored reference
#   (renders/gauntlet/ref/<tag>.npy) also RMSE / relative-error.
#
# Images go to <repo>/renders/gauntlet/<scene>_<engine>_<label>.png and
# linear float dumps to the same dir as .npy; results JSON to
# dev-tools/gauntlet/results/.
#
# Run from repo root:
#   python3.13 dev-tools/g1_gauntlet_bench.py --bench 45
#   python3.13 dev-tools/g1_gauntlet_bench.py --refgen 512 --scene pool
#   python3.13 dev-tools/g1_gauntlet_bench.py --parity --fixedspp 64
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
REF_DIR = IMG_DIR / "ref"

TASK_COUNT = 65536
CHILD_TIMEOUT_S = 7200

# Gauntlet v2 corpus. Each row: tag, scene path (repo-relative), hero
# flag, optional working dir for scenes whose internal refs are
# directory-relative, optional per-scene extra props.
SCENES = [
    ("prism-conservatory", "scenes/gauntlet/prism-conservatory.scn",
     True, None, ""),
    ("multi-caustic-chain", "scenes/gauntlet/multi-caustic-chain.scn",
     True, None, ""),
    ("portal-interior", "scenes/gauntlet/portal-interior.scn",
     True, None, ""),
    ("glossy-caustic-mix", "scenes/gauntlet/glossy-caustic-mix.scn",
     False, None, ""),
    ("vol-caustic-deep", "scenes/caustics/vol-caustic-deep.scn",
     False, None, ""),
    ("focused-ring", "scenes/caustics/focused-caustic-ring.scn",
     False, None, ""),
    ("mirror-maze", "scenes/caustics/mirror-maze.scn",
     False, None, ""),
    ("caustic-stress-many", "scenes/caustics/caustic-stress-many.scn",
     False, None, ""),
    ("prism-spectral", "scenes/caustics/prism-spectral-caustic.scn",
     False, None, ""),
    ("pool", "scenes/pool/scene.scn",
     False, REPO / "scenes/pool", ""),
    ("manylights", "scenes/manylights/scene.scn",
     False, None, ""),
    ("vol-densitygrid", "scenes/media/vol-densitygrid.scn",
     False, None, ""),
    ("hair", "scenes/strands/hair.scn",
     False, None, ""),
    ("classroom", "scenes/classroom/classroom-hdr.scn",
     False, None, ""),
    ("bigmonkey-inst", "scenes/bigmonkey/bigmonkey-instances.scn",
     False, None, ""),
    ("cd-rainbow", "scenes/diffraction/cd-rainbow.scn",
     False, None, ""),
    ("luxball", "scenes/luxball/lightball.scn",
     False, None, ""),
    ("openpbr-lobes", "scenes/openpbr/openpbr-lobes.scn",
     False, None, ""),
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
                 ref_npy, extra, fixedspp=0, cwd=None, npy_out=None):
    """Child mode: render `seconds` walltime (or `fixedspp` samples),
    emit RESULT json + PNG + linear .npy dump."""
    import numpy as np
    import pysuperluxcore
    import resource

    if cwd:
        os.chdir(str(cwd))
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
    if engine in ("PATHOCL", "TILEPATHOCL", "RTPATHOCL"):
        sel = device_mask()
        if sel:
            cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    t0 = time.monotonic()
    ses.Start()
    curve = []
    last_poll = 0.0
    while True:
        elapsed = time.monotonic() - t0
        if elapsed - last_poll >= 1.0:
            ses.UpdateStats()
            st = ses.GetStats()
            curve.append([round(elapsed, 2),
                          st.Get("stats.renderengine.pass").GetInt()])
            last_poll = elapsed
            if fixedspp and curve[-1][1] >= fixedspp:
                break
        if not fixedspp and elapsed >= seconds:
            break
        if elapsed > CHILD_TIMEOUT_S:
            break
        time.sleep(0.25)
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

    if npy_out:
        np.save(npy_out, img)

    res = {"spp": spp, "seconds": elapsed, "mean": float(img.mean()),
           "median": float(np.median(img)),
           "finite": bool(np.isfinite(img).all()),
           "convergence": conv,
           "samples_per_s": float(sample_sec),
           "peak_rss_mb": round(
               resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
               / (1024 * 1024), 1),
           "curve": curve}

    # Reference comparison (linear float32 .npy from --refgen)
    if ref_npy and Path(ref_npy).exists():
        ref = np.load(ref_npy)
        if ref.shape == img.shape:
            diff = img - ref
            res["rmse"] = float(np.sqrt((diff ** 2).mean()))
            denom = np.abs(ref).clip(1e-3)
            res["rel_err"] = float((np.abs(diff) / denom).mean())
        else:
            res["rmse"] = -1.0

    print("RESULT " + json.dumps(res), flush=True)


def render(scene_path, engine, seconds, width, height, out_png=None,
           extra="", fixedspp=0, cwd=None, ref_npy=None, npy_out=None):
    args = [sys.executable, str(Path(__file__).resolve()), "--render",
            str(scene_path), engine, f"{seconds}", f"{width}", f"{height}",
            out_png or "", ref_npy or "", extra,
            str(fixedspp), str(cwd or ""), npy_out or ""]
    try:
        r = subprocess.run(args, capture_output=True, text=True,
                           timeout=CHILD_TIMEOUT_S + 60)
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


def check_idle():
    """Shared-machine hygiene: warn/refuse when the box is loaded."""
    try:
        load1 = os.getloadavg()[0]
        cpus = os.cpu_count() or 8
        busy = load1 > 0.5 * cpus
        return busy, load1, cpus
    except OSError:
        return False, 0.0, 0


def cmd_compare(a_path, b_path):
    a = json.load(open(a_path)); b = json.load(open(b_path))
    print(f"{'scene':22s} {'engine':8s} {'sppA':>6s} {'sppB':>6s} "
          f"{'spsA':>10s} {'spsB':>10s} {'sps Δ':>8s} {'rmseA':>8s} "
          f"{'rmseB':>8s}")
    for (scene, engine), ra in sorted(flatten(a).items()):
        rb = flatten(b).get((scene, engine))
        if not rb or "error" in ra or "error" in rb:
            continue
        d = ra["samples_per_s"] and \
            rb["samples_per_s"] / ra["samples_per_s"] - 1
        print(f"{scene:22s} {engine:8s} {ra['spp']:6d} {rb['spp']:6d} "
              f"{ra['samples_per_s']:10.0f} {rb['samples_per_s']:10.0f} "
              f"{d:8.1%} {ra.get('rmse', -1):8.4f} {rb.get('rmse', -1):8.4f}")


def flatten(res):
    out = {}
    for scene, row in res.get("renders", {}).items():
        for engine, r in row.items():
            out[(scene, engine)] = r
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--render", nargs=11, help=argparse.SUPPRESS)
    ap.add_argument("--quick", action="store_true")
    ap.add_argument("--bench", type=float, metavar="SECONDS")
    ap.add_argument("--fixedspp", type=int, metavar="N",
                    help="render until N spp; wall time is the metric")
    ap.add_argument("--refgen", type=int, metavar="SPP",
                    help="render PATHCPU references into renders/gauntlet/ref")
    ap.add_argument("--parity", action="store_true",
                    help="PATHCPU vs PATHOCL RMSE at --fixedspp")
    ap.add_argument("--hero", type=float, metavar="SECONDS")
    ap.add_argument("--scene")
    ap.add_argument("--extra", default="")
    ap.add_argument("--engines", default="PATHOCL,PATHCPU")
    ap.add_argument("--compare", nargs=2)
    ap.add_argument("--label", default=None)
    ap.add_argument("--require-idle", action="store_true")
    args = ap.parse_args()

    if args.render:
        (scene, engine, seconds, w, h, out_png, ref_npy, extra,
         fixedspp, cwd, npy_out) = args.render
        child_render(scene, engine, float(seconds), int(w), int(h),
                     out_png or None, ref_npy or None, extra,
                     int(fixedspp), cwd or None, npy_out or None)
        return

    if args.compare:
        cmd_compare(args.compare[0], args.compare[1])
        return

    busy, load1, cpus = check_idle()
    if busy:
        msg = f"WARNING: machine loaded (load1={load1:.1f} / {cpus} cpus)"
        if args.require_idle:
            print(msg + " -> aborting (--require-idle)")
            sys.exit(2)
        print(msg + " -> timing may be contaminated")

    RESULTS_DIR.mkdir(parents=True, exist_ok=True)
    IMG_DIR.mkdir(parents=True, exist_ok=True)

    stamp = time.strftime("%Y%m%d-%H%M%S")
    label = args.label or stamp
    scenes = [s for s in SCENES if not args.scene or s[0] == args.scene]

    if args.quick:
        engines, seconds, (w, h), do_png, fspp = \
            ["PATHOCL"], 20, (480, 270), True, 0
    elif args.hero:
        engines, seconds, (w, h), do_png, fspp = \
            ["PATHOCL"], args.hero, (1920, 1080), True, 0
        if not args.scene:
            scenes = [s for s in scenes if s[2]]
    elif args.refgen:
        engines, seconds, (w, h), do_png = ["PATHCPU"], 0, (1280, 720), True
        fspp = args.refgen
    else:
        engines = args.engines.split(",")
        if args.parity:
            engines = ["PATHCPU", "PATHOCL"]
        seconds = args.bench or 45
        (w, h), do_png = (1280, 720), True
        fspp = args.fixedspp or 0

    results = {"label": label, "stamp": stamp, "walltime": seconds,
               "fixedspp": fspp, "renders": {}}
    for tag, rel_scn, _hero, cwd, sextra in scenes:
        scn = REPO / rel_scn
        if not scn.exists():
            print(f"{tag:22s} MISSING {scn}", flush=True)
            continue
        row = {}
        for eng in engines:
            png = str(IMG_DIR / f"{tag}_{eng}_{label}.png") \
                if do_png else None
            npy = str(IMG_DIR / f"{tag}_{eng}_{label}.npy")
            ref = REF_DIR / f"{tag}.npy"
            if args.refgen:
                ref.parent.mkdir(parents=True, exist_ok=True)
                npy = str(ref)  # the render itself becomes the reference
            t0 = time.monotonic()
            r = render(scn, eng, seconds, w, h, png,
                       extra=DEFAULT_PROPS + sextra + args.extra,
                       fixedspp=fspp, cwd=cwd,
                       ref_npy=None if args.refgen else str(ref),
                       npy_out=npy)
            dt = time.monotonic() - t0
            row[eng] = r
            if "error" in r:
                print(f"{tag:22s} {eng:8s} ERROR {r['error']}", flush=True)
            else:
                rmse = f" rmse={r['rmse']:.4f}" if "rmse" in r else ""
                print(f"{tag:22s} {eng:8s} spp={r['spp']:5d} "
                      f"{r['samples_per_s']/1e6:6.2f}Ms/s "
                      f"mean={r['mean']:.4f} finite={r['finite']}"
                      f"{rmse} rss={r['peak_rss_mb']:.0f}MB "
                      f"({dt:.0f}s)", flush=True)
        results["renders"][tag] = row

        # Parity gate: RMSE + mean-luminance ratio between PATHCPU and
        # PATHOCL linear dumps of the same scene at the same spp.
        if args.parity and "PATHCPU" in row and "PATHOCL" in row:
            try:
                import numpy as np
                a = np.load(IMG_DIR / f"{tag}_PATHCPU_{label}.npy")
                b = np.load(IMG_DIR / f"{tag}_PATHOCL_{label}.npy")
                rmse = float(np.sqrt(((a - b) ** 2).mean()))
                ratio = float(a.mean() / max(b.mean(), 1e-9))
                row["parity"] = {"rmse": rmse, "mean_ratio": ratio}
                print(f"{tag:22s} parity   rmse={rmse:.4f} "
                      f"cpu/gpu={ratio:.3f}", flush=True)
            except Exception as e:
                row["parity"] = {"error": str(e)}

    jf = RESULTS_DIR / f"gauntlet-{label}.json"
    jf.write_text(json.dumps(results, indent=1))
    print(f"\nresults -> {jf}")
    print(f"images  -> {IMG_DIR}")


if __name__ == "__main__":
    main()
