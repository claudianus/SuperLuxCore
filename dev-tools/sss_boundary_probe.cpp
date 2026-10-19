// Exercise the actual material sampler, forward/reverse PDF and physical BSDF.
// The CSV records independent IID estimates and their standard errors; no render clamp.
#include "luxrays/utils/properties.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/materials/materialdefs.h"
#include <cmath>
#include <fstream>
#include <iomanip>
#include <random>
#include <stdexcept>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
}
int main(int argc, char **argv) {
 using namespace luxrays; using namespace slg;
 if(argc!=3) return 2;
 const unsigned count=std::stoul(argv[2]); slg::Init();
 std::ofstream out(argv[1]);out<<std::setprecision(12);
 out<<"rough,side,cosine,from_light,mean,stderr,second_moment,reflection_mass,transmission_mass,max_weight,pdf_error,reverse_error,physical_sum\n";
 for(float rough:{.05f,.4f,1.f}) for(int side:{-1,1}) for(float z:{.2f,.65f,.8f,.99f}) for(bool adjoint:{false,true}) {
  Scene scene;auto props=std::make_unique<Properties>();
  props->SetFromString("scene.materials.test.type = openpbr\nscene.materials.test.subsurfaceweight = 1\nscene.materials.test.subsurfacecolor = .8 .6 .3\nscene.materials.test.specularior = 1.4\n");
  *props<<Property("scene.materials.test.specularroughness")(rough);
  scene.Parse(props);auto &mat=scene.GetMaterials().GetMaterial("test");
  HitPoint hp{};hp.Init();hp.fromLight=adjoint;hp.intoObject=side>0;
  hp.fixedDir=Vector(std::sqrt(1-z*z),0,side*z);
  hp.geometryN=hp.interpolatedN=hp.shadeN=hp.coatN=Normal(0,0,1);
  hp.dpdu=Vector(1,0,0);hp.dpdv=Vector(0,1,0);hp.regularization=0;
  hp.rayFlags=adjoint?LIGHT_RAY:EYE_RAY;
  std::mt19937 gen(12345);auto u=[&](){return std::generate_canonical<float,24>(gen);};
  double sum=0,sq=0,maxw=0,pdferr=0,reverr=0,physical=0;unsigned refl=0,trans=0;
  for(unsigned i=0;i<count;++i) {
   Vector wi;float pdf=0;BSDFEvent ev=NONE;
   const float a=u(),b=u(),c=u();const Spectrum weight=mat.Sample(hp,hp.fixedDir,&wi,a,b,c,&pdf,&ev);
   if(weight.IsNaN() || weight.IsInf()) throw std::runtime_error("nonfinite weight");
   double w=weight.Filter();sum+=w;sq+=w*w;maxw=std::max(maxw,w);
   if(weight.Black())continue;
   const Vector &light=adjoint?hp.fixedDir:wi,&eye=adjoint?wi:hp.fixedDir;
   float ep=0,rp=0;BSDFEvent ee;const Spectrum f=mat.Evaluate(hp,light,eye,&ee,&ep,&rp);
   pdferr=std::max(pdferr,std::abs(double(ep-pdf))/std::max(1e-20,double(pdf)));
   float check=0;HitPoint reverse=hp;reverse.fromLight=!adjoint;reverse.fixedDir=wi;
   mat.Evaluate(reverse,light,eye,&ee,&check,nullptr);
   reverr=std::max(reverr,std::abs(double(rp-check))/std::max(1e-20,double(check)));
   if(hp.fixedDir.z*wi.z>0)++refl;else++trans;
  }
  // Same fixed direction grid in both modes verifies the scattering law is unchanged.
  for(unsigned i=0;i<1024;++i) {
   const float cz=-1.f+2.f*(i+.5f)/1024.f, phi=i*2.39996323f;
   const Vector w(std::sqrt(1-cz*cz)*std::cos(phi),std::sqrt(1-cz*cz)*std::sin(phi),cz);
   BSDFEvent ev;float pdf;
   physical+=mat.Evaluate(hp,adjoint?hp.fixedDir:w,adjoint?w:hp.fixedDir,&ev,&pdf,nullptr).Filter();
  }
  const double mean=sum/count, variance=std::max(0.,(sq-count*mean*mean)/(count-1));
  out<<rough<<','<<side<<','<<z<<','<<adjoint<<','<<mean<<','<<std::sqrt(variance/count)<<','<<sq/count<<','<<double(refl)/count<<','<<double(trans)/count<<','<<maxw<<','<<pdferr<<','<<reverr<<','<<physical<<'\n';
 }
}
