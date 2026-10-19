"""Run real film pipeline execution with driver/plugin failure injection."""
import argparse,json,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import method,ROOT
PRE=r'''
#include <cassert>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>
using namespace std;using u_int=unsigned int;
string fault;int current=42;
void point(const char *name){if(fault==name)throw runtime_error(name);}
struct Device {void PushThreadCurrentDevice(){current=7;}void PopThreadCurrentDevice(){current=42;}};
struct Film;
struct Pipeline {bool CanUseHW(){return true;}void Apply(Film &,u_int){point("plugin");}};
struct Film {
 bool hwEnable=true;int ctx=1;Device *hardwareDevice;
 vector<Pipeline *> imagePipelines;
 enum {RADIANCE_PER_PIXEL_NORMALIZED,RADIANCE_PER_SCREEN_NORMALIZED,IMAGEPIPELINE};
 bool HasChannel(int){return true;}
 void CreateHWContext(){} void AllocateHWBuffers(){}void CompileHWKernels(){}
 void MergeSampleBuffersHW(u_int){point("merge");}
 void MergeSampleBuffers(u_int){point("cpu-merge");}
 void WriteAllHWBuffers(){point("write");}
 void ExecuteImagePipelineImpl(u_int);
};
'''
MAIN=r'''
int main(int argc,char **argv){fault=argv[1];Device d;Pipeline p;Film f;f.hardwareDevice=&d;f.imagePipelines.push_back(&p);
 if(fault=="cpu-merge"){f.hwEnable=false;f.hardwareDevice=nullptr;}
 string error;try{f.ExecuteImagePipelineImpl(0);}catch(const exception &e){error=e.what();}
 assert(current==42);assert(error==(fault=="success"?"":fault));puts("PASS context restored and original error retained");}
'''
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'src/slg/film/filmimagepipeline.cpp');p.add_argument('--out',type=Path,required=True);p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
records=[]
with tempfile.TemporaryDirectory(prefix='slc-context-fault-') as tmp:
 s=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';s.write_text(PRE+method(a.source.read_text(),'void Film::ExecuteImagePipelineImpl(')+MAIN)
 q=subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(s),'-o',str(exe)],capture_output=True,text=True,timeout=40);assert q.returncode==0,q.stderr
 for mode in ['success','merge','write','plugin','cpu-merge']:
  q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=5);records.append(dict(mode=mode,returncode=q.returncode,stderr=q.stderr))
a.out.write_text(json.dumps({'scope':'actual production execution method; simulated device/plugin failures; ASan/UBSan','records':records},indent=2)+'\n');print([(r['mode'],r['returncode']) for r in records])
assert all(r['returncode']==0 for r in records) if not a.expect_failure else sum(r['returncode']!=0 for r in records)==3
