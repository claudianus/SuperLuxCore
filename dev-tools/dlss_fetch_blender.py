"""Download official hash-verified stock Blender into one owned runner-temp folder."""
import hashlib,os,shutil,sys,tarfile,urllib.request,zipfile
from pathlib import Path
windows=sys.platform=='win32';name='blender-5.2.2-'+('windows-x64.zip' if windows else 'linux-x64.tar.xz')
root=Path(os.environ['RUNNER_TEMP'])/'slc-fix3-stock-blender';root.mkdir();archive=root/name
try:
    base='https://download.blender.org/release/Blender5.2/'
    sums=urllib.request.urlopen(base+'blender-5.2.2.sha256',timeout=60).read().decode()
    expected,=[line.split()[0] for line in sums.splitlines() if line.split()[-1].lstrip('*')==name]
    with urllib.request.urlopen(base+name,timeout=120) as response,archive.open('wb') as output:shutil.copyfileobj(response,output)
    with archive.open('rb') as data:assert hashlib.file_digest(data,'sha256').hexdigest()==expected
    if windows:
        with zipfile.ZipFile(archive) as z:z.extractall(root)
    else:
        with tarfile.open(archive) as t:t.extractall(root,filter='data')
    target=root/('blender-5.2.2-'+('windows-x64' if windows else 'linux-x64'));assert target.is_dir()
    with open(os.environ['GITHUB_ENV'],'a',encoding='utf-8') as f:f.write('BLENDER_ROOT='+str(target)+'\n')
finally:
    archive.unlink(missing_ok=True)
