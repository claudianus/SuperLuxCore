# SPDX-License-Identifier: Apache-2.0
#
# gabornoise texture test (Cycles ShaderNodeTexGabor coverage support).
#
# Renders an emissive quad whose albedo is a gabornoise texture driven by
# the world-space hit position. Checks:
#   - value output: spatially varying, mean inside [0,1] range band
#   - phase/intensity outputs: in [0,1], non-degenerate
#   - CPU (PATHCPU) and GPU (TILEPATHOCL via cl2msl) results agree closely
#     (same cell hash + same impulse schedule on both paths)
#
# PATHOCL is covered too: it regressed on this scene because bucketed
# samplers swept pixel offsets in lockstep across tasks, leaving a
# periodic morton-hole mask when rendering halted early. The sampler
# now uses a staggered cyclic bucket sweep (see bucketCycleStart in
# include/slg/samplers/sampler_types.cl).
#
# Run:
#   LUXCORE_PY=out/build/src/pyluxcore/Release \
#     <python3.13> dev-tools/e11_gabor_test.py
#
# Exits 0 when all checks pass.

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

FAILURES = []


def check(name, ok, detail=""):
    print(("PASS" if ok else "FAIL") + f" {name} {detail}", flush=True)
    if not ok:
        FAILURES.append(name)


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


def render(engine, out, extra="", w=128, h=128, spp=16):
    props = pyluxcore.Properties()
    props.SetFromString("""
scene.camera.lookat.orig = 0 -1.5 0
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 0 1
scene.objects.quad.ply = /tmp/gabor_quad.ply
scene.materials.white.type = matte
scene.materials.white.kd = 0.0
scene.materials.white.emission = gb0
scene.objects.quad.material = white
scene.textures.pos.type = position
""")
    props.Set(pyluxcore.Property("scene.textures.gb0.type", "gabornoise"))
    props.Set(pyluxcore.Property("scene.textures.gb0.vector", "pos"))
    props.Set(pyluxcore.Property("scene.textures.gb0.scale", 3.0))
    props.Set(pyluxcore.Property("scene.textures.gb0.output", out))
    props.SetFromString(extra)
    scene = pyluxcore.Scene()
    scene.Parse(props)

    rcfg = pyluxcore.Properties()
    rcfg.Set(pyluxcore.Property("renderengine.type", engine))
    rcfg.Set(pyluxcore.Property("sampler.type",
            "TILEPATHSAMPLER" if engine == "TILEPATHOCL" else "SOBOL"))
    rcfg.Set(pyluxcore.Property("opencl.cpu.use", [0]))
    rcfg.Set(pyluxcore.Property("opencl.gpu.use", [1 if engine != "PATHCPU" else 0]))
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
    return rgb


def main():
    pyluxcore.Init()
    open("/tmp/gabor_quad.ply", "w").write(QUAD_PLY)

    for engine in ("PATHCPU", "TILEPATHOCL", "PATHOCL"):
        # value output: must vary spatially and sit near mid-range
        img = render(engine, "value",
                     "scene.textures.gb0.frequency = 4.0\n"
                     "scene.textures.gb0.isotropy = 0.0\n")
        m, s, mn, mx = img.mean(), img.std(), img.min(), img.max()
        check(f"{engine} gabor value varies", s > 0.02,
              f"mean={m:.3f} std={s:.3f}")
        check(f"{engine} gabor value range", 0.0 <= mn and mx <= 1.0,
              f"min={mn:.3f} max={mx:.3f}")
        check(f"{engine} gabor value mean sane", 0.15 < m < 0.85,
              f"mean={m:.3f}")

        # anisotropic (isotropy=1) still varies, different pattern
        img2 = render(engine, "value",
                      "scene.textures.gb0.frequency = 6.0\n"
                      "scene.textures.gb0.isotropy = 1.0\n"
                      "scene.textures.gb0.orientation = 0.0\n")
        check(f"{engine} gabor aniso varies", img2.std() > 0.02,
              f"mean={img2.mean():.3f} std={img2.std():.3f}")

        # phase / intensity outputs: defined, in range, varying
        img3 = render(engine, "phase",
                      "scene.textures.gb0.frequency = 4.0\n")
        check(f"{engine} gabor phase range",
              0.0 <= img3.min() and img3.max() <= 1.0 and img3.std() > 0.01,
              f"min={img3.min():.3f} max={img3.max():.3f} std={img3.std():.3f}")

        img4 = render(engine, "intensity",
                      "scene.textures.gb0.frequency = 4.0\n")
        check(f"{engine} gabor intensity range",
              0.0 <= img4.min() and img4.max() <= 1.0,
              f"min={img4.min():.3f} max={img4.max():.3f} mean={img4.mean():.3f}")

        globals()[engine] = img

    # CPU vs GPU determinism: the mean should agree within Monte Carlo and
    # tonemapping noise; std (pattern contrast) is the strongest signal
    cpu, gpu = globals()["PATHCPU"], globals()["TILEPATHOCL"]
    check("CPU/OCL gabor mean match", abs(cpu.mean() - gpu.mean()) < 0.08,
          f"cpu={cpu.mean():.3f} gpu={gpu.mean():.3f}")
    check("CPU/OCL gabor std match", abs(cpu.std() - gpu.std()) < 0.08,
          f"cpu={cpu.std():.3f} gpu={gpu.std():.3f}")


main()

print()
if FAILURES:
    print(f"{len(FAILURES)} FAILURES: {FAILURES}")
    sys.exit(1)
print("ALL GABOR TESTS PASSED")
