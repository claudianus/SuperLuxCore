"""Stage tested DLSS components and version metadata without rewriting renderer payload."""
import argparse,base64,csv,hashlib,io,json,os,shutil,subprocess,sys,tempfile,zipfile
from email.parser import BytesParser
from pathlib import Path

def sha(data):return hashlib.sha256(data).hexdigest()
def record_gate(z):
    record,=[n for n in z.namelist() if n.endswith('.dist-info/RECORD')]
    for name,digest,size in csv.reader(io.StringIO(z.read(record).decode())):
        if digest:
            data=z.read(name);assert digest=='sha256='+base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b'=').decode() and len(data)==int(size),name

def main():
    p=argparse.ArgumentParser();p.add_argument('--platform',choices=['windows','linux'],required=True);p.add_argument('--wheel',type=Path,required=True);p.add_argument('--helper',type=Path,required=True);p.add_argument('--helper-proof',type=Path,required=True);p.add_argument('--runtime',type=Path);p.add_argument('--runtime-license',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--version',required=True);p.add_argument('--renderer-commit',required=True);p.add_argument('--renderer-ci',required=True);p.add_argument('--helper-commit',required=True);p.add_argument('--package-commit',required=True);p.add_argument('--fixture-only',action='store_true');a=p.parse_args()
    assert not a.fixture_only or os.environ.get('SLC_PACKAGING_FIXTURE_ONLY')=='1'
    proof=json.loads(a.helper_proof.read_text());assert proof['ABI']==1 and proof['invalid_frame_failure_output_preserved']
    libname='superluxcore_dlss.dll' if a.platform=='windows' else 'libsuperluxcore_dlss.so';runtime_name='nvngx_dlssd.dll' if a.platform=='windows' else 'libnvidia-ngx-dlssd.so.310.9.16'
    helper=a.helper/libname;assert sha(helper.read_bytes())==proof['helper_sha256']
    if not a.fixture_only:
        source=Path(__file__).resolve().parents[1]/'src/slg/film/denoiser/dlss/runtime.cpp'
        assert sha(source.read_bytes())==proof['source_sha256']
    with zipfile.ZipFile(a.wheel) as z:
        record_gate(z);info,=[n.rsplit('/',1)[0] for n in z.namelist() if n.endswith('.dist-info/METADATA')]
        old_version=BytesParser().parsebytes(z.read(info+'/METADATA'))['Version'];assert old_version and old_version!=a.version
        original={n:sha(z.read(n)) for n in z.namelist() if not n.endswith('/') and '.dist-info/' not in n}
        module,=[n for n in original if n.endswith('/pysuperluxcore.pyd') or Path(n).name.startswith('pysuperluxcore.') and n.endswith('.so')]
        original_module_sha=original[module]
    with tempfile.TemporaryDirectory(prefix='slc-components-') as temp:
        t=Path(temp);subprocess.run([sys.executable,'-m','wheel','unpack',str(a.wheel.resolve()),'-d',str(t)],check=True,timeout=60)
        unpacked,=t.iterdir();package=unpacked/'pysuperluxcore';distinfo,=unpacked.glob('*.dist-info')
        metadata=distinfo/'METADATA';text=metadata.read_text(encoding='utf-8');old='\nVersion: '+old_version+'\n';assert text.count(old)==1
        metadata.write_text(text.replace(old,'\nVersion: '+a.version+'\n'),encoding='utf-8')
        distinfo.rename(unpacked/('pysuperluxcore-'+a.version+'.dist-info'))
        data=unpacked/('pysuperluxcore-'+old_version+'.data')
        if data.exists():data.rename(unpacked/('pysuperluxcore-'+a.version+'.data'))
        shutil.copy2(helper,package/libname)
        shutil.copy2(a.helper/'CUEW-LICENSE.txt',package/'CUEW-LICENSE.txt')
        runtime=package/'dlss-runtime';runtime.mkdir(exist_ok=True)
        if a.runtime:
            assert a.runtime.name==runtime_name and a.runtime_license
            shutil.copy2(a.runtime,runtime/runtime_name);shutil.copy2(a.runtime_license,runtime/'NVIDIA-DLSS-LICENSE.txt')
        assert (runtime/runtime_name).is_file() and (runtime/'NVIDIA-DLSS-LICENSE.txt').is_file()
        assert (runtime/'NVIDIA-DLSS-LICENSE.txt').read_bytes()==(a.helper/'NVIDIA-DLSS-LICENSE.txt').read_bytes()
        runtime_sha=sha((runtime/runtime_name).read_bytes())
        if not a.fixture_only:
            expected='b49435ae346f227feafe7bbe937d88277ea0ee7323975ec06488f377fdb46cee' if a.platform=='windows' else 'a5d59de18841f15779039dda567a15dd723ba737915f489271fdb5f109fbfb96'
            assert runtime_sha==expected
        provenance=dict(classification='packaging fixture only' if a.fixture_only else 'experimental DLSS stability components',engine_commit=a.renderer_commit,renderer_native_commit=a.renderer_commit,renderer_native_ci=a.renderer_ci,helper_source_commit=a.helper_commit,helper_acceptance_ci='https://github.com/claudianus/SuperLuxCore/actions/runs/39202645597',package_commit=a.package_commit,distribution_version=a.version,native_version='2.11.34',sdk_commit='9981c6350dc58c0ce21f024fd65e31e851c31689',helper_source_sha256=proof.get('source_sha256'),helper_sha256=proof['helper_sha256'],nvidia_release_runtime_sha256=runtime_sha,native_module_sha256=original_module_sha,GPU_evaluation_in_CI=False,RTX_quality_and_performance_accepted=False,renderer_source_baseline='Phase70 plus bounded native cleanup; unaccepted Phase71-79 renderer implementation excluded',original_wheel_sha256=sha(a.wheel.read_bytes()),unchanged_renderer_payload=True)
        (package/'dlss-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n',encoding='utf-8')
        variant=unpacked.with_name('pysuperluxcore-'+a.version);unpacked.rename(variant);a.out.mkdir(parents=True,exist_ok=True)
        assert not list(a.out.glob('pysuperluxcore-*.whl'))
        subprocess.run([sys.executable,'-m','wheel','pack',str(variant),'-d',str(a.out)],check=True,timeout=60)
    wheel,=a.out.glob('pysuperluxcore-*.whl');preserved=[]
    with zipfile.ZipFile(wheel) as z:
        record_gate(z)
        for name,digest in original.items():
            if name in ['pysuperluxcore/'+libname,'pysuperluxcore/dlss-provenance.json']:continue
            mapped=name.replace('pysuperluxcore-'+old_version+'.data/','pysuperluxcore-'+a.version+'.data/',1)
            assert sha(z.read(mapped))==digest,name;preserved.append(mapped)
        assert sha(z.read(module))==original_module_sha
    provenance.update(original_payload_files_preserved=len(preserved),wheel=wheel.name,wheel_sha256=sha(wheel.read_bytes()))
    (a.out/'component-provenance.json').write_text(json.dumps(provenance,indent=2)+'\n');print(json.dumps(provenance,indent=2))

if __name__=='__main__':main()
