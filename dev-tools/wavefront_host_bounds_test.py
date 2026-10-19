"""Execute production wavefront dispatch bounds with stale host counts.

Models the unchanged device queue guard; actual GPU AOV tests are separate.
A host-visible zero must never suppress a currently populated device queue.
"""
import argparse,hashlib,json,re,subprocess,tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=ROOT/'src/slg/engines/pathoclbase/pathoclbaseoclthreadkernels.cpp');p.add_argument('--out',type=Path,required=True);p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
s=a.source.read_text();function=s[s.index('void PathOCLBaseOCLRenderThread::EnqueueAdvancePathsWavefront()'):];expression=re.search(r'const u_int count = ([^;]+);',function).group(1)
guard=(ROOT/'include/slg/engines/pathoclbase/kernels/pathoclbase_funcs.cl').read_text();assert 'if (get_global_id(0) >= taskQueueTotals[taskQueueState])' in guard
code=r'''
#include <algorithm>
#include <cstdio>
#include <vector>
using u_int=unsigned int;
int check(const char*name,bool deviceBoundsLaunch,u_int taskCount,u_int cached,u_int current){
 const u_int state=0;std::vector<u_int>wavefrontQueueTotals{cached};
 const u_int count=EXPRESSION;
 const u_int workgroup=8;const u_int launch=count?(count+workgroup-1)/workgroup*workgroup:0;
 u_int executed=0;for(u_int lane=0;lane<launch;++lane){if(lane>=current)continue;++executed;}
 bool ok=executed==current && launch<=(taskCount+workgroup-1)/workgroup*workgroup;
 if(!deviceBoundsLaunch)ok=ok&&count==current;
 printf("%s %s launch=%u executed=%u expected=%u\n",ok?"PASS":"FAIL",name,launch,executed,current);
 return !ok;
}
int main(){int errors=0;
 errors+=check("mapped_zero_cache_new_work",true,16,0,9);
 errors+=check("mapped_short_cache_full_tail",true,16,1,16);
 errors+=check("mapped_empty_queue_guard",true,16,0,0);
 errors+=check("mapped_workgroup_tail_guard",true,17,0,17);
 errors+=check("synchronized_compact_nonmapped",false,17,3,3);
 errors+=check("synchronized_empty_nonmapped",false,17,0,0);
 return errors?1:0;}
'''.replace('EXPRESSION',expression)
with tempfile.TemporaryDirectory(prefix='slc-wf-bounds-') as tmp:
 f=Path(tmp)/'test.cpp';exe=Path(tmp)/'test';f.write_text(code);q=subprocess.run(['clang++','-std=c++20',str(f),'-o',str(exe)],capture_output=True,text=True,timeout=30);assert q.returncode==0,q.stderr;r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5)
report={'source':str(a.source),'source_sha256':hashlib.sha256(s.encode()).hexdigest(),'production_count_expression':expression,'returncode':r.returncode,'output':r.stdout,'scope':__doc__};a.out.write_text(json.dumps(report,indent=2)+'\n');print(r.stdout);assert r.returncode==(1 if a.expect_failure else 0)
