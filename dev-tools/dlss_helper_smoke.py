"""Built helper ABI/driver-discovery smoke; never real DLSS frame acceptance."""
import argparse,ctypes as C,hashlib,json,os,re,struct,subprocess,tempfile
from pathlib import Path

def digest(path):
    with path.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()

def elf_gate(path):
    header=subprocess.check_output(['readelf','-h',str(path)],text=True)
    assert 'ELF64' in header and 'Advanced Micro Devices X86-64' in header
    versions=subprocess.check_output(['readelf','--version-info',str(path)],text=True)
    glibc=[tuple(map(int,v.split('.'))) for v in re.findall(r'Name: GLIBC_([0-9.]+)',versions)]
    assert all(v<=(2,28) for v in glibc),glibc
    dynamic=subprocess.check_output(['readelf','-d',str(path)],text=True)
    needed=re.findall(r'Shared library: \[(.*?)\]',dynamic)
    assert set(needed)<={'libc.so.6','libm.so.6','libdl.so.2','libpthread.so.0','librt.so.1','ld-linux-x86-64.so.2'},needed
    return dict(GLIBC_versions=sorted(set(glibc)),DT_NEEDED=needed)

a=argparse.ArgumentParser();a.add_argument('--library',type=Path,required=True);a.add_argument('--runtime',type=Path,required=True);a.add_argument('--out',type=Path,required=True);a.add_argument('--elf-runtime',type=Path);args=a.parse_args()
library=args.library.resolve();runtime=args.runtime.resolve();platform={}
if args.elf_runtime:
    platform=dict(helper=elf_gate(library),runtime=elf_gate(args.elf_runtime),nvidia_runtime_sha256=digest(args.elf_runtime))
    runtime_handle=C.CDLL(str(args.elf_runtime.resolve()));platform['nvidia_runtime_load']=True
else:
    data=library.read_bytes();offset=struct.unpack_from('<I',data,0x3c)[0]
    assert data[:2]==b'MZ' and data[offset:offset+4]==b'PE\0\0' and struct.unpack_from('<H',data,offset+4)[0]==0x8664
lib=C.CDLL(str(library));lib.slc_dlss_abi.restype=C.c_uint32;assert lib.slc_dlss_abi()==1
lib.slc_dlss_process.argtypes=[C.c_void_p,C.c_void_p,C.POINTER(C.c_float),C.c_uint64,C.c_char_p,C.c_uint32];lib.slc_dlss_process.restype=C.c_int
lib.slc_dlss_create.argtypes=[C.c_uint32,C.c_char_p,C.c_char_p,C.c_uint64,C.c_char_p,C.c_uint32];lib.slc_dlss_create.restype=C.c_void_p
lib.slc_dlss_destroy.argtypes=[C.c_void_p];lib.slc_dlss_destroy.restype=None
target=(C.c_float*8)(*([7.25]*8));message=C.create_string_buffer(1024)
assert lib.slc_dlss_process(None,None,target,8,message,len(message))==0 and message.value and list(target)==[7.25]*8
with tempfile.TemporaryDirectory(prefix='slc-helper-abi-') as cache:
    handle=lib.slc_dlss_create(0,os.fsencode(runtime),os.fsencode(cache),4096,message,len(message))
    if not handle:assert message.value
    lib.slc_dlss_destroy(handle);lib.slc_dlss_destroy(None)
result=dict(ABI=1,invalid_frame_failure_output_preserved=True,create_returned_handle=bool(handle),create_error=message.value.decode('utf8',errors='replace'),actual_NGX_evaluations=0,helper_sha256=digest(library),source_sha256=digest(Path(__file__).resolve().parents[1]/'src/slg/film/denoiser/dlss/runtime.cpp'),platform=platform,scope='Actual compiled library/load, invalid-frame output preservation, create discovery and close only; no GPU frame/quality/performance acceptance')
args.out.write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
