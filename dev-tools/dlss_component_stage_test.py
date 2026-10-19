"""Packaging-only fixtures: real wheel pack/unpack, preservation and rejection gates."""
import base64,csv,hashlib,io,json,os,subprocess,sys,tempfile,zipfile
from pathlib import Path

def digest(data):return hashlib.sha256(data).hexdigest()

def main():
    results=[]
    with tempfile.TemporaryDirectory(prefix='slc-stage-test-') as temp:
        root=Path(temp)
        for platform in ('windows','linux'):
            p=root/platform;p.mkdir();helper=p/'helper';helper.mkdir()
            lib='superluxcore_dlss.dll' if platform=='windows' else 'libsuperluxcore_dlss.so'
            runtime='nvngx_dlssd.dll' if platform=='windows' else 'libnvidia-ngx-dlssd.so.310.9.16'
            native='pysuperluxcore.pyd' if platform=='windows' else 'pysuperluxcore.cpython-313-x86_64-linux-gnu.so'
            tag='cp313-cp313-win_amd64' if platform=='windows' else 'cp313-cp313-manylinux_2_28_x86_64'
            old='2.11.34+dlss.exp20261019.fix2' if platform=='windows' else '2.11.34'
            info='pysuperluxcore-'+old+'.dist-info/'
            files={info+'METADATA':('Metadata-Version: 2.4\nName: pysuperluxcore\nVersion: '+old+'\nSummary: 한글 fixture\n').encode(),info+'WHEEL':('Wheel-Version: 1.0\nGenerator: fixture\nRoot-Is-Purelib: false\nTag: '+tag+'\n').encode(),'pysuperluxcore/'+native:b'PACKAGING FIXTURE native never load','pysuperluxcore/__init__.py':b'# preserved Python\n','pysuperluxcore/cl2msl.py':b'# preserved translator\n','pysuperluxcore-'+old+'.data/data/sentinel':b'preserved data'}
            (helper/lib).write_bytes(b'PACKAGING FIXTURE new helper never load');(helper/'CUEW-LICENSE.txt').write_bytes(b'fixture cuew');(helper/'NVIDIA-DLSS-LICENSE.txt').write_bytes(b'fixture runtime license')
            if platform=='windows':
                (helper/'CUEW-LICENSE.txt').write_bytes(b'fixture cuew\n');(helper/'NVIDIA-DLSS-LICENSE.txt').write_bytes(b'fixture runtime license\n')
                files.update({'pysuperluxcore/'+lib:b'PACKAGING FIXTURE old helper never load','pysuperluxcore/CUEW-LICENSE.txt':b'fixture cuew\r\n','pysuperluxcore/dlss-runtime/'+runtime:b'PACKAGING FIXTURE runtime never load','pysuperluxcore/dlss-runtime/NVIDIA-DLSS-LICENSE.txt':b'fixture runtime license\r\n','pysuperluxcore/dlss-provenance.json':b'{}'})
            (p/runtime).write_bytes(b'PACKAGING FIXTURE runtime never load')
            proof=dict(ABI=1,invalid_frame_failure_output_preserved=True,helper_sha256=digest((helper/lib).read_bytes()),source_sha256='fixture')
            (p/'proof.json').write_text(json.dumps(proof))
            wheel=p/('pysuperluxcore-'+old+'-'+tag+'.whl')
            def write_wheel(bad_record=False):
                record=io.StringIO();writer=csv.writer(record)
                for name,data in files.items():writer.writerow((name,'sha256='+base64.urlsafe_b64encode(hashlib.sha256(data).digest()).rstrip(b'=').decode(),len(data)))
                writer.writerow((info+'RECORD','',''))
                with zipfile.ZipFile(wheel,'w',zipfile.ZIP_DEFLATED) as z:
                    for name,data in files.items():z.writestr(name,data+(b'corrupt' if bad_record and name.endswith(native) else b''))
                    z.writestr(info+'RECORD',record.getvalue())
            write_wheel()
            args=[sys.executable,str(Path(__file__).with_name('dlss_component_stage.py')),'--platform',platform,'--wheel',str(wheel),'--helper',str(helper),'--helper-proof',str(p/'proof.json'),'--out',str(p/'out'),'--version','2.11.34+dlss.exp20261019.fix3','--renderer-commit','fixture-native','--renderer-ci','fixture-ci','--helper-commit','fixture-helper','--package-commit','fixture-package','--fixture-only']
            if platform=='linux':args+=['--runtime',str(p/runtime),'--runtime-license',str(helper/'NVIDIA-DLSS-LICENSE.txt')]
            env=dict(os.environ,SLC_PACKAGING_FIXTURE_ONLY='1')
            q=subprocess.run(args,env=env,capture_output=True,text=True,timeout=60);assert q.returncode==0,q.stdout+q.stderr
            with zipfile.ZipFile(next((p/'out').glob('*.whl'))) as z:
                metadata=z.read('pysuperluxcore-2.11.34+dlss.exp20261019.fix3.dist-info/METADATA').decode()
                assert 'Summary: 한글 fixture' in metadata and 'Version: 2.11.34+dlss.exp20261019.fix3\n' in metadata
                assert z.read('pysuperluxcore/'+native)==files['pysuperluxcore/'+native]
                assert z.read('pysuperluxcore/'+lib)==(helper/lib).read_bytes()
            results.append(dict(platform=platform,case='version_UTF8_RECORD_data_native_preservation_and_helper_replacement',passed=True))
            for case in ('wrong_helper_hash','corrupt_original_RECORD'):
                if case=='wrong_helper_hash':(helper/lib).write_bytes(b'wrong')
                else:
                    (helper/lib).write_bytes(b'PACKAGING FIXTURE new helper never load');write_wheel(bad_record=True)
                q=subprocess.run(args,env=env,capture_output=True,text=True,timeout=60);assert q.returncode!=0,case
                results.append(dict(platform=platform,case=case,passed=True,rejected=True))
    result=dict(passed=True,cases=results,scope='Packaging fixtures only; no binary execution or renderer/GPU acceptance')
    if len(sys.argv)>1:Path(sys.argv[1]).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__=='__main__':main()
