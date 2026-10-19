"""Real Film/plugin/OpenCL cleanup methods under driver-failure injection.

This checks CPU ownership and release attempts; a failing real driver cannot
be certified leak-free by a simulated device test. No Windows GPU claim.
"""
import argparse,json,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import ROOT,method
PRE=r'''
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <set>
using namespace std;using u_int=unsigned int;using cl_int=int;constexpr int CL_SUCCESS=0;
string fault;int current=42,wrappers=0,deletedPipelines=0,releaseCalls=0,contextReleases=0;
set<int> resources;int serial=0;
int clReleaseMemObject(void *value){++releaseCalls;if(fault=="release")return -5;resources.erase(int(reinterpret_cast<uintptr_t>(value)));return 0;}
int clReleaseCommandQueue(int){return 0;}
int clReleaseContext(int){++contextReleases;resources.clear();return 0;}
#define CHECK_OCL_ERROR(e) do{if((e)!=0)throw runtime_error("OpenCL error");}while(0)
template<class T> string ToString(T v){return to_string(v);}
struct HardwareDeviceBuffer{virtual ~HardwareDeviceBuffer()=default;virtual bool IsNull()const=0;virtual size_t GetSize()const=0;};
struct OpenCLDeviceBuffer:HardwareDeviceBuffer {
 void *oclBuff=reinterpret_cast<void *>(uintptr_t(++serial));
 OpenCLDeviceBuffer(){++wrappers;resources.insert(int(reinterpret_cast<uintptr_t>(oclBuff)));}
 static void operator delete(void *p){--wrappers;::operator delete(p);}
 bool IsNull()const override{return !oclBuff;}
 size_t GetSize()const override{if(fault=="size")throw runtime_error("query error");return 16;}
 DESTRUCTOR
};
struct OpenCLDevice {
 int oclQueue=1,oclContext=1;bool running=true,cleanupFailed=false;size_t memory=1024;
 void MarkCleanupError()noexcept{cleanupFailed=true;}bool HasCleanupError()const noexcept{return cleanupFailed;}
 ~OpenCLDevice();
 void FreeMemory(size_t v){memory-=v;}
 void FreeBuffer(HardwareDeviceBuffer **);
 void FreeBufferForDestruction(HardwareDeviceBuffer **)noexcept;
 void PushThreadCurrentDevice(){if(fault=="push")throw runtime_error("push");current=7;}
 void PopThreadCurrentDevice(){current=42;if(fault=="pop")throw runtime_error("pop");}
 void FinishQueue(){if(fault=="finish")throw runtime_error("finish");}
 bool IsRunning(){return running;}
 void Interrupt(){}
 void Stop(){if(fault=="stop")throw runtime_error("stop");running=false;}
};
struct Context {
 bool started=true;vector<int> idevices;vector<unique_ptr<OpenCLDevice>> devices;
 ~Context(){assert(!started);}
 void StopHardwareForDestruction()noexcept;
};
struct ImagePipelinePlugin{virtual ~ImagePipelinePlugin()=default;};
struct GammaCorrectionPlugin:ImagePipelinePlugin{
 OpenCLDevice *hardwareDevice;HardwareDeviceBuffer *hwGammaTable=new OpenCLDeviceBuffer;
 explicit GammaCorrectionPlugin(OpenCLDevice *d):hardwareDevice(d){}
 ~GammaCorrectionPlugin();
};
struct ImagePipeline{
 vector<ImagePipelinePlugin *> pipeline;
 explicit ImagePipeline(OpenCLDevice *d=nullptr){if(d)pipeline.push_back(new GammaCorrectionPlugin(d));}
 ImagePipeline *Copy()const{return new ImagePipeline();}
 ~ImagePipeline(){for(auto p:pipeline)delete p;++deletedPipelines;}
};
struct Denoiser{bool IsEnabled()const{return false;}void SetEnabled(bool){}};
struct Film {
 vector<ImagePipeline *> imagePipelines;uint64_t imagePipelineRevision=0;
 unique_ptr<jthread> imagePipelineThread;
 OpenCLDevice *hardwareDevice;
 unique_ptr<Context> ctx;unique_ptr<int> dataSet;
 int channels=0,maskMaterialIDs=0,byMaterialIDs=0,maskObjectIDs=0,byObjectIDs=0,radianceGroupCount=0,lpeExpressions=0,lpeAutomata=0,filmMetadata=0;
 Denoiser denoiser;Denoiser *filmDenoiser=&denoiser;
 HardwareDeviceBuffer *hw_IMAGEPIPELINE=nullptr,*hw_ALPHA=nullptr,*hw_OBJECT_ID=nullptr,*hw_ALBEDO=nullptr,*hw_AVG_SHADING_NORMAL=nullptr,*hw_mergeBuffer=nullptr;
 int *convTest=nullptr,*noiseEstimation=nullptr,*adaptiveError=nullptr;
 Film(bool passive=false){hardwareDevice=nullptr;if(!passive){ctx=make_unique<Context>();dataSet=make_unique<int>(1);ctx->devices.push_back(make_unique<OpenCLDevice>());hardwareDevice=ctx->devices[0].get();hw_IMAGEPIPELINE=new OpenCLDeviceBuffer;hw_ALPHA=new OpenCLDeviceBuffer;}imagePipelines.push_back(new ImagePipeline(hardwareDevice));}
 ~Film();
 void FreeChannels(){}
 void WaitAsyncExecuteImagePipeline();bool DeleteImagePipeline(ImagePipeline *);
 void DeleteHWContext()noexcept;
 void CopyDynamicSettings(const Film &);
 void SetImagePipelines(u_int,ImagePipeline *);
 void SetImagePipelines(ImagePipeline *);
 void SetImagePipelines(vector<ImagePipeline *>&);
};
'''
MAIN=r'''
int main(int argc,char **argv){fault=argv[1];string op=argv[2];bool threw=false;
 {Film f;
  if(op!="destroy"){
   try{
    if(op=="copy"){Film source(true);try{f.CopyDynamicSettings(source);}catch(...){assert(f.imagePipelines.size()==1);throw;}}
    if(op=="index")f.SetImagePipelines(0,new ImagePipeline(f.hardwareDevice));
    if(op=="scalar")f.SetImagePipelines(new ImagePipeline(f.hardwareDevice));
    if(op=="vector"){vector<ImagePipeline *> replacement{new ImagePipeline(f.hardwareDevice),new ImagePipeline(f.hardwareDevice)};f.SetImagePipelines(replacement);}
   }catch(const runtime_error &){threw=true;}
   bool expected=fault=="push"||fault=="finish"||fault=="pop"||fault=="size"||fault=="release";
   assert(threw==expected);assert(deletedPipelines==(op=="copy"?2:1));
   assert(f.imagePipelines.size()==(op=="vector"?2:1));
  }
 }
 assert(wrappers==0);assert(resources.empty());assert(contextReleases==1);
 assert(deletedPipelines==(op=="destroy"?1:(op=="vector"||op=="copy")?3:2));
 puts("PASS owned wrappers/pipelines retired exactly once, context reclaimed and API failure preserves replacement ownership");}
'''
def main():
 a=argparse.ArgumentParser();a.add_argument('--root',type=Path,default=ROOT);a.add_argument('--out',type=Path,required=True);args=a.parse_args();root=args.root
 header=(root/'include/luxrays/devices/ocldevice.h').read_text();source=(root/'src/slg/film/filmimagepipeline.cpp').read_text();body=PRE.replace('DESTRUCTOR',method(header,'virtual ~OpenCLDeviceBuffer()'))
 for path,sigs in [('src/luxrays/devices/ocldevice.cpp',['OpenCLDevice::~OpenCLDevice()','void OpenCLDevice::FreeBufferForDestruction(','void OpenCLDevice::FreeBuffer(']),('src/luxrays/core/context.cpp',['void Context::StopHardwareForDestruction(']),('src/slg/film/imagepipeline/plugins/gammacorrection.cpp',['GammaCorrectionPlugin::~GammaCorrectionPlugin()']),('src/slg/film/filmimagepipeline.cpp',['bool Film::DeleteImagePipeline(','void Film::WaitAsyncExecuteImagePipeline(','void Film::SetImagePipelines(const u_int','void Film::SetImagePipelines(ImagePipeline *','void Film::SetImagePipelines(std::vector<ImagePipeline *> &']),('src/slg/film/filmimagepipelinehw.cpp',['void Film::DeleteHWContext(']),('src/slg/film/film.cpp',['Film::~Film()','void Film::CopyDynamicSettings('])]:
  s=(root/path).read_text();body+='\n'.join(method(s,sig) for sig in sigs)
 rows=[]
 with tempfile.TemporaryDirectory(prefix='slc-film-owned-') as tmp:
  src=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';src.write_text(body+MAIN);q=subprocess.run(['clang++','-std=c++20','-pthread','-fsanitize=address,undefined',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=45);assert q.returncode==0,q.stderr
  for fault in ['success','push','finish','pop','size','release','stop']:
   for op in ['destroy','index','scalar','vector','copy']:
    q=subprocess.run([str(exe),fault,op],capture_output=True,text=True,timeout=5);rows.append(dict(fault=fault,operation=op,returncode=q.returncode,stdout=q.stdout,stderr=q.stderr))
 args.out.write_text(json.dumps(dict(scope='production Film, Gamma, OpenCL buffer/device and hardware Context retirement; ASan/UBSan with simulated driver',records=rows),indent=2)+'\n');print([(r['fault'],r['operation'],r['returncode']) for r in rows]);assert all(r['returncode']==0 for r in rows)
if __name__=='__main__':main()
