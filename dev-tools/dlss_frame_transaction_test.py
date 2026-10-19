"""Production frame validation/evaluation/publication with faulting driver stubs.

Compiles the full Instance::process, Parameters and C entry bodies unchanged.
Only GPU textures/resize, CUDA/NGX and instance construction are simulated.
"""
import argparse,hashlib,json,os,re,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import ROOT,method

PRE=r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
using CUcontext=void*;using CUresult=int;using NVSDK_NGX_Result=unsigned;using NVSDK_NGX_Handle=int;
constexpr int CUDA_SUCCESS=0;constexpr unsigned NGX_SUCCESS=1,NGX_FAILURE=0xBAD00001u;
#define NVSDK_NGX_FAILED(r) ((unsigned(r)&0xFFF00000u)==0xBAD00000u)
std::string fault;std::vector<int> stack{42};bool popFaultUsed=false;unsigned evaluations=0,liveParameters=0,pushes=0,pops=0;
struct Manager{CUcontext context=reinterpret_cast<void*>(uintptr_t(7));bool cleanupFailed=false;}manager;
std::mutex mutex;
int cuCtxPushCurrent(CUcontext){++pushes;if(fault=="push")return 99;stack.push_back(7);return 0;}
int cuCtxPopCurrent(CUcontext *c){++pops;assert(stack.back()==7);*c=reinterpret_cast<void*>(uintptr_t(7));
 bool fail=fault.find("pop")!=std::string::npos&&!popFaultUsed;popFaultUsed|=fail;
 if(!(fail&&fault=="pop-held"))stack.pop_back();return fail?99:0;}
int cuCtxSynchronize(){assert(stack.back()==7);return fault=="sync"?99:0;}
void cuda(int r){if(r)throw std::runtime_error("CUDA result "+std::to_string(r));}
void ngx(unsigned r){if(NVSDK_NGX_FAILED(r))throw std::runtime_error("NGX result "+std::to_string(r));}
void error(char *p,uint32_t n,const char *s){if(p&&n){std::strncpy(p,s,n-1);p[n-1]=0;}}
struct NVSDK_NGX_Parameter{template<class T>void Set(const char *,T){assert(stack.back()==7);}}parameterStorage;
unsigned NVSDK_NGX_CUDA_AllocateParameters(NVSDK_NGX_Parameter **p){assert(stack.back()==7);if(fault=="parameter-allocate")return NGX_FAILURE;assert(!liveParameters);++liveParameters;*p=&parameterStorage;return NGX_SUCCESS;}
unsigned NVSDK_NGX_CUDA_DestroyParameters(NVSDK_NGX_Parameter *){assert(stack.back()==7&&liveParameters==1);if(fault=="parameter-destroy")return NGX_FAILURE;--liveParameters;return NGX_SUCCESS;}
unsigned NVSDK_NGX_CUDA_EvaluateFeature(int *,NVSDK_NGX_Parameter *,void *){assert(stack.back()==7);++evaluations;return fault.find("evaluate")!=std::string::npos?NGX_FAILURE:NGX_SUCCESS;}
struct Texture{
 unsigned width,height,channels;uint64_t texture=1,surface=2;
 Texture(unsigned w,unsigned h,unsigned c):width(w),height(h),channels(c){}
 void upload(const float *){assert(stack.back()==7);if(fault=="upload")throw std::runtime_error("upload failed");}
 void download(float *p){assert(stack.back()==7);if(fault=="download")throw std::runtime_error("download failed");
  const float pixel[]={.25f,.5f,1.5f,1.4f};for(size_t i=0;i<size_t(width)*height*channels;++i)p[i]=pixel[i%4];
  if(fault=="nonfinite-output")p[0]=std::numeric_limits<float>::quiet_NaN();}
};
'''
INSTANCE=r'''
struct Instance{
 uint64_t limit=1<<24;SlcDLSSStats stats={};int *feature=reinterpret_cast<int*>(uintptr_t(1));
 std::unique_ptr<Texture> color,depth,diffuse,specular,normal,motion,specMotion,output;
 std::vector<float> scratch;uint64_t lastId=0,lastScene=0;bool history=false;
 bool resize(const SlcDLSSFrame &f){
  if(fault=="resize")throw std::bad_alloc();
  if(output&&output->width==f.output_width&&output->height==f.output_height)return false;
  color=std::make_unique<Texture>(f.width,f.height,4);depth=std::make_unique<Texture>(f.width,f.height,1);
  diffuse=std::make_unique<Texture>(f.width,f.height,4);specular=std::make_unique<Texture>(f.width,f.height,4);normal=std::make_unique<Texture>(f.width,f.height,4);
  motion=std::make_unique<Texture>(f.width,f.height,2);specMotion=std::make_unique<Texture>(f.width,f.height,2);output=std::make_unique<Texture>(f.output_width,f.output_height,4);
  scratch.resize(size_t(f.output_width)*f.output_height*4);stats.width=f.width;stats.height=f.height;stats.output_width=f.output_width;stats.output_height=f.output_height;return true;
 }
'''
CHECK=r'''
Instance instance;SlcDLSSFrame frame={};std::vector<std::vector<float>> inputs,beforeInputs;std::vector<float> output,beforeOutput;
uint64_t capacity;int mode;bool expectedSuccess;unsigned committedBefore=0;
extern "C" void probe_setup(const char *name,int m){fault=name;mode=m;frame.width=frame.height=32;frame.output_width=frame.output_height=m==1?32:64;frame.independent_samples=1;frame.frame_id=1;frame.scene_serial=1;
 const unsigned channels[]={4,1,4,4,4,2,2};for(unsigned c:channels)inputs.emplace_back(32*32*c,.4f);
 for(size_t i=0;i<1024;++i)inputs[0][i*4+3]=float(i%7)/6.f;
 frame.color=inputs[0].data();frame.depth=inputs[1].data();frame.diffuse=inputs[2].data();frame.specular=inputs[3].data();frame.normal_roughness=inputs[4].data();frame.motion=inputs[5].data();frame.specular_motion=inputs[6].data();
 output.assign(size_t(frame.output_width)*frame.output_height*4,-123.5f);capacity=output.size();expectedSuccess=fault=="success";
 if(fault=="duplicate"){
  const auto saved=fault;fault="success";char message[256];assert(slc_dlss_process(&instance,&frame,output.data(),capacity,message,sizeof(message))==1);fault=saved;
  std::fill(output.begin(),output.end(),-123.5f);committedBefore=1;
 }
 if(fault=="dimensions")frame.width=0;if(fault=="independent")frame.independent_samples=0;
 if(fault=="jitter")frame.jitter_x=.75f;if(fault=="guide-missing")frame.motion=nullptr;
 if(fault=="nonfinite-input")inputs[0][0]=std::numeric_limits<float>::quiet_NaN();
 if(fault=="depth")inputs[1][0]=-1;if(fault=="roughness")inputs[4][3]=1.2f;if(fault=="alpha")inputs[0][3]=1.2f;
 if(fault=="capacity")capacity=1;if(fault=="budget")instance.limit=1;
 if(fault=="poisoned")manager.cleanupFailed=true;
 beforeInputs=inputs;beforeOutput=output;
}
extern "C" void *probe_handle(){return fault=="null-handle"?nullptr:&instance;}
extern "C" const void *probe_frame(){return fault=="null-frame"?nullptr:&frame;}
extern "C" float *probe_output(){return fault=="null-output"?nullptr:fault=="alias"?inputs[0].data():output.data();}
extern "C" uint64_t probe_capacity(){return capacity;}
extern "C" int probe_check(int result,const char *message){
 bool rawUnchanged=true;for(size_t j=0;j<inputs.size();++j)rawUnchanged&=std::memcmp(inputs[j].data(),beforeInputs[j].data(),inputs[j].size()*4)==0;
 bool outUnchanged=std::memcmp(output.data(),beforeOutput.data(),output.size()*4)==0;
 std::printf("STATE result=%d frames=%llu output_unchanged=%d raw_unchanged=%d poisoned=%d parameters_retained=%u fake_evaluations=%u\n",result,(unsigned long long)instance.stats.frames,int(outUnchanged),int(rawUnchanged),int(manager.cleanupFailed),liveParameters,evaluations);
 if(bool(result)!=expectedSuccess)return 21;if(!rawUnchanged)return 22;
 if(!expectedSuccess){if(!outUnchanged||!message[0])return 23;if(instance.stats.frames!=committedBefore)return 24;}
 else{
  if(instance.stats.frames!=1||!instance.history||instance.lastId!=frame.frame_id||instance.lastScene!=frame.scene_serial)return 25;
  for(size_t i=0;i<output.size()/4;++i){const float alpha=mode==1?inputs[0][i*4+3]:1.f;
   const float expected[]={.25f,.5f,1.5f,alpha};if(std::memcmp(output.data()+i*4,expected,sizeof(expected)))return 26;}
 }
 bool cleanup=fault=="push"||fault.find("pop")!=std::string::npos||fault=="parameter-destroy"||fault=="poisoned";
 if(manager.cleanupFailed!=cleanup)return 27;
 if(liveParameters!=(fault=="parameter-destroy"?1u:0u))return 28;
 if(fault=="pop-held"){if(stack!=std::vector<int>({42,7}))return 29;}
 else if(stack!=std::vector<int>({42}))return 30;
 if(cleanup){auto prior=output;unsigned calls=evaluations;char msg[256];++frame.frame_id;
  if(slc_dlss_process(&instance,&frame,output.data(),capacity,msg,sizeof(msg))!=0||!msg[0]||evaluations!=calls||std::memcmp(prior.data(),output.data(),output.size()*4))return 31;}
 return 0;
}
'''
DRIVER=r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <sys/resource.h>
#endif
void probe_setup(const char *,int);void *probe_handle(void);const void *probe_frame(void);float *probe_output(void);uint64_t probe_capacity(void);int probe_check(int,const char *);
int slc_dlss_process(void *,const void *,float *,uint64_t,char *,uint32_t);
int run(int argc,char **argv){
#ifdef _WIN32
 SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
#else
 struct rlimit limit={0,0};setrlimit(RLIMIT_CORE,&limit);
#endif
 probe_setup(argv[1],argv[2][0]-'0');char message[1024]={0};int result=slc_dlss_process(probe_handle(),probe_frame(),probe_output(),probe_capacity(),message,sizeof(message));int checked=probe_check(result,message);
#ifdef SLC_TEST_GUI
 if(_fileno(stdout)!=-2||_fileno(stderr)!=-2)return 12;
 FILE *receipt=fopen(argv[3],"w");if(!receipt)return 13;fprintf(receipt,"{\"stdout_fd\":%d,\"stderr_fd\":%d}\n",_fileno(stdout),_fileno(stderr));fclose(receipt);
#endif
 return checked;
}
#ifdef SLC_TEST_GUI
int WINAPI WinMain(HINSTANCE a,HINSTANCE b,LPSTR c,int d){return run(__argc,__argv);}
#else
int main(int argc,char **argv){return run(argc,argv);}
#endif
'''

def main():
    p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'src/slg/film/denoiser/dlss/runtime.cpp');p.add_argument('--out',type=Path,required=True);p.add_argument('--baseline',action='store_true');p.add_argument('--msvc',action='store_true');p.add_argument('--no-console',action='store_true');a=p.parse_args()
    source=a.source.read_text();header=(ROOT/'src/slg/film/denoiser/dlss/api.h').read_text();types=header[header.index('typedef struct SlcDLSSFrame'):header.index('// Synchronous API')]
    constants='\n'.join('constexpr const char *'+n+'="'+n+'";' for n in sorted(set(re.findall(r'NVSDK_NGX_Parameter_\w+',method(source,'void process(const SlcDLSSFrame')))))
    helpers=method(source,'bool cudaCleanup(')+method(source,'bool ngxCleanup(')
    if 'struct Scope {' in source:helpers+=source[source.index('struct Scope {'):source.index('void releaseManager(')]
    parameters=source[source.index('struct Parameters {'):source.index('struct Instance {')]
    unit=PRE+types+constants+helpers+parameters+INSTANCE+method(source,'void process(const SlcDLSSFrame')+'};\nextern "C" {\n'+method(source,'int slc_dlss_process(')+'}\n'+CHECK
    records=[]
    with tempfile.TemporaryDirectory(prefix='slc-frame-transaction-') as temp:
        t=Path(temp);cpp=t/'test.cpp';driver=t/'caller.c';obj=t/'caller.o';exe=t/'test';cpp.write_text(unit);driver.write_text(DRIVER)
        if a.msvc:
            obj=t/'caller.obj';exe=t/'test.exe'
            subprocess.run(['cl','/nologo','/TC','/c',*(['/DSLC_TEST_GUI'] if a.no_console else []),str(driver),'/Fo'+str(obj)],cwd=t,capture_output=True,text=True,check=True,timeout=30)
            build=subprocess.run(['cl','/nologo','/EHsc','/std:c++17',str(cpp),str(obj),'/Fe'+str(exe),*(['/link','/SUBSYSTEM:WINDOWS','user32.lib'] if a.no_console else [])],cwd=t,capture_output=True,text=True,timeout=40)
        else:
            assert not a.no_console
            subprocess.run(['clang','-fsanitize=address,undefined','-c',str(driver),'-o',str(obj)],capture_output=True,text=True,check=True,timeout=30)
            build=subprocess.run(['clang++','-std=c++17','-fsanitize=address,undefined',str(cpp),str(obj),'-o',str(exe)],capture_output=True,text=True,timeout=40)
        assert build.returncode==0,build.stdout+build.stderr
        faults=['success','pop','pop-held','parameter-destroy'] if a.baseline else ['success','push','pop','pop-held','parameter-allocate','parameter-destroy','evaluate','evaluate+pop','sync','download','upload','resize','nonfinite-output','dimensions','independent','jitter','guide-missing','nonfinite-input','depth','roughness','alpha','capacity','budget','duplicate','null-handle','null-frame','null-output','alias','poisoned']
        for fault in faults:
            for mode in [1,2]:
                receipt=t/'stdio.json';receipt.unlink(missing_ok=True)
                q=subprocess.run([str(exe),fault,str(mode),str(receipt)],capture_output=not a.no_console,text=True,timeout=5);row=dict(fault=fault,size='native' if mode==1 else 'upscale',returncode=q.returncode,stdout=q.stdout,stderr=q.stderr)
                if a.no_console and q.returncode==0:row['no_console_receipt']=json.loads(receipt.read_text())
                records.append(row)
    a.out.write_text(json.dumps(dict(scope='Full production frame body/Parameters/C entry, direct C caller, actual public frame/stats declarations; textures/resize and driver/NGX mocked. Native/upscale positive fixtures verify exact RGB/alpha and bit-exact raw preservation, not real DLSS quality. Selected pop models consume or retain stack entry; no arbitrary driver recovery proof.',source_sha256=hashlib.sha256(source.encode()).hexdigest(),baseline=a.baseline,Windows_MSVC=a.msvc,no_console=a.no_console,ASan_UBSan=not a.msvc,actual_NGX_evaluations=0,records=records),indent=2)+'\n')
    print([(r['fault'],r['size'],r['returncode']) for r in records]);assert any(r['returncode']!=0 for r in records) if a.baseline else all(r['returncode']==0 for r in records)

if __name__=='__main__':main()
