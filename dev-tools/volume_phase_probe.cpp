// Actual BSDF/volume sampler, not a replacement phase implementation.
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/bsdf/bsdf.h"
#include "slg/volumes/homogenous.h"
#include "slg/textures/constfloat.h"
#include "slg/textures/constfloat3.h"
#include <iostream>
#include <stdexcept>
using namespace slg;using namespace luxrays;
static void require(bool b,const char *message){if(!b)throw std::runtime_error(message);}
int main(int argc,char **argv) try {
    const bool legacy=argc>1 && std::string(argv[1])=="legacy";
    slg::Init();Scene scene;unsigned checks=0;double maxError=0,eyeMean=0,lightMean=0;unsigned graySamples=0;
    ConstFloatTexture ior(1.f),zero(0.f),one(1.f);
    ConstFloat3Texture coloredAbsorption(Spectrum(.2f,.1f,.3f)),coloredScattering(Spectrum(.8f,.9f,.7f));
    for(bool colored:{false,true})for(bool hg:{false,true})for(float g:{-.6f,0.f,.6f}){
        ConstFloatTexture asymmetry(g);
        HomogeneousVolume medium(ior,nullptr,colored?static_cast<const Texture&>(coloredAbsorption):zero,
            colored?static_cast<const Texture&>(coloredScattering):one,asymmetry,true,hg,false);
        for(Vector axis:{Vector(0,0,1),Vector(.2f,.5f,1),Vector(-.7f,.3f,1)}){
            Ray ray(Point(),Normalize(axis),0.f,1.f,.5f);BSDF eye,light;
            eye.Init(false,false,scene,ray,medium,.5f,.5f);light.Init(true,false,scene,ray,medium,.5f,.5f);
            for(unsigned n=0;n<1024;++n){
                const float u0=(n+.5f)/1024,u1=((n*673u)%1024+.5f)/1024;
                Vector de,dl;float pe,pl,ce,cl;BSDFEvent ee,el;
                const Spectrum we=eye.Sample(&de,u0,u1,&pe,&ce,&ee),wl=light.Sample(&dl,u0,u1,&pl,&cl,&el);
                require(de==dl && pe==pl && ee==el,"phase strategy changed with transport mode");
                const Spectrum albedo=eye.Albedo();
                const float expectedCos=legacy?AbsDot(de,eye.hitPoint.geometryN):1.f;
                require(fabsf(ce-expectedCos)<2e-6f && fabsf(cl-expectedCos)<2e-6f,"phase returned projected surface cosine");
                const float oldFactor=AbsDot(dl,light.hitPoint.geometryN)/AbsDot(light.hitPoint.fixedDir,light.hitPoint.geometryN);
                Spectrum expectedLight(albedo);
                if (legacy) expectedLight *= oldFactor;
                for(unsigned i=0;i<3;++i){
                    maxError=std::max(maxError,double(fabsf(we.c[i]-albedo.c[i])));
                    maxError=std::max(maxError,double(fabsf(wl.c[i]-expectedLight.c[i])));
                    require(fabsf(we.c[i]-albedo.c[i])<2e-6f && fabsf(wl.c[i]-expectedLight.c[i])<2e-6f,"phase sample weight differs from its physical albedo");++checks;
                }
                if(!colored && !hg && g==0.f){eyeMean+=we.c[0];lightMean+=wl.c[0];++graySamples;}
            }
        }
    }
    std::cout<<"{\"checks\":"<<checks<<",\"max_weight_error\":"<<maxError
        <<",\"gray_isotropic_eye_mean\":"<<eyeMean/graySamples
        <<",\"gray_isotropic_light_mean\":"<<lightMean/graySamples
        <<",\"legacy_negative_control\":"<<(legacy?"true":"false")<<",\"passed\":true}\n";
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 2;}
