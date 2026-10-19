"""Actual installed fix3 ZIP CPU/OIDN fallback; no RTX acceptance."""
import bpy,ctypes,hashlib,importlib,importlib.metadata,json,os,sys
from pathlib import Path
import numpy as np

out=Path(os.environ['SLC_SMOKE_OUT']);platform=sys.platform
package='bl_ext.user_default.superluxcore';bpy.ops.preferences.addon_enable(module=package)
addon=importlib.import_module(package)
import pysuperluxcore
root=Path(pysuperluxcore.__file__).parent;version=importlib.metadata.version('pysuperluxcore')
assert version=='2.11.34+dlss.exp20261019.fix3'
assert str(Path(os.environ['BLENDER_USER_RESOURCES']).resolve()).lower() in str(root.resolve()).lower(),root
previous=json.loads((out/'previous-install.json').read_text())
native,=root.glob('pysuperluxcore*.pyd' if platform=='win32' else 'pysuperluxcore*.so')
helper=root/('superluxcore_dlss.dll' if platform=='win32' else 'libsuperluxcore_dlss.so')
sha=lambda path:hashlib.sha256(path.read_bytes()).hexdigest()
provenance=json.loads((root/'dlss-provenance.json').read_text())
assert sha(native)==provenance['native_module_sha256'] and sha(helper)==provenance['helper_sha256']
if platform=='win32':
    assert previous['version']=='2.11.34+dlss.exp20261019.fix2'
    assert sha(native)==previous['native_sha256']=='207328da3920d73aed7b0dfa0c33c8a65f8861799b53dcc2a895456f1217db70'
else:
    assert previous['version']=='2.11.34+dlss.exp20261019'
    assert sha(native)!=previous['native_sha256']
assert sha(helper)!=previous['helper_sha256']
assert ctypes.CDLL(str(helper)).slc_dlss_abi()==1
dlss=importlib.import_module(package+'.engine.dlss');assert dlss._library_path().resolve()==helper.resolve()
runtime=root/'dlss-runtime'/('nvngx_dlssd.dll' if platform=='win32' else 'libnvidia-ngx-dlssd.so.310.9.16')
assert sha(runtime)==provenance['nvidia_release_runtime_sha256']
lib=ctypes.CDLL(str(helper));lib.slc_dlss_process.argtypes=[ctypes.c_void_p,ctypes.c_void_p,ctypes.POINTER(ctypes.c_float),ctypes.c_uint64,ctypes.c_char_p,ctypes.c_uint32];lib.slc_dlss_process.restype=ctypes.c_int
target=(ctypes.c_float*8)(*([7.25]*8));message=ctypes.create_string_buffer(1024)
assert lib.slc_dlss_process(None,None,target,8,message,len(message))==0 and message.value and list(target)==[7.25]*8
scene=bpy.context.scene;scene.render.engine='SUPERLUXCORE';scene.render.resolution_x=128;scene.render.resolution_y=72;scene.render.resolution_percentage=100
settings=scene.superluxcore;settings.config.engine='PATH';settings.config.device='CPU';settings.halt.use_samples=True;settings.halt.samples=2;settings.halt.use_time=False;settings.halt.enable=True;settings.halt.use_noise_level=False;settings.halt.use_noise_thresh=False
scene.view_layers[0].superluxcore.halt.enable=False;settings.denoiser.enabled=True;settings.denoiser.type='DLSS'
scene.render.image_settings.file_format='OPEN_EXR';scene.render.image_settings.color_mode='RGBA';scene.render.image_settings.color_depth='32';scene.render.filepath=str(out/('windows-cpu.exr' if platform=='win32' else 'linux-cpu.exr'))
bpy.ops.render.render(write_still=True)
image=bpy.data.images.load(scene.render.filepath,check_existing=False);values=np.asarray(image.pixels[:],dtype=np.float32)
assert values.size==128*72*4 and np.isfinite(values).all() and np.max(values.reshape(-1,4)[:,:3])>0
result=dict(passed=True,host=bpy.app.version_string,platform=platform,distribution_version=version,installed_zip_CPU_render=True,DLSS_helper_ABI_and_discovery=True,invalid_frame_output_preserved=True,whole_ZIP_upgrade=True,previous=previous,native_sha256=sha(native),helper_sha256=sha(helper),CPU_DLSS_unavailable_fallback_render=True,NGX_GPU_evaluations=0,actual_RTX_acceptance=False,RTX_quality_and_performance_accepted=False,component_provenance=provenance)
(out/'installed-blender-smoke.json').write_text(json.dumps(result,indent=2)+'\n')
print('PASS fix3 installed ZIP upgrade, helper discovery and CPU/OIDN fallback')
