// SPDX-License-Identifier: Apache-2.0
// Actual CPU BSDF::IsDelta vs production GPU BSDF_IsDelta contract replay.
// Usage: psr_delta_classification_probe output.csv
// Material binding uses the public BSDF setter; no surface scattering is evaluated.
#include "luxrays/core/context.h"
#include "luxrays/core/hardwaredevice.h"
#include "luxrays/core/device.h"
#include "luxrays/utils/properties.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/materials/materialdefs.h"
#include "slg/materials/openpbr.h"
#include "slg/bsdf/bsdf.h"
#include "slg/materials/cyclesbssrdf.h"
#include "slg/engines/pathoclbase/pathoclbaseoclthread.h"
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// Standalone static-library clients provide these application callbacks.
// Weak definitions also permit linking against a library owning the callbacks.
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak)) = nullptr;
}
namespace {
using namespace luxrays;
struct KernelBridge : slg::PathOCLBaseOCLRenderThread {
    using slg::PathOCLBaseOCLRenderThread::GetKernelSources;
};
void logMessage(const char *s) { std::cerr << s << '\n'; }
const char *probeKernel = R"CLC(
__kernel void PSRDeltaClassificationProbe(
    __global const Material *materials, __global const BSDF *bsdfs,
    __global const Texture *textures, __global const float *directions,
    __global float *stacks, __global float *output,
    __global const float *tables, const uint count) {
    const uint id = get_global_id(0);
    const uint gid = id;
    if (id >= count) return;
    __global const Material *mats = materials;
    __global const MaterialEvalOp *matEvalOps = 0;
    __global float *matEvalStacks = stacks;
    const uint maxMaterialEvalStackSize = 64;
    __global const Texture *texs = textures;
    __global const TextureEvalOp *texEvalOps = 0;
    __global float *texEvalStacks = stacks + count * 64;
    const uint maxTextureEvalStackSize = 64;
    __global const ImageMap *imageMapDescs = 0;
    __global const float * restrict * restrict imageMapBuff = 0;
    __constant const Scene *scene = 0;
    __global const SceneObject *sceneObjs = 0;
    __global const uint *lightIndexOffsetByMeshIndex = 0, *lightIndexByTriIndex = 0;
    __global const ExtMesh *meshDescs = 0;
    __global const Point *vertices = 0;
    __global const Normal *vertNormals = 0, *triNormals = 0;
    __global const UV *vertUVs = 0;
    __global const Spectrum *vertCols = 0;
    __global const float *vertAlphas = 0, *vertexAOVs = 0, *triAOVs = 0;
    __global const Triangle *triangles = 0;
    __global const InterpolatedTransform *interpolatedTransforms = 0;
    __global const float4 *curveCps = 0, *curveCpAttrs = 0;
    __global const uint *curveSegIndices = 0;
    __global const float *kernelTables = tables, *volMajorants = 0;
    __global const float4 *eqLightPoints = 0;
    const uint eqLightCount = 0;
    output[id*4] = (float)BSDF_IsDelta(bsdfs + id MATERIALS_PARAM);
    output[id*4+1] = (float)Material_IsDelta(id MATERIALS_PARAM);
    // Camera connections intentionally retain static material classification.
    output[id*4+2] = (float)(!BSDF_IsDeltaForCameraConnection(bsdfs + id MATERIALS_PARAM));
    output[id*4+3] = bsdfs[id].hitPoint.regularization;
}
)CLC";
}
int main(int argc,char**argv) try {
 using namespace luxrays;using namespace slg;
 if(argc!=2)throw std::runtime_error("output CSV required");
 SLG_DebugHandler=logMessage;LuxRays_DebugHandler=logMessage;slg::Init();
 slg::Scene scene;auto props=std::make_unique<Properties>();
 props->SetFromString("scene.materials.glass.type=glass\nscene.materials.mirror.type=mirror\nscene.materials.archglass.type=archglass\nscene.materials.null.type=null\nscene.materials.matte.type=matte\nscene.materials.roughglass.type=roughglass\nscene.materials.mix.type=mix\nscene.materials.mix.material1=glass\nscene.materials.mix.material2=mirror\nscene.materials.twosided.type=twosided\nscene.materials.twosided.frontmaterial=glass\nscene.materials.twosided.backmaterial=mirror\n");scene.Parse(props);
 std::vector<std::string> names;std::vector<bool> cpu,stat;std::vector<slg::ocl::Material> materials;std::vector<slg::ocl::BSDF> bsdfs;
 for(const auto &name:{"glass","mirror","archglass","null","matte","roughglass","mix","twosided"}) {
  auto &mat=scene.GetMaterials().GetMaterial(name);
  for(float sigma:{0.f,-.03f,1e-6f,.03f}) {
   slg::BSDF bsdf{};bsdf.SetSubsurfaceScatteringMaterial(mat);bsdf.hitPoint.Init();bsdf.hitPoint.regularization=sigma;
   // Only the actual public IsDelta() method is read after material binding;
   // no uninitialized surface/frame state is evaluated.
   names.emplace_back(name);cpu.push_back(bsdf.IsDelta());stat.push_back(mat.IsDelta());
   slg::ocl::Material m{};m.type=(slg::ocl::MaterialType)mat.GetType();m.isDelta=mat.IsDelta();materials.push_back(m);
   slg::ocl::BSDF g{};g.materialIndex=materials.size()-1;g.hitPoint.regularization=sigma;bsdfs.push_back(g);
  }
 }
 // Captured original GenmaB GLASS regularization at the three large NEE events.
 for(float sigma:{.0299999993f,.0289167799f,.0279748254f}) {
  auto &mat=scene.GetMaterials().GetMaterial("glass");slg::BSDF bsdf{};bsdf.SetSubsurfaceScatteringMaterial(mat);bsdf.hitPoint.Init();bsdf.hitPoint.regularization=sigma;
  names.emplace_back("GenmaB-glass");cpu.push_back(bsdf.IsDelta());stat.push_back(mat.IsDelta());
  slg::ocl::Material m{};m.type=slg::ocl::GLASS;m.isDelta=mat.IsDelta();materials.push_back(m);
  slg::ocl::BSDF g{};g.materialIndex=materials.size()-1;g.hitPoint.regularization=sigma;bsdfs.push_back(g);
 }
 // CPU proxy materials correspond to GPU's retained entry material + phase.
 for(unsigned phase:{2u,3u})for(float roughness:{0.f,.4f}) {
  if(phase==2u && roughness>0.f)continue;
  for(float sigma:{-.03f,0.f,.03f}) {
   auto pars=std::make_unique<Properties>();pars->Set(Property("scene.materials.sss.type")("cyclesbssrdf"));pars->Set(Property("scene.materials.sss.roughness")(roughness));scene.Parse(pars);
   const auto &sss=dynamic_cast<const CyclesBSSRDFMaterial&>(scene.GetMaterials().GetMaterial("sss"));
   const Material &proxy=phase==2u?static_cast<const Material&>(sss.GetExitMaterial()):static_cast<const Material&>(sss.GetAdjointBoundary());
   slg::BSDF bsdf{};bsdf.SetSubsurfaceScatteringMaterial(proxy);bsdf.hitPoint.Init();bsdf.hitPoint.regularization=sigma;
   names.emplace_back(phase==2u?"BSSRDF-diffuse-exit":roughness==0.f?"BSSRDF-sharp-adjoint":"BSSRDF-rough-adjoint");cpu.push_back(bsdf.IsDelta());stat.push_back(proxy.IsDelta());
   slg::ocl::Material m{};m.type=slg::ocl::CYCLES_BSSRDF;m.isDelta=sss.IsDelta();m.cyclesbssrdf.roughnessTexIndex=roughness==0.f?0:1;materials.push_back(m);
   slg::ocl::BSDF g{};g.materialIndex=materials.size()-1;g.cyclesBSSRDFPhase=phase;g.hitPoint.regularization=sigma;bsdfs.push_back(g);
  }
 }
 Context ctx(logMessage);DeviceDescriptions selected;for(auto d:ctx.GetAvailableDeviceDescriptions())if(d.get().GetType()&DEVICE_TYPE_METAL_ALL){selected.push_back(d);break;}
 if(selected.empty())throw std::runtime_error("Metal unavailable");auto devices=ctx.AddHardwareDevices(selected);auto &dev=devices.front().get();ctx.Start();dev.PushThreadCurrentDevice();
 std::vector<HardwareDeviceBuffer*> buffers;
 try {
  std::vector<std::string> opts={"-D LUXRAYS_OPENCL_KERNEL","-D SLG_OPENCL_KERNEL","-D RENDER_ENGINE_PATHOCL","-D PARAM_RAY_EPSILON_MIN=1e-5f","-D PARAM_RAY_EPSILON_MAX=0.1f","-D SLG_SHADOW_TERMINATOR_MODE=0","-D LUXCORE_METAL"};
  auto program=dev.CompileProgram(opts,KernelBridge::GetKernelSources()+probeKernel,"regularized-delta-classification");auto kernel=dev.GetKernel(*program,"PSRDeltaClassificationProbe");
  auto alloc=[&](const void*p,size_t size){HardwareDeviceBuffer*b=nullptr;dev.AllocBufferRW(&b,const_cast<void*>(p),size,"delta probe");buffers.push_back(b);return b;};
  std::vector<float> output(bsdfs.size()*4,0),stacks(bsdfs.size()*128,0);float dummy=0;std::array<slg::ocl::Texture,2> textures{};textures[0].type=textures[1].type=slg::ocl::CONST_FLOAT;textures[0].constFloat.value=0.f;textures[1].constFloat.value=.4f;
  HardwareDeviceBuffer*args[]={alloc(materials.data(),materials.size()*sizeof(materials[0])),alloc(bsdfs.data(),bsdfs.size()*sizeof(bsdfs[0])),alloc(textures.data(),sizeof(textures)),alloc(&dummy,sizeof(dummy)),alloc(stacks.data(),stacks.size()*sizeof(float)),alloc(output.data(),output.size()*sizeof(float)),alloc(&dummy,sizeof(dummy))};
  for(unsigned i=0;i<7;++i)dev.SetKernelArg<HardwareDeviceBufferRPtr>(kernel,i,args[i]);unsigned count=bsdfs.size();dev.SetKernelArg(kernel,7,count);
  dev.EnqueueKernel(kernel,HardwareDeviceRange(count),HardwareDeviceRange(1));dev.FinishQueue();dev.EnqueueReadBuffer(args[5],true,output.size()*sizeof(float),output.data());dev.FinishQueue();
  std::ofstream out(argv[1]);if(!out)throw std::runtime_error("CSV open failed");out<<std::setprecision(10)<<"material,sigma,cpu_delta,cpu_static,gpu_delta,gpu_static,gpu_camera_eligible,gpu_sigma\n";
  for(unsigned i=0;i<count;++i){out<<names[i]<<','<<bsdfs[i].hitPoint.regularization<<','<<cpu[i]<<','<<stat[i];for(unsigned j=0;j<4;++j)out<<','<<output[i*4+j];out<<'\n';}
  for(auto&b:buffers)dev.FreeBuffer(&b);dev.PopThreadCurrentDevice();ctx.Stop();std::cout<<"captured actual CPU/Metal BSDF classification "<<count<<" cases\n";
 }catch(...){try{dev.FinishQueue();}catch(...){}for(auto&b:buffers)if(b)dev.FreeBuffer(&b);dev.PopThreadCurrentDevice();ctx.Stop();throw;}
 return 0;
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
