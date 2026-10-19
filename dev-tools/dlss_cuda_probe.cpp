// SPDX-License-Identifier: Apache-2.0
// Standalone diagnostic: query and create CUDA Ray Reconstruction using our
// custom project identity. Does not borrow Blender's NVIDIA application ID.
#include <dlfcn.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <fstream>
#include <random>
#include <chrono>
#if __has_include(<cuda.h>)
#include <cuda.h>
#else
#include <cuew.h>
#endif
#define NVSDK_NGX_HEADER_ONLY
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>


struct TextureAPI {
    CUresult (*arrayCreate)(CUarray*,const CUDA_ARRAY3D_DESCRIPTOR*);
    CUresult (*arrayDestroy)(CUarray);
    CUresult (*textureCreate)(CUtexObject*,const CUDA_RESOURCE_DESC*,const CUDA_TEXTURE_DESC*,const CUDA_RESOURCE_VIEW_DESC*);
    CUresult (*textureDestroy)(CUtexObject);
    CUresult (*surfaceCreate)(CUsurfObject*,const CUDA_RESOURCE_DESC*);
    CUresult (*surfaceDestroy)(CUsurfObject);
    CUresult (*copy)(const CUDA_MEMCPY2D*);
    CUresult (*sync)();
    template<class T>static T load(void *driver,const char *name) {
        auto p=reinterpret_cast<T>(dlsym(driver,name));if(!p)throw std::runtime_error(name);return p;
    }
    explicit TextureAPI(void *driver) {
        arrayCreate=load<decltype(arrayCreate)>(driver,"cuArray3DCreate_v2");
        arrayDestroy=load<decltype(arrayDestroy)>(driver,"cuArrayDestroy");
        textureCreate=load<decltype(textureCreate)>(driver,"cuTexObjectCreate");
        textureDestroy=load<decltype(textureDestroy)>(driver,"cuTexObjectDestroy");
        surfaceCreate=load<decltype(surfaceCreate)>(driver,"cuSurfObjectCreate");
        surfaceDestroy=load<decltype(surfaceDestroy)>(driver,"cuSurfObjectDestroy");
        copy=load<decltype(copy)>(driver,"cuMemcpy2D_v2");sync=load<decltype(sync)>(driver,"cuCtxSynchronize");
    }
    static void check(CUresult result){if(result!=CUDA_SUCCESS)throw std::runtime_error("CUDA error "+std::to_string(result));}
};
struct Texture {
    TextureAPI &api;CUarray array=nullptr;CUtexObject texture=0;CUsurfObject surface=0;
    unsigned width,height,channels;
    Texture(TextureAPI &a,unsigned w,unsigned h,unsigned c):api(a),width(w),height(h),channels(c) {
        CUDA_ARRAY3D_DESCRIPTOR desc={};desc.Width=w;desc.Height=h;desc.NumChannels=c;
        desc.Format=CU_AD_FORMAT_FLOAT;desc.Flags=CUDA_ARRAY3D_SURFACE_LDST;
        TextureAPI::check(api.arrayCreate(&array,&desc));
        try {
            CUDA_RESOURCE_DESC resource={};resource.resType=CU_RESOURCE_TYPE_ARRAY;resource.res.array.hArray=array;
            CUDA_TEXTURE_DESC textureDesc={};textureDesc.addressMode[0]=textureDesc.addressMode[1]=textureDesc.addressMode[2]=CU_TR_ADDRESS_MODE_CLAMP;
            textureDesc.flags=CU_TRSF_NORMALIZED_COORDINATES;
            TextureAPI::check(api.textureCreate(&texture,&resource,&textureDesc,nullptr));
            TextureAPI::check(api.surfaceCreate(&surface,&resource));
        }catch(...){release();throw;}
    }
    void release(){if(surface)api.surfaceDestroy(surface);if(texture)api.textureDestroy(texture);if(array)api.arrayDestroy(array);surface=0;texture=0;array=nullptr;}
    ~Texture(){release();}
    void upload(const std::vector<float> &values){
        if(values.size()!=size_t(width)*height*channels)throw std::runtime_error("input dimensions");
        CUDA_MEMCPY2D c={};c.srcMemoryType=CU_MEMORYTYPE_HOST;c.srcHost=values.data();c.srcPitch=width*channels*sizeof(float);
        c.dstMemoryType=CU_MEMORYTYPE_ARRAY;c.dstArray=array;c.WidthInBytes=c.srcPitch;c.Height=height;TextureAPI::check(api.copy(&c));
    }
    std::vector<float> download(){
        std::vector<float> values(size_t(width)*height*channels);
        CUDA_MEMCPY2D c={};c.srcMemoryType=CU_MEMORYTYPE_ARRAY;c.srcArray=array;
        c.dstMemoryType=CU_MEMORYTYPE_HOST;c.dstHost=values.data();c.dstPitch=width*channels*sizeof(float);
        c.WidthInBytes=c.dstPitch;c.Height=height;TextureAPI::check(api.copy(&c));return values;
    }
};
static void evaluate(void *driver,NVSDK_NGX_Handle *handle,const std::filesystem::path &logs) {
    TextureAPI api(driver);constexpr unsigned w=128,h=72;
    Texture color(api,w,h,4),depth(api,w,h,1),diffuse(api,w,h,4),specular(api,w,h,4),normal(api,w,h,4),motion(api,w,h,2),specMotion(api,w,h,2),output(api,w,h,4);
    std::vector<float> dep(w*h,2.f),diff(w*h*4,.5f),spec(w*h*4,.04f),norm(w*h*4,0.f),mv(w*h*2,0.f),rgb(w*h*4,0.f);
    for(unsigned i=0;i<w*h;++i){norm[i*4+2]=1.f;norm[i*4+3]=.5f;diff[i*4+3]=spec[i*4+3]=1.f;rgb[i*4+3]=1.f;}
    depth.upload(dep);diffuse.upload(diff);specular.upload(spec);normal.upload(norm);motion.upload(mv);specMotion.upload(mv);
    std::mt19937 random(42);std::uniform_real_distribution<float> noise(-.15f,.15f);
    for(unsigned frame=0;frame<8;++frame) {
        for(unsigned i=0;i<w*h;++i){rgb[i*4]=.3f+noise(random);rgb[i*4+1]=.4f+noise(random);rgb[i*4+2]=.8f+noise(random);}
        color.upload(rgb);NVSDK_NGX_Parameter *params=nullptr;
        if(NVSDK_NGX_FAILED(NVSDK_NGX_CUDA_AllocateParameters(&params)))throw std::runtime_error("allocate evaluate parameters");
        params->Set(NVSDK_NGX_Parameter_Reset,frame==0 ? 1 : 0);
        params->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,0.f);params->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,0.f);
        params->Set(NVSDK_NGX_Parameter_Color,&color.texture);params->Set(NVSDK_NGX_Parameter_Depth,&depth.texture);
        params->Set(NVSDK_NGX_Parameter_DiffuseAlbedo,&diffuse.texture);params->Set(NVSDK_NGX_Parameter_SpecularAlbedo,&specular.texture);
        params->Set(NVSDK_NGX_Parameter_GBuffer_Normals,&normal.texture);params->Set(NVSDK_NGX_Parameter_GBuffer_Roughness,&normal.texture);
        params->Set(NVSDK_NGX_Parameter_MotionVectors,&motion.texture);params->Set(NVSDK_NGX_Parameter_GBuffer_SpecularMvec,&specMotion.texture);
        params->Set(NVSDK_NGX_Parameter_Output,&output.surface);
        params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,w);params->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,h);
        params->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis,1);
        const auto start=std::chrono::steady_clock::now();
        const auto result=NVSDK_NGX_CUDA_EvaluateFeature(handle,params,nullptr);NVSDK_NGX_CUDA_DestroyParameters(params);
        TextureAPI::check(api.sync());const auto values=output.download();
        const auto seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
        if(NVSDK_NGX_FAILED(result))throw std::runtime_error("evaluate result "+std::to_string(unsigned(result)));
        double mean[3]={};for(unsigned i=0;i<w*h;++i)for(unsigned c=0;c<4;++c){if(!std::isfinite(values[i*4+c]))throw std::runtime_error("nonfinite output");if(c<3)mean[c]+=values[i*4+c]/double(w*h);}
        if(mean[0]+mean[1]+mean[2]<.01)throw std::runtime_error("empty output");
        std::cout<<"evaluate frame="<<frame<<" result=0x"<<std::hex<<unsigned(result)<<std::dec<<" seconds="<<seconds<<" mean="<<mean[0]<<","<<mean[1]<<","<<mean[2]<<std::endl;
        if(frame==7){std::ofstream file(logs/"synthetic-output-rgba.f32",std::ios::binary);file.write(reinterpret_cast<const char*>(values.data()),values.size()*sizeof(float));}
    }
}

int main(int argc, char **argv) {
    if (argc != 3) { std::cerr << "usage: dlss_cuda_probe runtime-directory log-directory\n"; return 2; }
    void *driver = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!driver) { std::cerr << "CUDA driver unavailable\n"; return 3; }
    using Ctx = void *;
    auto init = reinterpret_cast<int(*)(unsigned)>(dlsym(driver,"cuInit"));
    auto get = reinterpret_cast<int(*)(int*,int)>(dlsym(driver,"cuDeviceGet"));
    auto name = reinterpret_cast<int(*)(char*,int,int)>(dlsym(driver,"cuDeviceGetName"));
    auto make = reinterpret_cast<int(*)(Ctx*,unsigned,int)>(dlsym(driver,"cuCtxCreate_v2"));
    auto destroy = reinterpret_cast<int(*)(Ctx)>(dlsym(driver,"cuCtxDestroy_v2"));
    if (!init || !get || !name || !make || !destroy) { dlclose(driver); return 4; }
    int device = -1; Ctx ctx = nullptr; char adapter[256] = {};
    if (init(0) || get(&device,0) || name(adapter,256,device) || make(&ctx,0,device)) {
        std::cerr << "CUDA initialization failed\n"; dlclose(driver); return 5;
    }
    std::filesystem::create_directories(argv[2]);
    const std::wstring log = std::filesystem::path(argv[2]).wstring();
    const std::wstring runtime = std::filesystem::path(argv[1]).wstring();
    const wchar_t *paths[] = {runtime.c_str()};
    NVSDK_NGX_FeatureCommonInfo common = {};
    common.PathListInfo.Path = paths; common.PathListInfo.Length = 1;
    const char *project = "dce55843-16d5-4ed3-82d7-969199dc950c";
    NVSDK_NGX_FeatureDiscoveryInfo discovery = {};
    discovery.SDKVersion = NVSDK_NGX_Version_API;
    discovery.FeatureID = NVSDK_NGX_Feature_RayReconstruction;
    discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    discovery.Identifier.v.ProjectDesc = {project,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"SuperLuxCore-2.11.34"};
    discovery.ApplicationDataPath = log.c_str(); discovery.FeatureInfo = &common;
    NVSDK_NGX_FeatureRequirement requirement = {};
    const auto query = NVSDK_NGX_CUDA_GetFeatureRequirements(device,&discovery,&requirement);
    std::cout << "adapter=" << adapter << "\nproject=" << project
              << "\nrequirements_result=0x" << std::hex << unsigned(query)
              << "\nrequirements_support=0x" << unsigned(requirement.FeatureSupported) << std::dec << std::endl;
    const auto initialized = NVSDK_NGX_CUDA_Init_with_ProjectID(project,NVSDK_NGX_ENGINE_TYPE_CUSTOM,
            "SuperLuxCore-2.11.34",log.c_str(),&common);
    std::cout << "initialize_result=0x" << std::hex << unsigned(initialized) << std::dec << std::endl;
    NVSDK_NGX_Handle *handle = nullptr;
    NVSDK_NGX_Result created = NVSDK_NGX_Result_FAIL_NotInitialized;
    if (NVSDK_NGX_SUCCEED(initialized)) {
        NVSDK_NGX_Parameter *params = nullptr;
        if (NVSDK_NGX_SUCCEED(NVSDK_NGX_CUDA_AllocateParameters(&params))) {
            params->Set(NVSDK_NGX_Parameter_Width,128u);params->Set(NVSDK_NGX_Parameter_Height,72u);
            params->Set(NVSDK_NGX_Parameter_OutWidth,128u);params->Set(NVSDK_NGX_Parameter_OutHeight,72u);
            params->Set(NVSDK_NGX_Parameter_PerfQualityValue,NVSDK_NGX_PerfQuality_Value_DLAA);
            params->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode,NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
            params->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,
                    NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling);
            params->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,0);
            params->Set(NVSDK_NGX_Parameter_Use_HW_Depth,NVSDK_NGX_DLSS_Depth_Type_Linear);
            params->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode,NVSDK_NGX_DLSS_Roughness_Mode_Packed);
            params->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature,1);
            created = NVSDK_NGX_CUDA_CreateFeature(NVSDK_NGX_Feature_RayReconstruction,params,&handle);
            NVSDK_NGX_CUDA_DestroyParameters(params);
        }
        std::cout << "create_result=0x" << std::hex << unsigned(created) << std::dec << std::endl;
        if (handle) {
            try { evaluate(driver,handle,std::filesystem::path(argv[2])); }
            catch(const std::exception &e){std::cerr<<"evaluation failed: "<<e.what()<<std::endl;created=NVSDK_NGX_Result_FAIL_PlatformError;}
            NVSDK_NGX_CUDA_ReleaseFeature(handle);
        }
        const auto shutdown = NVSDK_NGX_CUDA_Shutdown();
        std::cout << "shutdown_result=0x" << std::hex << unsigned(shutdown) << std::dec << std::endl;
    }
    destroy(ctx);dlclose(driver);
    return NVSDK_NGX_SUCCEED(created) ? 0 : 6;
}
