"""Production RT reset calls must retire previous-camera canonical radiance.

Executes the actual Film::Reset body with observable accumulation channels;
actual GPU camera/color and viewport validation remain separate gates.
"""
import argparse,re,tempfile,subprocess,json,hashlib
from pathlib import Path
root=Path(__file__).resolve().parents[1];p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=root/'src/slg/engines/rtpathocl/rtpathoclthread.cpp');p.add_argument('--out',type=Path,required=True);p.add_argument('--expect-failure',action='store_true');a=p.parse_args()
s=a.source.read_text();calls=re.findall(r'engine->GetFilm\(\)\.Reset\([^;]*\);',s);assert len(calls)==2
film=(root/'src/slg/film/film.cpp').read_text();start=film.index('void Film::Reset(');begin=film.index('{',start);end=begin+1;depth=1
while depth:depth+=(film[end]=='{')-(film[end]=='}');end+=1
body=film[begin:end]
code=r'''
#include <algorithm>
#include <vector>
#include <cstdio>
#include <cmath>
namespace luxrays {double WallClockTime(){return 1.;}}
using luxrays::WallClockTime;
struct Counts{int n=20;void Clear(){n=0;}};
struct Film{double sum=20,weight=20,statsConvergence=1,statsStartSampleTime=0;Counts samplesCounts;std::vector<float>pixelLumaMoments{4};
 void Clear(){sum=weight=0;}
 void Reset(const bool onlyCounters=false)BODY
 void add(double x){sum+=x;weight++;samplesCounts.n++;}
};
struct Engine{bool denoisingFramesEnabled;Film film;Film&GetFilm(){return film;}};
int test(const char*name,bool independent,bool black,int path){Engine e;e.denoisingFramesEnabled=independent;Engine*engine=&e;
 if(path==0){CALL0}else{CALL1}
 double target=black?0.:.1;e.film.add(target);e.film.add(target);
 double rgb=e.film.sum/e.film.weight;bool ok=std::abs(rgb-target)<1e-12&&e.film.samplesCounts.n==2&&e.film.weight==2;
 printf("%s %s value=%.12f expected=%.12f weight=%.0f\n",ok?"PASS":"FAIL",name,rgb,target,e.film.weight);return !ok;}
int main(){int errors=0;errors+=test("ordinary_fast_new_camera",false,false,0);errors+=test("ordinary_full_edit_black",false,true,1);errors+=test("independent_fast_control",true,false,0);errors+=test("independent_full_control",true,true,1);return errors?1:0;}
'''.replace('BODY',body).replace('CALL0',calls[0]).replace('CALL1',calls[1])
with tempfile.TemporaryDirectory(prefix='slc-camera-reset-') as d:
 f=Path(d)/'test.cpp';exe=Path(d)/'test';f.write_text(code);c=subprocess.run(['clang++','-std=c++20',str(f),'-o',str(exe)],text=True,capture_output=True,timeout=30);assert c.returncode==0,c.stderr;r=subprocess.run([str(exe)],text=True,capture_output=True,timeout=5)
a.out.write_text(json.dumps({'source':str(a.source),'source_sha256':hashlib.sha256(s.encode()).hexdigest(),'film_reset_body_sha256':hashlib.sha256(body.encode()).hexdigest(),'calls':calls,'returncode':r.returncode,'output':r.stdout,'scope':__doc__},indent=2)+'\n');print(r.stdout);assert r.returncode==(1 if a.expect_failure else 0)
