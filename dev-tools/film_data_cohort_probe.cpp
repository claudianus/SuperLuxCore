// Actual production GPU data resolver, with tagged samples and adversarial guards.
#include "luxrays/core/context.h"
#include "luxrays/core/hardwareintersectiondevice.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/engines/pathoclbase/pathoclbaseoclthread.h"
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak))=nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak))=nullptr;
}
using namespace luxrays;
struct KernelBridge:slg::PathOCLBaseOCLRenderThread {using slg::PathOCLBaseOCLRenderThread::GetKernelSources;};
int main(int argc,char **argv)try{
 if(argc!=2)throw std::runtime_error("usage: film-data-cohort-probe result.json");slg::Init();
 slg::Scene scene;auto props=std::make_unique<Properties>();props->SetFromString(
 "scene.camera.lookat.orig=0 0 0\nscene.camera.lookat.target=0 0 1\nscene.camera.up=0 1 0\n"
 "scene.materials.m.type=matte\nscene.shapes.t.type=inlinedmesh\nscene.shapes.t.vertices=-1 -1 10 1 -1 10 0 1 10\nscene.shapes.t.faces=0 1 2\nscene.objects.t.shape=t\nscene.objects.t.material=m\nscene.lights.env.type=constantinfinite\n");scene.Parse(props);
 Context ctx(nullptr);DeviceDescriptions selected;for(auto d:ctx.GetAvailableDeviceDescriptions())if(d.get().GetType()&DEVICE_TYPE_METAL_ALL){selected.push_back(d);break;}if(selected.empty())throw std::runtime_error("No Metal");auto devices=ctx.AddIntersectionDevices(selected);const unsigned region[]={0,7,0,3};scene.Preprocess(ctx,8,4,region,false);
 auto &device=dynamic_cast<HardwareIntersectionDevice&>(devices[0].get());device.PushThreadCurrentDevice();std::vector<HardwareDeviceBuffer*> buffers;
 try{
  auto alloc=[&](const void *data,size_t bytes){HardwareDeviceBuffer *b=nullptr;device.AllocBufferRW(&b,const_cast<void*>(data),bytes,"cohort QA");buffers.push_back(b);return b;};
  constexpr unsigned pixels=32,aa=8,total=pixels*aa*aa,eyeCount=total-17;
  std::vector<slg::ocl::SampleResult> samples(total);
  std::vector<slg::ocl::pathoclbase::GPUTaskStats> stats(total);
  std::vector<unsigned> winner(pixels);
  for(unsigned p=0;p<pixels;++p){
   winner[p]=p*64;
   for(unsigned i=0;i<64;++i){unsigned g=p*64+i;auto &s=samples[g];s.pixelX=p%8;s.pixelY=p/8;s.depth=1.f+i;
    const float tag=g+.25f;s.position={tag,tag+1,tag+2};s.geometryNormal={tag+3,tag+4,tag+5};s.shadingNormal={tag+6,tag+7,tag+8};s.materialID=g+100;s.objectID=g+200;s.uv={tag+9,tag+10};for(unsigned j=0;j<4;++j)s.motionVector[j]=tag+11+j;stats[g].activeEyeSample=1;stats[g].sampleCount=1;
   }
  }
  stats[64].sampleCount=0;winner[1]=65; // unfinished nearest sample
  samples[128].pixelX=7;winner[2]=129; // wrong pixel must not leak in
  for(unsigned i=0;i<64;++i){stats[3*64+i].activeEyeSample=0;samples[4*64+i].depth=std::numeric_limits<float>::infinity();}
  winner[3]=winner[4]=~0u; // inactive/light task and miss-only pixels
  samples[5*64].depth=2;samples[5*64+1].depth=1;samples[5*64+3].depth=1;winner[5]=5*64+1; // stable tie
  stats[7*64].activeEyeSample=0;winner[7]=7*64+1;
  samples[total-1].depth=.01f; // belongs outside the bounded eye population
  slg::ocl::pathoclbase::GPUTaskConfiguration config{};config.film.hasChannelDepth=1;config.film.hasChannelPosition=1;
  config.film.hasChannelGeometryNormal=1;config.film.hasChannelShadingNormal=1;config.film.hasChannelMaterialID=1;config.film.hasChannelUV=1;config.film.hasChannelObjectID=1;config.film.hasChannelMotionVector=1;
  auto cb=alloc(&config,sizeof(config)),sb=alloc(stats.data(),sizeof(stats[0])*total),rb=alloc(samples.data(),sizeof(samples[0])*total);
  std::vector<float> depth(pixels,-10),pos(pixels*3,-10),geom=pos,shade=pos,uv(pixels*2,-10),motion(pixels*4,-10);std::vector<unsigned> mat(pixels,~0u),obj=mat;
  auto db=alloc(depth.data(),depth.size()*4),pb=alloc(pos.data(),pos.size()*4),gb=alloc(geom.data(),geom.size()*4),nb=alloc(shade.data(),shade.size()*4),mb=alloc(mat.data(),mat.size()*4),ub=alloc(uv.data(),uv.size()*4),ob=alloc(obj.data(),obj.size()*4),vb=alloc(motion.data(),motion.size()*4);
  auto program=device.CompileProgram({"-D LUXRAYS_OPENCL_KERNEL","-D SLG_OPENCL_KERNEL","-D RENDER_ENGINE_TILEPATHOCL","-D LUXCORE_METAL","-D SLG_SHADOW_TERMINATOR_MODE=0","-D PARAM_RAY_EPSILON_MIN=1e-5f","-D PARAM_RAY_EPSILON_MAX=1e-1f"},KernelBridge::GetKernelSources(),"production-film-cohort-QA");auto k=device.GetKernel(*program,"TilePathResolveFilmData");
  unsigned arg=0;for(auto *b:{cb,sb,rb})device.SetKernelArg(k,arg++,b);for(unsigned n:{eyeCount,aa,8u,4u})device.SetKernelArg(k,arg++,n);for(auto *b:{db,pb,gb,nb,mb,ub,ob,vb})device.SetKernelArg(k,arg++,b);
  unsigned checks=0;
  for(unsigned repeat=0;repeat<8;++repeat){
   device.EnqueueKernel(k,HardwareDeviceRange(64),HardwareDeviceRange(1));device.FinishQueue();
   auto read=[&](HardwareDeviceBuffer *b,auto &a){device.EnqueueReadBuffer(b,true,a.size()*sizeof(a[0]),a.data());};read(db,depth);read(pb,pos);read(gb,geom);read(nb,shade);read(mb,mat);read(ub,uv);read(ob,obj);read(vb,motion);
   for(unsigned p=0;p<pixels;++p){unsigned g=winner[p];if(g==~0u){if(depth[p]!=-10 || pos[p*3]!=-10)throw std::runtime_error("Inactive/miss data overwritten");continue;}auto &s=samples[g];
    if(depth[p]!=s.depth || mat[p]!=s.materialID || obj[p]!=s.objectID)throw std::runtime_error("Wrong depth or ID cohort");
    const float expected[]={s.position.x,s.position.y,s.position.z,s.geometryNormal.x,s.geometryNormal.y,s.geometryNormal.z,s.shadingNormal.x,s.shadingNormal.y,s.shadingNormal.z,s.uv.u,s.uv.v,s.motionVector[0],s.motionVector[1],s.motionVector[2],s.motionVector[3]};
    const float actual[]={pos[p*3],pos[p*3+1],pos[p*3+2],geom[p*3],geom[p*3+1],geom[p*3+2],shade[p*3],shade[p*3+1],shade[p*3+2],uv[p*2],uv[p*2+1],motion[p*4],motion[p*4+1],motion[p*4+2],motion[p*4+3]};
    for(unsigned j=0;j<15;++j){if(actual[j]!=expected[j])throw std::runtime_error("Mixed primary data sample");++checks;}
   }
  }
  // Disabled channels retain the existing flag contract with null buffers.
  config.film.hasChannelGeometryNormal=0;config.film.hasChannelShadingNormal=0;config.film.hasChannelMaterialID=0;config.film.hasChannelUV=0;config.film.hasChannelObjectID=0;config.film.hasChannelMotionVector=0;
  device.EnqueueWriteBuffer(cb,true,sizeof(config),&config);
  for(unsigned a=9;a<15;++a)device.SetKernelArg<HardwareDeviceBufferRPtr>(k,a,nullptr);
  const auto before=pos;
  device.EnqueueKernel(k,HardwareDeviceRange(64),HardwareDeviceRange(1));device.FinishQueue();
  device.EnqueueReadBuffer(pb,true,pos.size()*4,pos.data());if(pos!=before)throw std::runtime_error("Optional buffers changed cohort");
  // No-depth dispatch is a no-op.
  device.SetKernelArg<HardwareDeviceBufferRPtr>(k,7,nullptr);device.EnqueueKernel(k,HardwareDeviceRange(64),HardwareDeviceRange(1));device.FinishQueue();
  device.EnqueueReadBuffer(pb,true,pos.size()*4,pos.data());if(pos!=before)throw std::runtime_error("No-depth dispatch changed data");
  for(auto &b:buffers)device.FreeBuffer(&b);device.PopThreadCurrentDevice();ctx.Stop();
  std::ofstream out(argv[1]);out<<"{\"passed\":true,\"pixels\":32,\"samples_per_pixel\":64,\"repeats\":8,\"exact_float_component_checks\":"<<checks<<",\"coherent_IDs\":true,\"partial_eye_tail_guard\":true,\"inactive_miss_incomplete_wrong_pixel_tie_guards\":true,\"no_depth_noop\":true,\"actual_production_kernel\":true}\n";
 }catch(...){try{device.FinishQueue();}catch(...){}for(auto &b:buffers)if(b)device.FreeBuffer(&b);device.PopThreadCurrentDevice();ctx.Stop();throw;}
 return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
