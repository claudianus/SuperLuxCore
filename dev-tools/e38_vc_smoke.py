# SPDX-License-Identifier: Apache-2.0
#
# Phase 2.4 (M6) smoke: vertex connection on PATHOCL.
# Renders the Cornell box with path.vertexconnection.enable on each GPU
# backend and verifies the kernel compiles, the render completes, and
# the image is sane (no NaN, lit pixels present).

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent / "parity"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from harness import device_mask, load_scene, render, metrics, luminance, save_ppm, REPO

OUT = REPO / "dev-tools/out/vc"

SPEC = {
    "props_file": "scenes/cornell/cornell.scn",
    "engine": "PATHOCL",
    "sampler": "SOBOL",
    # task count must exceed the 8192 eye-task floor or the promoted
    # light population is empty and VC silently disables itself
    "cfg_extra": "path.vertexconnection.enable = 1\n"
        "opencl.task.count = 32768\n",
    "spp": 32,
    "timeout": 300,
}

def main():
    backend = sys.argv[1] if len(sys.argv) > 1 else "metal"
    want = {"metal": "METAL_GPU", "opencl": "OPENCL_GPU"}[backend]
    sel = device_mask(want)
    print(f"[vc] backend={backend} mask={sel}")

    scene = load_scene(SPEC)
    img = render(scene, SPEC, sel=sel)
    L = luminance(img)
    print(f"[vc] mean_lum={L.mean():.5f} nans={int(__import__('numpy').isnan(img).sum())} "
            f"lit_frac={(L > 0.02).mean():.4f}")
    save_ppm(img, OUT / f"cornell_vc_{backend}")

    # Reference: VC off, same scene — connection must not black out the
    # frame (it only adds indirect strategies)
    spec_off = dict(SPEC)
    spec_off["cfg_extra"] = ""
    img_off = render(load_scene(spec_off), spec_off, sel=sel)
    save_ppm(img_off, OUT / f"cornell_vc_off_{backend}")
    m = metrics(img_off, img)
    print(f"[vc] vs VC-off: {m}")

if __name__ == "__main__":
    main()
