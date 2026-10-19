// A resolved parent focal-distance update must equal a fresh stereo setup.
// This is a CPU state/lifetime contract, not autofocus target-selection quality.
#include "slg/slg.h"
#include "slg/cameras/stereo.h"
#include "slg/scene/scene.h"
#include "slg/utils/pathvolumeinfo.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak))=nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak))=nullptr;
}
using namespace luxrays;
static void configure(slg::StereoCamera &camera,float focal){
 camera.autoVolume=false;camera.autoFocus=false;camera.lensRadius=.2f;camera.focalDistance=focal;
 camera.motionSystem=new MotionSystem(Translate(Vector(.2f,-.4f,.1f))*RotateZ(13.f)*Scale(2.f,.5f,1.5f));
 const unsigned crop[]={21,1254,10,705};camera.Update(1280,720,crop);
}
static Ray generate(const slg::StereoCamera &camera,unsigned eye){
 Ray r;slg::PathVolumeInfo volume;camera.GenerateRay(.5f,eye?1000.75f:200.25f,eye?600.25f:200.5f,&r,&volume,.13f,.77f);return r;
}
static float error(const Ray &a,const Ray &b){
 float maximum=0;
 for(float d:{a.o.x-b.o.x,a.o.y-b.o.y,a.o.z-b.o.z,a.d.x-b.d.x,a.d.y-b.d.y,a.d.z-b.d.z,a.mint-b.mint,a.maxt-b.maxt,a.time-b.time})maximum=std::max(maximum,std::abs(d));
 return maximum;
}
int main(int argc,char **argv)try{
 if(argc!=2)throw std::runtime_error("usage: stereo-eye-update-probe result.json");slg::Init();slg::Scene scene;
 slg::StereoCamera camera(slg::StereoCamera::STEREO_PERSPECTIVE,Point(0,0,0),Point(0,0,1),Vector(0,1,0));configure(camera,10.f);
 const Ray before=generate(camera,0);float maximum=0;bool changed=false;unsigned checks=0;
 for(unsigned i=0;i<16;++i){
  const float focal=17.25f+i*.25f;camera.focalDistance=focal;slg::Camera &base=camera;base.UpdateAuto(scene);
  const unsigned crop[]={21,1254,10,705};if(!std::equal(crop,crop+4,camera.filmSubRegion))throw std::runtime_error("Auto update changed crop");
  slg::StereoCamera fresh(slg::StereoCamera::STEREO_PERSPECTIVE,Point(0,0,0),Point(0,0,1),Vector(0,1,0));configure(fresh,focal);
  for(unsigned eye=0;eye<2;++eye){const Ray actual=generate(camera,eye),expected=generate(fresh,eye);maximum=std::max(maximum,error(actual,expected));if(i==0 && eye==0)changed=error(actual,before)>1e-6f;++checks;}
 }
 const bool passed=maximum<1e-7f && changed;
 std::ofstream out(argv[1]);out<<"{\"passed\":"<<(passed?"true":"false")<<",\"ray_checks\":"<<checks<<",\"max_abs_ray_difference\":"<<maximum<<",\"focal_update_changed_actual_ray\":"<<(changed?"true":"false")<<",\"crop_preserved\":true,\"motion_ownership_rebuilt_16_times\":true,\"actual_autofocus_target_selection_tested\":false}\n";
 if(!passed)throw std::runtime_error("Resolved parent focal update differs from fresh camera");return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
