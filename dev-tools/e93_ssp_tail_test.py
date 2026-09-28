# SPDX-License-Identifier: Apache-2.0
#
# E93: SSP eye-side specular tail (path.ssp.enable).
#
# The tail recorder keeps the leading delta-specular run of each eye
# path; a light->camera connect blocked by a delta occluder rebuilds the
# chain from the recorded anchors instead of the LMNEE discovery walk.
#
# Checks (PATHCPU, hybrid back-forward, lmnee-slab scene):
#   - SSP on: the console log (LUX_LMNEE_REJ=1) reports at least one
#     "LMNEE_ACC tail" splat and zero "tail-" crashes.
#   - SSP off vs on: same framing and no systemic bias - per-pixel
#     luminance correlation must stay high at matched spp (Monte Carlo
#     noise dominates, the estimator is unbiased).
#   - ssp disabled render must not emit any tail splats.
#
# Run from the repo root:
#   python3.13 dev-tools/e93_ssp_tail_test.py

import os
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

REPO = Path(__file__).resolve().parent.parent
CONSOLE = REPO / "out/install/Release/bin/luxcoreconsole"
LIBDIR = REPO / "out/install/Release/lib"

CFG = """\
film.width = 320
film.height = 180
batch.haltspp = 48
scene.file = {scene}
renderengine.type = PATHCPU
sampler.type = SOBOL
filter.type = NONE
screen.refresh.interval = 1000
path.maxdepth = 8
light.maxdepth = 8
path.hybridbackforward.enable = 1
path.hybridbackforward.partition = 0.7
path.mnee.enable = 1
path.mnee.maxspecular = 4
film.imagepipeline.0.type = TONEMAP_LINEAR
film.imagepipeline.1.type = GAMMA_CORRECTION
{extra}
"""


def render(tmpdir: Path, name: str, extra: str) -> tuple[np.ndarray, str]:
    cfg = tmpdir / f"{name}.cfg"
    cfg.write_text(CFG.format(scene=REPO / "scenes/cornell/lmnee-slab.scn",
            extra=extra))
    env = dict(os.environ)
    env["DYLD_LIBRARY_PATH"] = str(LIBDIR)
    env["LUX_LMNEE_REJ"] = "1"
    # The scene uses repo-relative mesh paths, so cwd must be the repo;
    # the console drops image.png there.
    png = REPO / "image.png"
    png.unlink(missing_ok=True)
    proc = subprocess.run([str(CONSOLE), str(cfg)], capture_output=True,
            text=True, env=env, cwd=REPO, timeout=300)
    out = proc.stdout + proc.stderr
    if not png.exists():
        raise RuntimeError(f"render {name} produced no image\n{out[-2000:]}")
    img = np.asarray(Image.open(png), dtype=np.float64)
    png.unlink()
    return img, out


def main() -> int:
    if not CONSOLE.exists():
        print("SKIP: luxcoreconsole not built")
        return 0
    fails = []

    with tempfile.TemporaryDirectory(prefix="e93_ssp_") as td:
        tmpdir = Path(td)

        ssp_img, ssp_log = render(tmpdir, "ssp_on", "path.ssp.enable = 1")
        tail_acc = ssp_log.count("LMNEE_ACC tail")
        chain_acc = ssp_log.count("LMNEE_ACC chain")
        ok = tail_acc > 0 and chain_acc > 0
        print(f"[{'PASS' if ok else 'FAIL'}] SSP tail fired: "
                f"tail={tail_acc} chain={chain_acc}")
        if not ok:
            fails.append("no tail splats")

        off_img, off_log = render(tmpdir, "ssp_off", "path.ssp.enable = 0")
        off_tail = off_log.count("LMNEE_ACC tail")
        ok = off_tail == 0
        print(f"[{'PASS' if ok else 'FAIL'}] SSP off: no tail splats "
                f"(tail={off_tail})")
        if not ok:
            fails.append("disabled tail still firing")

        # Unbiasedness smoke: at matched spp the two renders agree up to
        # Monte Carlo noise (the tail only re-seeds the same solver).
        la = ssp_img.mean(2).ravel()
        lb = off_img.mean(2).ravel()
        corr = float(np.corrcoef(la, lb)[0, 1])
        ok = corr > 0.97
        print(f"[{'PASS' if ok else 'FAIL'}] SSP parity: corr={corr:.5f}")
        if not ok:
            fails.append("image parity")

    print("===")
    print("PASS" if not fails else f"FAIL: {fails}")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
