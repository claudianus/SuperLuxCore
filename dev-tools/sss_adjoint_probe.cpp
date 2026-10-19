// Ordered light/eye BSDF values must describe the same scattering operator
// when the traversal endpoint changes. PDFs are allowed to change.
#include "luxrays/utils/properties.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/materials/materialdefs.h"
#include <fstream>
#include <iomanip>
#include <cmath>
#include <stdexcept>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
}
int main(int argc,char **argv) {
 using namespace luxrays;using namespace slg;if(argc!=2)return 2;slg::Init();
 std::ofstream out(argv[1]);out<<std::setprecision(12)<<"rough,light_side,eye_side,light_cos,eye_cos,eye_value,light_value,eye_pdf,light_pdf,relative_difference\n";
 for(float rough:{.1f,.4f,1.f}) {
  Scene scene;auto p=std::make_unique<Properties>();
  p->SetFromString("scene.materials.test.type = openpbr\nscene.materials.test.subsurfaceweight = 1\nscene.materials.test.subsurfacecolor = .8 .6 .3\nscene.materials.test.specularior = 1.4\n");
  *p<<Property("scene.materials.test.specularroughness")(rough);scene.Parse(p);
  const auto &mat=scene.GetMaterials().GetMaterial("test");
  for(int ls:{-1,1})for(int es:{-1,1})for(float lc:{.2f,.5f,.9f})for(float ec:{.2f,.5f,.9f}) {
   Vector light(std::sqrt(1-lc*lc),0,ls*lc),eye(0,std::sqrt(1-ec*ec),es*ec);Spectrum value[2]; float pdf[2];
   for(unsigned mode=0;mode<2;++mode) {
    HitPoint h{};h.Init();h.fromLight=mode;h.fixedDir=mode?light:eye;h.intoObject=h.fixedDir.z>0;
    h.geometryN=h.interpolatedN=h.shadeN=h.coatN=Normal(0,0,1);h.dpdu=Vector(1,0,0);h.dpdv=Vector(0,1,0);
    h.rayFlags=mode?LIGHT_RAY:EYE_RAY;h.regularization=0;h.interiorVolume=mat.GetInteriorVolume();
    BSDFEvent event;value[mode]=mat.Evaluate(h,light,eye,&event,pdf+mode,nullptr);
   }
   double relative=0.;
   for(unsigned ch=0;ch<3;++ch) relative=std::max(relative,double(std::abs(value[0].c[ch]-value[1].c[ch])/std::max(1e-20f,std::max(std::abs(value[0].c[ch]),std::abs(value[1].c[ch])))));
   if(relative>1e-6)throw std::runtime_error("Traversal-dependent ordered SSS operator");
   out<<rough<<','<<ls<<','<<es<<','<<lc<<','<<ec<<','<<value[0].Filter()<<','<<value[1].Filter()<<','<<pdf[0]<<','<<pdf[1]<<','<<relative<<'\n';
  }
 }
}
