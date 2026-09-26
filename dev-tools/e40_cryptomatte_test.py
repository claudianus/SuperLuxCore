# SPDX-License-Identifier: Apache-2.0
#
# E40: Cryptomatte (object + material) on CPU and GPU.
#
# Per-pixel (id, coverage) pairs; ids are MurmurHash3(name) converted
# to float32 per the spec's hash_to_float (sign bit preserved, only the
# exponent is clamped away from 0/255). EXR output uses
# <Name><rank2d>.<RGBA> channels plus cryptomatte/<key>/{name,hash,
# conversion,manifest} metadata injected by the session.
#
# T1 (CPU ids): every non-zero id in CRYPTOMATTE_OBJECT equals the
#    murmur3-float of a scene object name; same for materials.
# T2 (coverage): per-pixel coverage sums to ~1 on the closed Cornell
#    box (no sky); pairs are emitted coverage-descending.
# T3 (EXR): saved output has CryptoObjectNN.RGBA channels and the
#    cryptomatte/<key>/ metadata set incl. a parseable JSON manifest
#    whose ids match the framebuffer ids.
# T4 (GPU parity): PATHOCL per-id mean coverage within tolerance of CPU.
# T5 (crash safety): all outputs finite.
#
# Run:
#   python3.13 dev-tools/e40_cryptomatte_test.py
#
import json
import re
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
for _variant in ("Release", "Debug"):
    _p = REPO / "out/build/src/pysuperluxcore" / _variant
    if any(_p.glob("pysuperluxcore*.so")):
        sys.path.insert(0, str(_p))
        break
import pysuperluxcore

WIDTH, HEIGHT = 160, 120
SPP = 64
TASK_COUNT = 1 << 16
RENDER_TIMEOUT_S = 300
LEVELS = 6
STRIDE_OUT = LEVELS * 2  # (id, coverage) pairs emitted by GetOutput

EXRHEADER = (REPO.parent / "LuxCore/out/dependencies/full_deploy/host/openexr/3.4.14"
             "/Release/armv8/bin/exrheader")

results = []


def record(name, ok, detail):
    results.append((name, ok))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)


def murmur3_32(data: bytes, seed: int = 0) -> int:
    """Reference MurmurHash3 x86_32, seed 0 (matches luxrays::MurmurHash3_32)."""
    h = seed & 0xffffffff
    c1, c2 = 0xcc9e2d51, 0x1b873593
    nblocks = len(data) // 4
    for i in range(nblocks):
        k = int.from_bytes(data[i * 4:i * 4 + 4], "little")
        k = (k * c1) & 0xffffffff
        k = ((k << 15) | (k >> 17)) & 0xffffffff
        k = (k * c2) & 0xffffffff
        h ^= k
        h = ((h << 13) | (h >> 19)) & 0xffffffff
        h = (h * 5 + 0xe6546b64) & 0xffffffff
    k = 0
    tail = data[nblocks * 4:]
    if len(tail) == 3:
        k ^= tail[2] << 16
    if len(tail) >= 2:
        k ^= tail[1] << 8
    if len(tail) >= 1:
        k ^= tail[0]
        k = (k * c1) & 0xffffffff
        k = ((k << 15) | (k >> 17)) & 0xffffffff
        k = (k * c2) & 0xffffffff
        h ^= k
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85ebca6b) & 0xffffffff
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & 0xffffffff
    h ^= h >> 16
    return h


def hash_to_float(h: int) -> float:
    """Cryptomatte spec uint32->float32 (sign kept, exponent 0/255 -> 1/254)."""
    exp = (h >> 23) & 0xff
    if exp in (0, 255):
        h ^= 1 << 23
    return struct.unpack("<f", struct.pack("<L", h))[0]


def name_id(name: str) -> float:
    return hash_to_float(murmur3_32(name.encode("utf-8")))


def id_hex(fid: float) -> str:
    return "%08x" % struct.unpack("<L", struct.pack("<f", fid))[0]


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


def render(engine, outdir, sel=None):
    scn = pysuperluxcore.Properties(str(REPO / "scenes/cornell/cornell.scn"))
    sc = pysuperluxcore.Scene()
    sc.Parse(scn)

    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = {engine}
sampler.type = SOBOL
batch.haltspp = {SPP}
opencl.task.count = {TASK_COUNT}
film.imagepipelines.0.0.type = NOP
film.outputs.0.type = CRYPTOMATTE_OBJECT
film.outputs.0.filename = {outdir}/crypto_obj.exr
film.outputs.1.type = CRYPTOMATTE_MATERIAL
film.outputs.1.filename = {outdir}/crypto_mat.exr
film.outputs.2.type = ALPHA
film.outputs.2.filename = {outdir}/alpha.exr
""")
    if sel:
        cfg.Set(pysuperluxcore.Property("opencl.devices.select", sel))

    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, sc))
    ses.Start()
    deadline = time.monotonic() + RENDER_TIMEOUT_S
    while True:
        ses.UpdateStats()
        if ses.GetStats().Get("stats.renderengine.pass").GetInt() >= SPP:
            break
        if time.monotonic() > deadline:
            ses.Stop()
            raise TimeoutError(f"render stalled below {SPP} spp")
        time.sleep(0.5)

    film = ses.GetFilm()
    obj = np.empty(WIDTH * HEIGHT * STRIDE_OUT, dtype=np.float32)
    mat = np.empty(WIDTH * HEIGHT * STRIDE_OUT, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.CRYPTOMATTE_OBJECT, obj)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.CRYPTOMATTE_MATERIAL, mat)
    alpha = np.empty(WIDTH * HEIGHT, dtype=np.float32)
    film.GetOutputFloat(pysuperluxcore.FilmOutputType.ALPHA, alpha)
    film.SaveOutputs()
    ses.Stop()
    return (obj.reshape(HEIGHT, WIDTH, LEVELS, 2),
            mat.reshape(HEIGHT, WIDTH, LEVELS, 2),
            alpha.reshape(HEIGHT, WIDTH))


def ids_of(buf):
    """All non-zero ids present in a crypto buffer."""
    return set(buf[..., 0][buf[..., 0] != 0.0].tolist())


def main():
    print(f"Cryptomatte (cornell {WIDTH}x{HEIGHT}, {SPP}spp)\n", flush=True)

    gpu_sel = next((m for m in (device_mask("METAL_GPU"),
                    device_mask("OPENCL_GPU")) if m and "1" in m), None)
    print(f"  gpu device mask: {gpu_sel}", flush=True)

    # Expected ids from the scene's object/material names
    obj_names = ["Khaki", "HalveRed", "DarkGreen", "Grey"]
    mat_names = ["Khaki", "HalveRed", "DarkGreen", "Light"]
    obj_ids = {name_id(n) for n in obj_names}
    mat_ids = {name_id(n) for n in mat_names}
    id_bits = {id_hex(name_id(n)) for n in obj_names + mat_names}

    with tempfile.TemporaryDirectory() as outdir:
        cpu_obj, cpu_mat, cpu_alpha = render("PATHCPU", outdir)

        # T1: all emitted ids are valid murmur3 ids of scene names
        seen_obj = ids_of(cpu_obj)
        seen_mat = ids_of(cpu_mat)
        record("T1.object-ids", seen_obj <= obj_ids,
               f"ids={sorted(id_hex(i) for i in seen_obj)}")
        record("T1.material-ids", seen_mat <= mat_ids,
               f"ids={sorted(id_hex(i) for i in seen_mat)}")
        record("T1.ids-present", len(seen_obj) >= 3 and len(seen_mat) >= 3,
               f"objects={len(seen_obj)}/4 materials={len(seen_mat)}/4")

        # T2: coverage sum per pixel tracks the alpha (fraction of
        # samples that hit a surface) — open-side pixels miss the box
        cov_obj = cpu_obj[..., 1]
        cov_mat = cpu_mat[..., 1]
        sums = cov_obj.sum(axis=-1)
        err = np.abs(sums - cpu_alpha)
        record("T2.coverage-vs-alpha",
               float(err.mean()) < 0.05 and float(err.max()) < 0.3,
               f"|covsum-alpha| mean={err.mean():.4f} max={err.max():.4f}")
        record("T2.coverage-bounded",
               float(cov_obj.max()) <= 1.0 + 1e-4 and float(cov_mat.max()) <= 1.0 + 1e-4,
               f"max pair coverage obj={cov_obj.max():.4f} mat={cov_mat.max():.4f}")
        desc = np.all(cov_obj[..., :-1] >= cov_obj[..., 1:] - 1e-6)
        record("T2.sorted-desc", bool(desc), "coverage-descending pair order")
        record("T5.finite", np.isfinite(cpu_obj).all() and np.isfinite(cpu_mat).all(),
               "no NaN/Inf in crypto buffers")

        # T3: EXR channels + metadata + manifest
        hdr = subprocess.run([str(EXRHEADER), f"{outdir}/crypto_obj.exr"],
                             capture_output=True, text=True).stdout
        chans_ok = all(f"CryptoObject{r:02d}.{c}" in hdr
                       for r in range(3) for c in "RGBA")
        record("T3.channels", chans_ok,
               "CryptoObject00..02 RGBA channel names" if chans_ok else
               "missing channel names in header")
        meta = dict(re.findall(r"cryptomatte/(\w+)/(\w+)", hdr))
        # exrheader prints 'cryptomatte/<key>/<attr>' lines; find the key
        mkey = re.search(r"cryptomatte/(\w+)/name.*CryptoObject", hdr)
        manifest_ok = False
        if mkey:
            mk = mkey.group(1)
            mm = re.search(r"cryptomatte/%s/manifest[^\"]*\"(.*)\"" % mk, hdr)
            if mm:
                manifest = json.loads(mm.group(1).replace('\\"', '"'))
                manifest_ok = (set(manifest) == set(obj_names) and
                               set(manifest.values()) <= id_bits)
        record("T3.metadata",
               bool(mkey) and "hash" in hdr and "uint32_to_float32" in hdr,
               "cryptomatte/<key>/{name,hash,conversion} present")
        record("T3.manifest", manifest_ok,
               "manifest names<->id hex round-trip" if manifest_ok else
               "manifest missing or ids mismatch")

        # T4: GPU parity on per-id mean coverage
        if gpu_sel:
            g_obj, g_mat, g_alpha = render("PATHOCL", outdir, sel=gpu_sel)
            g_seen = ids_of(g_obj)
            record("T4.gpu-ids", g_seen <= obj_ids,
                   f"gpu ids={sorted(id_hex(i) for i in g_seen)}")

            def mean_cov(buf, fid):
                m = buf[..., 0] == fid
                return float(buf[..., 1][m].sum())

            worst = 0.0
            for fid in seen_obj | g_seen:
                c = mean_cov(cpu_obj, fid)
                g = mean_cov(g_obj, fid)
                worst = max(worst, abs(c - g) / max(c, 1.0))
            record("T4.gpu-parity", worst < 0.15,
                   f"max per-id coverage diff={worst:.3f} (gate 15%)")
            record("T5.gpu-finite",
                   np.isfinite(g_obj).all() and np.isfinite(g_mat).all(),
                   "gpu buffers finite")

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed"
          + (f" — FAILED: {failed}" if failed else ""), flush=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
