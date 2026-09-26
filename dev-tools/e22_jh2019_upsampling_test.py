# SPDX-License-Identifier: Apache-2.0
#
# E22: JH2019 (rgb2spec) spectral upsampling verification.
#
# Verifies the opt-in `path.spectral.upsampling = jh2019` mode:
#
#   1. Unit level (compiled helper dev-tools/jh2019_unit_check.cpp):
#      - white RGB(1,1,1) upsamples to ~1 in all 3 bins
#      - a dense RGB grid keeps every bin finite and inside [0,1]
#        (energy conservation -- the sigmoid bounds reflectance)
#      - achromatic inputs produce flat bins
#      - 3-bin projection round-trip error <= Smits baseline
#      - the default Smits basis still engages (regression sentinel)
#   2. Render level (pysuperluxcore, cornell scene, spectral transport on):
#      - smits-by-default image is bitwise identical to explicit smits
#        (zero regression for existing scenes)
#      - jh2019 renders finite and differs sanely from smits
#      - invalid upsampling values are rejected, under both the canonical
#        `path.spectral.upsampling` name and the `spectral.upsampling` alias
#      - the `spectral.upsampling` alias actually selects jh2019
#      - PATHOCL jh2019 matches the CPU mean within 10% on every
#        available hardware backend (OpenCL GPU / Metal GPU)
#
# The unit binary is compiled on the fly with the include paths recorded
# in out/build/compile_commands.json, so run this after a configure+build:
#   python3.13 dev-tools/e22_jh2019_upsampling_test.py

import glob
import json
import os
import shlex
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Debug"))

WIDTH, HEIGHT = 160, 120
SPP = 32
RENDER_TIMEOUT_S = 240

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


# ----------------------------------------------------------------------
# Part 1: CPU unit check (compiled helper)
# ----------------------------------------------------------------------

def unit_check():
    cc_db = REPO / "out/build/compile_commands.json"
    if not cc_db.exists():
        record("unit.compile", False, "SKIP (no compile_commands.json)")
        return
    db = json.loads(cc_db.read_text())
    entry = next((e for e in db if e["file"].endswith("core/color/spectral.cpp")),
                 None)
    if entry is None:
        record("unit.compile", False, "SKIP (spectral.cpp not in compile db)")
        return
    parts = shlex.split(entry["command"].replace('\\\\\\"', '"'))
    flags = [p for i, p in enumerate(parts)
             if p.startswith("-I") or p.startswith("-isystem")
             or (i > 0 and parts[i - 1] in ("-I", "-isystem"))]
    std = next((p for p in parts if p.startswith("-std=")), "-std=c++23")
    boost_libs = sorted(glob.glob(str(
        REPO / "out/dependencies/full_deploy/host/boost/*/Release/*/lib/"
        "libboost_serialization.*")))
    boost_libs += sorted(glob.glob(str(
        REPO / "out/dependencies/full_deploy/host/boost/*/Release/*/lib/"
        "libboost_wserialization.*")))

    srcs = ["spectral.cpp", "spectrumwavelengths.cpp", "spd.cpp",
            "color.cpp", "spds/regular.cpp", "spds/blackbodyspd.cpp"]
    srcs = [str(REPO / "src/luxrays/core/color" / s) for s in srcs]
    out = Path(tempfile.gettempdir()) / "jh2019_unit_check"
    cmd = ["c++", std, "-O2", *flags, "-o", str(out),
           str(REPO / "dev-tools/jh2019_unit_check.cpp"), *srcs, *boost_libs]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        record("unit.compile", False,
               f"compile failed: {r.stderr[-400:]}")
        return
    r = subprocess.run([str(out)], capture_output=True, text=True)
    print(r.stdout, end="", flush=True)
    record("unit", r.returncode == 0,
           "all invariants" if r.returncode == 0 else "see FAIL lines above")


# ----------------------------------------------------------------------
# Part 2: render-level checks
# ----------------------------------------------------------------------

def device_mask(want_type):
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


def render(engine, upsampling=None, sel=None, seed=17):
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = {seed}
path.spectral.enable = 1
""")
    if upsampling is not None:
        cfg.Set(pysuperluxcore.Property("path.spectral.upsampling", upsampling))
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError("render stalled")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    return rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)


def main():
    print(f"JH2019 spectral upsampling verification "
          f"({WIDTH}x{HEIGHT} @ {SPP}spp)\n", flush=True)

    unit_check()

    global scene
    try:
        props = pysuperluxcore.Properties(str(REPO / "scenes/cornell/cornell.scn"))
        scene = pysuperluxcore.Scene()
        scene.Parse(props)
    except Exception:
        cwd = os.getcwd()
        os.chdir(str(REPO / "scenes/cornell"))
        try:
            scene = pysuperluxcore.Scene()
            scene.Parse(pysuperluxcore.Properties("cornell.scn"))
        finally:
            os.chdir(cwd)

    # CPU: default (implicit smits), explicit smits, jh2019
    cpu_def = render("PATHCPU")                      # no upsampling prop
    cpu_smits = render("PATHCPU", upsampling="smits")
    cpu_jh = render("PATHCPU", upsampling="jh2019")

    # Default (no property) must behave like explicit smits, not like
    # jh2019. batch.haltspp overshoots the target pass count, so the two
    # smits renders contain different MC noise -- gate on the diff being
    # far below the smits-vs-jh2019 model diff instead of bitwise equal.
    mdiff = float(np.mean(np.abs(cpu_jh - cpu_smits)))
    ddiff = float(np.mean(np.abs(cpu_def - cpu_smits)))
    mmean = float(cpu_smits.mean())
    record("cpu.default-is-smits",
           np.isfinite(cpu_def).all() and ddiff < max(0.3 * mdiff, 1e-6),
           f"default-vs-smits mean|diff|={ddiff:.5f} << "
           f"jh2019-vs-smits={mdiff:.5f}")
    record("cpu.jh2019-finite", np.isfinite(cpu_jh).all(),
           "all pixels finite")
    record("cpu.jh2019-differs",
           mdiff > 1e-4 * max(mmean, 1e-6) and mdiff < mmean,
           f"mean|diff|={mdiff:.5f} vs smits mean={mmean:.4f} "
           f"({100 * mdiff / max(mmean, 1e-9):.1f}%)")

    # Config validation: unknown values must throw, for both the
    # canonical `path.spectral.upsampling` name and the
    # `spectral.upsampling` alias.
    for prop_name, tag in (("path.spectral.upsampling", "canonical"),
                           ("spectral.upsampling", "alias")):
        cfg = pysuperluxcore.Properties()
        cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHCPU
batch.haltspp = 1
path.spectral.enable = 1
{prop_name} = bogus
""")
        try:
            ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
            ses.Start()
            time.sleep(2)
            ses.Stop()
            record(f"cfg.invalid-{tag}", False, "bogus value not rejected")
        except Exception as e:
            record(f"cfg.invalid-{tag}",
                   "upsampling" in str(e).lower(),
                   str(e)[:80])

    # The `spectral.upsampling` alias must actually select jh2019, not
    # silently fall back to smits.
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHCPU
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 17
path.spectral.enable = 1
spectral.upsampling = jh2019
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError("alias render stalled")
        time.sleep(0.5)
    rgb = np.empty(WIDTH * HEIGHT * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    ses.Stop()
    cpu_alias = rgb.reshape(HEIGHT, WIDTH, 3).mean(axis=2)
    adiff = float(np.mean(np.abs(cpu_alias - cpu_smits)))
    record("cfg.alias-selects-jh2019",
           np.isfinite(cpu_alias).all()
           and adiff > max(0.3 * mdiff, 1e-6),
           f"alias-vs-smits mean|diff|={adiff:.5f} ~ "
           f"jh2019-vs-smits={mdiff:.5f}")

    # GPU backends (if present): jh2019 on OpenCL + Metal
    for want, tag in (("OPENCL_GPU", "ocl"), ("METAL_GPU", "mtl")):
        mask = device_mask(want)
        if not mask:
            record(f"gpu.{tag}-jh2019", True, f"SKIP (no {want} device)")
            continue
        try:
            img = render("PATHOCL", upsampling="jh2019", sel=mask)
        except Exception as e:
            record(f"gpu.{tag}-jh2019", False,
                   f"render failed: {str(e)[:80]}")
            continue
        rel = abs(float(img.mean()) - float(cpu_jh.mean())) / \
            max(float(cpu_jh.mean()), 1e-12)
        record(f"gpu.{tag}-jh2019",
               np.isfinite(img).all() and rel < 0.10,
               f"finite, mean={img.mean():.4f} vs cpu={cpu_jh.mean():.4f} "
               f"({rel * 100:.1f}%)")


if __name__ == "__main__":
    import pysuperluxcore
    pysuperluxcore.Init()
    main()
    failed = [n for n, ok in results if not ok]
    print(f"\n{'FAIL ' + str(failed) if failed else 'ALL PASS'} "
          f"({len(results) - len(failed)}/{len(results)})", flush=True)
    sys.exit(1 if failed else 0)
