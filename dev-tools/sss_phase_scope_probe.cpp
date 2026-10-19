// Exact constant/isotropic phase reuse, including all cache miss boundaries.
#include "luxrays/utils/properties.h"
#include "luxrays/core/color/spectral.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/materials/materialdefs.h"
#include "slg/volumes/homogenous.h"
#include <iostream>
#include <stdexcept>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
}
using namespace slg;
using namespace luxrays;
static void require(bool condition, const char *why) {
    if (!condition) throw std::runtime_error(why);
}
int main() {
    slg::Init(); unsigned hits=0, misses=0;
    for (bool spectral : {false,true}) for (bool hg : {false,true}) {
        Spectral::SetEnabled(spectral);
        PathWavelengths sw; sw.Sample(.217f); Spectral::ScopeWavelengths wavelengthScope(sw);
        Scene scene; auto props=std::make_unique<Properties>();
        props->SetFromString(
            "scene.volumes.a.type = homogeneous\n"
            "scene.volumes.a.ior = 1.4\n"
            "scene.volumes.a.sssalbedo = .8 .5 .2\n"
            "scene.volumes.a.sssmfp = .01 .02 .04\n"
            "scene.volumes.b.type = homogeneous\n"
            "scene.volumes.b.ior = 1.4\n"
            "scene.volumes.b.sssalbedo = .2 .3 .4\n"
            "scene.volumes.b.sssmfp = .02 .02 .02\n"
            "scene.volumes.anisotropic.type = homogeneous\n"
            "scene.volumes.anisotropic.sssalbedo = .8 .8 .8\n"
            "scene.volumes.anisotropic.sssmfp = .01 .01 .01\n"
            "scene.volumes.anisotropic.asymmetry = .0000001\n"
            "scene.textures.varying.type = checkerboard3d\n"
            "scene.textures.varying.texture1 = .2 .3 .4\n"
            "scene.textures.varying.texture2 = .8 .5 .2\n"
            "scene.volumes.textured.type = homogeneous\n"
            "scene.volumes.textured.sssalbedo = varying\n"
            "scene.volumes.textured.sssmfp = .01 .01 .01\n");
        *props << Property("scene.volumes.a.phase")(hg?"hg":"schlick")
               << Property("scene.volumes.b.phase")(hg?"hg":"schlick");
        scene.Parse(props);
        const auto &a=static_cast<const HomogeneousVolume &>(scene.GetMaterials().GetMaterial("a"));
        const auto &b=static_cast<const HomogeneousVolume &>(scene.GetMaterials().GetMaterial("b"));
        HitPoint hp; hp.Init(); hp.p=Point(.1,.2,.3);
        HomogeneousVolume::InteriorScope outer(a,hp);
        Spectrum value; float pdf;
        require(outer.IsotropicPhase(value,pdf),"constant isotropic phase missed");
        const Spectrum original=value; const float originalPdf=pdf;
        for (Vector d : {Vector(1,0,0),Vector(0,1,0),Vector(0,0,-1)}) {
            BSDFEvent event; float expectedPdf;
            const Spectrum expected=a.Evaluate(hp,d,Vector(0,0,1),&event,&expectedPdf,nullptr);
            require(value==expected && pdf==expectedPdf,"phase arithmetic changed"); ++hits;
        }
        {
            HomogeneousVolume::InteriorScope inner(b,hp);
            require(inner.IsotropicPhase(value,pdf),"nested constant phase missed");
            require(value!=original,"nested volumes aliased"); ++hits;
        }
        require(outer.IsotropicPhase(value,pdf) && value==original && pdf==originalPdf,"outer phase not restored"); ++hits;
        if (spectral) {
            auto changed=sw; changed.Sample(.731f); Spectral::SetPathWavelengths(changed);
            require(!outer.IsotropicPhase(value,pdf),"wavelength change reused old phase"); ++misses;
            Spectral::SetPathWavelengths(sw); changed=sw; changed.aliveMask=1u<<sw.hero;
            Spectral::SetPathWavelengths(changed);
            require(!outer.IsotropicPhase(value,pdf),"live mask change reused old phase"); ++misses;
            Spectral::SetPathWavelengths(sw);
            { Spectral::ScopePause pause; require(!outer.IsotropicPhase(value,pdf),"spectral pause reused phase"); ++misses; }
            const auto upsampling=Spectral::GetUpsamplingModel();
            Spectral::SetUpsamplingModel(upsampling==Spectral::UPSAMPLING_SMITS?Spectral::UPSAMPLING_JH2019:Spectral::UPSAMPLING_SMITS);
            require(!outer.IsotropicPhase(value,pdf),"upsampling change reused phase"); ++misses;
            Spectral::SetUpsamplingModel(upsampling);
        }
        for (const char *name : {"anisotropic","textured"}) {
            const auto &v=static_cast<const HomogeneousVolume &>(scene.GetMaterials().GetMaterial(name));
            HomogeneousVolume::InteriorScope other(v,hp);
            require(!other.IsotropicPhase(value,pdf),"nonconstant/nonisotropic phase cached"); ++misses;
        }
    }
    std::cout<<"phase_scope_pass hits="<<hits<<" misses="<<misses<<'\n';
}
