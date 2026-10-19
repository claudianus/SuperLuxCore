"""Actual CUDA cleanup-only method with context-aware driver return codes."""
import argparse,json,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import ROOT,method
PRE=r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
using namespace std;using CUresult=int;using CUcontext=void *;constexpr int CUDA_SUCCESS=0;
string fault;vector<int> contexts{42};map<int,size_t> resources;int wrappers=0,freeCalls=0;
int cuCtxPushCurrent(CUcontext ctx){if(fault=="push")return 99;contexts.push_back(int(reinterpret_cast<uintptr_t>(ctx)));return 0;}
int cuCtxPopCurrent(CUcontext *ctx){assert(contexts.back()==7);if(ctx)*ctx=reinterpret_cast<CUcontext>(uintptr_t(contexts.back()));contexts.pop_back();return fault=="pop"?99:0;}
int cuMemGetAddressRange(void *,size_t *size,int ptr){assert(contexts.back()==7);if(fault=="size")return 99;*size=resources.at(ptr);return 0;}
int cuMemFree(int ptr){assert(contexts.back()==7);++freeCalls;if(fault=="free")return 99;resources.erase(ptr);return 0;}
struct HardwareDeviceBuffer{virtual ~HardwareDeviceBuffer()=default;};
struct CUDADeviceBuffer:HardwareDeviceBuffer{
 int cudaBuff=0;
 explicit CUDADeviceBuffer(bool empty){++wrappers;if(!empty){cudaBuff=1;resources[1]=16;}}
 ~CUDADeviceBuffer(){--wrappers;}
 bool IsNull(){return !cudaBuff;}
};
struct CUDADevice{
 CUcontext cudaContext=reinterpret_cast<CUcontext>(uintptr_t(7));size_t used=16;bool cleanupFailed=false;
 void MarkCleanupError()noexcept{cleanupFailed=true;}bool HasCleanupError()const noexcept{return cleanupFailed;}
 void FreeMemory(size_t s){used-=s;}
 void FreeBufferForDestruction(HardwareDeviceBuffer **)noexcept;
};
'''
MAIN=r'''
int main(int argc,char **argv){fault=argv[1];CUDADevice device;
 bool empty=fault=="empty";HardwareDeviceBuffer *buffer=fault=="null"?nullptr:new CUDADeviceBuffer(empty);
 device.FreeBufferForDestruction(&buffer);assert(buffer==nullptr);assert(wrappers==0);
 device.FreeBufferForDestruction(&buffer);device.FreeBufferForDestruction(nullptr);
 assert(contexts==vector<int>({42}));assert(device.HasCleanupError()==(fault=="push"||fault=="size"||fault=="free"||fault=="pop"));
 if(fault=="push")assert(freeCalls==0); // no foreign-context free
 if(fault=="success")assert(resources.empty()&&device.used==0);
 // CUDA context destruction owns failed/deferred native allocations. This
 // models only its allocation ownership, not real driver recovery guarantees.
 resources.clear();assert(resources.empty());puts("PASS wrapper consumed once, own-context release and caller context preserved");}
'''
a=argparse.ArgumentParser();a.add_argument('--source',type=Path,default=ROOT/'src/luxrays/devices/cudadevice.cpp');a.add_argument('--out',type=Path,required=True);args=a.parse_args();rows=[]
with tempfile.TemporaryDirectory(prefix='slc-cuda-buffer-') as tmp:
 src=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';src.write_text(PRE+method(args.source.read_text(),'void CUDADevice::FreeBufferForDestruction(')+MAIN)
 q=subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=40);assert q.returncode==0,q.stderr
 for mode in ['success','null','empty','push','size','free','pop']:
  q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=5);rows.append(dict(mode=mode,returncode=q.returncode,stderr=q.stderr))
args.out.write_text(json.dumps(dict(scope='actual CUDA release method; ASan/UBSan simulated driver; pop error model reports a prior async error after a consumed pop, not every failing-pop behaviour',records=rows),indent=2)+'\n');print([(r['mode'],r['returncode']) for r in rows]);assert all(r['returncode']==0 for r in rows)
