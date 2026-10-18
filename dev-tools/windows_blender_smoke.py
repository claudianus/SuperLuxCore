"""Installed ZIP smoke on a GPU-less Windows runner; never DLSS acceptance."""
import bpy
import ctypes
import importlib
import json
import os
from pathlib import Path
import sys

package = 'bl_ext.user_default.superluxcore'
bpy.ops.preferences.addon_enable(module=package)
addon = importlib.import_module(package)
import pysuperluxcore
root = Path(pysuperluxcore.__file__).parent
assert str(Path(os.environ['BLENDER_USER_RESOURCES']).resolve()).lower() in str(root.resolve()).lower(), root
native = root / 'superluxcore_dlss.dll'
assert ctypes.CDLL(str(native)).slc_dlss_abi() == 1
assert (root / 'dlss-runtime/nvngx_dlssd.dll').is_file()
dlss = importlib.import_module(package + '.engine.dlss')
assert dlss._library_path().resolve() == native.resolve()

scene = bpy.context.scene
scene.render.engine = 'SUPERLUXCORE'
scene.render.resolution_x = 128
scene.render.resolution_y = 72
scene.render.resolution_percentage = 100
settings = scene.superluxcore
settings.config.engine = 'PATH'
settings.config.device = 'CPU'
settings.halt.use_samples = True
settings.halt.samples = 2
settings.halt.use_time = False
settings.halt.enable = True
settings.halt.use_noise_level = False
settings.halt.use_noise_thresh = False
scene.view_layers[0].superluxcore.halt.enable = False
scene.render.image_settings.file_format = 'OPEN_EXR'
scene.render.image_settings.color_mode = 'RGBA'
scene.render.image_settings.color_depth = '32'
scene.render.filepath = str(Path(os.environ['SLC_SMOKE_OUT']) / 'windows-cpu.exr')
bpy.ops.render.render(write_still=True)
image = bpy.data.images.load(scene.render.filepath, check_existing=False)
import numpy as np
values = np.asarray(image.pixels[:], dtype=np.float32)
assert values.size == 128 * 72 * 4 and np.isfinite(values).all()
assert np.max(values.reshape(-1, 4)[:, :3]) > 0
result = {
    'passed': True, 'host': bpy.app.version_string,
    'installed_zip_CPU_render': True, 'DLSS_helper_ABI_and_discovery': True,
    'NGX_GPU_evaluations': 0, 'RTX_quality_and_performance_accepted': False,
}
Path(os.environ['SLC_SMOKE_OUT'], 'windows-blender-smoke.json').write_text(json.dumps(result, indent=2))
print('PASS installed experimental Windows ZIP CPU render and DLSS helper discovery')
