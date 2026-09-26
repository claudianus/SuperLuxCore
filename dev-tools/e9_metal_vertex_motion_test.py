#!/usr/bin/env python3
"""E9 regression test: Metal HWRT per-vertex deformation motion blur.

Deterministic scene: an emissive red quad (4 verts, 2 tris) with vertex
motion keyframes, in front of a large non-emissive wall. The quad uses
material emission so its silhouette is readable without any lighting,
and a narrow camera shutter window pins every ray to ~one time so the
rendered pose is rigid and exactly checkable.

Run with the Blender-bundled python (or any python with the built
pyluxcore on sys.path):

    LUXCORE_PY=/path/to/pyluxcore/dir python3 e9_metal_vertex_motion_test.py

Requires a Metal-capable device (Apple Silicon); skips cleanly otherwise.
"""

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
            "..",
            "out",
            "build",
            "src",
            "pyluxcore",
            "Release",
        ),
    ),
)
import pyluxcore

pyluxcore.Init()

FAILURES = []


def check(name, cond, detail=""):
    tag = "PASS" if cond else "FAIL"
    print(f"[{tag}] {name} {detail}")
    if not cond:
        FAILURES.append(name)


def build_scene(steps_dx=None, times=None, shutter=(0.0, 1.0), with_wall=True):
    """Quad at z=0 moving along +x by steps_dx; wall at z=-2 behind it."""
    props = pyluxcore.Properties()
    props.Set(pyluxcore.Property("scene.materials.mat.type", "matte"))
    props.Set(pyluxcore.Property("scene.materials.mat.kd", [0.9, 0.2, 0.2]))
    props.Set(pyluxcore.Property("scene.materials.mat.emission", [4.0, 0.0, 0.0]))
    props.Set(pyluxcore.Property("scene.materials.bgmat.type", "matte"))
    props.Set(pyluxcore.Property("scene.materials.bgmat.kd", [0.8, 0.8, 0.8]))
    if with_wall:
        props.Set(pyluxcore.Property("scene.objects.bg.material", "bgmat"))
        props.Set(
            pyluxcore.Property(
                "scene.objects.bg.vertices",
                [-5.0, -0.5, -2.0, 5.0, -0.5, -2.0, 5.0, 3.5, -2.0, -5.0, 3.5, -2.0],
            )
        )
        props.Set(pyluxcore.Property("scene.objects.bg.faces", [0, 1, 2, 0, 2, 3]))

    base = np.array(
        [[-0.4, 0.3, 0], [0.4, 0.3, 0], [0.4, 1.1, 0], [-0.4, 1.1, 0]],
        dtype=np.float32,
    )
    tris = np.array([[0, 1, 2], [0, 2, 3]], dtype=np.uint32)

    scene = pyluxcore.Scene()
    scene.DefineMeshExt("quad", base, tris)
    if steps_dx:
        t = np.array(
            times if times else np.linspace(0.0, 1.0, len(steps_dx)),
            dtype=np.float32,
        )
        steps = [base + np.array([d, 0, 0], dtype=np.float32) for d in steps_dx]
        scene.SetMeshVertexMotion("quad", t, steps)

    props.Set(pyluxcore.Property("scene.objects.quad.material", "mat"))
    props.Set(pyluxcore.Property("scene.objects.quad.shape", "quad"))
    props.Set(pyluxcore.Property("scene.camera.type", "perspective"))
    props.Set(pyluxcore.Property("scene.camera.lookat.orig", [0.0, 1.2, 4.0]))
    props.Set(pyluxcore.Property("scene.camera.lookat.target", [0.0, 0.7, 0.0]))
    props.Set(pyluxcore.Property("scene.camera.fieldofview", [35.0]))
    props.Set(pyluxcore.Property("scene.camera.shutteropen", [shutter[0]]))
    props.Set(pyluxcore.Property("scene.camera.shutterclose", [shutter[1]]))
    # No lights: only the emissive quad is visible.
    scene.Parse(props)
    return scene


def render(scene, w=128, h=128, spp=32):
    rcfg = pyluxcore.Properties()
    rcfg.Set(pyluxcore.Property("renderengine.type", "TILEPATHOCL"))
    rcfg.Set(pyluxcore.Property("sampler.type", "TILEPATHSAMPLER"))
    rcfg.Set(pyluxcore.Property("batch.haltspp", [spp]))
    rcfg.Set(pyluxcore.Property("film.width", [w]))
    rcfg.Set(pyluxcore.Property("film.height", [h]))
    rcfg.Set(pyluxcore.Property("opencl.cpu.use", [0]))
    rcfg.Set(pyluxcore.Property("opencl.gpu.use", [1]))
    rcfg.Set(pyluxcore.Property("opencl.native.threads.count", [0]))
    # Second device slot is the Metal intersection device in this build.
    rcfg.Set(pyluxcore.Property("opencl.devices.select", "01"))
    session = pyluxcore.RenderSession(pyluxcore.RenderConfig(rcfg, scene))
    session.Start()
    t0 = _t.time()
    while _t.time() - t0 < 60:
        session.UpdateStats()
        if (
            session.HasDone()
            or session.GetStats().Get("stats.renderengine.pass").GetInt() >= spp
        ):
            break
        _t.sleep(0.05)
    session.Pause()
    rgb = np.zeros(w * h * 3, dtype=np.float32)
    session.GetFilm().GetOutputFloat(pyluxcore.FilmOutputType.RGB, rgb)
    session.Stop()
    return rgb.reshape(h, w, 3)


def quad_cols(rgb):
    """Columns where the emissive quad is visible."""
    vis = rgb[..., 0] > 0.3
    ys, xs = np.where(vis)
    return (int(xs.min()), int(xs.max()), len(ys)) if len(ys) else None


def main():
    # --- baseline: static quad ---
    c = quad_cols(render(build_scene()))
    check("static quad visible", c is not None and 1000 < c[2], str(c))
    if c is None:
        print("no quad pixels at all - cannot continue")
        return 1
    static_min, static_max = c[0], c[1]
    check(
        "static quad placement",
        35 <= static_min <= 50 and 75 <= static_max <= 95,
        f"cols {c[0]}-{c[1]}",
    )

    # --- fixed-pose checks via narrow shutter windows (K=3) ---
    # Expected: t=0 -> base cols; t=0.25 -> +0.75 world (screen-left);
    # t=0.5 -> +1.5 (mostly off-frame left); t=1 -> base.
    expected = {
        0.00: (35, 95, True),
        0.25: (0, 50, True),
        0.50: (0, 15, True),
        1.00: (35, 95, True),
    }
    for tc, (lo, hi, _) in expected.items():
        w = 0.02
        c = quad_cols(
            render(
                build_scene(
                    [0.0, 1.5, 0.0],
                    [0.0, 0.5, 1.0],
                    shutter=(max(0.0, tc - w), min(1.0, tc + w)),
                )
            )
        )
        ok = c is not None and c[0] >= lo and c[1] <= hi
        check(f"K=3 pose t~{tc}", ok, f"cols {c}")

    # --- full-shutter sweep: silhouette must extend past static span ---
    for name, sdx, tms in [
        ("K=2", [0.0, 1.5], None),
        ("K=3", [0.0, 1.5, 0.0], [0.0, 0.5, 1.0]),
        ("K=4", [0.0, 0.5, 1.5, 0.0], None),
        ("K=3 nonuniform", [0.0, 1.5, 0.0], [0.0, 0.3, 1.0]),
    ]:
        c = quad_cols(render(build_scene(sdx, tms)))
        ok = c is not None and c[2] > 2500 and c[0] < static_min - 30
        check(f"{name} sweep extends", ok, f"cols {c} px={c[2] if c else 0}")

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILURES: {FAILURES}")
        return 1
    print("ALL PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
