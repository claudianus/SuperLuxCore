// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>
#if defined(_WIN32)
# define SLC_DLSS_API __declspec(dllexport)
#else
# define SLC_DLSS_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
// All inputs are tightly packed, bottom-up, linear HDR float planes. Motion is
// backward current-to-previous pixel displacement, without camera jitter.
// Color/diffuse/specular are RGBA; normal_roughness is camera XYZ + perceptual R;
// depth is positive linear camera depth; the two motion planes contain XY.
// A frame must contain fresh independent samples, never overlapping averages.
typedef struct SlcDLSSFrame {
    uint32_t width,height,output_width,output_height,reset,independent_samples;
    uint64_t frame_id,scene_serial;
    float jitter_x,jitter_y;
    const float *color,*depth,*diffuse,*specular,*normal_roughness,*motion,*specular_motion;
} SlcDLSSFrame;
typedef struct SlcDLSSStats {
    // Requested texture payload and host scratch only. CUDA allocation padding,
    // context overhead and opaque NGX/model allocations are additional.
    uint64_t texture_bytes,host_scratch_bytes,sdk_private_bytes,frames;
    uint32_t width,height,output_width,output_height;
    double evaluate_readback_seconds;
} SlcDLSSStats;
// Synchronous API; instance lifetime must be owned by the caller. Separate
// instances have independent history and share a serialized CUDA/NGX context.
// Failure leaves caller output untouched. Alpha is exact for native resolution.
SLC_DLSS_API uint32_t slc_dlss_abi(void);
SLC_DLSS_API void *slc_dlss_create(uint32_t device,const char *runtime_dir,const char *cache_dir,uint64_t owned_memory_limit,char *error,uint32_t capacity);
// Optional ABI-1 extension. Select an official RR model hint before the first
// process call: 0=SDK default, 4=D, 5=E, 6=F. Changing a live history requires a
// new instance. Older ABI-1 libraries continue to support the default model.
SLC_DLSS_API int slc_dlss_set_preset(void *,uint32_t preset,char *error,uint32_t capacity);
SLC_DLSS_API int slc_dlss_process(void *,const SlcDLSSFrame *,float *output,uint64_t output_float_capacity,char *error,uint32_t capacity);
SLC_DLSS_API void slc_dlss_stats(void *,SlcDLSSStats *);
// Consumes a non-null instance once; destroy(NULL) is a no-op. Driver cleanup
// failures do not escape the C ABI and disable further use until host restart.
SLC_DLSS_API void slc_dlss_destroy(void *);
#ifdef __cplusplus
}
#endif
