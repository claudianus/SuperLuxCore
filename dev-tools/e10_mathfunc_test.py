# SPDX-License-Identifier: Apache-2.0
#
# mathfunc texture test (Cycles trig/log coverage support).
#
# Renders a diffuse quad whose albedo is a mathfunc texture result and
# checks the mean pixel value against the expected bright/dark outcome.
# Runs on PATHCPU and PATHOCL — the OCL run exercises the cl2msl'd
# MathFuncTexture_EvalOp kernel path.
#
# Run:
#   LUXCORE_PY=out/build/src/pyluxcore/Release \
#     <python3.13> dev-tools/e10_mathfunc_test.py
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


def render(engine, op, t1, t2=None, w=128, h=128, spp=16):
    props = pyluxcore.Properties()
    props.SetFromString("""
scene.camera.lookat.orig = 0 -1.5 0
scene.camera.lookat.target = 0 0 0
scene.camera.up = 0 0 1
scene.objects.quad.ply = /tmp/mathfunc_quad.ply
scene.materials.white.type = matte
scene.materials.white.kd = 0.0
scene.materials.white.emission = mf0
scene.objects.quad.material = white
""")
    props.Set(pyluxcore.Property("scene.textures.mf0.type", "mathfunc"))
    props.Set(pyluxcore.Property("scene.textures.mf0.op", op))
    props.Set(pyluxcore.Property("scene.textures.mf0.texture1", t1))
    if t2 is not None:
        props.Set(pyluxcore.Property("scene.textures.mf0.texture2", t2))
    scene = pyluxcore.Scene()
    scene.Parse(props)

    rcfg = pyluxcore.Properties()
    rcfg.Set(pyluxcore.Property("renderengine.type", engine))
    rcfg.Set(pyluxcore.Property("sampler.type", "SOBOL"))
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
    return float(rgb.mean())


def main():
    pyluxcore.Init()
    open("/tmp/mathfunc_quad.ply", "w").write(QUAD_PLY)

    # op, arg1, arg2, expected brightness (True=lit quad, False=dark quad)
    cases = [
        ("sin", 1.5707963, None, True),    # sin(pi/2)=1 -> lit
        ("cos", 0.0, None, True),          # cos(0)=1 -> lit
        ("atan2", 0.0, 1.0, False),        # atan2(0,1)=0 -> dark
        ("ln", 2.718281828, None, True),   # ln(e)=1 -> lit
        ("exp", 0.0, None, True),          # exp(0)=1 -> lit
        ("tan", 0.0, None, False),         # tan(0)=0 -> dark
        ("asin", 0.0, None, False),        # asin(0)=0 -> dark
        ("sinh", 0.0, None, False),        # sinh(0)=0 -> dark
        ("cosh", 0.0, None, True),         # cosh(0)=1 -> lit
        ("tanh", 0.0, None, False),        # tanh(0)=0 -> dark
        ("invsqrt", 0.25, None, True),     # 1/sqrt(0.25)=2 -> lit
        ("floormod", 2.0, 2.0, False),     # 2 mod 2 = 0 -> dark
        ("floormod", -0.5, 1.0, True),     # floored mod: -0.5 mod 1 = 0.5 -> lit
    ]

    for engine in ("PATHCPU", "PATHOCL"):
        for op, t1, t2, bright in cases:
            try:
                m = render(engine, op, t1, t2)
            except Exception as e:
                check(f"{engine} {op}", False, f"exception: {e}")
                continue
            ok = (m > 0.03) if bright else (m < 0.03)
            check(f"{engine} mathfunc {op}", ok, f"mean={m:.3f}")


main()

print()
if FAILURES:
    print(f"{len(FAILURES)} FAILURES: {FAILURES}")
    sys.exit(1)
print("ALL MATHFUNC TESTS PASSED")
