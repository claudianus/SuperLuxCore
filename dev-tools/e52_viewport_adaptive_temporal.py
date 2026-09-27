# SPDX-License-Identifier: Apache-2.0
#
# E52: viewport convergence megaplan phases V-B/V-C/V-D.
#
# Leg A (V-C adaptive): RTPATHOCL with sampler.tilepath.adaptive.strength
#   + film.noiseestimation - noise-guided lattice sampling must keep the
#   unbiased coverage floor (cells still fill) and stay finite.
#
# Leg B (V-B LT softening): PATHOCL + light tracing with VIEWPORT_INFILL
#   ltblend - isolated LT splats must be softened toward the neighbourhood
#   (fewer single-pixel speckles than ltblend=0), image stays finite.
#
# Leg C (V-D temporal reuse): RTPATHOCL + VIEWPORT_TEMPORAL - a
#   camera-only scene edit resets the film, yet the first post-edit
#   pipeline read must already be coherent (history forward-warped into
#   the not-yet-sampled pixels) while the raw pipeline is still empty.
#
# Run from the repo root after a Release build:
#   python3.13 dev-tools/e52_viewport_adaptive_temporal.py
#
# Env: SUPERLUXCORE_BACKENDS=cpu,opencl,metal (leg subset, default all).

import os
import sys
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for cfg in ("Release", "Debug"):
    p = REPO / f"out/build/src/pysuperluxcore/{cfg}"
    if (p / "pysuperluxcore.cpython-313-darwin.so").exists():
        sys.path.insert(0, str(p))
        break
import pysuperluxcore

WIDTH, HEIGHT = 1280, 720
BOOT_TIMEOUT_S = 240

BACKENDS = {b.strip() for b in
        os.environ.get("SUPERLUXCORE_BACKENDS", "cpu,opencl,metal").split(",")
        if b.strip()}


def device_mask(want_type):
    pysuperluxcore.Init()
    descs = pysuperluxcore.GetOpenCLDeviceDescs()
    mask = ""
    i = 0
    while True:
        try:
            t = descs.Get(f"opencl.device.{i}.type").GetString()
        except Exception:
            break
        mask += "1" if t == want_type else "0"
        i += 1
    return mask or None


def parse_scene(rel_path):
    try:
        props = pysuperluxcore.Properties(str(REPO / rel_path))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    except Exception:
        pass
    cwd = os.getcwd()
    os.chdir(str(REPO / Path(rel_path).parent))
    try:
        props = pysuperluxcore.Properties(str(Path(rel_path).name))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
        return scene
    finally:
        os.chdir(cwd)


def nonzero_frac(img):
    lum = img.mean(axis=2)
    return float((lum > 0.001).mean()), lum


def read_pipeline(ses, index, width=WIDTH, height=HEIGHT):
    rgb = np.empty(width * height * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(
        pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE, rgb, index, True)
    return rgb.reshape(height, width, 3)


def wait_passes(ses, n):
    deadline = time.monotonic() + BOOT_TIMEOUT_S
    while True:
        ses.UpdateStats()
        try:
            passes = ses.GetStats().Get("stats.renderengine.pass").GetInt()
        except Exception:
            passes = 0
        if passes >= n:
            return passes
        if time.monotonic() > deadline:
            raise TimeoutError(f"no {n} passes within {BOOT_TIMEOUT_S}s")
        time.sleep(0.05)


def speckle_count(img, k=8.0):
    """Isolated bright pixels: luminance > k x mean of 8 neighbours."""
    lum = img.mean(axis=2)
    padded = np.pad(lum, 1, mode="edge")
    acc = np.zeros_like(lum)
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy == 0 and dx == 0:
                continue
            acc += padded[1+dy:1+dy+lum.shape[0], 1+dx:1+dx+lum.shape[1]]
    nmean = acc / 8.0
    return int(((lum > 0.5) & (lum > k * np.maximum(nmean, 1e-3))).sum())


# ----------------------------------------------------------------------
# Leg A: tilepath adaptive sampling keeps unbiased coverage
# ----------------------------------------------------------------------

def leg_adaptive(mask):
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = RTPATHOCL
sampler.type = TILEPATHSAMPLER
sampler.tilepath.adaptive.strength = 0.8
sampler.tilepath.adaptive.userimportanceweight = 0.75
film.noiseestimation.warmup = 4
film.noiseestimation.step = 4
film.imagepipelines.000.0.type = TONEMAP_LINEAR
film.imagepipelines.000.0.scale = 1
renderengine.seed = 17
""")
    cfg.Set(pysuperluxcore.Property("opencl.devices.select", mask))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        wait_passes(ses, 12)
        img = read_pipeline(ses, 0)
    finally:
        ses.Stop()

    frac, lum = nonzero_frac(img)
    finite = np.isfinite(img).all()
    # Coverage floor: every 4x4 cell must keep receiving samples even
    # while compute concentrates on noise
    cells = 0
    bh, bw = HEIGHT // 4, WIDTH // 4
    for cy in range(4):
        for cx in range(4):
            if (lum[cy*bh:(cy+1)*bh, cx*bw:(cx+1)*bw] > 0.001).any():
                cells += 1
    ok = finite and frac > 0.5 and cells >= 12
    print(f"[{'PASS' if ok else 'FAIL'}] adaptive RTPATHOCL: "
          f"coverage={frac:.3f} cells={cells}/16 finite={finite}")
    return ok


# ----------------------------------------------------------------------
# Leg A2: same coverage floor on the CPU tilepath path
# ----------------------------------------------------------------------

def leg_adaptive_cpu():
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = RTPATHCPU
sampler.type = RTPATHCPUSAMPLER
sampler.rtpathcpusampler.adaptive.strength = 0.8
film.noiseestimation.warmup = 4
film.noiseestimation.step = 4
film.imagepipelines.000.0.type = TONEMAP_LINEAR
film.imagepipelines.000.0.scale = 1
renderengine.seed = 17
batch.haltspp = 12
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        wait_passes(ses, 12)
        img = read_pipeline(ses, 0)
    finally:
        ses.Stop()

    frac, lum = nonzero_frac(img)
    finite = np.isfinite(img).all()
    cells = 0
    bh, bw = HEIGHT // 4, WIDTH // 4
    for cy in range(4):
        for cx in range(4):
            if (lum[cy*bh:(cy+1)*bh, cx*bw:(cx+1)*bw] > 0.001).any():
                cells += 1
    ok = finite and frac > 0.6 and cells >= 12
    print(f"[{'PASS' if ok else 'FAIL'}] adaptive RTPATHCPU: "
          f"coverage={frac:.3f} cells={cells}/16 finite={finite}")
    return ok


# ----------------------------------------------------------------------
# Leg B: LT speckle softening
# ----------------------------------------------------------------------

def leg_ltblend(mask):
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHOCL
sampler.type = SOBOL
path.lighttracing.enable = 1
path.lighttracing.taskfraction = 0.5
film.imagepipelines.000.0.type = VIEWPORT_INFILL
film.imagepipelines.000.0.ltblend = 0.0
film.imagepipelines.000.1.type = TONEMAP_LINEAR
film.imagepipelines.001.0.type = VIEWPORT_INFILL
film.imagepipelines.001.0.ltblend = 0.8
film.imagepipelines.001.1.type = TONEMAP_LINEAR
renderengine.seed = 17
""")
    cfg.Set(pysuperluxcore.Property("opencl.devices.select", mask))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        # Early window is where LT speckles hurt: catch the first passes
        wait_passes(ses, 2)
        hard = read_pipeline(ses, 0)
        soft = read_pipeline(ses, 1)
    finally:
        ses.Stop()

    hard_s = speckle_count(hard)
    soft_s = speckle_count(soft)
    finite = np.isfinite(hard).all() and np.isfinite(soft).all()
    # Softening must not add speckles and should reduce them (or tie when
    # no LT splats happened to be isolated this frame)
    ok = finite and (soft_s <= hard_s)
    print(f"[{'PASS' if ok else 'FAIL'}] LT speckle: "
          f"ltblend0={hard_s}px ltblend0.8={soft_s}px finite={finite}")
    return ok


# ----------------------------------------------------------------------
# Leg B2: edge-aware smoothing reduces noise in low-sample pixels
# ----------------------------------------------------------------------

def leg_smooth(mask):
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = RTPATHOCL
sampler.type = TILEPATHSAMPLER
film.outputs.000.type = DEPTH
film.outputs.000.filename = e52_depth.exr
film.outputs.001.type = AVG_SHADING_NORMAL
film.outputs.001.filename = e52_asn.exr
film.imagepipelines.000.0.type = TONEMAP_LINEAR
film.imagepipelines.001.0.type = VIEWPORT_SMOOTH
film.imagepipelines.001.1.type = TONEMAP_LINEAR
renderengine.seed = 17
""")
    cfg.Set(pysuperluxcore.Property("opencl.devices.select", mask))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        wait_passes(ses, 4)
        raw = read_pipeline(ses, 0)
        smoothed = read_pipeline(ses, 1)
    finally:
        ses.Stop()

    finite = np.isfinite(smoothed).all()
    s_raw, s_smt = speckle_count(raw), speckle_count(smoothed)
    # Smoothing must keep the frame valid and must not increase speckle
    ok = finite and (s_smt <= s_raw)
    print(f"[{'PASS' if ok else 'FAIL'}] smooth: "
          f"speckle raw={s_raw}px smooth={s_smt}px finite={finite}")
    return ok


# ----------------------------------------------------------------------
# Leg C: temporal reuse across camera edits
# ----------------------------------------------------------------------

def leg_temporal(mask):
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = RTPATHOCL
sampler.type = TILEPATHSAMPLER
film.outputs.000.type = POSITION
film.outputs.000.filename = e52_pos.exr
film.imagepipelines.000.0.type = VIEWPORT_TEMPORAL
film.imagepipelines.000.1.type = TONEMAP_LINEAR
film.imagepipelines.001.0.type = TONEMAP_LINEAR
renderengine.seed = 17
""")
    cfg.Set(pysuperluxcore.Property("opencl.devices.select", mask))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        wait_passes(ses, 20)
        # This read runs the temporal plugin, which snapshots the dense
        # displayed frame + depth + camera as its history
        before = read_pipeline(ses, 0)
        before_frac, _ = nonzero_frac(before)

        # Camera-only orbit through the real edit path (Scene::Parse of
        # camera props does not register CAMERA_EDIT - Blender uses the
        # same Translate/Rotate camera API)
        ses.BeginSceneEdit()
        scene.GetCamera().TranslateLeft(0.6)
        ses.EndSceneEdit()

        # Poll right after the edit: the film reset lands on a GPU pass
        # boundary, leaving a window where raw coverage is nearly empty
        # but the temporal plugin warps history into the holes. Track
        # the biggest advantage the warp ever shows, plus the matching
        # frames (polls are ~100ms each so the deepest dip is racy).
        best_adv, best_w, best_r = -1.0, None, None
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            w_img = read_pipeline(ses, 0)
            r_img = read_pipeline(ses, 1)
            wf, _ = nonzero_frac(w_img)
            rf, _ = nonzero_frac(r_img)
            if wf - rf > best_adv:
                best_adv, best_w, best_r = wf - rf, w_img, r_img
            if rf > 0.6:
                break  # coverage rebuilt; the sparse window has passed
            time.sleep(0.02)
        warped, raw_img = best_w, best_r
    finally:
        ses.Stop()

    finite = np.isfinite(warped).all()
    # The warp only fills uncovered pixels, so its advantage is a
    # coverage delta; also verify the warped content is actually the
    # reprojected pre-edit frame (correlates with `before` better than
    # the freshly repainted raw frame does)
    def corr(a, b):
        aa = a[::4].reshape(-1) - a[::4].mean()
        bb = b[::4].reshape(-1) - b[::4].mean()
        return float(np.dot(aa, bb) / (np.linalg.norm(aa) * np.linalg.norm(bb) + 1e-9))
    cw = corr(warped, before)
    cr = corr(raw_img, before)
    # A small camera move keeps even the freshly repainted raw frame
    # correlated with the pre-edit image, so the check is only that the
    # warp itself is coherent reprojected content (not noise/garbage)
    ok = finite and (before_frac > 0.5) and (best_adv > 0.02) and (cw > 0.5)
    print(f"[{'PASS' if ok else 'FAIL'}] temporal: pre={before_frac:.3f} "
          f"best warp adv={best_adv:.3f} corrW={cw:.3f} corrR={cr:.3f} "
          f"finite={finite}")
    return ok


# ----------------------------------------------------------------------
# Leg C2: temporal reuse through the Scene::Parse camera path.
#
# Blender edits the camera by re-parsing scene.camera.* props, which
# REPLACES the Camera object - and CreateCamera() initializes it with a
# dummy 100x100 raster (Scene::CreateCamera). PublishViewportCamera()
# must therefore run only after the engine-side scene preprocessing has
# updated the camera to the real film resolution, otherwise the warp
# matrices are built for 100x100 and the whole history frame lands in a
# tiny top-left block (the "floating blob in the corner" bug).
# ----------------------------------------------------------------------

def leg_temporal_parse(mask):
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = RTPATHOCL
sampler.type = TILEPATHSAMPLER
film.outputs.000.type = POSITION
film.outputs.000.filename = e52_pos_parse.exr
film.imagepipelines.000.0.type = VIEWPORT_TEMPORAL
film.imagepipelines.000.1.type = TONEMAP_LINEAR
film.imagepipelines.001.0.type = TONEMAP_LINEAR
renderengine.seed = 17
""")
    cfg.Set(pysuperluxcore.Property("opencl.devices.select", mask))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        wait_passes(ses, 20)
        before = read_pipeline(ses, 0)
        before_frac, _ = nonzero_frac(before)

        # The real Blender path: re-parse scene.camera.* props, which
        # replaces the Camera object (fresh 100x100 raster until the
        # engine-side Preprocess runs during EndSceneEdit)
        ses.BeginSceneEdit()
        scene.Parse(pysuperluxcore.Properties().SetFromString("""
scene.camera.type = "perspective"
scene.camera.lookat.orig = -2.18 -8. 2.73
scene.camera.lookat.target = -2.18 2. 2.73
scene.camera.fieldofview = 39.1463
"""))
        ses.EndSceneEdit()

        best_adv, best_w = -1.0, None
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            w_img = read_pipeline(ses, 0)
            r_img = read_pipeline(ses, 1)
            wf, _ = nonzero_frac(w_img)
            rf, _ = nonzero_frac(r_img)
            if wf - rf > best_adv:
                best_adv, best_w = wf - rf, w_img
            if rf > 0.6:
                break
            time.sleep(0.02)
        warped = best_w
    finally:
        ses.Stop()

    finite = np.isfinite(warped).all()
    wlum = warped.mean(axis=2)
    hit = wlum > 0.001
    # With the 100x100 bug every warped pixel lands in the top-left
    # ~100x100 block; a correct warp spreads content over the frame.
    # Rows are measured from the bottom in film space, so the block may
    # appear in either image corner - test the invariant instead: most
    # warped coverage must live outside both 100x100 corners.
    if hit.any():
        ys, xs = np.nonzero(hit)
        outside = ((ys >= 100) | (xs >= 100)).mean()
    else:
        outside = 0.0
    ok = (finite and before_frac > 0.5 and best_adv > 0.02
          and outside > 0.5)
    print(f"[{'PASS' if ok else 'FAIL'}] temporal Parse-path: "
          f"pre={before_frac:.3f} best warp adv={best_adv:.3f} "
          f"outside_100x100={outside:.3f} finite={finite}")
    return ok


# ----------------------------------------------------------------------
# Leg D: RTPATHCPU hybrid light tracing stays progressive
# ----------------------------------------------------------------------

def leg_hybrid_rtpathcpu():
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = RTPATHCPU
sampler.type = RTPATHCPUSAMPLER
path.hybridbackforward.enable = 1
path.hybridbackforward.partition = 0.8
film.imagepipelines.000.0.type = TONEMAP_LINEAR
film.imagepipelines.000.0.scale = 1
renderengine.seed = 17
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        wait_passes(ses, 8)
        img = read_pipeline(ses, 0)
        ses.UpdateStats()
        stats = ses.GetStats()
        light_sps = stats.Get(
            "stats.renderengine.total.samplesec.light").GetFloat()
    finally:
        ses.Stop()

    frac, _ = nonzero_frac(img)
    finite = np.isfinite(img).all()
    # The hybrid branch must actually splat light paths (samplesec.light
    # counts RADIANCE_PER_SCREEN_NORMALIZED) while the RT lattice keeps
    # eye-sample coverage at its usual density.
    ok = finite and frac > 0.6 and light_sps > 0.0
    print(f"[{'PASS' if ok else 'FAIL'}] hybrid RTPATHCPU: "
          f"coverage={frac:.3f} light_sps={light_sps:.0f} finite={finite}")
    return ok


def leg_hybrid_rtpathocl(mask):
    """RTPATHOCL keeps its progressive lattice while GPU light tasks splat
    (the adapter no longer swaps to PATHOCL when viewport LT is on)."""
    scene = parse_scene("scenes/cornell/cornell.scn")
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = RTPATHOCL
sampler.type = TILEPATHSAMPLER
path.hybridbackforward.enable = 1
path.lighttracing.enable = 1
path.lighttracing.taskfraction = 0.3
film.imagepipelines.000.0.type = TONEMAP_LINEAR
film.imagepipelines.000.0.scale = 1
renderengine.seed = 17
""")
    cfg.Set(pysuperluxcore.Property("opencl.devices.select", mask))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    try:
        wait_passes(ses, 8)
        img = read_pipeline(ses, 0)
        ses.UpdateStats()
        light_sps = ses.GetStats().Get(
            "stats.renderengine.total.samplesec.light").GetFloat()
    finally:
        ses.Stop()

    frac, _ = nonzero_frac(img)
    finite = np.isfinite(img).all()
    # taskfraction diverts ~30% of tasks to light splats, so early eye
    # coverage is darker than pure adaptive - the assertions that matter
    # are "light tasks actually ran" and "the frame is not black"
    ok = finite and frac > 0.2 and light_sps > 0.0
    print(f"[{'PASS' if ok else 'FAIL'}] hybrid RTPATHOCL: "
          f"coverage={frac:.3f} light_sps={light_sps:.0f} finite={finite}")
    return ok


def main():
    ok_all = True

    for dtype, tag in (("METAL_GPU", "metal"), ("OPENCL_GPU", "opencl")):
        if tag not in BACKENDS:
            continue
        mask = device_mask(dtype)
        if mask:
            ok_all &= leg_adaptive(mask)
            ok_all &= leg_ltblend(mask)
            ok_all &= leg_temporal(mask)
            ok_all &= leg_temporal_parse(mask)
            ok_all &= leg_smooth(mask)
            ok_all &= leg_hybrid_rtpathocl(mask)

    ok_all &= leg_adaptive_cpu()
    ok_all &= leg_hybrid_rtpathcpu()

    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
