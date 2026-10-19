// SPDX-License-Identifier: Apache-2.0
// Exercise the actual public BSDF sampler and material-op interpreter on Metal.
#include "luxrays/core/context.h"
#include "luxrays/core/hardwaredevice.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/bsdf/bsdf.h"
#include "slg/volumes/homogenous.h"
#include "slg/textures/constfloat.h"
#include "slg/textures/constfloat3.h"
#include "slg/materials/materialdefs.h"
#include "slg/materials/ggxglass_energy_table.h"
#include "slg/engines/pathoclbase/pathoclbaseoclthread.h"
#include <vector>
#include <array>
#include <iostream>
#include <fstream>
#include <cmath>
#include <stdexcept>
using namespace luxrays;using namespace slg;
namespace slg {void (*SLG_DebugHandler)(const char *) __attribute__((weak))=nullptr;void (*LuxRays_DebugHandler)(const char *) __attribute__((weak))=nullptr;}
struct Bridge:PathOCLBaseOCLRenderThread{using PathOCLBaseOCLRenderThread::GetKernelSources;};
const char *kernelSource=R"CLC(
__kernel void PhaseProbe(__global const Material *materials,__global const BSDF *bsdfs,
 __global const Texture *textures,__global const MaterialEvalOp *operations,
 __global float *stacks,__global float *output,__global const float *tables,const uint count){
 const uint id=get_global_id(0),gid=id;if(id>=count)return;
    __global const Material *mats = materials;
    __global const MaterialEvalOp *matEvalOps = operations;
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

 float3 d;float pdf,cosine;BSDFEvent event;
 const uint n=id%32u;
 const float u0=n==16u?.5f:n==17u?.5000001f:n==18u?.4999999f:(n+.5f)/32.f;
 float3 value=BSDF_Sample(bsdfs+id,u0,((n*21u)%32u+.5f)/32.f,&d,&pdf,&cosine,&event MATERIALS_PARAM);
 output[id*9]=value.x;output[id*9+1]=value.y;output[id*9+2]=value.z;
 output[id*9+3]=d.x;output[id*9+4]=d.y;output[id*9+5]=d.z;
 output[id*9+6]=pdf;output[id*9+7]=cosine;output[id*9+8]=(float)event;
}
)CLC";
int main(int argc,char **argv)try{
 if(argc!=2)throw std::runtime_error("usage: volume_phase_device_probe receipt.json");
 slg::Init();Scene scene;std::vector<slg::ocl::Material> materials;std::vector<slg::ocl::BSDF> bsdfs;
 std::vector<slg::ocl::Texture> textures;std::vector<slg::ocl::MaterialEvalOp> operations;std::vector<std::array<float,9>> expected;
 ConstFloatTexture ior(1.f),zero(0.f),one(1.f);ConstFloat3Texture absorption(Spectrum(.2f,.1f,.3f)),scattering(Spectrum(.8f,.9f,.7f));
 for(bool colored:{false,true})for(bool hg:{false,true})for(float g:{-.6f,0.f,.6f})for(Vector axis:{Vector(0,0,1),Vector(.2f,.5f,1),Vector(-.7f,.3f,1)})for(bool light:{false,true}){
  ConstFloatTexture asym(g);HomogeneousVolume volume(ior,nullptr,colored?static_cast<const Texture&>(absorption):zero,colored?static_cast<const Texture&>(scattering):one,asym,true,hg,false);
  for(unsigned n=0;n<32;++n){
   BSDF cpu;Ray ray(Point(),Normalize(axis),0.f,1.f,.5f);cpu.Init(light,false,scene,ray,volume,.5f,.5f);
   const float u0=n==16u?.5f:n==17u?.5000001f:n==18u?.4999999f:(n+.5f)/32.f;
   Vector direction;float pdf,cosine;BSDFEvent event;Spectrum value=cpu.Sample(&direction,u0,((n*21u)%32+.5f)/32,&pdf,&cosine,&event);
   expected.push_back({value.c[0],value.c[1],value.c[2],direction.x,direction.y,direction.z,pdf,cosine,float(event)});
   slg::ocl::BSDF b{};b.materialIndex=materials.size();b.isVolume=1;b.hitPoint.fixedDir={cpu.hitPoint.fixedDir.x,cpu.hitPoint.fixedDir.y,cpu.hitPoint.fixedDir.z};
   b.hitPoint.geometryN=b.hitPoint.shadeN=b.hitPoint.interpolatedN={cpu.hitPoint.geometryN.x,cpu.hitPoint.geometryN.y,cpu.hitPoint.geometryN.z};
   b.hitPoint.rayFlags=light?LIGHT_RAY:EYE_RAY;b.hitPoint.passThroughEvent=.5f;b.hitPoint.interiorVolumeIndex=b.hitPoint.exteriorVolumeIndex=0xffffffffu;
   const auto &f=cpu.GetFrame();b.frame.X={f.X.x,f.X.y,f.X.z};b.frame.Y={f.Y.x,f.Y.y,f.Y.z};b.frame.Z={f.Z.x,f.Z.y,f.Z.z};bsdfs.push_back(b);
   slg::ocl::Material m{};m.type=slg::ocl::HOMOGENEOUS_VOL;m.evalSampleOpStartIndex=operations.size();m.evalSampleOpLength=1;
   auto addTexture=[&](Spectrum s){slg::ocl::Texture t{};t.type=slg::ocl::CONST_FLOAT3;t.constFloat3.color.c[0]=s.c[0];t.constFloat3.color.c[1]=s.c[1];t.constFloat3.color.c[2]=s.c[2];textures.push_back(t);return unsigned(textures.size()-1);};
   auto &v=m.volume.homogenous;v.sigmaATexIndex=addTexture(colored?Spectrum(.2f,.1f,.3f):Spectrum(0.f));v.sigmaSTexIndex=addTexture(colored?Spectrum(.8f,.9f,.7f):Spectrum(1.f));v.gTexIndex=addTexture(Spectrum(g));
   v.multiScattering=1;v.phaseFunc=hg;v.sssAlbedoTexIndex=v.sssMfpTexIndex=0xffffffffu;
   slg::ocl::MaterialEvalOp op{};op.matIndex=materials.size();op.evalType=slg::ocl::EVAL_SAMPLE;operations.push_back(op);materials.push_back(m);
  }
 }
 Context context;DeviceDescriptions selected;for(auto d:context.GetAvailableDeviceDescriptions())if(d.get().GetType()&DEVICE_TYPE_METAL_ALL){selected.push_back(d);break;}
 if(selected.empty())throw std::runtime_error("Metal unavailable");auto devices=context.AddHardwareDevices(selected);auto &device=devices.front().get();context.Start();device.PushThreadCurrentDevice();std::vector<HardwareDeviceBuffer*> buffers;
 try{
  std::vector<std::string> opts={"-D LUXRAYS_OPENCL_KERNEL","-D SLG_OPENCL_KERNEL","-D RENDER_ENGINE_PATHOCL","-D PARAM_RAY_EPSILON_MIN=1e-5f","-D PARAM_RAY_EPSILON_MAX=0.1f","-D SLG_SHADOW_TERMINATOR_MODE=0","-D LUXCORE_METAL"};
  auto program=device.CompileProgram(opts,Bridge::GetKernelSources()+kernelSource,"volume-phase-public-probe");auto kernel=device.GetKernel(*program,"PhaseProbe");
  const unsigned count=materials.size();std::vector<float> stacks(count*128,0),output(count*9,0);
  auto alloc=[&](const void *p,size_t size){HardwareDeviceBuffer *b=nullptr;device.AllocBufferRW(&b,const_cast<void*>(p),size,"phase probe");buffers.push_back(b);return b;};
  HardwareDeviceBuffer *args[]={alloc(materials.data(),count*sizeof(materials[0])),alloc(bsdfs.data(),count*sizeof(bsdfs[0])),alloc(textures.data(),textures.size()*sizeof(textures[0])),alloc(operations.data(),count*sizeof(operations[0])),alloc(stacks.data(),stacks.size()*sizeof(float)),alloc(output.data(),output.size()*sizeof(float)),alloc(GgxGlassETable,sizeof(GgxGlassETable))};
  for(unsigned i=0;i<7;++i)device.SetKernelArg<HardwareDeviceBufferRPtr>(kernel,i,args[i]);device.SetKernelArg(kernel,7,count);device.EnqueueKernel(kernel,HardwareDeviceRange(count),HardwareDeviceRange(1));device.FinishQueue();device.EnqueueReadBuffer(args[5],true,output.size()*sizeof(float),output.data());device.FinishQueue();
  double maxError=0;for(unsigned i=0;i<count;++i)for(unsigned j=0;j<9;++j){float a=expected[i][j],b=output[i*9+j];maxError=std::max(maxError,double(fabsf(a-b)));if(!std::isfinite(b)||fabsf(a-b)>2e-6f+1e-4f*std::max(fabsf(a),fabsf(b)))throw std::runtime_error("CPU/Metal phase mismatch case="+std::to_string(i)+" field="+std::to_string(j)+" expected="+std::to_string(a)+" got="+std::to_string(b));}
  std::ofstream out(argv[1]);out<<"{\"cases\":"<<count<<",\"checks\":"<<count*9<<",\"max_absolute_error\":"<<maxError<<",\"passed\":true}\n";
  for(auto &b:buffers)device.FreeBuffer(&b);device.PopThreadCurrentDevice();context.Stop();std::cout<<"phase_device_pass cases="<<count<<'\n';
 }catch(...){try{device.FinishQueue();}catch(...){}for(auto &b:buffers)if(b)device.FreeBuffer(&b);device.PopThreadCurrentDevice();context.Stop();throw;}
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 2;}
