# SPDX-License-Identifier: Apache-2.0
#
# M7e smoke: caustic-focus emission guidance on BIDIRCPU/BIDIRVMCPU.
# Renders cornell-glass-point with path.lighttracing.focus.enable
# on/off and verifies the guided emission compiles, renders, and does
# not lose the caustic energy (unbiased: same mean, lower variance).
# The mixture pdf must also feed the NEE weightCamera MIS term - a
# native-only pdf there leaks ~10% of scene energy.

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore  # noqa: F401  (pre-import so the Release module wins)

sys.path.insert(0, str(REPO / "dev-tools/parity"))
sys.path.insert(0, str(REPO / "dev-tools"))
from harness import load_scene, render, metrics, luminance, save_ppm

OUT = REPO / "dev-tools/out/bidir_focus"
# Point light + 3 delta-glass spheres: the only caustic producer is the
# guided emission path, so focus-on must converge visibly faster.
SCN = "dev-tools/out/bidir_focus/cornell-glass-point.scn"

BASE = {
    "props_file": SCN,
    "engine": "BIDIRCPU",
    "sampler": "SOBOL",
    "spp": 24,
    "timeout": 600,
}


def run(tag, engine, extra):
    spec = dict(BASE)
    spec["engine"] = engine
    spec["cfg_extra"] = extra
    scene = load_scene(spec)
    img = render(scene, spec)
    L = luminance(img)
    import numpy as np
    nans = int(np.isnan(img).sum())
    print(f"[{tag}] mean_lum={L.mean():.5f} nans={nans} black={float((L < 1e-6).mean()):.5f}")
    OUT.mkdir(parents=True, exist_ok=True)
    save_ppm(img, OUT / f"{tag}.ppm")
    assert nans == 0, f"{tag}: NaN pixels"
    return img


def main():
    ok = True
    for eng in ("BIDIRCPU", "BIDIRVMCPU"):
        off = run(f"{eng}_focus_off", eng, "path.lighttracing.focus.enable = 0\n")
        on = run(f"{eng}_focus_on", eng, "path.lighttracing.focus.enable = 1\n")
        m = metrics(off, on)
        print(f"[{eng}] on/off ratio={m['ratio']:.4f} rmse={m['rmse']:.5f}")
        # Unbiased: the guided mixture must preserve total energy. Allow
        # for RNG re-streaming noise between the on/off draws.
        if not (0.96 < m['ratio'] < 1.04):
            print(f"FAIL {eng}: focus lost {abs(1-m['ratio'])*100:.1f}% energy")
            ok = False
    print("images:", OUT)
    print("RESULT:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
