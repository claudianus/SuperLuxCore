"""Actual OptiX destructor: no thrown driver error skips owned buffers."""
import argparse,json,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import ROOT,method
PRE=r'''
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace std;using OptixResult=int;constexpr int OPTIX_SUCCESS=0;
string fault;vector<int> stack{42,7};int destroyed=0,freed=0,live=0;
int optixDenoiserDestroy(int){assert(stack.back()==7);++destroyed;return fault=="optix"?99:0;}
struct CUDADevice {
 bool cleanupFailed=false;void MarkCleanupError()noexcept{cleanupFailed=true;}bool HasCleanupError()const noexcept{return cleanupFailed;}
 void PushThreadCurrentDevice(){if(fault=="push")throw runtime_error("push");stack.push_back(7);}
 void PopThreadCurrentDevice(){assert(stack.back()==7);stack.pop_back();if(fault=="pop")throw runtime_error("pop");}
 void FreeBufferForDestruction(int **p)noexcept{if(*p){delete *p;*p=nullptr;++freed;--live;}}
};
int *buffer(){++live;return new int(1);}
struct OptixDenoiserPlugin{
 CUDADevice *cudaDevice;int denoiserHandle=1;
 int *denoiserStateScratchBuff=buffer(),*denoiserTmpBuff=buffer(),*albedoTmpBuff=buffer(),*avgShadingNormalTmpBuff=buffer();
 explicit OptixDenoiserPlugin(CUDADevice *d):cudaDevice(d){}
 ~OptixDenoiserPlugin();
};
'''
MAIN=r'''
int main(int argc,char **argv){fault=argv[1];CUDADevice d;{OptixDenoiserPlugin p(&d);if(fault=="empty-handle")p.denoiserHandle=0;}
 assert(freed==4&&live==0);assert(d.HasCleanupError()==(fault=="push"||fault=="pop"||fault=="optix"));assert(stack==vector<int>({42,7}));assert(destroyed==((fault=="push"||fault=="empty-handle")?0:1));puts("PASS context restored, handle release guarded and all four buffer owners consumed");}
'''
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'src/slg/film/imagepipeline/plugins/optixdenoiser.cpp');p.add_argument('--out',type=Path,required=True);a=p.parse_args();rows=[]
with tempfile.TemporaryDirectory(prefix='slc-optix-cleanup-') as tmp:
 src=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';src.write_text(PRE+method(a.source.read_text(),'OptixDenoiserPlugin::~OptixDenoiserPlugin()')+MAIN);q=subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=40);assert q.returncode==0,q.stderr
 for mode in ['success','push','pop','optix','empty-handle']:
  q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=5);rows.append(dict(mode=mode,returncode=q.returncode,stderr=q.stderr))
a.out.write_text(json.dumps(dict(scope='production destructor; simulated CUDA/OptiX context and buffer ownership; no real RTX/OptiX runtime claim',records=rows),indent=2)+'\n');print([(r['mode'],r['returncode']) for r in rows]);assert all(r['returncode']==0 for r in rows)
