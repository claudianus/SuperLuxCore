# SPDX-License-Identifier: Apache-2.0
#
# E91: PhotonGI progressive-update lifecycle smoke - the paths a plain
# haltspp render does not cover:
#
#   1. PATHOCL + caustic.updatespp > 0: GPU deposit drain, per-thread
#      taskConfig refresh on generation swaps, FinishUpdate drain.
#   2. Mid-flight Stop(): stop while the update worker is tracing so the
#      abort path (updateAbortRequested -> jthread stop -> early worker
#      exit) runs instead of waiting out a full trace.
#   3. PATHCPU progressive pgic hitting haltspp: Update/FinishUpdate
#      barrier pairing under normal shutdown.
#
# Every config renders in a subprocess: a barrier hang must surface as a
# timeout FAIL, not a stuck suite.
#
# Run from the repo root:
#   python3.13 dev-tools/e91_pgic_update_smoke.py

import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

WIDTH, HEIGHT = 320, 180
TASK_COUNT = 65536
RENDER_TIMEOUT_S = 300
CHILD_TIMEOUT_S = 360

SCENE = REPO / "scenes/caustics/focused-caustic-ring.scn"

PGIC = ("path.photongi.caustic.enabled = 1\n"
        "path.photongi.caustic.updatespp = 4\n"
        "path.photongi.photon.maxcount = 400000\n"
        "path.photongi.caustic.maxsize = 200000\n")


# -----------------------------------------------------------------------------
# child modes
# -----------------------------------------------------------------------------

def read_image(ses, width, height):
    import pysuperluxcore
    rgb = np.empty(width * height * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB,
                                 rgb, 0, True)
    return rgb.reshape(height, width, 3)


def start_session(engine, extra, spp):
    import pysuperluxcore
    os.chdir(str(REPO))
    props = pysuperluxcore.Properties(str(SCENE))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {spp}
renderengine.seed = 17
opencl.task.count = {TASK_COUNT}
opencl.native.threads.count = 0
{extra}
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    return ses


def child_haltspp(engine, extra, spp):
    """Render to haltspp; Stop() drains via FinishUpdate."""
    ses = start_session(engine, extra, spp)
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {spp} spp")
        time.sleep(0.5)
    ses.Stop()
    img = read_image(ses, WIDTH, HEIGHT)
    print(f"RESULT mean={img.mean():.6f} "
          f"finite={int(np.isfinite(img).all())}", flush=True)


def child_midstop(engine, extra, stop_after_s):
    """Stop() while the update worker is mid-trace: exercises the
    abort path + FinishUpdate's in-flight worker join."""
    ses = start_session(engine, extra, 100000)
    time.sleep(stop_after_s)
    t0 = time.monotonic()
    ses.Stop()
    stopped = time.monotonic() - t0
    img = read_image(ses, WIDTH, HEIGHT)
    # The join must not wait out a whole photon trace; a stuck barrier
    # or missing abort shows up here as a long stop
    print(f"RESULT mean={img.mean():.6f} "
          f"finite={int(np.isfinite(img).all())} "
          f"stopsecs={stopped:.2f}", flush=True)


# -----------------------------------------------------------------------------
# parent
# -----------------------------------------------------------------------------

def run(mode, engine, extra, arg):
    args = [sys.executable, str(Path(__file__).resolve()), mode, engine,
            str(arg), extra]
    try:
        r = subprocess.run(args, capture_output=True, text=True,
                           timeout=CHILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"timeout after {CHILD_TIMEOUT_S}s (hang?)")
    res = {}
    for line in r.stdout.splitlines():
        if line.startswith("RESULT "):
            res = dict(tok.split("=") for tok in line.split()[1:])
    if not res:
        tail = (r.stderr or r.stdout or "")[-400:].strip()
        raise RuntimeError(f"render died (rc={r.returncode}) {tail!r}")
    if r.returncode != 0:
        raise RuntimeError(f"render died (rc={r.returncode})")
    return res


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    ok = True
    print("PhotonGI progressive-update lifecycle smoke\n", flush=True)

    try:
        r = run("--haltspp", "PATHCPU", PGIC, 48)
        ok &= check(float(r["mean"]) > 0 and r["finite"] == "1",
                    "1 PATHCPU pgic updatespp=4 haltspp",
                    f"mean={r['mean']}")
    except Exception as e:
        ok &= check(False, "1 PATHCPU pgic updatespp=4 haltspp", e)

    try:
        r = run("--haltspp", "PATHOCL", PGIC, 32)
        ok &= check(float(r["mean"]) > 0 and r["finite"] == "1",
                    "2 PATHOCL pgic deposits + swaps",
                    f"mean={r['mean']}")
    except Exception as e:
        ok &= check(False, "2 PATHOCL pgic deposits + swaps", e)

    # updatespp=2 + a long per-generation trace keeps a worker in flight
    # almost continuously; stopping mid-trace must be fast (abort), not
    # trace-bounded. maxcount=8M makes one generation take ~seconds, so
    # the Stop() lands while the worker is actually tracing.
    pgic_fast = (PGIC.replace("updatespp = 4", "updatespp = 2")
                     .replace("maxcount = 400000", "maxcount = 8000000"))
    try:
        r = run("--midstop", "PATHCPU", pgic_fast, 12)
        ok &= check(float(r["mean"]) > 0 and r["finite"] == "1" and
                    float(r["stopsecs"]) < 120.0,
                    "3 PATHCPU mid-flight Stop (abort)",
                    f"mean={r['mean']} stop={r['stopsecs']}s")
    except Exception as e:
        ok &= check(False, "3 PATHCPU mid-flight Stop (abort)", e)

    print(f"\n{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--haltspp":
        child_haltspp(sys.argv[2], sys.argv[4], int(sys.argv[3]))
    elif len(sys.argv) > 1 and sys.argv[1] == "--midstop":
        child_midstop(sys.argv[2], sys.argv[4], float(sys.argv[3]))
    else:
        main()
