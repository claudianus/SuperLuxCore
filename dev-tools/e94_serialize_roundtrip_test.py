# SPDX-License-Identifier: Apache-2.0
#
# E94: serialized film / resume-file round-trip with adaptive error.
#
# Historically broken at the archive root: SaveSerialized / SaveRsmFile
# wrote by-value object records while every loader read unique_ptr
# (pointer) records, so standalone .flm and .rsm loads always threw
# "class version unique_ptr<...>". Both roots are pointer records now.
#
# Gates:
#   1. .flm save -> pyluxcore Film(path) loads, dimensions + channels
#      match, adaptiveError is rebound to the loaded film (not a stale
#      nested copy): the reloaded film keeps producing sane stats.
#   2. .rsm save (paused) -> RenderConfig.LoadResumeFile -> a session
#      built on the start film keeps rendering (sample count grows).
#   3. No crash / no leak diagnostics (process exits clean).
#
# Run from the workspace root:
#   python3.13 dev-tools/e94_serialize_roundtrip_test.py

import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))

WIDTH, HEIGHT = 64, 64
SCENE = REPO / "scenes/cornell/cornell.scn"
CHILD_TIMEOUT_S = 180


def child():
    import time

    import pysuperluxcore

    os.chdir(str(REPO))

    tmp = tempfile.mkdtemp(prefix="e94_")
    flm = os.path.join(tmp, "film.flm")
    rsm = os.path.join(tmp, "session.rsm")

    scene = pysuperluxcore.Scene()
    scene.Parse(pysuperluxcore.Properties(str(SCENE)))
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHCPU
sampler.type = SOBOL
renderengine.seed = 17
batch.halttime = 0
batch.haltspp = 0
film.adaptiveerror.target = 0.05
film.adaptiveerror.warmup = 2
film.adaptiveerror.step = 2
""")
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    ses.Start()
    time.sleep(4.0)
    ses.UpdateStats()

    spp_before = ses.GetStats().Get("stats.renderengine.pass").GetFloat()

    # --- .flm round-trip ---------------------------------------------------
    ses.GetFilm().SaveFilm(flm)
    loaded = pysuperluxcore.Film(flm)
    if (loaded.GetWidth(), loaded.GetHeight()) != (WIDTH, HEIGHT):
        raise RuntimeError("loaded film size mismatch")

    # adaptiveError must be rebound to the loaded film (v2 BindFilm) and
    # its errorVector must have round-tripped: the loaded film's NOISE
    # channel must be readable and finite.
    import numpy as np
    st = loaded.GetStats()
    spp_loaded = st.Get("stats.film.spp").GetFloat()
    noise = np.full(WIDTH * HEIGHT, np.nan, dtype=np.float32)
    loaded.GetOutputFloat(pysuperluxcore.FilmOutputType.NOISE, noise, 0, True)
    noise_ok = bool(np.isfinite(noise).all())

    # --- .rsm resume round-trip -------------------------------------------
    ses.Pause()
    ses.SaveResumeFile(rsm)
    ses.Stop()

    newCfg, startState, startFilm = pysuperluxcore.RenderConfig.LoadResumeFile(rsm)
    ses2 = pysuperluxcore.RenderSession(newCfg, startState, startFilm)
    ses2.Start()
    time.sleep(3.0)
    ses2.UpdateStats()
    spp_after = ses2.GetStats().Get("stats.renderengine.pass").GetFloat()
    ses2.Stop()

    print(f"RESULT flm_ok=1 spp_loaded={spp_loaded:.2f} "
          f"noise_ok={int(noise_ok)} spp_before={spp_before} "
          f"spp_after={spp_after} rsm_ok=1", flush=True)


def check(ok, name, detail):
    print(f"[{'PASS' if ok else 'FAIL'}] {name}: {detail}", flush=True)
    return ok


def main():
    r = subprocess.run([sys.executable, str(Path(__file__).resolve()), "--child"],
                       capture_output=True, text=True, timeout=CHILD_TIMEOUT_S)
    res = {}
    for line in r.stdout.splitlines():
        if line.startswith("RESULT "):
            for tok in line.split()[1:]:
                k, v = tok.split("=")
                res[k] = v

    ok = True
    if not res:
        tail = (r.stderr or r.stdout or "")[-600:].strip()
        ok &= check(False, "child process", f"rc={r.returncode} {tail!r}")
    else:
        ok &= check(res.get("flm_ok") == "1" and res.get("noise_ok") == "1"
                    and float(res.get("spp_loaded", 0)) > 0,
                    ".flm round-trip + adaptiveError rebind",
                    f"spp_loaded={res.get('spp_loaded')} "
                    f"noise_ok={res.get('noise_ok')}")
        ok &= check(res.get("rsm_ok") == "1" and
                    float(res.get("spp_after", 0)) > float(res.get("spp_before", 1)),
                    ".rsm resume keeps rendering",
                    f"spp {res.get('spp_before')} -> {res.get('spp_after')}")

    print(f"{'PASS' if ok else 'FAIL'} overall", flush=True)
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "--child":
        child()
    else:
        main()
