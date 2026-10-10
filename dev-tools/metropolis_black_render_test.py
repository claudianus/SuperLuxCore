# SPDX-License-Identifier: Apache-2.0
"""Real CPU eye/light/hybrid sample halts for an entirely black scene."""
import ast
import hashlib
import json
import os
from pathlib import Path
import time
import numpy as np
import pysuperluxcore as slc

repo = Path(__file__).resolve().parents[1]
folder = Path(os.environ['SUPERLUXCORE_AUDIT_DIR'])
folder.mkdir(parents=True, exist_ok=True)
identity = json.loads(Path(os.environ['SUPERLUXCORE_BSSRDF_IDENTITY']).read_text())
assert hashlib.sha256(Path(slc.pysuperluxcore.__file__).read_bytes()).hexdigest() == identity['native_sha256']
slc.Init()
namespace = {'slc': slc}
tree = ast.parse((repo / 'dev-tools/cycles_bssrdf_transport_test.py').read_text())
functions = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('properties', 'scene')]
exec(compile(ast.Module(body=functions, type_ignores=[]), '<black-scene-builder>', 'exec'), namespace)
props = namespace['properties']
scene = namespace['scene'](kind='matte')
scene.Parse(props({'scene.lights.env.type': 'constantinfinite', 'scene.lights.env.color': (0., 0., 0.)}))
(folder / 'scene.scn').write_text(scene.ToProperties().ToString())
records = []
for label, engine, hybrid in (('eye', 'PATHCPU', False), ('hybrid', 'PATHCPU', True), ('light', 'LIGHTCPU', False)):
    values = {'film.width': 16, 'film.height': 16, 'film.imagepipelines.0.0.type': 'NOP',
              'film.outputs.0.type': 'RGB', 'film.outputs.0.filename': 'unused.exr',
              'renderengine.type': engine, 'native.threads.count': 8,
              'sampler.type': 'METROPOLIS', 'batch.haltspp': 128,
              'path.hybridbackforward.enable': hybrid, 'path.lighttracing.enable': hybrid,
              'path.lighttracing.auto': False, 'path.mnee.auto': False,
              'path.regularization.auto': False, 'path.photongi.caustic.enabled': False,
              'path.photongi.indirect.enabled': False}
    (folder / (label + '.cfg')).write_text(props(values).ToString())
    session = slc.RenderSession(slc.RenderConfig(props(values), scene))
    session.Start()
    start = time.monotonic()
    try:
        while not session.HasDone():
            session.UpdateStats()
            assert time.monotonic() - start < 90, label + ' failed to reach the sample halt'
            time.sleep(.1)
        session.UpdateStats()
        stats = session.GetStats()
        passes = {name: stats.Get('stats.renderengine.pass.' + name).GetInt() for name in ('eye', 'light')}
    finally:
        session.Stop()
    rgb = np.empty(16 * 16 * 3, np.float32)
    session.GetFilm().GetOutputFloat(slc.FilmOutputType.RGB, rgb, 0, True)
    assert np.isfinite(rgb).all() and np.max(np.abs(rgb)) == 0., (label, rgb)
    assert max(passes.values()) >= 128, (label, passes)
    records.append({'case': label, 'counts': passes, 'maximum_absolute_rgb': float(np.max(np.abs(rgb))), 'passed': True})
    print('METROPOLIS_BLACK_RENDER_PASS', records[-1], flush=True)
(folder / 'metrics.json').write_text(json.dumps({'native_sha256': identity['native_sha256'], 'complete': True, 'records': records}, indent=2))
