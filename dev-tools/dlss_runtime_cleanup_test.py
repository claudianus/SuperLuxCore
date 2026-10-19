"""C caller exercises production DLSS teardown bodies with faulting CUDA/NGX.

No SDK or GPU is needed. The extracted production methods are unchanged; only
types, construction and driver entry points are mocked. Products are temporary.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
from cuda_teardown_test import ROOT, method

PRE = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
using CUcontext=void*;using CUarray=void*;using CUtexObject=uint64_t;using CUsurfObject=uint64_t;
using CUresult=int;using NVSDK_NGX_Result=int;using NVSDK_NGX_Handle=int;
constexpr int CUDA_SUCCESS=0;
#define NVSDK_NGX_FAILED(r) ((r)!=0)
struct SlcDLSSStats { uint64_t texture_bytes=0,host_scratch_bytes=0;unsigned width=0,height=0,output_width=0,output_height=0,frames=0; };
struct SlcDLSSFrame {};
std::string fault;std::vector<int> stack{42};std::map<uintptr_t,int> allocations;
int nextId=1,pushes=0,pops=0,shutdowns=0,destroys=0,wrappers=0,instances=0,processed=0;
bool injected=false;
unsigned currentRefs();
uintptr_t allocate(){allocations[nextId]=7;return nextId++;}
int result(const char *op){if(!injected&&fault==op){injected=true;return 99;}return 0;}
int release(uintptr_t id,const char *op){assert(stack.back()==7);assert(allocations.count(id));int r=result(op);if(!r)allocations.erase(id);return r;}
int cuCtxPushCurrent(CUcontext){++pushes;int r=result(currentRefs()?"push":"shutdown-push");if(!r)stack.push_back(7);return r;}
int cuCtxPopCurrent(CUcontext *p){assert(stack.back()==7);*p=reinterpret_cast<void*>(uintptr_t(7));stack.pop_back();++pops;return result(currentRefs()?"pop":"shutdown-pop");}
int cuCtxSynchronize(){assert(stack.back()==7);return result("sync");}
int NVSDK_NGX_CUDA_ReleaseFeature(int *p){int r=release(uintptr_t(p),"feature");return r;}
int cuSurfObjectDestroy(uint64_t id){return release(id,"surface");}
int cuTexObjectDestroy(uint64_t id){return release(id,"texture");}
int cuArrayDestroy(void *id){return release(uintptr_t(id),"array");}
int NVSDK_NGX_CUDA_Shutdown(){assert(stack.back()==7);++shutdowns;return result("shutdown");}
int cuCtxDestroy(CUcontext){++destroys;int r=result("destroy");if(!r)allocations.clear();return r;}
void cuda(CUresult r){if(r)throw std::runtime_error("CUDA result "+std::to_string(r));}
void error(char *p,uint32_t n,const char *s){if(p&&n){std::strncpy(p,s,n-1);p[n-1]=0;}}
std::mutex mutex;
'''

DRIVER = r'''
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/resource.h>
#endif
#include <stdio.h>
#include <stdlib.h>
void probe_setup(const char *,int);
void slc_dlss_destroy(void *);
void *probe_handle(int);
int probe_between(void);
int probe_check(void);
int run(int argc,char **argv){
#ifdef _WIN32
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#else
 struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
#endif
 int count=argv[2][0]-'0';probe_setup(argv[1],count);
 for(int i=0;i<count;++i){slc_dlss_destroy(probe_handle(i));if(i+1<count&&!probe_between())return 11;}
 slc_dlss_destroy(0);int result=probe_check();
#ifdef SLC_TEST_GUI
 if(_fileno(stdout)!=-2||_fileno(stderr)!=-2)return 12;
 FILE *receipt=fopen(argv[3],"w");if(!receipt)return 13;
 fprintf(receipt,"{\"passed\":true,\"stdout_fd\":%d,\"stderr_fd\":%d}\n",_fileno(stdout),_fileno(stderr));fclose(receipt);
#endif
 return result;
}
#ifdef SLC_TEST_GUI
int WINAPI WinMain(HINSTANCE a,HINSTANCE b,LPSTR c,int d){return run(__argc,__argv);}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
'''


def translation_unit(source, baseline):
    manager = source[source.index('struct Manager {'):source.index('void acquireManager(')]
    # Includes the actual Manager/Scope, cleanup result helpers and releaseManager.
    texture_methods = method(source, 'void release(')
    instance_methods = method(source, 'void clear(')
    if not baseline:
        texture_methods += method(source, 'void abandon(')
        start = source.index('struct Instance {')
        instance_methods += method(source[start:], 'void abandon(')
    texture = r'''
struct Texture {
 CUarray array=reinterpret_cast<void*>(allocate());CUtexObject texture=allocate();CUsurfObject surface=allocate();
 Texture(){++wrappers;}~Texture(){release();--wrappers;}
''' + texture_methods + '};\n'
    instance = r'''
struct Instance {
 SlcDLSSStats stats;unsigned preset=0;NVSDK_NGX_Handle *feature=reinterpret_cast<int*>(allocate());
 std::unique_ptr<Texture> color,depth,diffuse,specular,normal,motion,specMotion,output;
 std::vector<float> scratch;bool history=true;
 Instance(){++instances;for(auto *p:{&color,&depth,&diffuse,&specular,&normal,&motion,&specMotion,&output})*p=std::make_unique<Texture>();scratch.resize(64);}
 ~Instance(){clear();--instances;}
 void process(const SlcDLSSFrame &,float *out,uint64_t){++processed;*out=123;}
''' + instance_methods + '};\n'
    api = '\nextern "C" {\n' + '\n'.join(method(source, s) for s in [
        'int slc_dlss_set_preset(', 'int slc_dlss_process(', 'void slc_dlss_destroy(']) + '\n}\n'
    check = r'''
Instance *handles[2];int count=0;
extern "C" void probe_setup(const char *f,int n){fault=f;count=n;manager.context=reinterpret_cast<void*>(uintptr_t(7));manager.refs=n;for(int j=0;j<n;++j)handles[j]=new Instance();}
extern "C" void *probe_handle(int i){return handles[i];}
extern "C" int probe_between(){
 assert(instances==1&&wrappers==8&&manager.refs==1&&destroys==0);assert(stack==std::vector<int>({42}));
'''
    if not baseline:
        check += r'''
 if(injected){char message[256];SlcDLSSFrame f;float out=321;
 assert(manager.cleanupFailed);assert(slc_dlss_set_preset(handles[1],0,message,sizeof(message))==0);assert(std::strstr(message,"restart"));
 assert(slc_dlss_process(handles[1],&f,&out,1,message,sizeof(message))==0);assert(out==321&&processed==0);}
'''
    check += r'''
 return 1;
}
extern "C" int probe_check(){
 assert(instances==0&&wrappers==0&&manager.refs==0);assert(stack==std::vector<int>({42}));
 if(count==0){assert(destroys==0);return 0;}
 assert(destroys==1);if(fault!="destroy")assert(allocations.empty());
'''
    if not baseline:
        check += r'''
 assert(manager.cleanupFailed==(fault!="success"));
 if(fault=="destroy")assert(manager.context!=nullptr);else assert(manager.context==nullptr);
'''
    return PRE + manager + 'unsigned currentRefs(){return manager.refs;}\n' + texture + instance + api + check + 'return 0;}\n'


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--source', type=Path, default=ROOT/'src/slg/film/denoiser/dlss/runtime.cpp')
    parser.add_argument('--out', type=Path, required=True)
    parser.add_argument('--baseline', action='store_true')
    parser.add_argument('--msvc', action='store_true')
    parser.add_argument('--no-console', action='store_true')
    args = parser.parse_args()
    source = args.source.read_text()
    records = []
    with tempfile.TemporaryDirectory(prefix='slc-dlss-cleanup-') as tmp:
        path = Path(tmp)
        cpp, driver, obj, exe = [path/n for n in ['test.cpp','caller.c','caller.o','test']]
        cpp.write_text(translation_unit(source,args.baseline));driver.write_text(DRIVER)
        assert not args.no_console or args.msvc,'GUI probe requires Windows MSVC'
        if args.msvc:
            obj=path/'caller.obj';exe=path/'test.exe'
            subprocess.run(['cl','/nologo','/TC','/c',*(['/DSLC_TEST_GUI'] if args.no_console else []),str(driver),'/Fo'+str(obj)],cwd=path,check=True,capture_output=True,text=True,timeout=30)
            build=subprocess.run(['cl','/nologo','/EHsc','/std:c++17',str(cpp),str(obj),'/Fe'+str(exe),*(['/link','/SUBSYSTEM:WINDOWS','user32.lib'] if args.no_console else [])],cwd=path,capture_output=True,text=True,timeout=40)
        else:
            flags = ['-fsanitize=address,undefined']
            subprocess.run(['clang',*flags,'-c',str(driver),'-o',str(obj)],check=True,capture_output=True,text=True,timeout=30)
            build = subprocess.run(['clang++','-std=c++17',*flags,str(cpp),str(obj),'-o',str(exe)],capture_output=True,text=True,timeout=40)
        assert build.returncode==0,build.stderr
        faults = ['success','push','shutdown-push'] if args.baseline else ['success','push','shutdown-push','sync','feature','surface','texture','array','pop','shutdown-pop','shutdown','destroy']
        def run(fault,count):
            receipt=path/'stdio.json'
            receipt.unlink(missing_ok=True)
            # The GUI process gets no STARTF_USESTDHANDLES pipes. Its actual
            # CRT fd values must be -2, rather than a simulated closed fd.
            q=subprocess.run([str(exe),fault,str(count),str(receipt)],capture_output=not args.no_console,text=True,timeout=5)
            row=dict(fault=fault,instances=count,returncode=q.returncode,stderr=q.stderr)
            if args.no_console and q.returncode==0:
                row['no_console_receipt']=json.loads(receipt.read_text())
            return row
        for fault in faults:
            for count in [1,2]:
                records.append(run(fault,count))
        null=run('success',0);null['fault']='null';records.append(null)
    args.out.write_text(json.dumps(dict(scope='Actual production teardown, buffer release, manager release and C ABI entry methods called from C; construction/driver/NGX mocked. No GPU certification. Pop errors model an asynchronous error after a consumed pop.',source_sha256=hashlib.sha256(source.encode()).hexdigest(),baseline=args.baseline,ASan_UBSan=not args.msvc,Windows_MSVC=args.msvc,no_console=args.no_console,records=records),indent=2)+'\n')
    print([(r['fault'],r['instances'],r['returncode']) for r in records])
    if args.baseline:
        assert any(r['returncode']!=0 for r in records if r['fault']=='push')
    else:
        assert all(r['returncode']==0 for r in records)


if __name__=='__main__':
    main()
