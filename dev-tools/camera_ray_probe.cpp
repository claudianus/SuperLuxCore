// Actual production camera compiler + CPU camera + GPU Camera_GenerateRay.
// Captures rays without changing sampler, camera defaults, or intersection code.
#include "luxrays/core/context.h"
#include "luxrays/core/device.h"
#include "luxrays/core/hardwareintersectiondevice.h"
#include "luxrays/core/epsilon.h"
#include "luxrays/utils/properties.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/materials/materialdefs.h"
#include "slg/volumes/volume.h"
#include "slg/engines/pathoclbase/compiledscene.h"
#include "slg/engines/pathoclbase/pathoclbaseoclthread.h"
#include <array>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>
namespace slg {
void (*SLG_DebugHandler)(const char *) __attribute__((weak))=nullptr;
void (*LuxRays_DebugHandler)(const char *) __attribute__((weak))=nullptr;
}
using namespace luxrays;
struct KernelBridge:slg::PathOCLBaseOCLRenderThread {using slg::PathOCLBaseOCLRenderThread::GetKernelSources;};
static void logger(const char *s){std::cerr<<s<<'\n';}
static const char *kernel=R"CLC(
__kernel void CameraRayProbe(__global const Camera *camera, __global const float *bokeh,
 __global const float *input, __global Ray *rays, __global PathVolumeInfo *volumes) {
 uint i=get_global_id(0);if(i>=3u)return;
 // The production eye-path caller clears its volume state before the camera.
 // StartVolume only adds a non-null camera volume; it does not reset a path.
 PathVolumeInfo_Init(volumes+i);
 Camera_GenerateRay(camera,bokeh,1280u,720u,rays+i,volumes+i,
  input[5u*i],input[5u*i+1u],input[5u*i+2u],input[5u*i+3u],input[5u*i+4u]);
}
)CLC";
static void writeRay(std::ostream &o,const Ray &r){
 for(float f:{r.o.x,r.o.y,r.o.z,r.d.x,r.d.y,r.d.z,r.mint,r.maxt,r.time})o<<','<<f;
 o<<','<<r.flags;
}
int main(int argc,char **argv) try {
 if(argc!=2 && argc!=3)throw std::runtime_error("usage: camera-ray-probe output.csv [--settings]");
 const bool settings=argc==3 && std::string(argv[2])=="--settings";
 if(argc==3 && !settings)throw std::runtime_error("Unknown option");
 slg::SLG_DebugHandler=logger;slg::LuxRays_DebugHandler=logger;slg::Init();
 MachineEpsilon::SetMin(1e-5f);MachineEpsilon::SetMax(.1f);
 // Keep the scene alive until after the context/dataset teardown.
 slg::Scene scene;
 auto props=std::make_unique<Properties>();props->SetFromString(
  "scene.camera.lookat.orig=0 0 0\nscene.camera.lookat.target=0 0 1\nscene.camera.up=0 1 0\n"
  "scene.materials.white.type=matte\nscene.materials.white.kd=.5 .5 .5\n"
  "scene.shapes.tri.type=inlinedmesh\nscene.shapes.tri.vertices=-1 -1 10 1 -1 10 0 1 10\n"
  "scene.shapes.tri.faces=0 1 2\nscene.objects.tri.shape=tri\nscene.objects.tri.material=white\n"
  "scene.lights.env.type=constantinfinite\nscene.lights.env.color=1 1 1\n");
 if(settings)props->SetFromString("scene.volumes.world_medium.type=clear\nscene.volumes.world_medium.ior=1.2\nscene.volumes.world_medium.absorption=.01 .02 .03\nscene.volumes.explicit_medium.type=clear\nscene.volumes.explicit_medium.ior=1.4\nscene.volumes.explicit_medium.absorption=.03 .02 .01\nscene.world.volume.default=world_medium\n");
 scene.Parse(props);
 Context ctx(logger);DeviceDescriptions selected;
 for(auto d:ctx.GetAvailableDeviceDescriptions())if(d.get().GetType()&DEVICE_TYPE_METAL_ALL){selected.push_back(d);break;}
 if(selected.empty())throw std::runtime_error("No Metal");auto devices=ctx.AddIntersectionDevices(selected);
 const unsigned region[]={0,1279,0,719};scene.Preprocess(ctx,1280,720,region,false);
 auto &device=dynamic_cast<HardwareIntersectionDevice&>(devices[0].get());device.PushThreadCurrentDevice();
 std::vector<HardwareDeviceBuffer*> buffers;
 try {
  auto alloc=[&](const void *data,size_t bytes){HardwareDeviceBuffer *b=nullptr;device.AllocBufferRW(&b,const_cast<void*>(data),bytes,"camera QA");buffers.push_back(b);return b;};
  slg::PathTracer pathTracer;Properties cfg;pathTracer.ParseOptions(cfg,*slg::PathTracer::GetDefaultProps());
  slg::CompiledScene compiled(scene,&pathTracer);
  const std::array<float,15> input={13.9861364f,61.6149712f,.15f,.2f,.7f,
    506.245422f,233.273361f,.5f,.31f,.81f,931.004089f,533.313202f,.85f,.73f,.27f};
  std::array<Ray,3> gpuRays;std::array<slg::ocl::PathVolumeInfo,3> volumes;float dummy=0;
  auto cb=alloc(&compiled.camera,sizeof(compiled.camera));auto bb=alloc(&dummy,sizeof(dummy));
  auto ib=alloc(input.data(),sizeof(input));auto rb=alloc(gpuRays.data(),sizeof(gpuRays));auto vb=alloc(volumes.data(),sizeof(volumes));
  auto program=device.CompileProgram({"-D LUXRAYS_OPENCL_KERNEL","-D SLG_OPENCL_KERNEL","-D RENDER_ENGINE_PATHOCL","-D LUXCORE_METAL","-D SLG_SHADOW_TERMINATOR_MODE=0","-D PARAM_RAY_EPSILON_MIN=1e-5f","-D PARAM_RAY_EPSILON_MAX=1e-1f"},KernelBridge::GetKernelSources()+kernel,"production-camera-ray-QA");
  auto k=device.GetKernel(*program,"CameraRayProbe");HardwareDeviceBuffer *args[]={cb,bb,ib,rb,vb};
  for(unsigned a=0;a<5;++a)device.SetKernelArg<HardwareDeviceBufferRPtr>(k,a,args[a]);
  std::ofstream out(argv[1]);if(!out)throw std::runtime_error("CSV open");out<<std::setprecision(10);
  out<<"camera,scenario,sample,source,ox,oy,oz,dx,dy,dz,mint,maxt,time,flags,volume_index,expected_volume_index\n";
  for(const std::string type:{"perspective","orthographic","environment","stereo-perspective","stereo-environment_180","stereo-environment_360"})for(unsigned scenario=0;scenario<6;++scenario){
   const bool stereo=type.starts_with("stereo-");const float x=scenario==0?0.f:(scenario==2?1000.f:10.f);
   auto cam=std::make_unique<Properties>();cam->Set(Property("scene.camera.type")(stereo?"stereo":type));
   if(stereo)cam->Set(Property("scene.camera.stereo.type")(type.substr(7)));
   cam->Set(Property("scene.camera.lookat.orig")(x,-20.f,30.f));cam->Set(Property("scene.camera.lookat.target")(x,-20.f,31.f));cam->Set(Property("scene.camera.up")(0.f,1.f,0.f));
   cam->Set(Property("scene.camera.cliphither")(.001f));cam->Set(Property("scene.camera.clipyon")(100.f));
   cam->Set(Property("scene.camera.lensradius")((scenario==2 && !stereo && type!="environment")?.2f:0.f));cam->Set(Property("scene.camera.focaldistance")(10.f));
   cam->Set(Property("scene.camera.shutteropen")(.1f));cam->Set(Property("scene.camera.shutterclose")(.9f));
   if(scenario==3 || scenario==4){
    const Transform t=scenario==3?Scale(2.f,.5f,1.5f):Translate(Vector(.2f,-.4f,.1f))*RotateZ(13.f);
    for(unsigned key=0;key<2;++key){const std::string s="scene.camera.motion."+std::to_string(key);cam->Set(Property(s+".time")(float(key)));const Transform end=(settings && scenario==4 && key==1)?Translate(Vector(.8f,-.1f,.3f))*RotateZ(31.f):t;cam->Set(Property(s+".transformation")(end.m));}
   }
   if(scenario==5 && (type=="perspective"||type=="orthographic")){
    cam->Set(Property("scene.camera.clippingplane.enable")(true));cam->Set(Property("scene.camera.clippingplane.center")(x,-20.f,30.1f));cam->Set(Property("scene.camera.clippingplane.normal")(0.f,0.f,1.f));
   }
   if(settings && (scenario==1 || scenario==4)){cam->Set(Property("scene.camera.autovolume.enable")(false));cam->Set(Property("scene.camera.volume")("explicit_medium"));}
   scene.Parse(cam);scene.PreprocessCamera(1280,720,region);if(settings)scene.GetCamera().UpdateAuto(scene);slg::EditActionList edit;edit.AddAction(slg::CAMERA_EDIT);compiled.Recompile(edit);
   if(!compiled.cameraBokehDistribution.empty())throw std::runtime_error("Unexpected image bokeh distribution");
   device.EnqueueWriteBuffer(cb,true,sizeof(compiled.camera),&compiled.camera);
   device.EnqueueKernel(k,HardwareDeviceRange(3),HardwareDeviceRange(1));device.FinishQueue();device.EnqueueReadBuffer(rb,true,sizeof(gpuRays),gpuRays.data());device.EnqueueReadBuffer(vb,true,sizeof(volumes),volumes.data());
   for(unsigned j=0;j<3;++j){Ray cpu;slg::PathVolumeInfo volume;const float time=.1f+(.9f-.1f)*input[5*j+2];scene.GetCamera().GenerateRay(time,input[5*j],input[5*j+1],&cpu,&volume,input[5*j+3],input[5*j+4]);
    const unsigned cpuVolume=volume.HasCurrentVolume()?scene.GetMaterials().GetMaterialIndex(volume.GetCurrentVolume()):0xffffffffu;
    const unsigned expectedVolume=scene.GetCamera().HasVolume()?scene.GetMaterials().GetMaterialIndex(scene.GetCamera().GetVolume()):0xffffffffu;
    for(unsigned src=0;src<2;++src){out<<type<<','<<scenario<<','<<j<<','<<(src?"GPU":"CPU");writeRay(out,src?gpuRays[j]:cpu);out<<','<<(src?volumes[j].currentVolumeIndex:cpuVolume)<<','<<expectedVolume<<'\n';}
   }
  }
  for(auto &b:buffers)device.FreeBuffer(&b);device.PopThreadCurrentDevice();ctx.Stop();
 }catch(...){try{device.FinishQueue();}catch(...){}for(auto &b:buffers)if(b)device.FreeBuffer(&b);device.PopThreadCurrentDevice();ctx.Stop();throw;}
 return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
