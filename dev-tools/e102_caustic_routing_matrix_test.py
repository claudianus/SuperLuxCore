#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# e102: caustic-routing flag matrix — every estimator wiring touched by
# the zero-config auto gate (path.lighttracing.auto / path.mnee.auto)
# and the GPU zero-light-task demote must produce a finite, non-black
# film. The silent failure mode this guards: hybrid suppression removes
# the caustic class from the eye path while no light pass exists to
# deposit it (taskCount <= 8192 => lightTaskCount == 0).
#
# Matrix dimensions:
#   scene   - seedcache (caustic-capable) / bigmonkey (diffuse-only)
#   engine  - PATHOCL at opencl.task.count=8192 (zero tail) + PATHCPU
#   flags   - zero-config / lt / lt.only / hbf / vc / mnee-off / native=0
#
# Exit code 0 = all rows pass.

import sys, os, time, math
from array import array

sys.path.insert(0, "/Users/modumaru/Desktop/code/superluxcore/SuperLuxCore/out/build/src/pysuperluxcore/Release")
import pysuperluxcore

SCENE_CAUSTIC = "/Users/modumaru/Desktop/code/superluxcore/SuperLuxCore/scenes/mnee/seedcache.scn"
SCENE_CAUSTIC_DIR = "/Users/modumaru/Desktop/code/superluxcore/SuperLuxCore/scenes/mnee"
SCENE_DIFFUSE = "/Users/modumaru/Desktop/code/superluxcore/SuperLuxCore/scenes/bigmonkey/bigmonkey.scn"
# bigmonkey .scn uses repo-root-relative ply refs
SCENE_DIFFUSE_DIR = "/Users/modumaru/Desktop/code/superluxcore/SuperLuxCore"
W, H, SPP = 96, 64, 16
TIMEOUT = 180

ROWS = [
    # (name, scene, engine, extra props, min mean)
    ("caustic.zero-config",   SCENE_CAUSTIC, "PATHOCL", "", 0.001),
    ("caustic.lt-explicit",   SCENE_CAUSTIC, "PATHOCL",
     "path.lighttracing.enable = 1\n", 0.001),
    ("caustic.lt-native0",    SCENE_CAUSTIC, "PATHOCL",
     "path.lighttracing.enable = 1\nopencl.native.threads.count = 0\n", 0.001),
    ("caustic.hbf-native0",   SCENE_CAUSTIC, "PATHOCL",
     "path.hybridbackforward.enable = 1\nopencl.native.threads.count = 0\n"
     "path.lighttracing.auto = 0\n", 0.001),
    ("caustic.vc",            SCENE_CAUSTIC, "PATHOCL",
     "path.vertexconnection.enable = 1\n", 0.001),
    ("caustic.lt.only",       SCENE_CAUSTIC, "PATHOCL",
     "path.lighttracing.only = 1\n", 0.0005),
    ("caustic.mnee-off",      SCENE_CAUSTIC, "PATHOCL",
     "path.mnee.enable = 0\n", 0.001),
    ("diffuse.zero-config",   SCENE_DIFFUSE, "PATHOCL", "", 0.001),
    ("diffuse.lt-only",       SCENE_DIFFUSE, "PATHOCL",
     "path.lighttracing.only = 1\n", 0.0005),
    ("caustic.cpu-zero",      SCENE_CAUSTIC, "PATHCPU", "", 0.001),
    ("diffuse.cpu-zero",      SCENE_DIFFUSE, "PATHCPU", "", 0.001),
    # TILEPATHOCL: 32px tiles * aa=1 -> taskCount=8192 -> zero light-task
    # tail. Tile native threads are eye-only, so every variant must
    # demote on the parsed pathTracer members (InitTaskCount runs AFTER
    # ParseOptions - cfg writes would be dead).
    ("caustic.tile-zerotail", SCENE_CAUSTIC, "TILEPATHOCL",
     "sampler.type = TILEPATHSAMPLER\ntile.size = 32\n"
     "tilepath.sampling.aa.size = 1\npath.lighttracing.enable = 1\n"
     "opencl.native.threads.count = 0\n", 0.001),
    ("caustic.tile-hbf",      SCENE_CAUSTIC, "TILEPATHOCL",
     "sampler.type = TILEPATHSAMPLER\ntile.size = 32\n"
     "tilepath.sampling.aa.size = 1\npath.hybridbackforward.enable = 1\n"
     "opencl.native.threads.count = 0\n", 0.001),
    ("caustic.tile-natives",  SCENE_CAUSTIC, "TILEPATHOCL",
     "sampler.type = TILEPATHSAMPLER\ntile.size = 32\n"
     "tilepath.sampling.aa.size = 1\npath.lighttracing.enable = 1\n"
     "opencl.native.threads.count = 2\n", 0.001),
]


def render(scene_path, scene_dir, engine, extra):
    os.chdir(scene_dir)
    sc = pysuperluxcore.Scene()
    sc.Parse(pysuperluxcore.Properties(scene_path))
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
opencl.task.count = 8192
film.imagepipelines.0.0.type = NOP
{extra}
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    t0 = time.monotonic()
    while not ses.HasDone() and time.monotonic() - t0 < TIMEOUT:
        time.sleep(0.5)
    ses.Stop()
    buf = array('f', bytes(W * H * 4 * 4))
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, buf, 0)
    vals = list(buf)
    finite = all(math.isfinite(v) for v in vals)
    return (sum(vals) / len(vals), finite)


def main():
    checks = []
    for name, scn, eng, extra, minmean in ROWS:
        scn_dir = SCENE_CAUSTIC_DIR if scn == SCENE_CAUSTIC else SCENE_DIFFUSE_DIR
        try:
            mean, finite = render(scn, scn_dir, eng, extra)
        except Exception as e:
            checks.append((name, False, f"EXCEPTION {e}"))
            print(f"  [FAIL] {name}: EXCEPTION {e}", flush=True)
            continue
        ok = finite and mean > minmean
        checks.append((name, ok, f"mean={mean:.5f} finite={finite}"))
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}: "
              f"mean={mean:.5f} finite={finite}", flush=True)

    allok = all(ok for _, ok, _ in checks)
    print(f"===== caustic routing matrix: "
          f"{'ALL PASS' if allok else 'FAIL'} =====", flush=True)
    sys.exit(0 if allok else 1)


if __name__ == "__main__":
    main()
