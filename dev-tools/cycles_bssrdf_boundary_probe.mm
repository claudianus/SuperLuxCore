// SPDX-License-Identifier: Apache-2.0
// Compile with clang++ -std=c++17 -Iinclude -I<Boost include> -framework Metal
// -framework Foundation. The Python driver creates the deterministic samples.
// This probes the exact shared source on CPU and an actual Metal device; it
// does not implement a complete nonlocal adjoint BSSRDF estimator.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <fstream>
#include <iterator>
#include <iostream>
#include <vector>
#include <cstring>
#include "slg/materials/cyclesbssrdf_boundary.h"
using namespace luxrays;
using namespace slg::cyclesbssrdfboundary;

static Vector LegacyGGX(const Vector &v, const float alpha, const float u0, const float u1) {
    if (alpha == 0.f) return Vector(0.f, 0.f, 1.f);
    const Vector vh = Normalize(Vector(alpha * v.x, alpha * v.y, v.z));
    const float l = vh.x * vh.x + vh.y * vh.y;
    const Vector t1 = l > 0.f ? Vector(-vh.y, vh.x, 0.f) / sqrtf(l) : Vector(1.f, 0.f, 0.f);
    const Vector t2 = Cross(vh, t1);
    const float r = sqrtf(u0), phi = 2.f * M_PI * u1;
    const float x = r * cosf(phi), mix = .5f * (1.f + vh.z);
    const float y = (1.f - mix) * sqrtf(fmaxf(0.f, 1.f - x * x)) + mix * r * sinf(phi);
    const Vector n = x * t1 + y * t2 + sqrtf(fmaxf(0.f, 1.f - x * x - y * y)) * vh;
    return Normalize(Vector(alpha * n.x, alpha * n.y, fmaxf(0.f, n.z)));
}

int main(int argc, char **argv) { @autoreleasepool {
    if (argc != 5) return 2;
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.empty() || bytes.size() % (8 * sizeof(float))) return 3;
    const size_t count = bytes.size() / (8 * sizeof(float));
    std::vector<float> inputs(count * 8), cpu(count * 12);
    memcpy(inputs.data(), bytes.data(), bytes.size());
    float legacyMaxError = 0.f;
    for (size_t i = 0; i < count; ++i) {
        const float *p = inputs.data() + 8 * i;
        const Vector v(p[0], p[1], p[2]);
        const auto s = p[7] == 0.f ? CyclesBSSRDF_SampleEntryBoundary(v, p[3], p[4], p[5], p[6]) :
            CyclesBSSRDF_SampleAdjointBoundary(v, p[3], p[4], p[5], p[6]);
        const auto &d = s.densities;
        const float result[] = {s.direction.x, s.direction.y, s.direction.z,
            d.forwardPdf, d.reversePdf, d.adjointWeight, d.forwardMass,
            d.reverseMass, float(d.delta), float(d.valid), float(s.valid), 0.f};
        memcpy(cpu.data() + i * 12, result, sizeof(result));
        if (p[7] == 0.f) {
            const Vector h = LegacyGGX(v, p[4], p[5], p[6]);
            const float eta = 1.f / p[3], ci = Dot(h, v);
            const float ct = sqrtf(fmaxf(0.f, 1.f - eta * eta * (1.f - ci * ci)));
            const Vector error = s.direction - (-eta * v + (eta * ci - ct) * h);
            legacyMaxError = std::max(legacyMaxError, error.Length());
        }
    }
    std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char *>(cpu.data()), cpu.size() * sizeof(float));
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) return 4;
    std::ifstream sourceFile(argv[4]);
    const std::string shared((std::istreambuf_iterator<char>(sourceFile)), {});
    const std::string source =
        "#include <metal_stdlib>\nusing namespace metal;\n#define OPENCL_FORCE_INLINE inline\n"
        "#define MAKE_FLOAT3(x,y,z) float3(x,y,z)\n#define M_PI_F 3.14159265358979323846f\n" + shared +
        "\nkernel void probe(device const float *in [[buffer(0)]], device float *out [[buffer(1)]], uint id [[thread_position_in_grid]]) {\n"
        "device const float *p=in+8*id; float3 v=float3(p[0],p[1],p[2]);\n"
        "CyclesBSSRDFBoundarySample s=p[7]==0.f?CyclesBSSRDF_SampleEntryBoundary(v,p[3],p[4],p[5],p[6]):CyclesBSSRDF_SampleAdjointBoundary(v,p[3],p[4],p[5],p[6]);\n"
        "device float *o=out+12*id; o[0]=s.direction.x; o[1]=s.direction.y; o[2]=s.direction.z;\n"
        "o[3]=s.densities.forwardPdf; o[4]=s.densities.reversePdf; o[5]=s.densities.adjointWeight;\n"
        "o[6]=s.densities.forwardMass; o[7]=s.densities.reverseMass; o[8]=s.densities.delta;\n"
        "o[9]=s.densities.valid; o[10]=s.valid; o[11]=0.f; }\n";
    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    options.fastMathEnabled = NO;
    id<MTLLibrary> library = [device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()] options:options error:&error];
    if (!library) { std::cerr << error.description.UTF8String; return 5; }
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"probe"] error:&error];
    if (!pipeline) { std::cerr << error.description.UTF8String; return 6; }
    id<MTLBuffer> inBuffer = [device newBufferWithBytes:inputs.data() length:bytes.size() options:MTLResourceStorageModeShared];
    id<MTLBuffer> outBuffer = [device newBufferWithLength:cpu.size() * sizeof(float) options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:inBuffer offset:0 atIndex:0];
    [encoder setBuffer:outBuffer offset:0 atIndex:1];
    [encoder dispatchThreads:MTLSizeMake(count, 1, 1) threadsPerThreadgroup:MTLSizeMake(pipeline.threadExecutionWidth, 1, 1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) { std::cerr << command.error.description.UTF8String; return 7; }
    std::ofstream(argv[3], std::ios::binary).write(reinterpret_cast<const char *>(outBuffer.contents), outBuffer.length);
    std::cout << "{\"device\":\"" << device.name.UTF8String << "\",\"samples\":" << count
        << ",\"legacy_entry_max_direction_error\":" << legacyMaxError << ",\"metal_completed\":true}\n";
} }
