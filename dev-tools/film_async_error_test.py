"""Production image-pipeline worker failure containment; no real GPU claim."""
import argparse,json,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import method,ROOT
PRE=r'''
#include <atomic>
#include <cassert>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
using namespace std;using u_int=unsigned int;
namespace luxrays { using JThread=std::thread; }
template<class T> void SetThreadName(T &,const char *) {}
struct Film {
 unique_ptr<std::thread> imagePipelineThread;
 atomic<bool> isAsyncImagePipelineRunning{false};
 exception_ptr asyncImagePipelineError;
 int fault=0,calls=0;
 ~Film(){WaitAsyncExecuteImagePipeline();}
 void ExecuteImagePipelineImpl(u_int){++calls;if(fault==1)throw runtime_error("GPU execution failed");if(fault==2)throw 42;}
 void AsyncExecuteImagePipeline(u_int);
 bool HasDoneAsyncExecuteImagePipeline();
 void WaitAsyncExecuteImagePipeline();
 void ExecuteImagePipelineThreadImpl(u_int);
};
'''
MAIN=r'''
int main(int argc,char **argv){
 Film f;string mode=argv[1];f.fault=mode=="runtime"?1:mode=="unknown"?2:0;
 f.AsyncExecuteImagePipeline(0);
 f.WaitAsyncExecuteImagePipeline();
 bool caught=false;
 try{assert(f.HasDoneAsyncExecuteImagePipeline());}
 catch(const runtime_error &e){assert(f.fault==1 && string(e.what())=="GPU execution failed");caught=true;}
 catch(int e){assert(f.fault==2 && e==42);caught=true;}
 assert(caught==(f.fault!=0));assert(!f.isAsyncImagePipelineRunning);
 assert(f.HasDoneAsyncExecuteImagePipeline());
 f.fault=0;f.AsyncExecuteImagePipeline(0);f.WaitAsyncExecuteImagePipeline();
 assert(f.HasDoneAsyncExecuteImagePipeline());assert(f.calls==2);
 // An unobserved failure must still permit destructor/join cleanup.
 if(mode!="success"){f.fault=1;f.AsyncExecuteImagePipeline(0);f.WaitAsyncExecuteImagePipeline();}
 puts("PASS worker failure delivered once, restart and destruction");
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'src/slg/film/filmimagepipeline.cpp');p.add_argument('--out',type=Path,required=True);p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
 body='\n'.join(method(a.source.read_text(),s) for s in ['void Film::AsyncExecuteImagePipeline(','bool Film::HasDoneAsyncExecuteImagePipeline(','void Film::WaitAsyncExecuteImagePipeline(','void Film::ExecuteImagePipelineThreadImpl('])
 records=[]
 with tempfile.TemporaryDirectory(prefix='slc-async-fault-') as tmp:
  src=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';src.write_text(PRE+body+MAIN)
  q=subprocess.run(['clang++','-std=c++20','-pthread','-fsanitize=address,undefined',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=45);assert q.returncode==0,q.stderr
  for mode in ['success','runtime','unknown']:
   q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=5);records.append(dict(mode=mode,returncode=q.returncode,stdout=q.stdout,stderr=q.stderr))
 a.out.write_text(json.dumps({'scope':'production worker methods with simulated GPU exception; ASan/UBSan; no actual RTX','records':records},indent=2)+'\n')
 print([(r['mode'],r['returncode']) for r in records])
 assert all(r['returncode']==0 for r in records) if not a.expect_failure else all(r['returncode']!=0 for r in records[1:])
if __name__=='__main__':main()
