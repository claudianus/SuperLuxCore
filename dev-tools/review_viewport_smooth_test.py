"""Execute the real display smoothing body with finite signed spectral RGB.

Small film stand-ins isolate the filter arithmetic from sampling/Blender.
The old implementation overflows its color edge weight for luma < -0.1.
"""
import os
from pathlib import Path
import subprocess
import tempfile

root=Path(__file__).resolve().parents[1]
source=Path(os.environ.get('SLC_SMOOTH_SOURCE',str(root/'src/slg/film/imagepipeline/plugins/viewportsmooth.cpp'))).read_text()
body=source[source.index('void ViewportSmoothPlugin::Apply('):source.index('// vim:')]
old_body=body.replace('(fabsf(lC) + .1f)', '(lC + .1f)').replace('ViewportSmoothPlugin::Apply','ViewportSmoothBefore::Apply')
code=r'''
#include <vector>
#include <memory>
#include <cmath>
#include <iostream>
#include <cassert>
#include <algorithm>
#include <cstring>
using namespace std; using u_int=unsigned;
template<class T>T Max(T a,T b){return max(a,b);}
template<int N,int D,class T>struct GenericFrameBuffer {
 unsigned w,h;vector<T> v;
 GenericFrameBuffer(unsigned x,unsigned y):w(x),h(y),v(x*y*N){}
 unsigned GetWidth(){return w;} unsigned GetHeight(){return h;}
 T* GetPixels(){return v.data();}
 void GetWeightedPixel(int i,T* out){for(int c=0;c<N-1;++c)out[c]=v[i*N+c]/v[i*N+N-1];}
};
struct Film {
 enum {DEPTH,AVG_SHADING_NORMAL,RADIANCE_PER_PIXEL_NORMALIZED};
 vector<unique_ptr<GenericFrameBuffer<3,0,float>>> channel_IMAGEPIPELINEs;
 vector<unique_ptr<GenericFrameBuffer<4,0,float>>> channel_RADIANCE_PER_PIXEL_NORMALIZEDs;
 unique_ptr<GenericFrameBuffer<4,0,float>> channel_AVG_SHADING_NORMAL;
 unique_ptr<GenericFrameBuffer<1,0,float>> channel_DEPTH;
 Film(){channel_IMAGEPIPELINEs.emplace_back(new GenericFrameBuffer<3,0,float>(3,1));
 channel_RADIANCE_PER_PIXEL_NORMALIZEDs.emplace_back(new GenericFrameBuffer<4,0,float>(3,1));
 channel_AVG_SHADING_NORMAL.reset(new GenericFrameBuffer<4,0,float>(3,1));
 channel_DEPTH.reset(new GenericFrameBuffer<1,0,float>(3,1));
 for(int i=0;i<3;++i){channel_RADIANCE_PER_PIXEL_NORMALIZEDs[0]->v[i*4+3]=1;
 channel_DEPTH->v[i]=1;channel_AVG_SHADING_NORMAL->v[i*4+2]=1;channel_AVG_SHADING_NORMAL->v[i*4+3]=1;}}
 bool HasChannel(int){return true;}unsigned GetRadianceGroupCount(){return 1;}
};
struct ViewportSmoothPlugin{float minSamps=8;void Apply(Film&,u_int);};
struct ViewportSmoothBefore{float minSamps=8;void Apply(Film&,u_int);};
'''+body+old_body+r'''
int main(){
 ViewportSmoothPlugin filter;
 for (float center : {0.f, .2f, 1.f, 10000.f}) {
  Film actual,reference;
  auto &rgb=actual.channel_IMAGEPIPELINEs[0]->v;
  for(int c=0;c<3;++c){rgb[c]=10.f;rgb[3+c]=center;rgb[6+c]=.25f;}
  reference.channel_IMAGEPIPELINEs[0]->v=rgb;
  filter.Apply(actual,0);ViewportSmoothBefore().Apply(reference,0);
  assert(!memcmp(rgb.data(),reference.channel_IMAGEPIPELINEs[0]->v.data(),rgb.size()*sizeof(float)));
 }
 for (float center : {-.04f, -1.f/30.f, -4.f, 0.f, .2f, 10000.f}) {
  Film film;auto &rgb=film.channel_IMAGEPIPELINEs[0]->v;
  for(int c=0;c<3;++c){rgb[c]=10.f;rgb[3+c]=center;rgb[6+c]=.25f;}
  auto raw=film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[0]->v;
  filter.Apply(film,0);
  for(float v:rgb)if(!isfinite(v)){cerr<<"nonfinite filtered RGB at center "<<center<<endl;return 1;}
  assert(raw==film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[0]->v);
 }
 // Converged values and underlying radiance must remain bit exact.
 Film film;auto &rgb=film.channel_IMAGEPIPELINEs[0]->v;
 rgb={-2.f,.2f,10.f,100.f,1.f,.1f,-.04f,-.04f,-.04f};auto before=rgb;
 for(int i=0;i<3;++i)film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[0]->v[i*4+3]=8;
 filter.Apply(film,0);assert(!memcmp(rgb.data(),before.data(),rgb.size()*sizeof(float)));
 cout<<"signed spectral RGB, zero-scale boundary, HDR, raw/converged preservation: PASS"<<endl;
}
'''
with tempfile.TemporaryDirectory(prefix='slc-smooth-test-') as temp:
    cpp=Path(temp)/'test.cpp';exe=Path(temp)/'test';cpp.write_text(code)
    subprocess.run(['clang++','-std=c++17','-O2',str(cpp),'-o',str(exe)],check=True,timeout=30)
    subprocess.run([str(exe)],check=True,timeout=10)
