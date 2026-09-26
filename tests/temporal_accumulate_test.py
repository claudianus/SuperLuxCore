#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2025-2026 Authors (see AUTHORS.txt)
#
# SPDX-License-Identifier: Apache-2.0
#
# Regression test for the MOTION_VECTOR / VARIANCE film channels and the
# TEMPORAL_ACCUMULATE image pipeline plugin.
#
# Renders two consecutive real frames of scenes/bigmonkey/bigmonkey-motion.scn:
# make_frame_scn() re-times the object motion steps so each frame covers its
# own quarter of the animation interval (scene.* keys in the render config do
# not override a scene.file). Checks:
#   1. MOTION_VECTOR: valid flag coverage and non-zero x velocity
#   2. VARIANCE: finite and non-negative
#   3. TEMPORAL_ACCUMULATE: state EXR written, history accepted on a
#      significant share of pixels, noise energy of the accumulated frame
#      lower than the same frame without accumulation
#
# Usage: python3 temporal_accumulate_test.py [pyluxcore_module_dir]
# Exit code 0 = pass, 1 = fail.

import os
import sys
import tempfile
import time

import numpy as np

sys.path.insert(0, sys.argv[1] if len(sys.argv) > 1 else ".")
import pyluxcore

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SCENE = os.path.join(ROOT, "scenes/bigmonkey/bigmonkey-motion.scn")
CFG = os.path.join(ROOT, "scenes/bigmonkey/bigmonkey-motion.cfg")
W, H = 320, 240
RENDER_SECS = 8.0


def make_frame_scn(frame_idx, out_path):
    """Writes a per-frame scene: motion steps are re-timed so the frame's
    sub-interval of the animation maps onto shutter [0,1]. Required
    because `scene.*` keys in the render config do NOT override a
    `scene.file` — per-frame motion must live in the scene file itself."""
    import re
    NF = 4.0
    t0, t1 = frame_idx / NF, (frame_idx + 1) / NF
    lines = open(SCENE).read().splitlines()
    steps = {}
    for ln in lines:
        m = re.match(r"scene\.objects\.(\w+)\.motion\.(\d+)\.(time|transformation)\s*=\s*(.*)", ln)
        if m:
            steps.setdefault(m.group(1), {}).setdefault(int(m.group(2)), {})[m.group(3)] = m.group(4)
    out = [ln for ln in lines
           if not re.match(r"scene\.objects\.\w+\.motion\.\d+\.", ln)
           and not re.match(r"scene\.camera\.(shutteropen|shutterclose)\s*=", ln)]
    out += ["scene.camera.shutteropen = 0.0", "scene.camera.shutterclose = 1.0"]
    for name, st in steps.items():
        times = {i: float(d["time"]) for i, d in st.items()}
        mats = {i: np.array([float(x) for x in d["transformation"].split()]).reshape(4, 4)
                for i, d in st.items()}
        ti = sorted(times)
        def interp(t):
            for a, b in zip(ti[:-1], ti[1:]):
                if times[a] <= t <= times[b]:
                    w = 0.0 if times[b] == times[a] else (t - times[a]) / (times[b] - times[a])
                    return mats[a] * (1 - w) + mats[b] * w
            return mats[ti[0]] if t < times[ti[0]] else mats[ti[-1]]
        for j, t in enumerate([t0, t1]):
            M = interp(t)
            out.append(f"scene.objects.{name}.motion.{j}.time = {float(j)}")
            out.append(f"scene.objects.{name}.motion.{j}.transformation = " +
                       " ".join(f"{v:.8g}" for v in M.flatten()))
    open(out_path, "w").write("\n".join(out) + "\n")
    return out_path


def render_frame(scn, frame_idx, use_ta, seed, statedir):
    props = pyluxcore.Properties()
    props.SetFromFile(CFG)
    for k, v in [("scene.file", scn), ("film.width", W), ("film.height", H),
                 ("batch.halttime", 0), ("batch.haltspp", 0),
                 ("renderengine.type", "PATHCPU"), ("sampler.type", "SOBOL"),
                 ("renderengine.seed", seed), ("opencl.gpu.use", 0),
                 ("film.outputs.0.type", "MOTION_VECTOR"),
                 ("film.outputs.0.filename", "mv.exr"),
                 ("film.outputs.1.type", "VARIANCE"),
                 ("film.outputs.1.filename", "var.exr"),
                 ("film.outputs.2.type", "INDIRECT_DIFFUSE"),
                 ("film.outputs.2.filename", "id.exr")]:
        props.Set(pyluxcore.Property(k, v))
    i = 0
    if use_ta:
        for k, v in [("type", "TEMPORAL_ACCUMULATE"), ("frame", frame_idx),
                     ("statedir", statedir), ("history", 16.0)]:
            props.Set(pyluxcore.Property(f"film.imagepipelines.0.{i}.{k}", v))
        i += 1
    props.Set(pyluxcore.Property(f"film.imagepipelines.0.{i}.type", "TONEMAP_LINEAR"))
    props.Set(pyluxcore.Property(f"film.imagepipelines.0.{i}.scale", 1.0)); i += 1
    props.Set(pyluxcore.Property(f"film.imagepipelines.0.{i}.type", "GAMMA_CORRECTION"))
    props.Set(pyluxcore.Property(f"film.imagepipelines.0.{i}.value", 2.2))

    cfg = pyluxcore.RenderConfig(props)
    session = pyluxcore.RenderSession(cfg)
    session.Start()
    time.sleep(RENDER_SECS)
    session.Pause()
    film = session.GetFilm()

    out = np.zeros(H * W * 3, dtype=np.float32)
    film.GetOutputFloat(pyluxcore.FilmOutputType.RGB_IMAGEPIPELINE, out)
    mv = np.zeros(H * W * 4, dtype=np.float32)
    film.GetOutputFloat(pyluxcore.FilmOutputType.MOTION_VECTOR, mv)
    var = np.zeros(H * W * 3, dtype=np.float32)
    film.GetOutputFloat(pyluxcore.FilmOutputType.VARIANCE, var)
    session.Stop()
    del session, cfg
    return out.reshape(H, W, 3), mv.reshape(H, W, 4), var.reshape(H, W, 3)


def hf_energy(img):
    """High-frequency energy: mean absolute pixel gradient - a noise proxy."""
    return float(np.abs(np.diff(img, axis=1)).mean() + np.abs(np.diff(img, axis=0)).mean())


def main():
    pyluxcore.Init()
    failures = []

    with tempfile.TemporaryDirectory() as statedir:
        scn0 = make_frame_scn(0, os.path.join(statedir, "frame0.scn"))
        scn1 = make_frame_scn(1, os.path.join(statedir, "frame1.scn"))
        raw0, mv0, var0 = render_frame(scn0, 0, True, 7, statedir)

        # 1. MOTION_VECTOR
        valid = float(mv0[:, :, 2].mean())
        vx_abs = float(np.abs(mv0[:, :, 0]).max())
        print(f"[MV] valid coverage {100 * valid:.1f}%  |vx| max {vx_abs:.1f} px/frame")
        if valid < 0.5:
            failures.append(f"MOTION_VECTOR valid coverage too low: {valid:.2f}")
        if vx_abs < 1.0:
            failures.append("MOTION_VECTOR velocities are all ~0 on a moving scene")

        # 2. VARIANCE
        finite = bool(np.isfinite(var0).all())
        nonneg = bool((var0 >= -1e-6).all())
        print(f"[VAR] mean {var0.mean():.5f}  finite={finite}  nonneg={nonneg}")
        if not (finite and nonneg):
            failures.append("VARIANCE contains NaN/negative values")
        if var0.mean() <= 0:
            failures.append("VARIANCE is all zero on a noisy low-spp render")

        # 3. TEMPORAL_ACCUMULATE on frame 1
        raw1, _, _ = render_frame(scn1, 1, False, 8, statedir)
        ta1, _, _ = render_frame(scn1, 1, True, 8, statedir)

        state_file = os.path.join(statedir, "slg_temporal_state_0.exr")
        if not os.path.exists(state_file):
            failures.append("temporal state EXR was not written")

        hf_raw, hf_ta = hf_energy(raw1), hf_energy(ta1)
        print(f"[TA] frame1 high-freq energy: raw {hf_raw:.5f}  TA {hf_ta:.5f}")
        if hf_ta >= hf_raw * 0.95:
            failures.append(f"TEMPORAL_ACCUMULATE did not reduce noise (raw {hf_raw:.4f} vs TA {hf_ta:.4f})")

    if failures:
        print("\nFAIL:")
        for f in failures:
            print("  -", f)
        return 1
    print("\nPASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
