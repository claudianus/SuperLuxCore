# SPDX-License-Identifier: Apache-2.0
#
# E117: the CPU viewport engine (RTPATHCPU) builds the caustic cache in
# the background. Scene: a matte floor under three glass slabs (the
# E116 multi-interface case). The viewport polls the cache from the
# thread that calls UpdateStats(): the first generation is traced while
# the viewport already renders, camera edits keep the live generation,
# other edits rebuild it.
#
#   1  caustic.auto enables the cache for RTPATHCPU
#   2  the first generation lands (and resets the frame) while rendering
#   3  steady floor matches the PATHOCL reference (no cache)
#   4  camera edits keep the cache (no rebuild) and stay responsive
#   5  a material edit rebuilds the cache and still converges
#   6  edit latency with the cache stays within 2x of cache-off
#   7  stop with a background generation in flight returns
#
# Run from the repo root:
#   python3.13 dev-tools/e117_viewport_caustic_cache_test.py

import os
import sys
import tempfile
import threading
import time
from pathlib import Path

import numpy as np

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

W, H = 96, 96
OCL_DEV = os.environ.get("E117_OCL_DEV", "")
LOG = []
# Lines logged without the GIL (render/update threads, Start/Stop) go
# to stderr instead of the Python handler: collect fd 2 in a file
ERR_PATH = tempfile.mkstemp(prefix="e117-", suffix=".log")[1]


def box(name, x0, x1, y0, y1, z0, z1, mat):
    v = [(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
         (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)]
    f = [(0, 2, 1), (0, 3, 2), (4, 5, 6), (4, 6, 7), (0, 1, 5), (0, 5, 4),
         (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6), (3, 0, 4), (3, 4, 7)]
    return (f"scene.objects.{name}.material = {mat}\n"
            f"scene.objects.{name}.vertices = " +
            " ".join("%g %g %g" % p for p in v) + "\n" +
            f"scene.objects.{name}.faces = " +
            " ".join("%d %d %d" % t for t in f) + "\n")


def camera(x):
    return (f"scene.camera.lookat.orig = {x:g} 0 2.6\n"
            f"scene.camera.lookat.target = {x:g} 0 0\n"
            "scene.camera.up = 0 1 0\n"
            "scene.camera.fieldofview = 28\n")


SCENE = camera(0) + """
scene.materials.floor.type = matte
scene.materials.floor.kd = 0.7 0.7 0.7
scene.materials.glass.type = glass
scene.materials.glass.kr = 1 1 1
scene.materials.glass.kt = 1 1 1
scene.materials.glass.interiorior = 1.5
scene.materials.light.type = matte
scene.materials.light.kd = 0 0 0
scene.materials.light.emission = 20 20 20
scene.objects.lamp.material = light
scene.objects.lamp.vertices = -0.7 -0.7 3.2  0.7 -0.7 3.2  0.7 0.7 3.2  -0.7 0.7 3.2
scene.objects.lamp.faces = 0 2 1  0 3 2
scene.objects.ground.material = floor
scene.objects.ground.vertices = -3 -3 0  3 -3 0  3 3 0  -3 3 0
scene.objects.ground.faces = 0 1 2  0 2 3
""" + "".join(box(f"slab{i}", -0.9, 0.9, -0.9, 0.9, 0.3 + 0.25 * i,
                  0.35 + 0.25 * i, "glass") for i in range(3))

DEPTH = """
path.pathdepth.total = 32
path.pathdepth.diffuse = 8
path.pathdepth.glossy = 32
path.pathdepth.specular = 32
"""
REFERENCE = DEPTH + """
path.photongi.caustic.enabled = 0
path.lighttracing.auto = 0
path.mnee.auto = 0
path.regularization.auto = 0
"""
FLOOR = np.s_[30:66, 30:66]


def config(extra, engine, seconds=0):
    cfg = pysuperluxcore.Properties()
    dev = f'opencl.devices.select = "{OCL_DEV}"\n' if OCL_DEV else ""
    sampler = "sampler.type = RTPATHCPUSAMPLER\n" if engine == "RTPATHCPU" else ""
    halt = f"batch.halttime = {seconds}\n" if seconds else ""
    cfg.SetFromString(f"""
film.width = {W}
film.height = {H}
film.imagepipelines.0.0.type = NOP
renderengine.type = {engine}
renderengine.seed = 7
opencl.native.threads.count = 0
{sampler}{halt}{dev}{extra}
""")
    return cfg


def new_scene():
    scn = pysuperluxcore.Scene()
    p = pysuperluxcore.Properties()
    p.SetFromString(SCENE)
    scn.Parse(p)
    return scn


def floor(ses):
    rgb = np.empty(W * H * 3, dtype=np.float32)
    ses.GetFilm().GetOutputFloat(pysuperluxcore.FilmOutputType.RGB_IMAGEPIPELINE,
                                 rgb, 0, True)
    return float(rgb.reshape(H, W, 3).mean(axis=2)[::-1][FLOOR].mean())


def reference():
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(
        config(REFERENCE, "PATHOCL", 90), new_scene()))
    ses.Start()
    while not ses.HasDone():
        time.sleep(0.5)
        ses.UpdateStats()
    ses.Stop()
    return floor(ses)


def count(msg):
    with open(ERR_PATH, errors="replace") as f:
        err = f.read()
    return err.count(msg) + sum(msg in line for line in LOG)


def pump(ses, seconds, step=0.05):
    # The viewport's draw loop: UpdateStats() is what polls the cache
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        ses.UpdateStats()
        time.sleep(step)


def edit(ses, scn, props):
    # One viewport edit, timed up to the first new frame
    t0 = time.monotonic()
    ses.BeginSceneEdit()
    p = pysuperluxcore.Properties()
    p.SetFromString(props)
    scn.Parse(p)
    ses.EndSceneEdit()
    ses.WaitNewFrame()
    return time.monotonic() - t0


def orbit(ses, scn, n=20):
    lat = []
    for i in range(n):
        lat.append(edit(ses, scn, camera(0.02 * ((i % 2) * 2 - 1))))
        pump(ses, 0.05, 0.01)
    return float(np.median(lat))


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    fd = os.open(ERR_PATH, os.O_WRONLY | os.O_APPEND)
    os.dup2(fd, 2)
    pysuperluxcore.Init(lambda s: LOG.append(s))
    ok = True

    ref = reference()

    rc = pysuperluxcore.RenderConfig(config(DEPTH, "RTPATHCPU"), new_scene())
    LOG.clear()
    enabled = rc.GetProperties().Get("path.photongi.caustic.enabled").GetBool() \
        if rc.GetProperties().IsDefined("path.photongi.caustic.enabled") else None
    ok &= check(enabled is True, "1 caustic.auto on RTPATHCPU",
                f"caustic.enabled={enabled}")

    ses = pysuperluxcore.RenderSession(rc)
    scn = ses.GetRenderConfig().GetScene()
    t0 = time.monotonic()
    ses.Start()
    ses.WaitNewFrame()
    first_frame = time.monotonic() - t0
    deadline = time.monotonic() + 60
    while count("New PhotonGI caustic cache lookup radius") == 0 and \
            time.monotonic() < deadline:
        pump(ses, 0.1)
    landed = time.monotonic() - t0
    ok &= check(count("New PhotonGI caustic cache lookup radius") > 0,
                "2 first generation lands while rendering",
                f"first frame {first_frame * 1000:.0f} ms, generation at {landed:.2f} s")

    pump(ses, 30)
    got = floor(ses)
    ok &= check(abs(got / ref - 1) < 0.05, "3 steady floor vs reference",
                f"ratio={got / ref:.3f} (ref {ref:.4f})")

    builds = count("PhotonGI deferring initial caustic generation")
    lat_cache = orbit(ses, scn)
    pump(ses, 3)
    after = floor(ses)
    ok &= check(count("PhotonGI deferring initial caustic generation") == builds and
                abs(after / ref - 1) < 0.15,
                "4 camera edits keep the cache",
                f"rebuilds={count('PhotonGI deferring initial caustic generation') - builds}, "
                f"floor 3 s after orbit={after / ref:.3f}, median edit {lat_cache * 1000:.1f} ms")

    # back to the reference camera, then a material edit. Halving the
    # lamp scales every term (the floor region is mostly the lamp seen
    # in the slab reflections, kd would barely move it)
    edit(ses, scn, camera(0))
    t5 = edit(ses, scn, "scene.materials.light.type = matte\n"
                        "scene.materials.light.kd = 0 0 0\n"
                        "scene.materials.light.emission = 10 10 10\n")
    pump(ses, 30)
    got = floor(ses)
    exp = ref * 0.5
    ok &= check(count("PhotonGI deferring initial caustic generation") == builds + 1 and
                abs(got / exp - 1) < 0.05,
                "5 material edit rebuilds the cache",
                f"rebuilds={count('PhotonGI deferring initial caustic generation') - builds}, "
                f"ratio={got / exp:.3f} (emission halved), edit {t5 * 1000:.0f} ms")

    # 7 stop while a generation is in flight: a geometry-free light edit
    # rebuilds the cache, stop right away
    edit(ses, scn, "scene.materials.light.type = matte\n"
                   "scene.materials.light.kd = 0 0 0\n"
                   "scene.materials.light.emission = 11 11 11\n")
    stopped = threading.Event()

    def stopper():
        ses.Stop()
        stopped.set()
    th = threading.Thread(target=stopper, daemon=True)
    t1 = time.monotonic()
    th.start()
    th.join(30)
    ok &= check(stopped.is_set(), "7 stop with a generation in flight",
                f"{time.monotonic() - t1:.2f} s")
    if not stopped.is_set():
        os._exit(1)

    # 6 edit latency, cache off
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(
        config(DEPTH + "path.photongi.caustic.enabled = 0\n", "RTPATHCPU"),
        new_scene()))
    scn = ses.GetRenderConfig().GetScene()
    ses.Start()
    ses.WaitNewFrame()
    pump(ses, 3)
    lat_off = orbit(ses, scn)
    ses.Stop()
    ok &= check(lat_cache < max(2 * lat_off, lat_off + 0.005),
                "6 camera edit latency",
                f"cache {lat_cache * 1000:.1f} ms vs off {lat_off * 1000:.1f} ms")

    os.unlink(ERR_PATH)
    print("PASS overall" if ok else "FAIL overall")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
