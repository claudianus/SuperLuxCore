# SPDX-License-Identifier: Apache-2.0
# PSR auto-seed benefit: RMSE vs a converged reference at fixed time,
# focused-caustic-ring (pure SDS ring - the worst case).
# Usage: python3.13 dev-tools/psr_ab.py <secs> [ref|ab]
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

import numpy as np  # noqa: E402
import pysuperluxcore  # noqa: E402

SCN = "scenes/caustics/focused-caustic-ring.scn"
W, H = 640, 360
REF = REPO / "renders/gauntlet/focused-ring_PATHCPU_ref.npy"
SECS = float(sys.argv[1]) if len(sys.argv) > 1 else 30.0
MODE = sys.argv[2] if len(sys.argv) > 2 else "ab"


def run(extra_cfg, haltspp=None):
    props = pysuperluxcore.Properties(str(REPO / SCN))
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    cfg = pysuperluxcore.Properties()
    base = f"""
film.width = {W}
film.height = {H}
renderengine.type = PATHCPU
sampler.type = SOBOL
renderengine.seed = 31
{extra_cfg}
"""
    if haltspp:
        base += f"batch.haltspp = {haltspp}\n"
    cfg.SetFromString(base)
    ses = pysuperluxcore.RenderSession(
        pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    if haltspp:
        while not ses.HasDone():
            time.sleep(5)
            ses.UpdateStats()
    else:
        t0 = time.monotonic()
        while time.monotonic() - t0 < SECS:
            time.sleep(0.5)
    ses.UpdateStats()
    stats = ses.GetStats()
    spp = stats.Get("stats.renderengine.pass").GetInt()
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB, rgb, 0, True)
    ses.Stop()
    return spp, rgb.reshape(H, W, 3)


def rmse(a, ref):
    d = a - ref
    return float(np.sqrt((d * d).mean()))


if MODE == "ref":
    # Converged ground truth: unbiased caustic stack (light tracing +
    # MNEE, PSR off), 4096spp.
    spp, img = run(
        "path.regularization.auto = 0\n"
        "path.regularization.sigma = 0\n"
        "path.lighttracing.enable = 1\n"
        "path.mnee.enable = 1\n"
        "photongi.caustic.enabled = 0\n",
        haltspp=4096)
    np.save(REF, img.astype(np.float32))
    print(f"ref saved: {REF} spp={spp}")
else:
    ref = np.load(REF)
    # Auto stack (lt+mnee+psr auto): today's zero-config
    spp_a, a = run("")
    print(f"auto-stack : {spp_a}spp rmse={rmse(a, ref):.4f}")
    # Same stack, PSR suppressed
    spp_b, b = run("path.regularization.auto = 0\n")
    print(f"auto-no-psr: {spp_b}spp rmse={rmse(b, ref):.4f}")
    # PSR only (no lt, no mnee): isolate eye-side contribution
    spp_c, c = run(
        "path.lighttracing.enable = 0\n"
        "path.mnee.enable = 0\n")
    print(f"psr-only   : {spp_c}spp rmse={rmse(c, ref):.4f}")
