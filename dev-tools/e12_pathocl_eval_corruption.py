# SPDX-License-Identifier: Apache-2.0
#
# Regression test for the PATHOCL bucketed-sampler coverage defect.
#
# An emissive quad with `emission = add(0.3, 0.4)` must render at a
# constant 0.7 with every pixel covered. Previously PATHOCL (dense AND
# wavefront) produced a periodic `..##` zero mask (~0.7 * {1/4, 9/16,
# ...} of expected) when taskCount >> filmPixels and rendering halted
# early: tasks sharing a morton bucket swept pixelOffset 0..bucketSize-1
# in lockstep, so the first sample waves covered only a morton-clustered
# subset of the film.
#
# Fix: staggered cyclic bucket sweep (bucketCycleStart in
# include/slg/samplers/sampler_types.cl) — each task starts its bucket
# sweep at a gid-derived offset, so wave-1 coverage is spread over the
# whole film while per-bucket sweep completeness is preserved.
#
# Usage:
#   <python3.13> dev-tools/e12_pathocl_eval_corruption.py [runs]
#
# Exits 0 when all runs show full coverage at the expected mean.

import os
import sys
import time as _t

import numpy as np

sys.path.insert(
    0,
    os.environ.get(
        "LUXCORE_PY",
        os.path.join(
            os.path.dirname(os.path.abspath(__file__)),
            "..", "out", "build", "src", "pyluxcore", "Release"),
    ),
)
import pyluxcore

QUAD_PLY = """ply
format ascii 1.0
element vertex 4
property float x
property float y
property float z
element face 2
property list uchar int vertex_indices
end_header
-3 0 -3
3 0 -3
3 0 3
-3 0 3
3 0 1 2
3 0 2 3
"""


def render(engine, texdef, w=64, h=64, spp=8):
    props = pyluxcore.Properties()
    props.SetFromString("""
scene.camera.lookat.orig = 0 -1.5 0
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 0 1
scene.objects.quad.ply = /tmp/e12_quad.ply
scene.materials.white.type = matte
scene.materials.white.kd = 0.0
scene.materials.white.emission = gb0
scene.objects.quad.material = white
""" + texdef)
    scene = pyluxcore.Scene()
    scene.Parse(props)

    rcfg = pyluxcore.Properties()
    rcfg.Set(pyluxcore.Property("renderengine.type", engine))
    rcfg.Set(pyluxcore.Property("sampler.type",
            "TILEPATHSAMPLER" if engine == "TILEPATHOCL" else "SOBOL"))
    rcfg.Set(pyluxcore.Property("opencl.cpu.use", [0]))
    rcfg.Set(pyluxcore.Property("opencl.gpu.use",
            [1 if engine != "PATHCPU" else 0]))
    rcfg.Set(pyluxcore.Property("opencl.native.threads.count", [0]))
    rcfg.Set(pyluxcore.Property("accelerator.type", "MBVH"))
    rcfg.Set(pyluxcore.Property("batch.haltspp", [spp]))
    rcfg.Set(pyluxcore.Property("film.width", [w]))
    rcfg.Set(pyluxcore.Property("film.height", [h]))
    if engine == "PATHCPU":
        rcfg.Set(pyluxcore.Property("film.hw.enable", [0]))

    session = pyluxcore.RenderSession(pyluxcore.RenderConfig(rcfg, scene))
    session.Start()
    t0 = _t.time()
    while _t.time() - t0 < 120:
        session.UpdateStats()
        if session.HasDone() or (
            session.GetStats().Get("stats.renderengine.pass").GetInt() >= spp
        ):
            break
        _t.sleep(0.05)
    session.Pause()
    rgb = np.zeros(w * h * 3, dtype=np.float32)
    session.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB, rgb)
    session.Stop()
    return rgb[0::3].reshape(h, w)


def main():
    pyluxcore.Init()
    open("/tmp/e12_quad.ply", "w").write(QUAD_PLY)
    runs = int(sys.argv[1]) if len(sys.argv) > 1 else 6

    cases = {
        "const emission 0.7": "scene.textures.gb0.type = constfloat3\n"
                              "scene.textures.gb0.value = 0.7 0.7 0.7\n",
        "add(0.3,0.4)": "scene.textures.gb0.type = add\n"
                        "scene.textures.gb0.texture1 = 0.3\n"
                        "scene.textures.gb0.texture2 = 0.4\n",
    }
    failures = []
    for name, texdef in cases.items():
        for engine in ("PATHCPU", "PATHOCL", "TILEPATHOCL"):
            vals = []
            for _ in range(runs):
                img = render(engine, texdef)
                vals.append((float(img.mean()),
                             float((img > 0.35).mean())))
            ms = " ".join(f"{m:.3f}/{f:.2f}" for m, f in vals)
            ok = all(m > 0.6 and f == 1.0 for m, f in vals)
            print(f"{'PASS' if ok else 'FAIL'} {engine:12s} "
                  f"{name:20s} mean/litfrac: {ms}", flush=True)
            if not ok:
                failures.append((engine, name))

    print()
    if failures:
        print(f"{len(failures)} FAILURES: {failures}")
        sys.exit(1)
    print("ALL PATHOCL COVERAGE TESTS PASSED")


main()
