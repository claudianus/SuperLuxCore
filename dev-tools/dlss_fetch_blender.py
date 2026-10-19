"""Download official hash-verified stock Blender into one owned runner-temp folder."""
import hashlib,os,subprocess,sys,tarfile,zipfile
from pathlib import Path
windows=sys.platform=='win32';name='blender-5.2.2-'+('windows-x64.zip' if windows else 'linux-x64.tar.xz')
root=Path(os.environ['RUNNER_TEMP'])/'slc-fix3-stock-blender';root.mkdir();archive=root/name
try:
    base='https://download.blender.org/release/Blender5.2/'
    curl='curl.exe' if windows else 'curl'
    flags=['--fail','--location','--retry','3','--connect-timeout','30','--max-time','600']
    sums=subprocess.check_output([curl,*flags,base+'blender-5.2.2.sha256'],text=True,timeout=660)
    expected,=[line.split()[0] for line in sums.splitlines() if line.split()[-1].lstrip('*')==name]
    subprocess.run([curl,*flags,base+name,'--output',str(archive)],check=True,timeout=660)
    with archive.open('rb') as data:assert hashlib.file_digest(data,'sha256').hexdigest()==expected
    if windows:
        with zipfile.ZipFile(archive) as z:z.extractall(root)
    else:
        with tarfile.open(archive) as t:t.extractall(root,filter='data')
    target=root/('blender-5.2.2-'+('windows-x64' if windows else 'linux-x64'));assert target.is_dir()
    with open(os.environ['GITHUB_ENV'],'a',encoding='utf-8') as f:f.write('BLENDER_ROOT='+str(target)+'\n')
finally:
    archive.unlink(missing_ok=True)
