"""Build bounded fix3 components into complete ZIPs; verify installed upgrade."""
import argparse,hashlib,json,os,shutil,signal,subprocess,sys,zipfile
from pathlib import Path

VERSION='2.11.34+dlss.exp20261019.fix3'
HELPER_COMMIT='f3449d59676ebdf8fe1e3064e9d92ae36595eee6';HELPER_RUN=39202645597
LINUX_NATIVE='524385a46906b71cc7b9f5f4d68b42b4014bc7a6';LINUX_RUN=39203919041
WINDOWS_NATIVE='62551c4a8cb0097d35536ece26a79460e5f19db7';WINDOWS_RUN=39196423363

def run(args,timeout=600,env=None):
    print('RUN',args,flush=True)
    child=subprocess.Popen([str(x) for x in args],env=env,start_new_session=os.name!='nt')
    try:code=child.wait(timeout=timeout)
    except BaseException:
        if child.poll() is None:
            if os.name=='nt':subprocess.run(['taskkill','/PID',str(child.pid),'/T','/F'],check=False,timeout=20)
            else:
                try:os.killpg(child.pid,signal.SIGKILL)
                except ProcessLookupError:pass
        child.wait(timeout=30);raise
    assert code==0,(args,code)

def api(path):return json.loads(subprocess.check_output(['gh','api','repos/claudianus/SuperLuxCore/'+path],text=True,timeout=60))
def accepted(run_id,commit):
    r=api('actions/runs/'+str(run_id));assert r['head_sha']==commit and r['status']=='completed' and r['conclusion']=='success',r
    return r
def sha(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--platform',choices=['windows','linux'],required=True);args=parser.parse_args();windows=args.platform=='windows'
    assert (os.name=='nt')==windows
    base=Path.cwd();work=base/'package-input';work.mkdir();out=base/'addon-build/out';out.mkdir(parents=True);wheels=base/'addon/wheels';wheels.mkdir(exist_ok=True)
    platform='windows_x64' if windows else 'linux_x64';previous_version='2.11.34+dlss.exp20261019.fix2' if windows else '2.11.34+dlss.exp20261019';previous_tag='experimental-windows-dlss-20261019-fix2' if windows else 'experimental-windows-dlss-20261019'
    previous_name='SuperLuxCore-'+previous_version+'-'+platform+'.zip';previous_dir=work/'previous';previous_dir.mkdir()
    accepted(HELPER_RUN,HELPER_COMMIT);accepted(WINDOWS_RUN if windows else LINUX_RUN,WINDOWS_NATIVE if windows else LINUX_NATIVE)
    run(['gh','release','download',previous_tag,'--repo','claudianus/SuperBlendLuxCore','--pattern',previous_name,'--dir',previous_dir])
    previous=previous_dir/previous_name;assert sha(previous)==('49dcf1e834679861f9ca42c58f08108a1bfef61bd813be1bd241a62daf40ceeb' if windows else '830ca35479047570cb9fd9f50f48e59a5787b703e869911a17cdd82c46cb91f1')
    helper_dir=work/'helper';run(['gh','run','download',str(HELPER_RUN),'--repo','claudianus/SuperLuxCore','--name','dlss-frame-windows-helper' if windows else 'dlss-frame-linux-helper','--dir',helper_dir])
    install=helper_dir/'out'/('dlss-install' if windows else 'dlss-linux-install');proof=helper_dir/('helper-abi.json' if windows else 'linux-helper-abi.json')
    raw=work/'raw';raw.mkdir()
    if windows:
        with zipfile.ZipFile(previous) as z:
            name,=[n for n in z.namelist() if n.startswith('wheels/pysuperluxcore-') and n.endswith('.whl')]
            with z.open(name) as src,(raw/Path(name).name).open('wb') as dst:shutil.copyfileobj(src,dst)
    else:run(['gh','run','download',str(LINUX_RUN),'--repo','claudianus/SuperLuxCore','--name','native-linux-wheel-input','--dir',raw])
    wheel,=raw.glob('*.whl');native_commit=WINDOWS_NATIVE if windows else LINUX_NATIVE;native_run=WINDOWS_RUN if windows else LINUX_RUN
    stage=[sys.executable,'dev-tools/dlss_component_stage.py','--platform',args.platform,'--wheel',wheel,'--helper',install,'--helper-proof',proof,'--out',wheels,'--version',VERSION,'--renderer-commit',native_commit,'--renderer-ci','https://github.com/claudianus/SuperLuxCore/actions/runs/'+str(native_run),'--helper-commit',HELPER_COMMIT,'--package-commit',os.environ['GITHUB_SHA']]
    if not windows:stage+=['--runtime',base/'.dlss-sdk/lib/Linux_x86_64/rel/libnvidia-ngx-dlssd.so.310.9.16','--runtime-license',base/'.dlss-sdk/LICENSE.txt']
    run(stage);shutil.copy2(wheels/'component-provenance.json',out/'component-provenance.json')
    req='addon/cmake/bundled-wheels-'+('win' if windows else 'linux')+'.txt'
    run([sys.executable,'-m','pip','download','--require-hashes','--no-deps','--only-binary=:all:','--platform','win_amd64' if windows else 'manylinux2014_x86_64','--python-version','3.13','--implementation','cp','-d',wheels,'-r',req])
    for script in ('dlss_viewport_fallback_test.py','dlss_viewport_backend_ui_test.py'):run([sys.executable,'addon/dev-tools/'+script,'--out',str(out/script.replace('.py','.json'))])
    env=dict(os.environ);blender=Path(env['BLENDER_ROOT'])/('blender.exe' if windows else 'blender')
    run(['cmake','-S','addon','-B','addon-build','-G','Ninja','-DCMAKE_BUILD_TYPE=Release','-DBLC_BUNDLE_WHEELS=ON'],env=env)
    run(['cmake','--build','addon-build'],env=env)
    run([sys.executable,'addon/cmake/verify_bundle.py',out,'--platforms',platform],env=env)
    package,=out.glob('SuperLuxCore-*-'+platform+'.zip');profile=out/'profile-한글';env.update(BLENDER_USER_RESOURCES=str(profile.resolve()),SLC_SMOKE_OUT=str(out.resolve()),PYTHONUNBUFFERED='1')
    def blender_run(arguments,timeout=120):run([blender,*arguments],timeout,env)
    try:
        blender_run(['--background','--factory-startup','--command','extension','install-file','-r','user_default','-e',previous.resolve()])
        expression="import bpy,importlib.metadata,json,pathlib,hashlib; bpy.ops.preferences.addon_enable(module='bl_ext.user_default.superluxcore'); import pysuperluxcore; root=pathlib.Path(pysuperluxcore.__file__).parent; native=next(root.glob("+repr('pysuperluxcore*.pyd' if windows else 'pysuperluxcore*.so')+")); helper=root/"+repr('superluxcore_dlss.dll' if windows else 'libsuperluxcore_dlss.so')+"; assert importlib.metadata.version('pysuperluxcore')=="+repr(previous_version)+"; pathlib.Path("+repr(str((out/'previous-install.json').resolve()))+").write_text(json.dumps({'version':importlib.metadata.version('pysuperluxcore'),'native_sha256':hashlib.sha256(native.read_bytes()).hexdigest(),'helper_sha256':hashlib.sha256(helper.read_bytes()).hexdigest()}))"
        blender_run(['--background','--factory-startup','--python-exit-code','1','--python-expr',expression])
        blender_run(['--background','--factory-startup','--command','extension','install-file','-r','user_default','-e',package.resolve()])
        blender_run(['--background','--factory-startup','--python-exit-code','1','--python','dev-tools/dlss_stability_blender_smoke.py'],240)
        smoke=json.loads((out/'installed-blender-smoke.json').read_text());assert smoke['passed'] and smoke['NGX_GPU_evaluations']==0
        (out/'package-identity.json').write_text(json.dumps(dict(version=VERSION,platform=platform,package=package.name,package_sha256=sha(package),package_bytes=package.stat().st_size,previous_package_sha256=sha(previous),package_commit=os.environ['GITHUB_SHA'],no_RTX_acceptance=True),indent=2)+'\n')
    finally:
        shutil.rmtree(profile,ignore_errors=True);shutil.rmtree(work,ignore_errors=True)
        (out/'cleanup.json').write_text(json.dumps(dict(profile_removed=not profile.exists(),own_package_inputs_removed=not work.exists(),child_timeouts_reaped=True),indent=2)+'\n')

if __name__=='__main__':main()
