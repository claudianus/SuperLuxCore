# SPDX-License-Identifier: Apache-2.0
#
# E90: caustic stress suite - CPU/GPU parity and PhotonGI caustic-cache
# robustness on deliberately hard SDS scenes:
#
#   scenes/caustics/prism-spectral-caustic.scn - point source through a
#       dispersive (cauchyb) prism: spectral fan caustic, hard S*DS.
#   scenes/caustics/focused-caustic-ring.scn   - glass ball lens focuses a
#       point source into a ring caustic on the floor: classic SDS.
#   scenes/caustics/mirror-maze.scn            - laser bounced through 4
#       tilted mirror plates onto the floor: L S S S S D E chain.
#   scenes/caustics/vol-caustic-deep.scn       - laser through a dense fog
#       box and an embedded glass sphere: volume+surface caustic.
#   scenes/caustics/caustic-stress-many.scn    - 8 glass objects of varied
#       roughness on a table: photon/lookup scalability.
#
# Gates (per scene, all loose - these are stress scenes):
#   1. PATHCPU 64spp @320x180: all pixels finite, mean > 0.
#   2. PATHOCL 64spp @320x180: finite, mean within 25% of the CPU mean.
#   3. PATHCPU + PhotonGI caustic cache, progressive update every 4 spp
#      vs the same cache built in one shot (updatespp=0): finite and
#      within 2x mean. Self-consistency, not parity with the plain
#      render - on SDS/volume scenes the caustic estimator adds real
#      energy the plain path tracer cannot sample, so plain-vs-pgic
#      means legitimately diverge (vol-caustic-deep: ~3x mean, ~56x
#      median, both correct).
#
# Every render runs in its own subprocess: these scenes are designed to
# expose engine crashes and a segfault must surface as a FAIL row, not
# kill the whole suite.
#
# Run from the repo root:
#   python3.13 dev-tools/e90_caustic_stress_test.py

import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

WIDTH, HEIGHT = 320, 180
SPP = 64
TASK_COUNT = 65536  # see e25/e54: below ~16k the GPU light pass is off
RENDER_TIMEOUT_S = 360      # in-child stall limit
CHILD_TIMEOUT_S = 420       # parent-side hard kill, > RENDER_TIMEOUT_S
PARITY_GATE = 0.25   # gpu/cpu mean must land inside +/- 25%
PGIC_GATE = 2.0      # progressive-pgic mean within 2x of the one-shot
                     # pgic mean (self-consistency of the update path)

SCENES = [
    ("prism-spectral", REPO / "scenes/caustics/prism-spectral-caustic.scn"),
    ("focused-ring", REPO / "scenes/caustics/focused-caustic-ring.scn"),
    ("mirror-maze", REPO / "scenes/caustics/mirror-maze.scn"),
    ("vol-caustic-deep", REPO / "scenes/caustics/vol-caustic-deep.scn"),
    ("stress-many", REPO / "scenes/caustics/caustic-stress-many.scn"),
]

# Progressive PhotonGI caustic cache: the caustic photon map is re-traced
# and rebuilt every `caustic.updatespp` samples.
PGIC = ("path.photongi.caustic.enabled = 1\n"
        "path.photongi.caustic.updatespp = 4\n"
        "path.photongi.photon.maxcount = 400000\n"
        "path.photongi.caustic.maxsize = 200000\n")

# One-shot reference: same caustic cache traced once, no updates.
PGIC0 = PGIC.replace("updatespp = 4", "updatespp = 0")

# (label, engine, extra props)
CONFIGS = [
    ("cpu", "PATHCPU", ""),
    ("gpu", "PATHOCL", ""),
    ("pgic", "PATHCPU", PGIC),
    ("pgic0", "PATHCPU", PGIC0),
]


# -----------------------------------------------------------------------------
# child mode: single render, prints one RESULT line
# -----------------------------------------------------------------------------

def child_render(scene_path, engine, spp, width, height, extra):
    import pysuperluxcore

    os.chdir(str(REPO))
    props = pysuperluxcore.Properties(scene_path)
    scene = pysuperluxcore.Scene()
    scene.Parse(props)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {width}
film.height = {height}
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
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= spp:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {spp} spp")
        time.sleep(0.5)
    # Stop() performs the final UpdateFilmLockLess() - on GPU engines the
    # per-task films merge only then (see e54).
    ses.Stop()
    rgb = np.empty(width * height * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB,
                                 rgb, 0, True)
    img = rgb.reshape(height, width, 3)
    print(f"RESULT mean={img.mean():.6f} median={np.median(img):.6f} "
          f"finite={int(np.isfinite(img).all())}", flush=True)


def render(scene_path, engine, extra):
    """Run one render in a subprocess; returns (mean, median, finite)."""
    args = [sys.executable, str(Path(__file__).resolve()), "--render",
            str(scene_path), engine, str(SPP), str(WIDTH), str(HEIGHT), extra]
    try:
        r = subprocess.run(args, capture_output=True, text=True,
                           timeout=CHILD_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        raise RuntimeError(f"timeout after {CHILD_TIMEOUT_S}s")

    mean = median = finite = None
    for line in r.stdout.splitlines():
        if line.startswith("RESULT "):
            kv = dict(tok.split("=") for tok in line.split()[1:])
            mean = float(kv["mean"])
            median = float(kv.get("median", mean))
            finite = bool(int(kv["finite"]))
    if mean is None:
        tail = (r.stderr or r.stdout or "")[-400:].strip()
        raise RuntimeError(f"render died (rc={r.returncode}) {tail!r}")
    if r.returncode != 0:
        raise RuntimeError(f"render died (rc={r.returncode})")
    return mean, median, finite


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


# -----------------------------------------------------------------------------
# suite
# -----------------------------------------------------------------------------

def main():
    ok = True
    print(f"Caustic stress suite ({WIDTH}x{HEIGHT} @ {SPP}spp, "
          f"gpu gate +/-{PARITY_GATE:.0%})\n", flush=True)
    print(f"{'scene':18s} {'cpu':>9s} {'gpu':>9s} {'pgic':>9s} "
          f"{'pgic0':>9s}", flush=True)

    for tag, path in SCENES:
        means = {}
        for label, engine, extra in CONFIGS:
            try:
                means[label] = render(path, engine, extra)
            except Exception as e:
                means[label] = e

        cpu, gpu, pgic, pgic0 = (means["cpu"], means["gpu"],
                                 means["pgic"], means["pgic0"])

        # 1. CPU baseline
        if isinstance(cpu, Exception):
            ok &= check(False, f"{tag} PATHCPU sane", cpu)
        else:
            ok &= check(cpu[2] and cpu[0] > 0, f"{tag} PATHCPU sane",
                        f"mean={cpu[0]:.4f} finite={cpu[2]}")

        # 2. GPU parity
        if isinstance(gpu, Exception):
            ok &= check(False, f"{tag} PATHOCL parity", gpu)
        elif isinstance(cpu, Exception):
            ok &= check(False, f"{tag} PATHOCL parity", "no CPU reference")
        else:
            ratio = gpu[0] / max(cpu[0], 1e-6)
            ok &= check(gpu[2] and abs(ratio - 1.0) <= PARITY_GATE,
                        f"{tag} PATHOCL parity",
                        f"cpu={cpu[0]:.4f} gpu={gpu[0]:.4f} ratio={ratio:.3f}")

        # 3. Progressive PhotonGI (updatespp=4) vs one-shot (updatespp=0)
        # self-consistency: the update path must converge to the same
        # cache as a single build - plain-render parity is NOT the check
        # (the caustic estimator legitimately adds unsamplable energy).
        if isinstance(pgic, Exception):
            ok &= check(False, f"{tag} PhotonGI caustic", pgic)
        elif isinstance(pgic0, Exception):
            ok &= check(False, f"{tag} PhotonGI caustic", "no one-shot reference")
        else:
            ratio = pgic[0] / max(pgic0[0], 1e-6)
            ok &= check(pgic[2] and 1.0 / PGIC_GATE < ratio < PGIC_GATE,
                        f"{tag} PhotonGI caustic",
                        f"pgic0={pgic0[0]:.4f} pgic={pgic[0]:.4f} "
                        f"ratio={ratio:.3f}")

        def m(v):
            return f"{v[0]:9.4f}" if not isinstance(v, Exception) else "   crashed"
        print(f"{tag:18s} {m(cpu)} {m(gpu)} {m(pgic)} {m(pgic0)}\n",
              flush=True)

    print(f"{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--render":
        # --render <scene> <engine> <spp> <w> <h> <extra-props>
        child_render(sys.argv[2], sys.argv[3], int(sys.argv[4]),
                     int(sys.argv[5]), int(sys.argv[6]),
                     sys.argv[7] if len(sys.argv) > 7 else "")
    else:
        main()
