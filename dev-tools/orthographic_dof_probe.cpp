// Independent aperture distribution and focus-plane contracts for CPU ortho DOF.
#include "slg/slg.h"
#include "slg/cameras/orthographic.h"
#include "slg/utils/pathvolumeinfo.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
}
using namespace luxrays;
int main(int argc,char **argv) try {
 if(argc!=2) throw std::runtime_error("usage: orthographic-dof-probe result.json");
 slg::Init();
 slg::OrthographicCamera camera(Point(0,0,0),Point(0,0,1),Vector(0,1,0));
 camera.lensRadius=.2f;camera.focalDistance=10.f;camera.clipHither=.001f;
 const unsigned region[]={0,1279,0,719};camera.Update(1280,720,region);
 double sx=0,sy=0,sr2=0;float maxRadius=0,maxFocusError=0,maxOffFocusSpread=0;
 unsigned samples=0;
 for(unsigned y=0;y<64;++y) for(unsigned x=0;x<64;++x) {
  const float u=(x+.5f)/64,v=(y+.5f)/64;Point lens;
  camera.LocalSampleLens(.5f,u,v,&lens);
  sx+=lens.x;sy+=lens.y;sr2+=lens.x*lens.x+lens.y*lens.y;
  maxRadius=std::max(maxRadius,std::sqrt(lens.x*lens.x+lens.y*lens.y));
  slg::PathVolumeInfo volume;Ray ray,pinhole;
  camera.GenerateRay(.5f,233.25f,517.75f,&ray,&volume,u,v);
  camera.lensRadius=0;slg::PathVolumeInfo pinholeVolume;
  camera.GenerateRay(.5f,233.25f,517.75f,&pinhole,&pinholeVolume,u,v);camera.lensRadius=.2f;
  const Point focus=ray((camera.focalDistance-ray.o.z)/ray.d.z);
  const Point expected=pinhole((camera.focalDistance-pinhole.o.z)/pinhole.d.z);
  maxFocusError=std::max(maxFocusError,(focus-expected).Length());
  const Point near=ray((2.f-ray.o.z)/ray.d.z),sharp=pinhole((2.f-pinhole.o.z)/pinhole.d.z);
  maxOffFocusSpread=std::max(maxOffFocusSpread,(near-sharp).Length());++samples;
 }
 const double meanX=sx/samples,meanY=sy/samples,normalizedMoment=sr2/samples/(.2*.2);
 camera.lensRadius=0;Point disabled;camera.LocalSampleLens(.5f,.1f,.7f,&disabled);
 // A uniform disk has E[r^2]/R^2=1/2 and zero first moments.
 const bool aperture=std::abs(meanX)<1e-6 && std::abs(meanY)<1e-6 &&
   std::abs(normalizedMoment-.5)<.001 && maxRadius<=.200001f;
 const bool focus=maxFocusError<2e-6f && maxOffFocusSpread>.15f;
 const bool zero=disabled.x==0 && disabled.y==0 && disabled.z==0;
 const bool passed=aperture && focus && zero;
 std::ofstream out(argv[1]);out.precision(12);
 out<<"{\"passed\":"<<(passed?"true":"false")<<",\"samples\":"<<samples
 <<",\"aperture_distribution_passed\":"<<(aperture?"true":"false")
 <<",\"focus_contract_passed\":"<<(focus?"true":"false")<<",\"zero_aperture_exact\":"<<(zero?"true":"false")
 <<",\"mean_lens_x\":"<<meanX<<",\"mean_lens_y\":"<<meanY<<",\"normalized_radial_second_moment\":"<<normalizedMoment
 <<",\"max_lens_radius\":"<<maxRadius<<",\"max_focus_plane_error\":"<<maxFocusError
 <<",\"max_off_focus_spread\":"<<maxOffFocusSpread<<",\"light_to_camera_connection_tested\":false}\n";
 return passed?0:2;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
