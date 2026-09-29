# SPDX-License-Identifier: Apache-2.0
#
# Vertex-connection pool-multiplicity regression.
#
# The VC connect pool holds `path.vertexconnection.pool` INDEPENDENT
# light subpaths. Their connect contributions must AVERAGE (each is a
# full BDPT-style subpath estimator), not sum - before the
# subpath-normalization fix the summed estimate grew ~linearly with the
# pool size (+28% mean luminance at pool=4 on the thebox4 production
# scene, +74% at pool=16, while pool=1 stayed correct).
#
# This test renders a caustic-capable Cornell scene (VC is the only way
# the floor caustic converges on the GPU) at pool=1 and pool=8 and
# asserts the two means agree within noise. With the bug, pool=8 is
# ~1.4-2x brighter.

import sys
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
sys.path.insert(0, str(REPO / "dev-tools/parity"))
sys.path.insert(0, str(REPO / "dev-tools"))

# Import the binding BEFORE harness: harness prepends the Debug build
# dir to sys.path, which would shadow Release if the module is not
# already resolved.
import pysuperluxcore  # noqa: F401

from harness import device_mask, load_scene, render, luminance, save_ppm, REPO as _  # noqa

OUT = REPO / "dev-tools/out/vc_pool"

VC_EXTRA = (
    "path.vertexconnection.enable = 1\n"
    "path.vertexconnection.connects = 4\n"
    "path.vertexconnection.reuse = 0\n"   # isolate the pool term
    "path.vertexconnection.mergeradius = 0\n"
    "opencl.task.count = 32768\n"
)


def spec(pool):
    s = dict(
        props_file="scenes/cornell/cornell-area-caustic.scn",
        engine="PATHOCL",
        sampler="SOBOL",
        spp=64,
        timeout=600,
        cfg_extra=VC_EXTRA + f"path.vertexconnection.pool = {pool}\n",
    )
    return s


def main():
    backend = sys.argv[1] if len(sys.argv) > 1 else "metal"
    want = {"metal": "METAL_GPU", "opencl": "OPENCL_GPU"}[backend]
    sel = device_mask(want)
    print(f"[vc_pool] backend={backend}")

    img_p1 = render(load_scene(spec(1)), spec(1), sel=sel)
    img_p8 = render(load_scene(spec(8)), spec(8), sel=sel)
    save_ppm(img_p1, OUT / f"cornell_vc_pool1_{backend}")
    save_ppm(img_p8, OUT / f"cornell_vc_pool8_{backend}")

    l1, l8 = luminance(img_p1).mean(), luminance(img_p8).mean()
    ratio = l8 / l1
    nans = int(np.isnan(img_p1).sum() + np.isnan(img_p8).sum())
    print(f"[vc_pool] lum pool1={l1:.5f} pool8={l8:.5f} ratio={ratio:.4f} nans={nans}")

    ok = (0.97 < ratio < 1.03) and (nans == 0)
    print("PASS" if ok else "FAIL: pool size changes mean energy - "
            "connect contributions must average over pool subpaths, not sum")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
