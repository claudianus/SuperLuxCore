"""Execute production RT runtime-policy upload guard with observable buffers.

Checks first-tile ordering, restart/repeat, per-device state, restoring the
configured policy, coherent dense input and unrelated config preservation.
This is host control-flow validation, not GPU/driver or performance acceptance.
"""
import argparse,hashlib,json,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'src/slg/engines/rtpathocl/rtpathoclthread.cpp');p.add_argument('--out',type=Path,required=True);p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
s=a.source.read_text();i=s.index('u_int appliedReductionOverride =');init=s[i:s.index(';',i)+1]
pos=s.index('const u_int req = engine->runtimeResolutionReduction.load();');begin=s.rfind('{',0,pos);end=begin+1;depth=1
while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
block=s[begin:end];before=pos<s.index('RenderTileWork(',i)
code=r'''
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <vector>
using u_int=unsigned int;
namespace slg::ocl::pathoclbase {
struct GPUTaskConfiguration { unsigned preserved=12345;struct {struct {u_int resolutionReduction=16,previewResolutionReduction=32;}rtpathocl;}renderEngine;};
}
template<class T>T Min(T a,T b){return std::min(a,b);}
template<class T>T Max(T a,T b){return std::max(a,b);}
u_int RoundUpPow2(u_int v){u_int r=1;while(r<v)r*=2;return r;}
struct Engine{std::atomic<u_int>runtimeResolutionReduction{64};u_int resolutionReduction=16,previewResolutionReduction=32;};
struct Device{int writes=0;slg::ocl::pathoclbase::GPUTaskConfiguration gpu;
 void EnqueueWriteBuffer(void*,bool blocking,size_t,const void* data){if(!blocking)throw 1;gpu=*static_cast<const slg::ocl::pathoclbase::GPUTaskConfiguration*>(data);++writes;}};
int run(const char*name,u_int base,std::vector<u_int> requests,std::vector<u_int> expected,bool mirror){
 Engine object;Engine*engine=&object;engine->resolutionReduction=base;engine->previewResolutionReduction=base==1?1:32;
 engine->runtimeResolutionReduction=requests[0];
 Device intersectionDevice;void*taskConfigBuff=nullptr;slg::ocl::pathoclbase::GPUTaskConfiguration threadTaskConfig;
 threadTaskConfig.renderEngine.rtpathocl.resolutionReduction=base;
 threadTaskConfig.renderEngine.rtpathocl.previewResolutionReduction=engine->previewResolutionReduction;
 intersectionDevice.gpu=threadTaskConfig;
'''+init+r'''
 int errors=0;
 auto observe=[&](u_int expected){
  if(intersectionDevice.gpu.renderEngine.rtpathocl.resolutionReduction!=expected||intersectionDevice.gpu.preserved!=12345)++errors;
 };
 for(size_t frame=0;frame<requests.size();++frame){engine->runtimeResolutionReduction=requests[frame];
'''+('' if before else 'observe(expected[frame]);\n')+block+('observe(expected[frame]);\n' if before else '')+r'''
 }
 if(mirror){intersectionDevice.EnqueueWriteBuffer(taskConfigBuff,true,sizeof(threadTaskConfig),&threadTaskConfig);observe(expected.back());}
 printf("%s %s writes=%d failures=%d\n",errors?"FAIL":"PASS",name,intersectionDevice.writes,errors);
 return errors;
}
int main(){int errors=0;
 errors+=run("first_restart_and_repeat",16,{64,64,64},{64,64,64},false);
 errors+=run("restore_configured_policy",16,{64,0,64,0},{64,16,64,16},false);
 errors+=run("device_A",16,{64},{64},false);
 errors+=run("device_B_independent_upload",16,{64},{64},false);
 errors+=run("preserve_runtime_across_scene_only_upload",16,{0,64},{16,64},true);
 errors+=run("coherent_dense_input",1,{0,0,0},{1,1,1},true);
 errors+=run("clamp_to_allocated_task_density",16,{2,18,999,0},{16,32,64,16},false);
 return errors?1:0;}
'''
with tempfile.TemporaryDirectory(prefix='slc-runtime-policy-') as tmp:
 f=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';f.write_text(code)
 compiled=subprocess.run(['clang++','-std=c++20',str(f),'-o',str(exe)],capture_output=True,text=True,timeout=30);assert compiled.returncode==0,compiled.stderr
 result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
report=dict(source=str(a.source),source_sha256=hashlib.sha256(s.encode()).hexdigest(),guard_before_tile=before,returncode=result.returncode,output=result.stdout,scope=__doc__)
a.out.parent.mkdir(parents=True,exist_ok=True);a.out.write_text(json.dumps(report,indent=2)+'\n');print(result.stdout)
assert result.returncode==(1 if a.expect_failure else 0),report
