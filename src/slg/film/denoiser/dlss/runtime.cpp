// SPDX-License-Identifier: Apache-2.0
#include "api.h"
#include <cuew.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <utility>
#define NVSDK_NGX_HEADER_ONLY
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs_dlssd.h>

namespace {
constexpr const char *projectId="dce55843-16d5-4ed3-82d7-969199dc950c";
std::mutex mutex;
void cuda(CUresult r){if(r!=CUDA_SUCCESS)throw std::runtime_error("CUDA result "+std::to_string(r));}
void ngx(NVSDK_NGX_Result r){if(NVSDK_NGX_FAILED(r))throw std::runtime_error("NGX result "+std::to_string(unsigned(r)));}
void error(char *target,uint32_t capacity,const char *message){if(target&&capacity){std::strncpy(target,message,capacity-1);target[capacity-1]=0;}}
struct Manager {
    CUcontext context=nullptr;unsigned refs=0,device=0;std::string runtime,cache;
    bool cleanupFailed=false;
} manager;
bool cudaCleanup(CUresult result,const char *operation) noexcept {
    if(result==CUDA_SUCCESS)return true;
    manager.cleanupFailed=true;
    std::fprintf(stderr,"DLSS cleanup: %s failed (CUDA %d); restart Blender\n",operation,int(result));
    return false;
}
bool ngxCleanup(NVSDK_NGX_Result result,const char *operation) noexcept {
    if(!NVSDK_NGX_FAILED(result))return true;
    manager.cleanupFailed=true;
    std::fprintf(stderr,"DLSS cleanup: %s failed (NGX %u); restart Blender\n",operation,unsigned(result));
    return false;
}
void releaseManager() noexcept {
    if(!manager.refs||--manager.refs)return;
    if(cudaCleanup(cuCtxPushCurrent(manager.context),"shutdown context push")){
        ngxCleanup(NVSDK_NGX_CUDA_Shutdown(),"NGX shutdown");
        CUcontext previous=nullptr;
        cudaCleanup(cuCtxPopCurrent(&previous),"shutdown context pop");
    }
    // The context owns allocations whose explicit release failed. Do not
    // replace a still-owned context after a failed destroy or reuse failed NGX.
    if(cudaCleanup(cuCtxDestroy(manager.context),"context destroy")){
        const bool failed=manager.cleanupFailed;
        manager=Manager();manager.cleanupFailed=failed;
    }
}
void acquireManager(unsigned device,const char *runtime,const char *cache){
    if(manager.cleanupFailed)throw std::runtime_error("DLSS cleanup failed; restart Blender before creating another instance");
    if(!runtime||!cache||!*cache)throw std::runtime_error("Runtime and writable cache paths are required");
    if(manager.refs){
        if(device!=manager.device||runtime!=manager.runtime||cache!=manager.cache)throw std::runtime_error("Active DLSS instances must share one GPU/runtime/cache");
        ++manager.refs;return;
    }
    // Allocate publication strings before acquiring a native context. Moving
    // this completed state into manager cannot strand it on a host allocation.
    Manager prepared;prepared.device=device;prepared.runtime=runtime;prepared.cache=cache;
    if(cuewInit(CUEW_INIT_CUDA)!=CUEW_SUCCESS)throw std::runtime_error("CUDA driver is unavailable");
    cuda(cuInit(0));CUdevice gpu;cuda(cuDeviceGet(&gpu,device));
    CUcontext context=nullptr;cuda(cuCtxCreate(&context,0,gpu));
    bool current=true,ngxInitialized=false;
    try{
        CUcontext popped=nullptr;
        const CUresult initialPop=cuCtxPopCurrent(&popped);current=false;
        cudaCleanup(initialPop,"initial context pop");cuda(initialPop);
        const CUresult pushed=cuCtxPushCurrent(context);
        cudaCleanup(pushed,"initialization context push");cuda(pushed);current=true;
        // Python's os.fsencode supplies UTF-8 on Windows as well. Narrow
        // filesystem::path would interpret Korean user directories as ANSI.
        const auto runtimePath=std::filesystem::u8path(runtime),cachePath=std::filesystem::u8path(cache);
        std::filesystem::create_directories(cachePath);
        const auto wRuntime=runtimePath.wstring(),wCache=cachePath.wstring();
        const wchar_t *paths[]={wRuntime.c_str()};NVSDK_NGX_FeatureCommonInfo common={};common.PathListInfo.Path=paths;common.PathListInfo.Length=1;
        NVSDK_NGX_FeatureDiscoveryInfo discovery={};discovery.SDKVersion=NVSDK_NGX_Version_API;discovery.FeatureID=NVSDK_NGX_Feature_RayReconstruction;
        discovery.Identifier.IdentifierType=NVSDK_NGX_Application_Identifier_Type_Project_Id;
        discovery.Identifier.v.ProjectDesc={projectId,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"SuperLuxCore-2.11.34"};discovery.ApplicationDataPath=wCache.c_str();discovery.FeatureInfo=&common;
        ngx(NVSDK_NGX_CUDA_Init_with_ProjectID(projectId,NVSDK_NGX_ENGINE_TYPE_CUSTOM,"SuperLuxCore-2.11.34",wCache.c_str(),&common));
        ngxInitialized=true;
        // The header-only SDK resets GContext on Shutdown but retains GLib.
        // Its pre-init query wrapper then skips reloading the cleared query
        // pointer and returns NotImplemented. Init reloads the full API table.
        NVSDK_NGX_FeatureRequirement requirement={};ngx(NVSDK_NGX_CUDA_GetFeatureRequirements(gpu,&discovery,&requirement));
        if(requirement.FeatureSupported!=NVSDK_NGX_FeatureSupportResult_Supported)throw std::runtime_error("CUDA Ray Reconstruction is unsupported on the selected device/driver");
        const CUresult finalPop=cuCtxPopCurrent(&popped);current=false;
        cudaCleanup(finalPop,"initialization context pop");cuda(finalPop);
    }catch(...){
        // A failing pop may already have consumed its stack entry. Never pop
        // blindly: reacquire our own context if NGX still needs shutdown.
        if(ngxInitialized&&!current)
            current=cudaCleanup(cuCtxPushCurrent(context),"initialization rollback push");
        if(current){
            if(ngxInitialized)ngxCleanup(NVSDK_NGX_CUDA_Shutdown(),"initialization NGX rollback");
            CUcontext popped=nullptr;cudaCleanup(cuCtxPopCurrent(&popped),"initialization rollback pop");
        }
        if(!cudaCleanup(cuCtxDestroy(context),"initialization context rollback"))
            manager.context=context; // Retain identity; cleanupFailed blocks reuse.
        throw;
    }
    prepared.context=context;prepared.refs=1;manager=std::move(prepared);
}
struct Texture {
    CUarray array=nullptr;CUtexObject texture=0;CUsurfObject surface=0;
    unsigned width,height,channels;
    Texture(unsigned w,unsigned h,unsigned c):width(w),height(h),channels(c){
        CUDA_ARRAY3D_DESCRIPTOR desc={};desc.Width=w;desc.Height=h;desc.NumChannels=c;desc.Format=CU_AD_FORMAT_FLOAT;desc.Flags=CUDA_ARRAY3D_SURFACE_LDST;
        cuda(cuArray3DCreate(&array,&desc));
        try{
            CUDA_RESOURCE_DESC resource={};resource.resType=CU_RESOURCE_TYPE_ARRAY;resource.res.array.hArray=array;
            CUDA_TEXTURE_DESC desc={};desc.addressMode[0]=desc.addressMode[1]=desc.addressMode[2]=CU_TR_ADDRESS_MODE_CLAMP;desc.flags=CU_TRSF_NORMALIZED_COORDINATES;
            cuda(cuTexObjectCreate(&texture,&resource,&desc,nullptr));cuda(cuSurfObjectCreate(&surface,&resource));
        }catch(...){release();throw;}
    }
    void release() noexcept {
        if(surface)cudaCleanup(cuSurfObjectDestroy(surface),"surface destroy");
        if(texture)cudaCleanup(cuTexObjectDestroy(texture),"texture destroy");
        if(array)cudaCleanup(cuArrayDestroy(array),"array destroy");
        surface=0;texture=0;array=nullptr;
    }
    // A failed owning-context push must never free objects in the caller's
    // context. Retain native ownership in manager.context, consume host owners.
    void abandon() noexcept {surface=0;texture=0;array=nullptr;}
    ~Texture(){release();}
    Texture(const Texture&)=delete;Texture&operator=(const Texture&)=delete;
    void upload(const float *values){
        CUDA_MEMCPY2D c={};c.srcMemoryType=CU_MEMORYTYPE_HOST;c.srcHost=values;c.srcPitch=size_t(width)*channels*sizeof(float);
        c.dstMemoryType=CU_MEMORYTYPE_ARRAY;c.dstArray=array;c.WidthInBytes=c.srcPitch;c.Height=height;cuda(cuMemcpy2D(&c));
    }
    void download(float *values){
        CUDA_MEMCPY2D c={};c.srcMemoryType=CU_MEMORYTYPE_ARRAY;c.srcArray=array;c.dstMemoryType=CU_MEMORYTYPE_HOST;c.dstHost=values;
        c.dstPitch=size_t(width)*channels*sizeof(float);c.WidthInBytes=c.dstPitch;c.Height=height;cuda(cuMemcpy2D(&c));
    }
};
struct Parameters {
    NVSDK_NGX_Parameter *p=nullptr;
    Parameters(){ngx(NVSDK_NGX_CUDA_AllocateParameters(&p));}
    ~Parameters(){if(p)ngxCleanup(NVSDK_NGX_CUDA_DestroyParameters(p),"parameter destroy");}
    NVSDK_NGX_Parameter *operator->(){return p;}
};
struct Instance {
    uint64_t limit;SlcDLSSStats stats={};NVSDK_NGX_Handle *feature=nullptr;
    uint32_t preset=0;
    std::unique_ptr<Texture> color,depth,diffuse,specular,normal,motion,specMotion,output;
    std::vector<float> scratch;uint64_t lastId=0,lastScene=0;bool history=false;
    explicit Instance(uint64_t bytes):limit(bytes){stats.sdk_private_bytes=UINT64_MAX;}
    ~Instance(){clear();}
    void clear() noexcept {
        if(feature){cudaCleanup(cuCtxSynchronize(),"feature synchronize");ngxCleanup(NVSDK_NGX_CUDA_ReleaseFeature(feature),"feature release");feature=nullptr;}
        color.reset();depth.reset();diffuse.reset();specular.reset();normal.reset();motion.reset();specMotion.reset();output.reset();
        std::vector<float>().swap(scratch);history=false;stats.texture_bytes=stats.host_scratch_bytes=0;
        stats.width=stats.height=stats.output_width=stats.output_height=0;
    }
    void abandon() noexcept {
        feature=nullptr;
        for(auto *texture:{color.get(),depth.get(),diffuse.get(),specular.get(),normal.get(),motion.get(),specMotion.get(),output.get()})
            if(texture)texture->abandon();
        clear();
    }
    bool resize(const SlcDLSSFrame &f){
        if(feature&&stats.width==f.width&&stats.height==f.height&&stats.output_width==f.output_width&&stats.output_height==f.output_height)return false;
        const uint64_t textureBytes=uint64_t(f.width)*f.height*21*4+uint64_t(f.output_width)*f.output_height*4*4;
        const uint64_t hostBytes=uint64_t(f.output_width)*f.output_height*4*4;
        if(textureBytes+hostBytes>limit)throw std::runtime_error("DLSS owned-memory budget exceeded; SDK-private allocations are additional and unknown");
        clear();
        if(manager.cleanupFailed)throw std::runtime_error("DLSS cleanup failed; restart Blender before resizing");
        try{
            Parameters p;p->Set(NVSDK_NGX_Parameter_Width,f.width);p->Set(NVSDK_NGX_Parameter_Height,f.height);
            p->Set(NVSDK_NGX_Parameter_OutWidth,f.output_width);p->Set(NVSDK_NGX_Parameter_OutHeight,f.output_height);
            const bool nativeSize=f.width==f.output_width&&f.height==f.output_height;
            const double ratio=std::min(double(f.width)/f.output_width,double(f.height)/f.output_height);
            p->Set(NVSDK_NGX_Parameter_PerfQualityValue,nativeSize?NVSDK_NGX_PerfQuality_Value_DLAA:
                (ratio<=.55?NVSDK_NGX_PerfQuality_Value_MaxPerf:NVSDK_NGX_PerfQuality_Value_MaxQuality));
            p->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode,NVSDK_NGX_DLSS_Denoise_Mode_DLUnified);
            p->Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags,NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|NVSDK_NGX_DLSS_Feature_Flags_AlphaUpscaling);
            p->Set(NVSDK_NGX_Parameter_DLSS_Enable_Output_Subrects,0);p->Set(NVSDK_NGX_Parameter_Use_HW_Depth,NVSDK_NGX_DLSS_Depth_Type_Linear);
            p->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode,NVSDK_NGX_DLSS_Roughness_Mode_Packed);p->Set(NVSDK_NGX_Parameter_FreeMemOnReleaseFeature,1);
            if(preset) {
                for(const char *name : {NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
                        NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality})
                    p->Set(name,preset);
            }
            ngx(NVSDK_NGX_CUDA_CreateFeature(NVSDK_NGX_Feature_RayReconstruction,p.p,&feature));
            color=std::make_unique<Texture>(f.width,f.height,4);depth=std::make_unique<Texture>(f.width,f.height,1);
            diffuse=std::make_unique<Texture>(f.width,f.height,4);specular=std::make_unique<Texture>(f.width,f.height,4);normal=std::make_unique<Texture>(f.width,f.height,4);
            motion=std::make_unique<Texture>(f.width,f.height,2);specMotion=std::make_unique<Texture>(f.width,f.height,2);output=std::make_unique<Texture>(f.output_width,f.output_height,4);
            scratch.resize(size_t(f.output_width)*f.output_height*4);stats.texture_bytes=textureBytes;stats.host_scratch_bytes=hostBytes;
            stats.width=f.width;stats.height=f.height;stats.output_width=f.output_width;stats.output_height=f.output_height;
        }catch(...){clear();throw;}
        return true;
    }
    void process(const SlcDLSSFrame &f,float *target,uint64_t capacity){
        if(!f.width||!f.height||f.output_width<32||f.output_height<32||f.width>16384||f.height>16384||f.output_width>16384||f.output_height>16384)throw std::runtime_error("Invalid DLSS dimensions");
        if(!f.independent_samples)throw std::runtime_error("DLSS requires independent non-overlapping frame samples");
        if(stats.frames&&lastScene==f.scene_serial&&f.frame_id<=lastId)throw std::runtime_error("Duplicate or out-of-order DLSS frame");
        if(!target||capacity<uint64_t(f.output_width)*f.output_height*4)throw std::runtime_error("Output capacity is too small");
        const uint64_t required=uint64_t(f.width)*f.height*21*4+uint64_t(f.output_width)*f.output_height*4*4*2;
        if(required>limit)throw std::runtime_error("DLSS owned-memory budget exceeded; SDK-private allocations are additional and unknown");
        if(!std::isfinite(f.jitter_x)||!std::isfinite(f.jitter_y)||std::abs(f.jitter_x)>.5f||std::abs(f.jitter_y)>.5f)throw std::runtime_error("Jitter must match the traced frame and be within half a pixel");
        const float *planes[]={f.color,f.depth,f.diffuse,f.specular,f.normal_roughness,f.motion,f.specular_motion};const unsigned channels[]={4,1,4,4,4,2,2};
        for(unsigned p=0;p<7;++p){
            if(!planes[p])throw std::runtime_error("A required DLSS guide is missing");
            const auto begin=reinterpret_cast<uintptr_t>(planes[p]),end=begin+size_t(f.width)*f.height*channels[p]*sizeof(float);
            const uintptr_t outBegin=reinterpret_cast<uintptr_t>(target);
            const uintptr_t outEnd=outBegin+uintptr_t(f.output_width)*f.output_height*4*sizeof(float);
            if(outBegin<end&&begin<outEnd)throw std::runtime_error("DLSS output must not alias raw input or guides");
            for(size_t i=0;i<size_t(f.width)*f.height*channels[p];++i)if(!std::isfinite(planes[p][i]))throw std::runtime_error("Nonfinite DLSS input");
        }
        for(size_t i=0;i<size_t(f.width)*f.height;++i){
            if(f.depth[i]<0||f.normal_roughness[i*4+3]<0||f.normal_roughness[i*4+3]>1)throw std::runtime_error("Depth/roughness outside DLSS contract");
            if(f.color[i*4+3]<0||f.color[i*4+3]>1)throw std::runtime_error("Alpha outside [0, 1]");
        }
        const bool recreated=resize(f);Parameters p;
        color->upload(f.color);depth->upload(f.depth);diffuse->upload(f.diffuse);specular->upload(f.specular);normal->upload(f.normal_roughness);motion->upload(f.motion);specMotion->upload(f.specular_motion);
        p->Set(NVSDK_NGX_Parameter_Reset,(f.reset||recreated||!history||lastScene!=f.scene_serial)?1:0);
        p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,f.jitter_x);p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,f.jitter_y);
        p->Set(NVSDK_NGX_Parameter_Color,&color->texture);p->Set(NVSDK_NGX_Parameter_Depth,&depth->texture);p->Set(NVSDK_NGX_Parameter_DiffuseAlbedo,&diffuse->texture);p->Set(NVSDK_NGX_Parameter_SpecularAlbedo,&specular->texture);
        p->Set(NVSDK_NGX_Parameter_GBuffer_Normals,&normal->texture);p->Set(NVSDK_NGX_Parameter_GBuffer_Roughness,&normal->texture);p->Set(NVSDK_NGX_Parameter_MotionVectors,&motion->texture);p->Set(NVSDK_NGX_Parameter_GBuffer_SpecularMvec,&specMotion->texture);
        p->Set(NVSDK_NGX_Parameter_Output,&output->surface);p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,f.width);p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,f.height);p->Set(NVSDK_NGX_Parameter_DLSS_Indicator_Invert_Y_Axis,1);
        const auto start=std::chrono::steady_clock::now();
        try{
            ngx(NVSDK_NGX_CUDA_EvaluateFeature(feature,p.p,nullptr));cuda(cuCtxSynchronize());output->download(scratch.data());
            for(float x:scratch)if(!std::isfinite(x))throw std::runtime_error("Nonfinite DLSS output");
            if(f.width==f.output_width&&f.height==f.output_height){
                for(size_t i=0;i<size_t(f.width)*f.height;++i)scratch[i*4+3]=f.color[i*4+3];
            }else{
                // Coverage is bounded even when the neural reconstruction
                // overshoots. This does not certify its coverage accuracy or
                // infer opaque geometry from a finite input sample.
                for(size_t i=0;i<size_t(f.output_width)*f.output_height;++i)scratch[i*4+3]=std::clamp(scratch[i*4+3],0.f,1.f);
            }
        }catch(...){history=false;throw;}
        stats.evaluate_readback_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    }
};
}
extern "C" {
uint32_t slc_dlss_abi(){return 1;}
int slc_dlss_set_preset(void *handle,uint32_t preset,char *message,uint32_t capacity){
    std::lock_guard<std::mutex> lock(mutex);error(message,capacity,"");
    try{
        if(manager.cleanupFailed)throw std::runtime_error("DLSS cleanup failed; restart Blender before selecting a preset");
        if(!handle || (preset!=0 && preset!=4 && preset!=5 && preset!=6))throw std::runtime_error("Invalid DLSS model preset");
        auto &instance=*static_cast<Instance*>(handle);
        if(instance.feature || instance.stats.frames)throw std::runtime_error("DLSS model must be selected before the first frame");
        instance.preset=preset;return 1;
    }catch(const std::exception &e){error(message,capacity,e.what());return 0;}
}
void *slc_dlss_create(uint32_t device,const char *runtime,const char *cache,uint64_t limit,char *message,uint32_t capacity){
    std::lock_guard<std::mutex> lock(mutex);error(message,capacity,"");
    try{if(!limit)throw std::runtime_error("A nonzero owned-memory budget is required");acquireManager(device,runtime,cache);try{return new Instance(limit);}catch(...){releaseManager();throw;}}
    catch(const std::exception &e){error(message,capacity,e.what());return nullptr;}
}
int slc_dlss_process(void *handle,const SlcDLSSFrame *frame,float *output,uint64_t outputCapacity,char *message,uint32_t capacity){
    std::lock_guard<std::mutex> lock(mutex);error(message,capacity,"");
    try{
        if(manager.cleanupFailed)throw std::runtime_error("DLSS cleanup failed; restart Blender before processing");
        if(!handle||!frame)throw std::runtime_error("Invalid DLSS handle/frame");
        const CUresult pushed=cuCtxPushCurrent(manager.context);
        cudaCleanup(pushed,"frame context push");cuda(pushed);
        auto &instance=*static_cast<Instance*>(handle);
        try{instance.process(*frame,output,outputCapacity);}
        catch(...){
            CUcontext previous=nullptr;
            cudaCleanup(cuCtxPopCurrent(&previous),"failed frame context pop");
            throw;
        }
        CUcontext previous=nullptr;const CUresult popped=cuCtxPopCurrent(&previous);
        cudaCleanup(popped,"frame context pop");
        if(manager.cleanupFailed){instance.history=false;throw std::runtime_error("DLSS cleanup failed; restart Blender before processing");}
        // Publish only after parameter retirement and context restoration have
        // succeeded. Failed cleanup must not expose output or advance history.
        std::memcpy(output,instance.scratch.data(),instance.scratch.size()*sizeof(float));
        instance.history=true;instance.lastId=frame->frame_id;instance.lastScene=frame->scene_serial;++instance.stats.frames;
        return 1;
    }
    catch(const std::exception &e){error(message,capacity,e.what());return 0;}
}
void slc_dlss_stats(void *handle,SlcDLSSStats *stats){std::lock_guard<std::mutex> lock(mutex);if(handle&&stats)*stats=static_cast<Instance*>(handle)->stats;}
void slc_dlss_destroy(void *handle){
    if(!handle)return;
    try{
        std::lock_guard<std::mutex> lock(mutex);
        const bool pushed=cudaCleanup(cuCtxPushCurrent(manager.context),"instance context push");
        auto *instance=static_cast<Instance*>(handle);
        if(!pushed)instance->abandon();
        delete instance;
        if(pushed){CUcontext previous=nullptr;cudaCleanup(cuCtxPopCurrent(&previous),"instance context pop");}
        releaseManager();
    }catch(...){
        // Do not allow a C++ mutex/runtime exception to cross ctypes' C ABI.
        // If locking itself failed, shared ownership cannot safely be mutated.
        std::fprintf(stderr,"DLSS cleanup: runtime lock failure; restart Blender\n");
    }
}
}
