# SPDX-License-Identifier: Apache-2.0
#
# E112: Cycles Filter Glossy (path.filterglossy = Cycles blur_glossy).
#
# Cycles blurs every microfacet lobe reached after a low-probability
# bounce: alpha >= sqrt(1 - blur_pdf) / 2 with blur_pdf = (smallest BSDF
# pdf along the path) / blur_glossy. In the e109 room a polished metal2
# box top (roughness 0) reflects the ceiling panel onto the ceiling; the
# camera sees that caustic through camera -> ceiling (diffuse, pdf <=
# 1/pi) -> metal -> panel, so Filter Glossy 1 widens the metal lobe to
# alpha ~0.41+ there. The caustic must spread out (lower peak) while its
# energy stays put, and nothing changes with the filter off.
#
#   1  filterglossy 0 == property absent (frame mean, noise-level)
#   2  filterglossy 1: the caustic peak on the ceiling spreads out while
#      the frame mean (total energy) stays within 3%
#   3  the camera's own view of the metal (first vertex) is not blurred
#   4  PATHOCL matches PATHCPU with the filter on
#
# Run from the repo root:
#   python3.13 dev-tools/e112_cycles_filter_glossy_test.py

import importlib.util
import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location(
        "e109", REPO / "dev-tools/e109_pgic_hybrid_caustic_test.py")
e109 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(e109)
pysuperluxcore = e109.pysuperluxcore

BASE = """
path.hybridbackforward.enable = 0
path.lighttracing.enable = 0
path.mnee.enable = 0
path.photongi.caustic.enabled = 0
"""
METAL_SCENE = e109.SCENE
WHITE_SCENE = e109.SCENE.replace("scene.objects.mbox.material = mir",
                                 "scene.objects.mbox.material = white")
# the metal box's front face, seen directly by the camera
FRONT = np.s_[50:62, 58:78]


def render(scene, extra, engine="PATHCPU", spp=512):
    e109.SCENE = scene
    return e109.render(BASE + extra, engine, spp)


def blocks(a, k=8):
    h, w = a.shape
    return a[:h // k * k, :w // k * k].reshape(h // k, k, w // k, k).mean((1, 3))


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    pysuperluxcore.Init()
    ok = True
    white = render(WHITE_SCENE, "")
    absent = render(METAL_SCENE, "")
    off = render(METAL_SCENE, "path.filterglossy = 0\n")
    on = render(METAL_SCENE, "path.filterglossy = 1\n")

    r = off.mean() / absent.mean()
    ok &= check(abs(r - 1) < 0.005, "1 filterglossy 0 is a no-op", f"mean ratio {r:.4f}")

    peak_off = blocks(off - white)[0:4].max()
    peak_on = blocks(on - white)[0:4].max()
    r = on.mean() / off.mean()
    ok &= check(peak_on < 0.5 * peak_off and abs(r - 1) < 0.03,
                "2 filterglossy 1 spreads the caustic",
                f"peak {peak_off:.4f} -> {peak_on:.4f}, frame mean ratio {r:.4f}")

    # relative to the frame: the room it mirrors dims with the blurred
    # (single-scatter) metal top, the mirror image itself must not blur
    r = (on[FRONT].mean() / on.mean()) / (off[FRONT].mean() / off.mean())
    ok &= check(abs(r - 1) < 0.02, "3 first vertex stays sharp",
                f"box front / frame ratio {r:.4f}")

    try:
        gpu = render(METAL_SCENE, "path.filterglossy = 1\n", "PATHOCL")
        r = gpu.mean() / on.mean()
        ok &= check(abs(r - 1) < 0.02, "4 PATHOCL == PATHCPU", f"ratio {r:.4f}")
    except RuntimeError as e:
        print(f"[SKIP] 4 PATHOCL: {e}")

    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
