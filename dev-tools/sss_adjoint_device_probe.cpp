// Deterministic single-vertex CPU/material-eval-op parity capture.
// SSS shared-frame RGB boundary diagnostic; not a render/quality gate.
// Compile/link with the renderer's current native build and dependencies.
// Usage: sss_adjoint_device_probe result.csv [metal|opencl]
#include "luxrays/core/context.h"
#include "luxrays/core/hardwaredevice.h"
#include "luxrays/core/device.h"
#include "luxrays/utils/properties.h"
#include "slg/slg.h"
#include "slg/scene/scene.h"
#include "slg/materials/materialdefs.h"
#include "slg/materials/openpbr.h"
#include "slg/materials/ggxglass_energy_table.h"
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
struct Case {
    unsigned owns, fromLight, order, variant, angle;
    Vector light, eye;
    Spectrum rgb;
    float direct, reverse;
    Spectrum sampleValue; Vector sampleDirection; float samplePdf;
    slg::BSDFEvent event;
};

// Deliberately call the public OpenPBR evaluation stack entry point, rather
// than reproducing its implementation or calling a private parameter helper.
const char *probeKernel = R"CLC(
__kernel void OpenPBRPublicEvalProbe(
    __global const Material *materials, __global const HitPoint *hitPoints,
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
    __global float *evalStack = stacks + id * 64;
    uint offset = 0;
    uint *evalStackOffset = &offset;
    const float3 light = MAKE_FLOAT3(directions[id*6], directions[id*6+1], directions[id*6+2]);
    const float3 eye = MAKE_FLOAT3(directions[id*6+3], directions[id*6+4], directions[id*6+5]);
    EvalStack_PushFloat3(light);
    EvalStack_PushFloat3(eye);
    OpenPBRMat_Evaluate(materials + id, hitPoints + id, evalStack, evalStackOffset MATERIALS_PARAM);
    float pdf; BSDFEvent event; float3 value;
    EvalStack_PopFloat(pdf);
    EvalStack_PopBSDFEvent(event);
    EvalStack_PopFloat3(value);
    output[id*14] = value.x; output[id*14+1] = value.y; output[id*14+2] = value.z;
    output[id*14+3] = pdf; output[id*14+4] = (float)event; output[id*14+5] = (float)offset;
    const float3 fixed = MAKE_FLOAT3(hitPoints[id].fixedDir.x, hitPoints[id].fixedDir.y, hitPoints[id].fixedDir.z);
    EvalStack_PushFloat3(fixed);
    EvalStack_PushFloat(.37f); EvalStack_PushFloat(.61f);
    EvalStack_PushFloat(.01f+.24f*(float)((id/8)%4));
    OpenPBRMat_Sample(materials + id, hitPoints + id, evalStack, evalStackOffset MATERIALS_PARAM);
    float3 sampled; float samplePdf;
    EvalStack_PopBSDFEvent(event); EvalStack_PopFloat(samplePdf);
    EvalStack_PopFloat3(sampled); EvalStack_PopFloat3(value);
    output[id*14+6]=value.x; output[id*14+7]=value.y; output[id*14+8]=value.z;
    output[id*14+9]=sampled.x; output[id*14+10]=sampled.y; output[id*14+11]=sampled.z;
    output[id*14+12]=samplePdf; output[id*14+13]=(float)offset;
}
)CLC";

// Constant RGB textures and an explicit clear IOR volume isolate the SSS
// interface. Upload the actual GGX energy table used by the renderer.
std::vector<slg::ocl::Texture> makeTextures() {
    std::vector<slg::ocl::Texture> t(12);
    for (auto &v : t) std::memset(&v, 0, sizeof(v));
    const float scalar[] = {0.f, 1.f, .3f, 1.5f, .65f, .35f, .8f, .2f};
    for (unsigned i=0; i<8; ++i) { t[i].type=slg::ocl::CONST_FLOAT; t[i].constFloat.value=scalar[i]; }
    auto rgb=[&](unsigned i,float r,float g,float b) {
        t[i].type=slg::ocl::CONST_FLOAT3;
        t[i].constFloat3.color.c[0]=r; t[i].constFloat3.color.c[1]=g; t[i].constFloat3.color.c[2]=b;
    };
    rgb(8,.8f,.5f,.2f); rgb(9,1.f,1.f,1.f); rgb(10,.7f,.9f,1.f); rgb(11,0.f,0.f,0.f);
    return t;
}
slg::ocl::Material makeMaterial(unsigned owns,unsigned variant) {
    slg::ocl::Material m{}; m.type=slg::ocl::OPENPBR; m.ownsLobeNormals=owns;
    m.interiorVolumeIndex=m.exteriorVolumeIndex=0xffffffffu;
    auto &p=m.openpbr;
    p.baseColorTexIndex=8; p.baseWeightTexIndex=1;
    p.baseDiffuseRoughnessTexIndex=variant==1?6:0;
    p.specWeightTexIndex=1; p.specColorTexIndex=9;
    p.specRoughnessTexIndex=2; p.specIorTexIndex=3;
    p.transColorTexIndex=9; p.transScatterTexIndex=11;
    p.sssWeightTexIndex=1; p.sssColorTexIndex=8; p.sssRadiusTexIndex=1; p.sssRadiusScaleTexIndex=9;
    p.coatColorTexIndex=10; p.coatRoughnessTexIndex=5; p.coatIorTexIndex=3;
    p.coatDarkeningTexIndex=1; p.fuzzColorTexIndex=9; p.fuzzRoughnessTexIndex=4;
    p.filmIorTexIndex=3;
    p.sellmeierBTexIndex=p.sellmeierCTexIndex=p.coatNormalTexIndex=0xffffffffu;
    p.coatAffectsBaseIor=1; p.cyclesNormalSemantics=owns; p.reflectionNormalCorrection=0;
    if (variant==2) { p.coatWeightTexIndex=5; p.fuzzWeightTexIndex=7; }
    return m;
}
slg::ocl::HitPoint gpuHit(const Case &c) {
    slg::ocl::HitPoint h{};
    const Vector f=c.fromLight?c.light:c.eye;
    h.fixedDir={f.x,f.y,f.z};
    h.geometryN=h.interpolatedN=h.shadeN=h.coatN={0,0,1};
    h.dpdu={1,0,0}; h.dpdv={0,1,0}; h.intoObject=f.z>0.f;
    h.rayFlags=c.fromLight?slg::LIGHT_RAY:slg::EYE_RAY;
    h.interiorVolumeIndex=h.exteriorVolumeIndex=0xffffffffu;
    h.interiorIorTexIndex=3; h.exteriorIorTexIndex=0xffffffffu;
    return h;
}
}

int main(int argc,char **argv) try {
    if (argc<2 || argc>3) throw std::runtime_error("usage: sss_adjoint_device_probe result.csv [metal|opencl]");
    using namespace luxrays;
    slg::SLG_DebugHandler=logMessage; slg::LuxRays_DebugHandler=logMessage; slg::Init();
    std::vector<Case> cases;
    for(unsigned variant=0;variant<1;++variant) for(unsigned owns=0;owns<1;++owns) {
        slg::Scene scene;
        auto parsedProps=std::make_unique<Properties>();
        auto &props=*parsedProps;
        props.SetFromString("scene.materials.test.type = openpbr\n"
            "scene.volumes.clear.type = clear\nscene.volumes.clear.ior = 1.5\nscene.materials.test.volume.interior = clear\nscene.materials.test.subsurfaceweight = 1\nscene.materials.test.subsurfacecolor = .8 .5 .2\nscene.materials.test.basecolor = .8 .5 .2\n"
            "scene.materials.test.specularroughness = .3\n"
            "scene.materials.test.specularior = 1.5\n"
            "scene.materials.test.coatcolor = .7 .9 1\n"
            "scene.materials.test.coatroughness = .35\n"
            "scene.materials.test.fuzzroughness = .65\n");
        props << Property("scene.materials.test.cyclesnormalsemantics")(owns!=0)
              << Property("scene.materials.test.basediffuseroughness")(variant==1?.8f:0.f)
              << Property("scene.materials.test.coatweight")(variant==2?.35f:0.f)
              << Property("scene.materials.test.fuzzweight")(variant==2?.2f:0.f);
        scene.Parse(parsedProps);
        auto &material=scene.GetMaterials().GetMaterial("test");
        if (material.OwnsLobeNormals()!=(owns!=0)) throw std::runtime_error("CPU lobe-normal configuration mismatch");
        for(unsigned a=0;a<4;++a) for(unsigned light=0;light<2;++light) for(unsigned order=0;order<4;++order) {
            const float z[]={.05f,.3f,.7f,.95f};
            Vector wi(std::sqrt(1.f-z[a]*z[a]),0,z[a]);
            Vector wo(0,.6f,.8f); if(order & 1) wi.z=-wi.z; if(order & 2) wo.z=-wo.z;
            Case c{owns,light,order,variant,a,wi,wo,Spectrum(),0,0,Spectrum(),Vector(),0,slg::NONE};
            slg::HitPoint h{}; h.Init(); h.fromLight=light; h.intoObject=true;
            h.fixedDir=light?wi:wo; h.intoObject=h.fixedDir.z>0; h.interiorVolume=material.GetInteriorVolume(); h.geometryN=h.interpolatedN=h.shadeN=h.coatN=Normal(0,0,1);
            h.dpdu=Vector(1,0,0); h.dpdv=Vector(0,1,0);
            h.rayFlags=light?slg::LIGHT_RAY:slg::EYE_RAY; h.regularization=0;
            c.rgb=material.Evaluate(h,wi,wo,&c.event,&c.direct,&c.reverse);
            slg::BSDFEvent sampleEvent;
            c.sampleValue=material.Sample(h,h.fixedDir,&c.sampleDirection,.37f,.61f,
                    .01f+.24f*a,&c.samplePdf,&sampleEvent); cases.push_back(c);
        }
    }
    std::vector<slg::ocl::Material> materials; std::vector<slg::ocl::HitPoint> hits;
    std::vector<float> dirs;
    for(const auto &c:cases) {
        materials.push_back(makeMaterial(c.owns,c.variant)); hits.push_back(gpuHit(c));
        dirs.insert(dirs.end(),{c.light.x,c.light.y,c.light.z,c.eye.x,c.eye.y,c.eye.z});
    }
    const auto textures=makeTextures();
    Context ctx(logMessage); DeviceDescriptions selected;
    const bool metal=argc==2 || std::string(argv[2])=="metal";
    for(auto d:ctx.GetAvailableDeviceDescriptions())
        if(d.get().GetType() & (metal?DEVICE_TYPE_METAL_ALL:DEVICE_TYPE_OPENCL_ALL)) {selected.push_back(d);break;}
    if(selected.empty()) throw std::runtime_error("Requested hardware device unavailable");
    auto devices=ctx.AddHardwareDevices(selected); auto &dev=devices.front().get(); ctx.Start();
    dev.PushThreadCurrentDevice();
    std::vector<HardwareDeviceBuffer*> buffers;
    try {
        std::vector<std::string> opts={"-D LUXRAYS_OPENCL_KERNEL","-D SLG_OPENCL_KERNEL",
            "-D RENDER_ENGINE_PATHOCL","-D PARAM_RAY_EPSILON_MIN=1e-5f","-D PARAM_RAY_EPSILON_MAX=0.1f",
            "-D SLG_SHADOW_TERMINATOR_MODE=0",metal?"-D LUXCORE_METAL":"-D LUXCORE_GENERIC_OPENCL"};
        auto program=dev.CompileProgram(opts,KernelBridge::GetKernelSources()+probeKernel,"openpbr-public-eval-probe");
        auto kernel=dev.GetKernel(*program,"OpenPBRPublicEvalProbe");
        auto alloc=[&](const void *p,size_t size) {HardwareDeviceBuffer *b=nullptr;dev.AllocBufferRW(&b,const_cast<void *>(p),size,"OpenPBR probe");buffers.push_back(b);return b;};
        std::vector<float> output(cases.size()*14,0),stack(cases.size()*128,0); const float *table=slg::GgxGlassETable;
        HardwareDeviceBuffer *args[]={alloc(materials.data(),materials.size()*sizeof(materials[0])),
            alloc(hits.data(),hits.size()*sizeof(hits[0])),alloc(textures.data(),textures.size()*sizeof(textures[0])),
            alloc(dirs.data(),dirs.size()*sizeof(float)),alloc(stack.data(),stack.size()*sizeof(float)),
            alloc(output.data(),output.size()*sizeof(float)),alloc(table,sizeof(slg::GgxGlassETable))};
        for(unsigned i=0;i<7;++i) dev.SetKernelArg<HardwareDeviceBufferRPtr>(kernel,i,args[i]);
        const unsigned count=cases.size(); dev.SetKernelArg(kernel,7,count);
        dev.EnqueueKernel(kernel,HardwareDeviceRange(count),HardwareDeviceRange(1)); dev.FinishQueue();
        dev.EnqueueReadBuffer(args[5],true,output.size()*sizeof(float),output.data()); dev.FinishQueue();
        auto requireClose=[](float cpu,float gpu) {
            if (!std::isfinite(cpu) || !std::isfinite(gpu) ||
                    std::abs(cpu-gpu)>1e-8f+1e-4f*std::max(std::abs(cpu),std::abs(gpu)))
                throw std::runtime_error("CPU/device SSS boundary mismatch");
        };
        for(size_t i=0;i<cases.size();++i) {
            const auto &c=cases[i];
            for(unsigned j=0;j<3;++j) requireClose(c.rgb.c[j],output[i*14+j]);
            requireClose(c.fromLight?c.reverse:c.direct,output[i*14+3]);
            for(unsigned j=0;j<3;++j) requireClose(c.sampleValue.c[j],output[i*14+6+j]);
            requireClose(c.sampleDirection.x,output[i*14+9]);
            requireClose(c.sampleDirection.y,output[i*14+10]);
            requireClose(c.sampleDirection.z,output[i*14+11]);
            requireClose(c.samplePdf,output[i*14+12]);
            if(output[i*14+5]!=0.f || output[i*14+13]!=0.f)
                throw std::runtime_error("SSS material stack imbalance");
        }
        std::ofstream out(argv[1]); if(!out) throw std::runtime_error("Cannot open output CSV");
        out << std::setprecision(10) << "variant,owns,from_light,order,angle,light_x,light_y,light_z,eye_x,eye_y,eye_z,cpu_r,cpu_g,cpu_b,cpu_forward_pdf,cpu_reverse_pdf,cpu_material_light_pdf,cpu_event,gpu_r,gpu_g,gpu_b,gpu_material_light_pdf,gpu_event,stack_offset,cpu_sample_r,cpu_sample_g,cpu_sample_b,cpu_sample_x,cpu_sample_y,cpu_sample_z,cpu_sample_pdf,gpu_sample_r,gpu_sample_g,gpu_sample_b,gpu_sample_x,gpu_sample_y,gpu_sample_z,gpu_sample_pdf,sample_stack_offset\n";
        for(size_t i=0;i<cases.size();++i) {
            const auto &c=cases[i]; out<<c.variant<<','<<c.owns<<','<<c.fromLight<<','<<c.order<<','<<c.angle<<','
                <<c.light.x<<','<<c.light.y<<','<<c.light.z<<','<<c.eye.x<<','<<c.eye.y<<','<<c.eye.z<<','
                <<c.rgb.c[0]<<','<<c.rgb.c[1]<<','<<c.rgb.c[2]<<','<<c.direct<<','<<c.reverse<<','
                <<(c.fromLight?c.reverse:c.direct)<<','<<c.event;
            for(unsigned j=0;j<6;++j) out<<','<<output[i*14+j];
            for(unsigned j=0;j<3;++j)out<<','<<c.sampleValue.c[j];
            out<<','<<c.sampleDirection.x<<','<<c.sampleDirection.y<<','<<c.sampleDirection.z<<','<<c.samplePdf;
            for(unsigned j=6;j<14;++j)out<<','<<output[i*14+j];out<<'\n';
        }
        for(auto &b:buffers) dev.FreeBuffer(&b);
        dev.PopThreadCurrentDevice(); ctx.Stop();
        std::cout<<"capture_complete cases="<<cases.size()<<"; compare energy and material lightDir-density PDF separately\n";
    } catch(...) {
        try { dev.FinishQueue(); } catch(...) {}
        for(auto &b:buffers) if(b) dev.FreeBuffer(&b);
        dev.PopThreadCurrentDevice(); ctx.Stop(); throw;
    }
    return 0;
} catch(const std::exception &e) {std::cerr<<"probe_failed: "<<e.what()<<'\n';return 1;}
