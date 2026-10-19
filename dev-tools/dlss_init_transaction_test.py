"""Actual manager/create/destroy bodies with deterministic CUDA/NGX init faults.

No GPU or SDK behavior certification. Construction/driver state is mocked; the
full production manager and C API method bodies are compiled without rewriting.
"""
import argparse,json,os,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import ROOT,method
from dlss_runtime_cleanup_test import DRIVER

PRE=r'''
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
using CUcontext=void*;using CUdevice=int;using CUresult=int;using NVSDK_NGX_Result=unsigned;
constexpr int CUDA_SUCCESS=0,CUEW_INIT_CUDA=1,CUEW_SUCCESS=0;
constexpr unsigned NGX_SUCCESS=1,NGX_FAILURE=0xBAD00001;
#define NVSDK_NGX_FAILED(r) ((unsigned(r)&0xFFF00000u)==0xBAD00000u)
constexpr int NVSDK_NGX_Version_API=1,NVSDK_NGX_Feature_RayReconstruction=1,NVSDK_NGX_Application_Identifier_Type_Project_Id=1,NVSDK_NGX_ENGINE_TYPE_CUSTOM=1,NVSDK_NGX_FeatureSupportResult_Supported=1;
struct NVSDK_NGX_FeatureCommonInfo {struct {const wchar_t **Path=nullptr;unsigned Length=0;}PathListInfo;};
struct NVSDK_NGX_FeatureDiscoveryInfo {
 int SDKVersion=0,FeatureID=0;struct {int IdentifierType=0;struct {struct {const char *id=nullptr;int type=0;const char *version=nullptr;}ProjectDesc;}v;}Identifier;
 const wchar_t *ApplicationDataPath=nullptr;NVSDK_NGX_FeatureCommonInfo *FeatureInfo=nullptr;
};
struct NVSDK_NGX_FeatureRequirement {int FeatureSupported=NVSDK_NGX_FeatureSupportResult_Supported;};
std::string fault;std::set<std::string> used;std::set<uintptr_t> contexts;std::vector<uintptr_t> stack{42};
unsigned creates=0,destroys=0,initialPops=0,scopePops=0,instances=0,initializations=0,shutdowns=0;bool ngxActive=false;
bool inRollback=false,afterProbe=false,allocationFault=false;size_t cacheAllocation=0;
void *operator new(size_t n){
 // std::string implementations may round capacity; target the first large
 // allocation at the selected phase rather than a libc++/MSVC byte count.
 if(cacheAllocation&&n>=cacheAllocation&&!allocationFault&&((fault=="publication-alloc"&&afterProbe)||fault=="stage-alloc")){allocationFault=true;throw std::bad_alloc();}
 if(void *p=std::malloc(n?n:1))return p;throw std::bad_alloc();
}
void operator delete(void *p)noexcept{std::free(p);}
void operator delete(void *p,size_t)noexcept{std::free(p);}
bool hit(const char *name){if(used.count(name)||fault.find(name)==std::string::npos)return false;used.insert(name);return true;}
CUcontext pointer(uintptr_t n){return reinterpret_cast<void*>(n);}
int cuewInit(int){return hit("driver")?99:0;}
int cuInit(int){return hit("cuinit")?99:0;}
int cuDeviceGet(CUdevice *d,unsigned){*d=0;return hit("device")?99:0;}
int cuCtxCreate(CUcontext *out,int,CUdevice){if(hit("create"))return 99;*out=pointer(7+(++creates));contexts.insert(uintptr_t(*out));stack.push_back(uintptr_t(*out));return 0;}
int cuCtxPushCurrent(CUcontext c){if(hit(ngxActive?"rollback-push":"init-push"))return 99;assert(contexts.count(uintptr_t(c)));stack.push_back(uintptr_t(c));return 0;}
int cuCtxPopCurrent(CUcontext *c){assert(stack.size()>1);*c=pointer(stack.back());
 if(hit(inRollback?"rollback-pop-held":ngxActive?"init-pop-held":"initial-pop-held"))return 99;
 stack.pop_back();
 // Model a prior asynchronous error reported after a consumed pop.
 if(inRollback)return hit("rollback-pop")?99:0;
 if(!ngxActive){++initialPops;return hit("initial-pop")?99:0;}
 ++scopePops;return hit("init-pop")?99:0;
}
int cuCtxDestroy(CUcontext c){++destroys;inRollback=false;if(hit("destroy"))return 99;assert(contexts.erase(uintptr_t(c))==1);if(stack.back()==uintptr_t(c))stack.pop_back();return 0;}
unsigned NVSDK_NGX_CUDA_Init_with_ProjectID(const char *,int,const char *,const wchar_t *,NVSDK_NGX_FeatureCommonInfo *){
 ++initializations;if(hit("ngx-init")||ngxActive)return NGX_FAILURE;assert(stack.size()>1);ngxActive=true;return NGX_SUCCESS;}
unsigned NVSDK_NGX_CUDA_GetFeatureRequirements(CUdevice,NVSDK_NGX_FeatureDiscoveryInfo *,NVSDK_NGX_FeatureRequirement *r){assert(ngxActive&&stack.size()>1);if(hit("requirements"))return NGX_FAILURE;if(hit("unsupported"))r->FeatureSupported=0;afterProbe=true;return NGX_SUCCESS;}
unsigned NVSDK_NGX_CUDA_Shutdown(){++shutdowns;inRollback=true;assert(ngxActive&&stack.size()>1);if(hit("shutdown"))return NGX_FAILURE;ngxActive=false;return NGX_SUCCESS;}
void cuda(CUresult r){if(r)throw std::runtime_error("CUDA result "+std::to_string(r));}
void ngx(NVSDK_NGX_Result r){if(NVSDK_NGX_FAILED(r))throw std::runtime_error("NGX result "+std::to_string(r));}
void error(char *p,uint32_t n,const char *s){if(p&&n){std::strncpy(p,s,n-1);p[n-1]=0;}}
std::mutex mutex;
'''
INSTANCE=r'''
struct Instance {
 explicit Instance(uint64_t){if(hit("instance"))throw std::bad_alloc();++instances;}
 ~Instance(){--instances;}
 void abandon()noexcept{}
};
'''
CHECK=r'''
void *handles[2]={};int count=0;bool firstSucceeded=false;unsigned createsAfterFirst=0,refsAfterFirst=0,contextsAfterFirst=0;
extern "C" void probe_setup(const char *f,int n){fault=f;count=n;char message[1024];const char *cache=std::getenv("SLC_INIT_TEST_CACHE");assert(cache);cacheAllocation=std::strlen(cache)+1;
 for(int j=0;j<n;++j){handles[j]=slc_dlss_create(0,(fault=="mismatch"&&j==1)?"different":"runtime",cache,4096,message,sizeof(message));if(j==0){firstSucceeded=handles[j]!=nullptr;createsAfterFirst=creates;refsAfterFirst=manager.refs;contextsAfterFirst=unsigned(contexts.size());}}
}
extern "C" void *probe_handle(int i){return handles[i];}
extern "C" int probe_between(){return 1;}
extern "C" int probe_check(){
 bool clean=contexts.empty(),stackOK=stack==std::vector<uintptr_t>({42});
 std::printf("STATE first_succeeded=%d first_refs=%u first_contexts=%u final_contexts=%zu final_refs=%u allocation_fault=%d\n",int(firstSucceeded),refsAfterFirst,contextsAfterFirst,contexts.size(),manager.refs,int(allocationFault));
 bool expectSuccess=fault=="success"||fault=="mismatch"||fault=="publication-alloc";
 if(firstSucceeded!=expectSuccess){std::printf("FAIL init result: first succeeded=%d, expected=%d\n",int(firstSucceeded),int(expectSuccess));return 21;}
 if(instances||manager.refs){std::puts("FAIL CPU instance/ref ownership");return 22;}
 if(fault.find("destroy")!=std::string::npos){
  if(contexts.size()!=1||!manager.context||!manager.cleanupFailed||creates!=createsAfterFirst){std::puts("FAIL failed-destroy context identity/retry guard");return 23;}
 }else if(!clean||!stackOK){std::printf("FAIL unowned contexts=%zu, stack=%zu, destroys=%u\n",contexts.size(),stack.size(),destroys);return 24;}
 if(manager.cleanupFailed&&count==2&&creates!=createsAfterFirst){std::puts("FAIL poisoned initialization retried");return 25;}
 if(fault.find("shutdown")!=std::string::npos||fault.find("rollback-push")!=std::string::npos){if(!manager.cleanupFailed||!ngxActive){std::puts("FAIL failed shutdown hidden");return 26;}}
 else if(ngxActive){std::puts("FAIL NGX shutdown skipped");return 27;}
 if(used.count("rollback-pop")&&!manager.cleanupFailed){std::puts("FAIL rollback pop ignored");return 28;}
 if(fault=="mismatch"&&handles[1]){std::puts("FAIL incompatible shared manager accepted");return 29;}
 if(fault=="stage-alloc"&&createsAfterFirst!=0){std::puts("FAIL native context acquired before fallible publication preparation");return 30;}
 std::printf("PASS contexts created=%u destroyed=%u retained=%zu; init=%u shutdown=%u; poisoned=%d\n",creates,destroys,contexts.size(),initializations,shutdowns,int(manager.cleanupFailed));return 0;
}
'''

def main():
    a=argparse.ArgumentParser();a.add_argument('--source',type=Path,default=ROOT/'src/slg/film/denoiser/dlss/runtime.cpp');a.add_argument('--out',type=Path,required=True);a.add_argument('--baseline',action='store_true');a.add_argument('--msvc',action='store_true');a.add_argument('--no-console',action='store_true');args=a.parse_args()
    source=args.source.read_text();manager=source[source.index('struct Manager {'):source.index('struct Texture {')]
    identity=source[source.index('constexpr const char *projectId='):source.index('std::mutex mutex;')]
    unit=PRE+identity+manager+INSTANCE+'extern "C" {\n'+method(source,'void *slc_dlss_create(')+method(source,'void slc_dlss_destroy(')+'}\n'+CHECK
    rows=[]
    with tempfile.TemporaryDirectory(prefix='slc-dlss-init-') as temp:
        t=Path(temp);cpp=t/'test.cpp';driver=t/'caller.c';obj=t/'caller.o';exe=t/'test';cpp.write_text(unit);driver.write_text(DRIVER)
        if args.msvc:
            obj=t/'caller.obj';exe=t/'test.exe'
            subprocess.run(['cl','/nologo','/TC','/c',*(['/DSLC_TEST_GUI'] if args.no_console else []),str(driver),'/Fo'+str(obj)],cwd=t,capture_output=True,text=True,check=True,timeout=30)
            build=subprocess.run(['cl','/nologo','/EHsc','/std:c++17',str(cpp),str(obj),'/Fe'+str(exe),*(['/link','/SUBSYSTEM:WINDOWS','user32.lib'] if args.no_console else [])],cwd=t,capture_output=True,text=True,timeout=40)
        else:
            assert not args.no_console
            subprocess.run(['clang','-fsanitize=address,undefined','-c',str(driver),'-o',str(obj)],capture_output=True,text=True,check=True,timeout=30)
            build=subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',str(cpp),str(obj),'-o',str(exe)],capture_output=True,text=True,timeout=40)
        assert build.returncode==0,build.stderr+build.stdout
        faults=['success','initial-pop','init-pop','requirements+destroy','requirements+shutdown','publication-alloc'] if args.baseline else ['success','driver','cuinit','device','create','initial-pop','initial-pop-held','init-push','directory','ngx-init','requirements','unsupported','init-pop','init-pop-held','instance','requirements+destroy','initial-pop+destroy','requirements+shutdown','init-pop+rollback-push','requirements+rollback-pop','publication-alloc','stage-alloc','mismatch']
        for fault in faults:
            for n in ([2] if fault=='mismatch' else [1,2]):
                cache=t/(fault+'-'+str(n));
                if fault=='directory':cache.write_text('not a directory')
                receipt=t/'stdio.json';receipt.unlink(missing_ok=True)
                q=subprocess.run([str(exe),fault,str(n),str(receipt)],env={**os.environ,'SLC_INIT_TEST_CACHE':str(cache)},capture_output=not args.no_console,text=True,timeout=5)
                row=dict(fault=fault,attempts=n,returncode=q.returncode,stdout=q.stdout,stderr=q.stderr)
                if args.no_console and q.returncode==0:row['no_console_receipt']=json.loads(receipt.read_text())
                rows.append(row)
    import hashlib
    args.out.write_text(json.dumps(dict(scope='Complete production manager/init/create/destroy bodies, actual C caller, mocked CUDA/NGX/construction; no real GPU/SDK recovery or quality proof. Selected pop models consume the entry or retain it; arbitrary driver state is not certified. Targeted standard allocation failure tests preallocation/publication ownership.',source_sha256=hashlib.sha256(source.encode()).hexdigest(),baseline=args.baseline,Windows_MSVC=args.msvc,no_console=args.no_console,ASan_UBSan=not args.msvc,records=rows),indent=2)+'\n')
    print([(r['fault'],r['attempts'],r['returncode']) for r in rows])
    assert any(r['returncode']!=0 for r in rows) if args.baseline else all(r['returncode']==0 for r in rows)

if __name__=='__main__':main()
