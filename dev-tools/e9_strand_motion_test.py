#!/usr/bin/env python3
"""E9 phase 5b: strands (hair) deformation motion blur via pyluxcore.

Defines a strands mesh (two strands, 3 control points each, SOLID
tessellation), installs a 2-step control-point motion series through
Scene.SetStrandsVertexMotion and verifies:

  * a shuttered PATHCPU render shows the strands smeared between the
    base and displaced poses while a static render stays sharp;
  * a narrow shutter window at t~=0.5 renders only the interpolated
    mid-pose (not the full sweep);
  * wrong control-point counts and non-strands meshes raise errors.

Run with the Release pyluxcore:

    LUXCORE_PY=/path/to/pyluxcore/dir python3 dev-tools/e9_strand_motion_test.py
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


def base_points():
    # Two strands along +y, each 3 control points, at x=-0.4 / z=0.
    return np.array(
        [
            [-0.4, -0.6, 0.0],
            [-0.4, 0.0, 0.0],
            [-0.4, 0.6, 0.0],
            [-0.4, -0.6, 0.15],
            [-0.4, 0.0, 0.15],
            [-0.4, 0.6, 0.15],
        ],
        dtype=np.float32,
    )


def define_strands(scene, pts):
    scene.DefineStrands(
        "hair",
        2,                     # strand count
        len(pts),              # total control points
        [tuple(p) for p in pts],
        2,                     # segments per strand (int = default count)
        0.03,                  # default thickness
        0.0,                   # default transparency
        (0.8, 0.2, 0.2),       # default color
        None,                  # uvs
        "solid",               # fixed-topology tessellation
        4,                     # adaptive max depth (unused for solid)
        0.0,                   # adaptive error (unused)
        4,                     # solid side count
        False, False,          # caps
        False,                 # useCameraPosition
    )


def build_scene(motion, shutter):
    props = pyluxcore.Properties()
    props.Set(pyluxcore.Property("scene.materials.mat.type", "matte"))
    props.Set(pyluxcore.Property("scene.materials.mat.kd", [0.9, 0.2, 0.2]))
    props.Set(pyluxcore.Property("scene.materials.mat.emission", [3.0, 0.0, 0.0]))
    props.Set(pyluxcore.Property("scene.materials.bgmat.type", "matte"))
    props.Set(pyluxcore.Property("scene.materials.bgmat.kd", [0.8, 0.8, 0.8]))
    props.Set(pyluxcore.Property("scene.objects.bg.material", "bgmat"))
    props.Set(
        pyluxcore.Property(
            "scene.objects.bg.vertices",
            [-5.0, -1.0, -2.0, 5.0, -1.0, -2.0, 5.0, 1.5, -2.0, -5.0, 1.5, -2.0],
        )
    )
    props.Set(pyluxcore.Property("scene.objects.bg.faces", [0, 1, 2, 0, 2, 3]))
    props.Set(pyluxcore.Property("scene.objects.hair.material", "mat"))
    props.Set(pyluxcore.Property("scene.objects.hair.shape", "hair"))
    props.Set(pyluxcore.Property("scene.camera.type", "perspective"))
    props.Set(pyluxcore.Property("scene.camera.lookat.orig", [0.0, 0.3, 3.0]))
    props.Set(pyluxcore.Property("scene.camera.lookat.target", [0.0, 0.0, 0.0]))
    props.Set(pyluxcore.Property("scene.camera.fieldofview", [40.0]))
    props.Set(pyluxcore.Property("scene.camera.shutteropen", [shutter[0]]))
    props.Set(pyluxcore.Property("scene.camera.shutterclose", [shutter[1]]))

    scene = pyluxcore.Scene()
    define_strands(scene, base_points())
    if motion:
        p1 = base_points()
        p1[:, 0] += 0.8
        scene.SetStrandsVertexMotion(
            "hair",
            np.array([0.0, 1.0], dtype=np.float32),
            [base_points(), p1],
        )
    scene.Parse(props)
    return scene


def render(scene, engine="PATHCPU", w=128, h=128, spp=48):
    rcfg = pyluxcore.Properties()
    if engine == "PATHCPU":
        rcfg.Set(pyluxcore.Property("renderengine.type", "PATHCPU"))
        rcfg.Set(pyluxcore.Property("sampler.type", "SOBOL"))
        # The Metal film pipeline crashes during kernel compile in this
        # environment (pre-existing issue, unrelated to motion); keep the
        # film on CPU for the PATHCPU leg.
        rcfg.Set(pyluxcore.Property("film.hw.enable", [0]))
    else:
        rcfg.Set(pyluxcore.Property("renderengine.type", "TILEPATHOCL"))
        rcfg.Set(pyluxcore.Property("sampler.type", "TILEPATHSAMPLER"))
        rcfg.Set(pyluxcore.Property("opencl.devices.select", "01"))
    rcfg.Set(pyluxcore.Property("opencl.cpu.use", [0]))
    rcfg.Set(pyluxcore.Property("opencl.gpu.use", [1 if engine != "PATHCPU" else 0]))
    rcfg.Set(pyluxcore.Property("opencl.native.threads.count", [0]))
    rcfg.Set(pyluxcore.Property("accelerator.type", "MBVH"))
    rcfg.Set(pyluxcore.Property("batch.haltspp", [spp]))
    rcfg.Set(pyluxcore.Property("film.width", [w]))
    rcfg.Set(pyluxcore.Property("film.height", [h]))
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
    return rgb.reshape(h, w, 3)


def hair_extent(rgb):
    """(min_col, max_col) of columns containing emissive-hair pixels."""
    vis = rgb[..., 0] > (rgb[..., 1] * 2.5 + 0.1)
    colcount = vis.sum(axis=0)
    cols = np.nonzero(colcount >= 2)[0]
    if len(cols) == 0:
        return None
    return int(cols[0]), int(cols[-1])


def main():
    w = h = 128

    # -- error paths ------------------------------------------------
    scene = pyluxcore.Scene()
    define_strands(scene, base_points())
    try:
        scene.SetStrandsVertexMotion(
            "hair",
            np.array([0.0, 1.0], dtype=np.float32),
            [base_points(), base_points()[:3]],
        )
        check("rejects wrong point count", False)
    except Exception as e:
        check("rejects wrong point count", "point" in str(e).lower(), str(e)[:60])

    scene.DefineMeshExt(
        "plain", base_points()[:4],
        np.array([[0, 1, 2], [0, 2, 3]], dtype=np.uint32))
    try:
        scene.SetStrandsVertexMotion(
            "plain",
            np.array([0.0, 1.0], dtype=np.float32),
            [base_points(), base_points()],
        )
        check("rejects non-strands mesh", False)
    except Exception as e:
        check("rejects non-strands mesh", "strands" in str(e), str(e)[:60])

    # -- static baseline --------------------------------------------
    st = render(build_scene(False, (0.0, 1.0)), w, h)
    st_ext = hair_extent(st)
    check("static hair visible", st_ext is not None, str(st_ext))

    # -- full-shutter motion -----------------------------------------
    mo = render(build_scene(True, (0.0, 1.0)), w, h)
    mo_ext = hair_extent(mo)
    check("motion hair visible", mo_ext is not None, str(mo_ext))
    if st_ext and mo_ext:
        check(
            "motion smears hair wider than static",
            mo_ext[1] - mo_ext[0] >= st_ext[1] - st_ext[0] + 10,
            f"static={st_ext} motion={mo_ext}",
        )

    # -- narrow shutter at t=0.5: interpolated mid-pose only ---------
    mid = render(build_scene(True, (0.49, 0.51)), w, h)
    mid_ext = hair_extent(mid)
    check("mid-shutter hair visible", mid_ext is not None, str(mid_ext))
    if st_ext and mo_ext and mid_ext:
        # +x maps to lower screen columns in this camera: the mid pose
        # must sit inside the full sweep and away from the base pose.
        check(
            "mid pose between base and full-sweep extents",
            mid_ext[0] >= mo_ext[0] - 2 and mid_ext[1] <= st_ext[1] + 2
            and mid_ext != st_ext
            and (mid_ext[1] - mid_ext[0]) < (mo_ext[1] - mo_ext[0]),
            f"static={st_ext} mid={mid_ext} full={mo_ext}",
        )

    # -- Blender-curve path with filtered points --------------------
    # DefineBlenderCurveStrands drops invalid (0,0,0) and zero-length
    # points; the recorded source map lets SetStrandsVertexMotion accept
    # steps in the RAW input layout.
    raw_pts = np.array(
        [
            # strand 0: duplicated mid point (filtered to 2 kept)
            [-0.4, -0.6, 0.0],
            [-0.4, -0.6, 0.0],   # zero-length segment -> dropped
            [-0.4, 0.6, 0.0],
            # strand 1
            [-0.4, -0.6, 0.15],
            [-0.4, 0.0, 0.15],
            [-0.4, 0.6, 0.15],
        ],
        dtype=np.float32,
    )
    cscene = pyluxcore.Scene()
    # DefineBlenderCurveStrands uses camera-facing tessellation
    # internally, so a camera must be parsed first.
    cprops = pyluxcore.Properties()
    cprops.Set(pyluxcore.Property("scene.camera.type", "perspective"))
    cprops.Set(pyluxcore.Property("scene.camera.lookat.orig", [0.0, 0.3, 3.0]))
    cprops.Set(pyluxcore.Property("scene.camera.lookat.target", [0.0, 0.0, 0.0]))
    cprops.Set(pyluxcore.Property("scene.camera.fieldofview", [40.0]))
    cscene.Parse(cprops)
    ok = cscene.DefineBlenderCurveStrands(
        "chair",
        np.array([3, 3], dtype=np.int32),
        raw_pts.reshape(-1),
        np.empty(0, dtype=np.float32),   # colors
        np.empty(0, dtype=np.float32),   # uvs
        "", 1.0, False, None,            # image, gamma, copy_uvs, transform
        0.03, 1.0, 1.0, 1.0,             # diameter, root/tip width, offset
        "solid", 4, 0.0, 4, False, False,
        [1.0, 1.0, 1.0], [1.0, 1.0, 1.0],
    )
    check("curve strands defined", ok)
    try:
        moved = raw_pts.copy()
        moved[:, 0] += 0.8
        cscene.SetStrandsVertexMotion(
            "chair",
            np.array([0.0, 1.0], dtype=np.float32),
            [raw_pts.reshape(6, 3), moved.reshape(6, 3)],
        )
        check("raw-layout step accepted", True)
    except Exception as e:
        check("raw-layout step accepted", False, str(e)[:80])
    # Wrong raw count must raise
    try:
        cscene.SetStrandsVertexMotion(
            "chair",
            np.array([0.0, 1.0], dtype=np.float32),
            [raw_pts.reshape(6, 3), raw_pts.reshape(6, 3)[:4]],
        )
        check("raw-layout wrong count rejected", False)
    except Exception:
        check("raw-layout wrong count rejected", True)

    # -- Metal HWRT native-curve motion leg --------------------------
    # The strands mesh carries curve data, so the Metal path should use
    # the motion-curve descriptor; fall back skips cleanly off-Metal.
    try:
        mo_hw = render(build_scene(True, (0.0, 1.0)), "TILEPATHOCL", w, h)
        mo_hw_ext = hair_extent(mo_hw)
        check("HWRT motion hair visible", mo_hw_ext is not None, str(mo_hw_ext))
        if mo_ext and mo_hw_ext:
            check(
                "HWRT motion sweep matches CPU",
                abs(mo_hw_ext[0] - mo_ext[0]) <= 4
                and abs(mo_hw_ext[1] - mo_ext[1]) <= 4,
                f"cpu={mo_ext} hwrt={mo_hw_ext}",
            )
    except Exception as e:
        check("HWRT motion hair", False, str(e)[:80])

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILURES: {FAILURES}")
        sys.exit(1)
    print("All strand motion checks passed.")


if __name__ == "__main__":
    main()
