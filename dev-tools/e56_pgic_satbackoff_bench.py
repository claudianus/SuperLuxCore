# e56 — PhotonGI caustic saturation-backoff bench (LUX_PGIC_SATBACKOFF).
# Renders a caustic scene on PATHCPU with a fast caustic update period so
# the radius refinement reaches its floor mid-render; compares the number
# of update passes and wall time with the backoff disabled/enabled.
# Env: E56_SPP (default 300), E56_UPDATESPP (default 1).
import os
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "out/build/src/pysuperluxcore/Release"))
import pysuperluxcore

WIDTH, HEIGHT = 320, 240
SPP = int(os.environ.get("E56_SPP", "300"))
UPDATESPP = int(os.environ.get("E56_UPDATESPP", "1"))
SCENE = REPO / "scenes/cornell/cornell-area-caustic.scn"


def run(tag, extra_env):
    props = pysuperluxcore.Properties(str(SCENE))
    cfg = pysuperluxcore.Properties()
    cfg.SetFromString(f"""
film.width = {WIDTH}
film.height = {HEIGHT}
renderengine.type = PATHCPU
sampler.type = SOBOL
batch.haltspp = {SPP}
renderengine.seed = 17
path.photongi.caustic.enabled = 1
path.photongi.caustic.updatespp = {UPDATESPP}
path.photongi.indirect.enabled = 0
""")
    scene = pysuperluxcore.Scene()
    scene.Parse(props)
    pysuperluxcore.SetLogHandler(
        lambda msgType, msg: print(f"[lux] {msg.strip()}", flush=True))
    ses = pysuperluxcore.RenderSession(pysuperluxcore.RenderConfig(cfg, scene))
    t0 = time.time()
    ses.Start()
    deadline = t0 + 900
    passes = 0
    while True:
        ses.UpdateStats()
        st = ses.GetStats()
        spp = st.Get("stats.renderengine.pass").GetInt()
        if spp >= SPP:
            break
        if time.monotonic() > deadline:
            raise TimeoutError("render stalled")
        time.sleep(0.5)
    wall = time.time() - t0
    ses.Stop()
    print(f"E56-RESULT {tag} wall={wall:.1f}s spp={SPP}", flush=True)
    return wall


if __name__ == "__main__":
    # The engine reads the kill switch from the environment at Update time.
    os.environ["LUX_PGIC_SATBACKOFF"] = "0"
    off = run("backoff_off", None)
    os.environ["LUX_PGIC_SATBACKOFF"] = "1"
    on = run("backoff_on", None)
    print(f"E56-SUMMARY off={off:.1f}s on={on:.1f}s delta={(on / off - 1) * 100:+.1f}%",
          flush=True)
