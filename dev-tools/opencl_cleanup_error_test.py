"""Production OpenCL destruction methods with deterministic driver errors.

Driver errors are return codes, never C++ exceptions in the stub itself.
No OpenCL runtime/GPU or Windows acceptance is inferred from this harness.
"""
import argparse,json,subprocess,tempfile
from pathlib import Path
from cuda_teardown_test import method,ROOT
PRE=r'''
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <cassert>
using namespace std;using cl_int=int;constexpr int CL_SUCCESS=0;
string fault;vector<string> releases;
int release(const char *name){releases.push_back(name);return fault==name?-5:CL_SUCCESS;}
int clReleaseKernel(int){return release("kernel");}
int clReleaseProgram(int){return release("program");}
int clReleaseMemObject(int){return release("buffer");}
int clReleaseCommandQueue(int){return release("queue");}
int clReleaseContext(int){return release("context");}
#define CHECK_OCL_ERROR(e) do {if((e)!=CL_SUCCESS)throw runtime_error("injected OpenCL driver error");}while(0)
'''
MAIN=r'''
int main(int argc,char **argv){fault=argv[1];
 {OpenCLDeviceKernel k;OpenCLDeviceProgram p;OpenCLDeviceBuffer b;OpenCLDevice d;}
 assert(releases==vector<string>({"queue","context","buffer","program","kernel"}));
 puts("PASS all resources attempted and return-code errors contained");}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--header',type=Path,default=ROOT/'include/luxrays/devices/ocldevice.h');p.add_argument('--source',type=Path,default=ROOT/'src/luxrays/devices/ocldevice.cpp');p.add_argument('--out',type=Path,required=True);p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
 body=PRE
 for name,field in [('OpenCLDeviceKernel','oclKernel'),('OpenCLDeviceProgram','oclProgram'),('OpenCLDeviceBuffer','oclBuff')]:
  body+='struct '+name+'{int '+field+'=1;'+method(a.header.read_text(),'virtual ~'+name+'()')+'};\n'
 body+='struct OpenCLDevice{int oclQueue=1,oclContext=1;~OpenCLDevice();};\n'+method(a.source.read_text(),'OpenCLDevice::~OpenCLDevice()')+MAIN
 rows=[]
 with tempfile.TemporaryDirectory(prefix='slc-opencl-cleanup-') as tmp:
  src=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';src.write_text(body)
  q=subprocess.run(['clang++','-std=c++20','-fsanitize=address,undefined',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=40);assert q.returncode==0,q.stderr
  for mode in ['success','kernel','program','buffer','queue','context']:
   q=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=5);rows.append(dict(mode=mode,returncode=q.returncode,stdout=q.stdout,stderr=q.stderr))
 a.out.write_text(json.dumps(dict(scope='actual four production destructors, simulated OpenCL return-code failures, ASan/UBSan; no real device',records=rows),indent=2)+'\n');print([(r['mode'],r['returncode']) for r in rows])
 assert all(r['returncode']==0 for r in rows) if not a.expect_failure else rows[0]['returncode']==0 and all(r['returncode']!=0 for r in rows[1:])
if __name__=='__main__':main()
