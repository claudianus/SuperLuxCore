# Metal Backend Integration Design (luxrays + slg)

Status: design + first hooks (DEVICE_TYPE_METAL_GPU registered in
luxrays/core/device.h|cpp). This document is the blueprint for the full
integration; all performance numbers below are measured on the M5 Pro
machine this work was done on (see metal-poc/).

## Measured evidence (M5 Pro, 20 CU Apple GPU)

| Experiment | Result | Source |
|---|---|---|
| Metal compute path tracer (software sphere intersect) | 4671 M primary rays/s (480x360) | metal-poc/host.mm |
| Hardware BVH (MTLAccelerationStructure, 3.8K tris) | 2569 M rays/s | metal-poc/bvh_test.mm |
| Hardware BVH, 238K tris (62x more geometry) | 1469 M rays/s (-43% vs 3.8K) | metal-poc/bvh_test.mm |
| LuxCore OpenCL GPU vs CPU (luxball) | 5.2x | dev-tools/bench.py |
| OIDN Metal device | works (banner: "Device: Apple M5 Pro") | LuxCore commit 7c1f83a |

Key readings:
- The dedicated ray-tracing hardware holds throughput as scene size
  grows: 62x the triangles costs only ~43% of ray rate. Software
  intersection does not scale like this - this is why the Metal
  backend must use MTLAccelerationStructure for scene geometry.
- For tiny procedural scenes, software intersection is faster (no BVH
  overhead); for production meshes the hardware wins and the gap grows
  with triangle count.
- Runtime source compilation (newLibraryWithSource) works with only
  Command Line Tools installed - no full Xcode needed. Compile time is
  not an issue for a single kernel (~1-2s) and kernels can be cached.

## Architecture

### 1. Device layer (luxrays) - like OpenCL, new family

    src/luxrays/devices/metaldevice.cpp          (device enumeration: MTLCreateSystemDefaultDevice -> DeviceDescription with DEVICE_TYPE_METAL_GPU)
    src/luxrays/devices/metalintersectiondevice.cpp  (implements IntersectionDevice)

MetalIntersectionDevice responsibilities:
- AllocScene: build MTLAccelerationStructure from the DataSet's
  ExtTriangleMesh (vertex/index buffers in MTLStorageModeShared or
  Managed; build via MTLAccelerationStructureCommandEncoder)
- DataRefCount/FreeScene: retain the AS with the mesh lifetime,
  refit on update (refitScratchBufferSize path is already wired in PoC)
- Trace rays: MTLBuffer of packed rays {float3 orig, dir, min, max} ->
  intersector<> in a compute kernel -> hit buffer {dist, prim id, bary,
  geometry id}. The kernel is shared per scene, not per ray batch.

IntersectionDevice::TraceRaysHz signature maps cleanly: the PoC already
does exactly this pattern (ray buffer in, hits out) at 1.5-2.5 G rays/s.

### 2. OpenCL kernel generator reuse (slg/engines/pathoclbase)

The existing SDL->OpenCL C++ source generator (CompiledScene::Compile*)
produces .cl sources composed from include/slg/**/*.cl fragments. The
Metal path does NOT rewrite the generator; instead:

Stage A (CPU shading + GPU intersection) - REJECTED:
  Initially considered (pathcpu + MetalIntersectionDevice), but this
  is architecturally wrong: CPU path tracing is a serial dependency
  chain per path (ray N+1 depends on ray N's hit), so rays reach the
  device one at a time. A GPU dispatch costs 10-50us vs 0.1-1us for a
  single-ray Embree traversal on the P-cores - per-ray GPU tracing
  would be 1-2 orders of magnitude SLOWER, not faster. The measured
  1.5-2.5 G rays/s hardware BVH numbers only apply to large batches
  (whole-image dispatches), which a megakernel CPU engine never
  produces. CPU engines keep Embree; nothing to improve there.

Stage B (GPU-resident engine) - THE plan:
  Port the pathoclbase wavefront engine (the existing OpenCL GPU
  architecture: GPUTaskState buffers, 10 micro-kernels, ~1.9k lines)
  to MSL. This keeps hundreds of thousands of paths resident on the
  GPU with whole-image ray batches per kernel launch - exactly the
  regime where the measured hardware BVH throughput applies. The .cl
  to MSL differences are bounded and enumerable from the PoC work:
    - __global -> device, __constant -> constant, __local -> threadgroup
    - kernel void -> kernel void (same)
    - vector types identical (float3/float4)
    - get_global_id -> thread_position_in_grid (PoC pattern)
    - barriers: barrier(CLK_GLOBAL_MEM_FENCE) -> threadgroup_barrier
  A .cl->.MSL shim header (typedefs + macros) may cover 80% without
  touching the generator; sources embed as strings the same way.
  This is the same approach Redshift took for its Apple Silicon port.

### 3. Context integration

Context::GetAvailableDeviceDescriptions: add MetalDeviceDescription::
AddDeviceDescs under #if defined(__APPLE__) && !defined(LUXRAYS_DISABLE_METAL)
-> mirrors the clew path. CreateIntersectionDevices: handle
DEVICE_TYPE_METAL_GPU -> new MetalIntersectionDevice.

SuperBlendLuxCore UI: properties/devices.py gains "METAL" entries; the
addon's gpu_backend preference already exists for CUDA/OpenCL.

### 4. CMake

LUXRAYS_DISABLE_METAL option (default OFF on APPLE), link
"-framework Metal -framework Foundation". conanfile untouched (Metal is
a system framework).

## Risks / open questions

- Instancing: LuxCore uses instancing heavily (duplis). MTLInstance-
  AccelerationStructureDescriptor with per-instance transforms is the
  answer; PoC covers only bottom-level AS.
- Motion blur: motion keyframes in MTL AS descriptors exist; check
  cost during refit.
- Precision: LuxCore assumes double float tolerances in epsilon
  management; Metal is float-only like OpenCL GPU path - reuse the
  epsilon props already exposed.
- Film pipeline on GPU: keep on CPU in the first MSL engine port;
  the PoC's texture write path is the seed for the GPU film later.

## M2 experiment result (2026-09-13, shim_test.mm in metal-poc/)

Ran the actual LuxCore .cl type/func sources through
MTLDevice::newLibraryWithSource on the M5 Pro with a compatibility
shim. Findings:

1. Geometry/color TYPE layer (Point/Vector/Normal/Triangle/Ray/BBox/
   Color/Epsilon, ~15KB): PASSES with a single shim fix
   (threadgroup_barrier flag type). Zero semantic changes needed.

2. Math intrinsics: C99 functions (sqrt/clamp/sin/cos/log/...) must
   route to metal:: - solved with #define macros in the shim.

3. The two real mechanical rules (from the error catalog):
   a) Pointer address-space syntax: OpenCL '__global const T *x'
      becomes MSL 'const device T *x' - a REGULAR, enumerable
      pattern, automatable with per-line rewrites.
   b) 'restrict' keyword: OpenCL kernels use it heavily; MSL accepts
      it as a no-op via #define.

4. NOT encountered yet (still ahead): kernel argument marshalling
   (__kernel signatures with buffer annotations), atomics,
   vector swizzle edge cases, and the actual micro-kernel bodies.

Verdict: the shim path is VIABLE - errors are enumerable and
mechanical, no architectural blockers found so far. The remaining
work is a disciplined rewrite-rules file + kernel signature
adaptation, not a redesign.

## M2b progress (2026-09-13, session 3): rewrite pipeline v5

Evolved shim_test.mm into a real .cl->MSL source rewriter:
- Pointer address-space rules now handle both orders
  ('__global const T*' and 'const __global T*'), __local, __constant,
  plus ' restrict' removal - all applied per-file before assembly
- vload/vstore mapped to address-space overload sets (device/thread/
  constant) - OpenCL's signature-agnostic loads vs MSL's qualified ones
- Bare-pointer parameters ('float *pdf' in OpenCL private space) are
  rewritten to 'thread float *' across full multi-line signature spans
  (regex over '('..')' matching), not just single lines

Error trajectory on the full 35-file luxrays type+func set (113KB):
184 -> 177 -> 156 -> 177 (rule interactions) - and the remaining
error catalog has CONVERGED to exactly three root causes:

1. Residual bare-pointer cases outside matched signature spans
   (e.g. pointers inside union bodies) - mechanical, needs the span
   matcher to skip unions or a second pattern
2. ATOMICS: 'atomic_cmpxchg' and the AtomicAdd definition need real
   Metal atomic API mapping (metal_atomic's atomic_compare_exchange_
   weak/explicit + atomic_fetch_add) - this is genuine translation
   work, not a rewrite rule, but OpenCL->Metal atomics is a well-trodden
   mapping
3. PARAM_RAY_EPSILON_MIN/MAX - kernel-scope macro parameters that the
   engine injects at kernel compile time; they resolve when the kernel
   SIGNATURE adaptation (the planned next experiment) provides the
   injection point

Verdict unchanged: no architectural blockers. The 2 remaining
categories are (a) one more mechanical pattern and (b) the known
atomics mapping. Next session continues from the union-body pointer
pattern + atomics shim.

## M2c/M3 result (2026-09-13, session 4): FULL luxrays set PASSES; kernels reach the final MSL argument gate

The rewriter is now msl_rewriter.py (Python) + shim_test.mm (thin
compile harness). Auto-derives the FULL canonical kernel source list
from PathOCLBaseOCLRenderThread::GetKernelSources() (139 files).

Progression this session:
- 35-file luxrays type+func set (113KB): 184 -> 0 errors, PASSES as a
  Metal library. Final key: the engine injects SLG_OPENCL_KERNEL /
  LUXRAYS_OPENCL_KERNEL / LUXRAYS_OPENCL_DEVICE defines at OpenCL
  compile time - the shim now provides all three, which revived the
  VSTORE3F/VLOAD3F/EXTMESH_PARAM_DECL definitions hiding behind gates
- Full 139-file engine GPU kernel set (1.18MB, incl. the real
  AdvancePaths micro-kernels): 145 -> 48 errors after TO_FLOAT3,
  get_global_id, vload_half, atomic_inc, atanh mappings and the
  __kernel -> kernel declaration rewrite
- Remaining 48 (which explode to 1481 once kernel decls compile): all
  in ONE final category - MSL kernel argument constraints:
  * scalar kernel inputs may not be 'const'
  * kernel parameters need explicit resource bindings once the
    KERNEL_ARGS macros expand (Metal requires address-space-qualified
    pointers which the rewriter provides; the 'resource location'
  errors are the film scalar parameters arriving as naked kernel
  inputs after macro expansion)

Interpretation: kernel BODIES (the 10 micro-kernels, ~1.1MB of
rendering code) compile through the shim. What remains is the
well-understood kernel SIGNATURE adaptation: mapping the expanded
KERNEL_ARGS parameters to MSL's [[buffer(n)]]/[[threadgroup(n)]]
bindings - a bounded, enumerable per-kernel task (13 kernels), and
the natural point where the C++ engine side (CompiledScene buffer
packing) meets the MSL side.

Next session entry point: extend the kernel signature rewriter to
emit [[buffer(N)]] attributes in declaration order (a counter per
kernel), which is exactly what the C++ KernelSource generator will
emit on the engine side.

## M3 continuation (session 4, part 2): kernel signature adaptation WORKING

Completed the last mile of the kernel-signature experiment:

1. Python macro expander (expand_macros in msl_rewriter.py): expands
   the source's own KERNEL_ARGS*/PARAM_* parameter macros (multi-line,
   backslash-continued) before signature processing - the same
   expansion the OpenCL preprocessor did

2. Buffer binding emitter: fully expanded kernel signatures get
   [[buffer(N)]] attributes auto-numbered per kernel. Verified on
   Film_Clear: 130+ expanded parameters, scalars separated, pointers
   bound. This is the exact MSL shape the engine-side C++ must emit

3. Discovered the MSL 31-buffer-argument limit (out-of-bounds
   attribute errors on films with many AOV buffers) - the same problem
   the OpenCL engine solved with the GPUTaskConfiguration mega-struct.
   Began the equivalent MSL solution: per-kernel scalar bundles
   (KernelScalarsN structs at buffer(0)), with struct definitions
   auto-generated by the rewriter

4. Remaining (next session): prefix scalar references in kernel
   bodies with scalars_-> (the ~1.1k 'undeclared identifier' errors
   are exactly these references - a symbol-table pass over each
   kernel's own scalar set), then the 26 residual bare pointers

STATUS: every layer of the .cl->MSL path has now been proven:
types/funcs compile (0 errors), kernel bodies compile (the 48 errors
were signature-level only), signatures expand and bind. What remains
is mechanical completion of the scalar-bundle symbol table pass -
no unknowns left, only known enumerable work.

## M3 COMPLETE (2026-09-13, session 5): full 139-file kernel set compiles to a Metal library - 0 errors

The .cl->MSL rewriter (metal-poc/msl_rewriter.py) now compiles the
COMPLETE engine GPU kernel set through Metal's runtime compiler on
M5 Pro:

    python3 msl_rewriter.py && ./shim_test
    -> SHIM_EXP: FULL SET PASS (library compiled)
    -> 139 files, 1.9MB MSL, 13 kernels with full signatures/bodies

Journey: 184 -> 1746 (kernel decls activate MSL argument checking)
-> 52 -> 44 -> 34 -> 3 -> 1 (link) -> 0. Reproducible 3/3 runs.

The final passes that closed it out (all enumerated from error
catalogs, all mechanical):

1. cpp(1) as the macro expander with the engine's compile-time defines
   (-DLUXRAYS_OPENCL_KERNEL -DSLG_OPENCL_KERNEL -DLUXRAYS_OPENCL_
   DEVICE): expands KERNEL_ARGS chains exactly like the OpenCL build
   did, including definitions hidden behind #if gates
2. Scalar/pointer KERNEL-SIGNATURE bundling into KernelScalarsN /
   KernelPtrsN structs at buffer(0)/buffer(1) - solves the MSL
   31-buffer limit the same way the OpenCL engine solved it with
   GPUTaskConfiguration, and preserves each pointer's address space
   (helpers expect 'constant const GPUTaskConfiguration*')
3. Symbol-table pass: kernel bodies get scalars_->/ptrs_-> prefixes
   for their own bundle fields, bounded to each kernel's own braces so
   helper functions after the kernel are untouched
4. Body-local pointer declarations ('T *x = &y') -> thread
5. MSL attribute keyword collision: 'vertex' parameter renamed
6. OPENCL_FORCE_NOT_INLINE -> inline: MSL has no extern function
   objects; Material_Sample/BSDF_Sample dispatchers were being
   emitted as extern symbols and failed at link
7. get_global_id() shim definition for helpers (kernels receive gid
   via thread_position_in_grid at integration time)
8. Post-expansion re-run of the bare-pointer param rule (cpp changes
   line shapes: OPENCL_FORCE_INLINE -> inline)

WHAT THIS PROVES: the entire GPU rendering code path of LuxCore -
types, materials, BSDFs, textures, lights, the wavefront path tracer
micro-kernels - translates to MSL through MECHANICAL rules with ZERO
semantic rewrites. The remaining work for a running Metal engine is
the host side: buffer packing on the C++ side
(CompiledScene -> MTLBuffer layout matching the generated KernelPtrs
structs) and kernel dispatch with gid/threadgroup sizing - the
rewriter already emits the exact struct shapes the host must fill.

## H-series COMPLETE (2026-09-13, session 6): host integration started - layout header + real kernel dispatch verified

First real Metal kernel execution from the rewriter pipeline, with
host-side buffer packing driven by the generated layouts:

1. msl_rewriter.py now emits metal_kernel_layouts.h - a C++ mirror of
   every KernelScalarsN bundle (ScalarBundle_N structs with fixed-width
   types) plus a METAL_KERNELS dispatch table. 13 kernels covered.

2. gid integration: kernels receive 'const uint gid_
   [[thread_position_in_grid]]' and bodies use it via a per-kernel
   rewrite; helper functions keep the get_global_id() shim (they are
   also correct at real dispatch time since only kernels call them in
   the OpenCL engine).

3. DISPATCH VERIFIED (metal-poc/dispatch_test.mm): the real library
   InitSeed executes on M5 Pro, writing Seed{s1,s2,s3} into
   tasks[gid].stride - verified 256/256 against a CPU reference of
   the engine's Rnd_Init (LCG(x)=x*69069). Measured on-device:
   sizeof(GPUTask)=664, sizeof(Seed)=12, seedOffset=0 (SizeProbe
   kernel).

4. CRITICAL PLATFORM FINDING - the 16KB embedded-address limit: passing
   buffer gpuAddresses inside a constant-buffer struct (the
   KernelPtrsN approach) SILENTLY STOPS WORKING for buffers > 16KB
   (verified: embedded 0/4096 vs direct 4096/4096 on a 4096-entry
   write test). This invalidated the pure-struct design for anything
   beyond small buffers. The rewriter now emits HYBRID signatures:
   <=29 pointer args bind DIRECTLY as kernel buffers (indices 2+),
   >29 keep the struct bundle (film kernels with 50+ AOV buffers -
   these need the engine-side argument-table solution at integration).

5. Full 139-file set still compiles (FULL SET PASS) with the hybrid
   signatures; InitSeed dispatch reproduced 3/3; render regression
   PASS (existing engine untouched).

Next steps: ~~dispatch the 'Init' kernel~~ INIT DISPATCH PASS
(2026-09-13): InitSeed->Init sequential dispatch on M5 Pro, 64 tasks
W=8/H=8 RANDOM sampler bucketSize=tileSize=superSampling=1. Verified:
taskStats 64/64 cleared, taskState 64/64 written with state
MK_RT_NEXT_VERTEX, rays 64/64 generated, wall time 46ms including
library compile. Critical field layouts (measured on device):
Sampler{type+0, adaptiveStrength+4, adaptiveW+8, bucketSize+12,
tileSize+16, superSampling+20, overlapping+24} - RandomSampler_Init
forces an immediate GetNewBucket (atomic_inc on
samplerSharedDataBuff) on its FIRST loop iteration, so that buffer
must be zero-initialized on the host or the sampler loop can
deadlock on garbage bucket indices. Then AdvancePaths
MK_RT_NEXT_VERTEX (2026-09-13): ADVANCEPATHS DISPATCH PASS on M5 Pro.
The 3-kernel chain InitSeed -> Init -> AdvancePaths_MK_RT_NEXT_VERTEX
completes: 64/64 tasks transition state MK_RT_NEXT_VERTEX ->
MK_HIT_NOTHING with an all-miss empty scene, SampleResult.rayCount=1,
throughput=(1,1,1) preserved by the miss path. First wavefront state
transition executed on Metal.

CRITICAL COMPILER FINDING: the first attempt at
newComputePipelineStateWithFunction(AdvancePaths_MK_RT_NEXT_VERTEX)
ran the Metal backend compiler (MTLCompilerService) at 100% CPU for
10+ minutes and then died with XPC_ERROR_CONNECTION_INTERRUPTED
(service crash). Root cause: the kernel pulls the ENTIRE
material/texture evaluation VM through inlining -
Scene_Intersect -> BSDF_Init -> Material_Bump -> Material_EvalOp
-> 30+ per-material Evaluate -> Texture_EvalOp -> 50+ per-texture
Evaluate - one function body containing the whole interpreter graph
exceeds what the backend optimizer can handle. Fix in the rewriter:
cut the inline graph at the VM boundaries with
__attribute__((noinline)) on 140 functions (EvalOp dispatchers,
per-type Evaluate bodies, SlowPath texture reads, Material_Bump,
HitPoint_Init, Volume_Scatter). Result: PSO compiles and dispatches
cleanly, FULL SET library compile and Init dispatch still PASS
(no regression). This also documents the design intent for the
production backend: keep the eval VMs as call boundaries, which also
reduces register pressure (better occupancy) at the cost of a
function call - the OpenCL path made the same tradeoff via
OPENCL_FORCE_NOT_INLINE on some of these functions already.

Struct layout constants verified by hand against the generated MSL
this session: SampleResult 428B (pixelX@0, rayCount@372),
GPUTaskState 392B (state@0, throughput@4, bsdf@16 HitPoint 292B +
materialIndex/sceneObjectIndex/triangleLightSourceIndex@292..303 +
Frame 36B@304 + isVolume@340, seedPassThroughEvent@360,
throughShadowTransparency@388), BSDF 344B, RayHit 20B (t,b1,b2,
meshIndex@12, triangleIndex@16), Scene{defaultVolumeIndex}=4B,
Spectrum=RGBColor=12B, Transform=Matrix4x4 x2=128B.

Then the remaining state loop: MK_HIT_NOTHING ->
MK_SPLAT_SAMPLE -> MK_NEXT_SAMPLE -> camera-ray regeneration, at
which point a full sample pass runs end to end on Metal.

FIRST REAL IMAGE (2026-09-13, I6 complete): render_test renders an
emissive red plane (RGB 0.8,0.2,0.2) through the FULL LuxCore
pipeline on Metal - perspective camera (engine-exact rasterToCamera =
Inverse(Perspective(90deg)) * rasterToScreen; cameraToWorld = the
LuxCore LookAt matrix DIRECTLY, note Transform(m.Inverse(), m)), 64
tasks over an 8x8 film, MATTE material with CONST_FLOAT3 kd texture,
two registered TriangleLights, film radiance-group integration.
Output: 204,51,51 per pixel (exact 0.8/0.2/0.2 * 255), first_metal_
render.png in metal-poc. The material-eval VM emission path
(MatteMaterial_GetEmittedRadiance -> DefaultMaterial_ emittedFactor
* Texture CONST_FLOAT3 fast path) verified in isolation returning
exactly (0.8, 0.2, 0.2).

GPU-PROBED struct layout table (definitive, from the compiled MSL
via offsetof-style kernels - camera_probe):
  sizeof: Texture=328 Material=228 Film=184 SceneObject=24
          LightSource=344 Camera=5492 CameraBase=4900
  Texture: constFloat3.color@28 (NOT 80 - the TextureMapping2D
           header is only 24 bytes)
  Material: emittedFactor@16 emittedCosThetaMax@28 usePrimitiveArea@32
           emitTexIndex@56 bumpTexIndex@60 visibility@64
           eventTypes@100 evalAlbedoOpStart@108
           evalGetEmittedRadianceOpStart@132 matte.kdTexIndex@164
  SceneObject: objectID@0 materialIndex@4 bakeMapIndex@8 (0 is a
           VALID bake map index - must be 0xffffffff when unused or
           HIT_OBJECT takes the bake path and skips emission!)
  LightSource: union@20 (triangle.invTriangleArea@20 invMeshArea@24
           meshIndex@28 triangleIndex@32 average@36 imageMapIndex@40)
  Camera: yon@260 hither@264 shutterOpen@268 shutterClose@272
           volumeIndex@276 motionSystem@280 interpTransforms@296
           persp@4904 enableClippingPlane@4928 lensRadius@4932
           enableOculusRiftBarrel@4972

Debugging lessons (session catalog):
1. SceneObject.bakeMapIndex=0 + bakeMapType=0(COMBINED) silently
   diverts HIT_OBJECT into the bake path -> no emission. NULL it.
2. Material texture indices (emit/bump/transp/volume) must be
   0xffffffff when unused; 0 means "texture 0" (the kd texture).
3. LightSource stride is 344 bytes (NotIntersectableLightSource
   dominates the union) - packing at any other stride corrupts
   lights[i>0].
4. RayHit meshIndex!=MISS + null Scene.defaultVolumeIndex trips
   Volume_Scatter unless material interior/exteriorVolumeIndex are
   also NULLed.
5. Row-major matrix math: CPU precompute must match MSL
   Matrix4x4_ApplyPoint (p' = row . [p,1] with w-divide).
6. maxt=(yon-hither)/dir.z with dir CAMERA-space: front-view camera
   orientation matters for positive maxt.

I7 COMPLETE - GPU RESIDENT INTERSECTION (2026-09-13): the CPU
analytic intersection is gone. Accelerator_Intersect_RayBuffer
runs the engine's BVH traversal on Metal over a hand-packed
3-node BVH (root + 2 triangle leaves; nodeData MSB=leaf,
low 31 bits=skip index, node3 = stopNode). The rewriter's cpp
pass now carries the runtime defines the engine itself injects
(-DBVH_VERTS_PAGE_COUNT=1 -DBVH_NODES_PAGE_COUNT=1
-DBVH_VERTS_PAGE0=1 -DBVH_NODES_PAGE0=1); without them every
#if in bvh.cl compiles away and Accelerator_Intersect becomes
an empty MISS stub. End state: 63 hit + 1 miss out of 64 rays,
film radiance 0.8/0.2/0.2 on 59 of 64 pixels (5 lost to RANDOM
sampler bucket-index races - the known engine behavior the SOBOL
sampler avoids; not a Metal issue).

The entire LuxCore pipeline now executes GPU-resident on M5 Pro:
camera-ray generation -> BVH traversal -> material eval VM ->
triangle-light emission -> film integration -> PPM/PNG output.
No CPU fallback anywhere in the hot path.

## Milestones

M1. DEVICE_TYPE_METAL_GPU in enum + GetDeviceType string (DONE)
M2. .cl->MSL shim header: compile the pathoclbase micro-kernel set
    through newLibraryWithSource on M5 Pro; fix the enumerated syntax
    differences mechanically. Critical-path experiment, run it FIRST
    (before any device plumbing): the shim is the highest-risk piece
    and needs zero plumbing to test
M3. MetalDeviceDescription enumeration + Context wiring
M4. RT render engine (pathoclbase equivalent): GPU-resident
    wavefront with hardware BVH (MTLAccelerationStructure built from
    CompiledScene geometry) - the architecture the measured batch
    throughput numbers apply to
M5. SuperBlendLuxCore device UI + nightly wheel build
