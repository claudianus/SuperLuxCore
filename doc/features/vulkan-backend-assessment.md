# Vulkan backend — cross-vendor GPU path + HWRT: feasibility assessment

Status: **M3 working on Apple Silicon (experimental)** — all 21 PATHOCL
kernels compile via clspv→SPIR-V (`spirv-val` clean) and a 1280×720
PATHOCL render passes regression (`vulkan-regression.sh --full`), with
BLAS/TLAS + `rayQuery` hardware intersection and scene-edit AS rebuild
(M2). Remaining: cold-compile time (tens of minutes, mitigated by two
cache layers), MoltenVK codegen nondeterminism (retry loop), teardown
872 B leak, and native Vulkan driver validation (M4). Earlier:
**M0 spike PASSED** (2026-09-23). `VK_KHR_ray_query`
verified working on Apple M5 Pro via MoltenVK PR #2771 experimental RT
build:
588 Mrays/s closest-hit, 508 Mrays/s occlusion, 9216/9216 rays matching
CPU brute force exactly (t, primID, instance index, barycentrics), 2-level
instance AS with transforms verified. Bench + reproduction:
`dev-tools/vkrt/` (workspace level, not in this repo).

## Question

Can Vulkan give LuxCore a single GPU backend covering NVIDIA + AMD + Intel
(+ Apple via MoltenVK), with hardware ray tracing on all of them?

## Verdict (short)

- **Direction is right**: Vulkan is the *only* single API covering all PC
  GPU vendors with HWRT (`VK_KHR_acceleration_structure` +
  `VK_KHR_ray_query`).
- **"Full replacement" is not realistic today**: MoltenVK ray tracing is
  still a WIP PR (KhronosGroup/MoltenVK#2771, opt-in via
  `MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING=1`; not in v1.4.2). Apple
  needs the Metal backend regardless. Realistic end state:
  **Vulkan (Windows/Linux) + Metal (macOS)** — 2 maintained backends
  instead of today's 4.
- **Feasibility is unusually good for this codebase**: GPU scene data is
  index/offset-based (no device pointers baked into structs), kernels are
  fp32-only, and the positional kernel-arg model maps cleanly onto Vulkan
  descriptor bindings.
- **Recommended path**: Vulkan compute backend with `clspv` translating
  the existing ~49k-LOC `.cl` corpus (zero kernel rewrite) + a
  hand-written GLSL `GL_EXT_ray_query` kernel — the exact analog of the
  `MetalRTKernel` architecture already delivered.

## Current backend landscape (measured in-tree)

| Fact | Value |
|---|---|
| OpenCL C kernel corpus | ~49,028 LOC, 170 `.cl` files (`include/luxrays/`, `include/slg/`) |
| Backends | OpenCL (all vendors, SW traversal), CUDA+OptiX (NVIDIA), Metal+HWRT (Apple), native CPU |
| HWRT coverage today | NVIDIA (OptiX) + Apple (Metal). AMD/Intel = software BVH only |
| Kernel args | positional `SetKernelArg` (126 sites in pathoclbase), ~10–40 args/kernel → deterministic descriptor mapping |
| Device pointers in compiled scene | none found — index/offset addressing (clspv/logical-addressing friendly) |
| FP precision | fp32 only ("double" hits are comments) |
| printf | used for kernel debug → not supported by clspv; workaround exists (debug output buffer, e.g. `taskStatsBuff`) |

## Vendor / HWRT coverage matrix

| Vendor | Vulkan RT support | HW accel | Measured notes |
|---|---|---|---|
| NVIDIA RTX | `ray_query` + RT pipeline | RT cores | VK RT ≈ DXR; **faster than OptiX** in ChameleonRT path-tracer (1.7 ms vs 8.0 ms/frame, Sponza — OptiX megakernel codegen issue); Tellusim: RQ-VK ≈ RT-VK ≈ D3D12 |
| AMD RDNA2+ | yes (RADV + AMDVLK) | ray accelerators | HW ~3.5× faster than compute-shader traversal; BLAS ~4× larger than NVIDIA, first build slow |
| Intel Arc/Xe | yes (ANV + proprietary) | RT units | functional; also enables Windows/Linux Intel coverage we lack entirely |
| Apple | MoltenVK PR #2771 (WIP, experimental opt-in) — **verified working on M5 Pro** in our spike | via Metal RT | Ray query + AS + instancing functional, 588 Mrays/s; PR not merged — production path stays native Metal |

## Design sketch

Two-layer design mirroring the Metal backend:

1. **`VulkanDevice` / `VulkanIntersectionDevice`** (~2–3k LOC): instance/
   device/queue, VMA (or manual) buffer alloc implementing
   `HardwareDeviceBuffer`, `shaderc` for runtime GLSL→SPIR-V,
   `VkPipelineCache` mirroring the existing kernel/PSO cache pattern.
   Positional `SetKernelArg` → descriptor binding map (clspv emits a
   deterministic `--descriptormap`).
2. **Kernel translation**: `clspv` (OpenCL C → Vulkan SPIR-V) fills the
   same role `cl2msl` does for Metal. Handles `__global` → StorageBuffer,
   `__local` → Workgroup, requires `VK_KHR_variable_pointers` (core in
   Vulkan 1.1). Fallback if clspv rejects our subset: hand-rolled
   `cl2glsl` rewriter + `GL_EXT_buffer_reference` — same pattern as the
   existing translator, more work.
3. **HWRT**: `VK_KHR_acceleration_structure` + `VK_KHR_ray_query` in a
   dedicated compute kernel consuming `rayBuff` → `rayHitBuff` — same
   contract as `MetalRTKernel` (primitive AS per MBVH leaf, instance AS,
   user-ID → meshIndex mapping). `VK_KHR_ray_tracing_pipeline`/SBT is
   *not* needed: `Scene_Intersect` is called inline inside our mega/micro
   kernels, so ray query is the correct primitive (same as Metal).
   Any-hit semantics for shadow rays via ray-query proceed/terminate.

## Alternatives considered

| Option | Coverage | Verdict |
|---|---|---|
| **Vulkan + clspv** | NV+AMD+Intel now, Apple later via MoltenVK | Recommended |
| HIPRT (AMD GPUOpen) | AMD+NVIDIA only — no Intel, no Apple | Dominated by Vulkan. (Unique trick: BVH import — irrelevant, our MBVH layout is proprietary) |
| Slang rewrite | SPIR-V/DXIL/Metal/CUDA/CPU from one language | Correct *long-term* answer (Khronos-backed, used by Adobe Substance renderer); rewriting 49k LOC is a multi-year effort. Non-blocking: new kernels (e.g. the ray-query kernel) can be written in GLSL/Slang today |
| Status quo + per-vendor RT libs | OptiX(NV)+Metal(Apple)+HIPRT(AMD)=3 RT paths, Intel never | Worst maintenance curve |
| DXR | Windows only | Dead end |

## Risks

1. **MoltenVK RT immaturity** — Apple stays on Metal for the foreseeable
   future. Vulkan does *not* reduce backend count on macOS today.
2. **clspv on our corpus** — untested at 49k LOC of this complexity.
   Known limits: no printf, opaque pointers, variable-pointers ext
   required (fine), descriptor arg-order mapping must be verified.
   This is the go/no-go spike.
3. **Per-scene defines** — we specialize kernels per scene config at
   runtime; runtime clspv/glslang compile + cache mirrors the existing
   oclcache/PSO-cache pattern.
4. **Driver matrix** — NV proprietary, AMD RADV+AMDVLK, Intel ANV+Windows
   proprietary: SPIR-V conformance is good but each driver has quirks;
   CI/parity cost is real for a hobby-scale team.
5. **Descriptor limits** — kernels with 30+ storage buffers exceed the
   spec minimum (27/stage) but are fine on all desktop RT-capable GPUs;
   verify per-device.
6. **SPIR-V codegen quality** on our megakernel vs NVRTC/MSL is unknown —
   ChameleonRT data suggests VK ≥ OptiX, but our kernels are far fatter.
   Must be gated by benchmark, not assumed.

## Phased plan

| Phase | Content | Gate |
|---|---|---|
| **M0 spike** ✅ | Standalone Vulkan ray-query microbench — DONE (`dev-tools/vkrt/`): MoltenVK #2771 RT path functional on M5 Pro, 588 Mrays/s, exact CPU parity | PASSED — gate was "works at all on Apple"; next: same-scene vs native Metal |
| **M1** | `VulkanDevice` + clspv translation of kernel set; PATHOCL SW-traversal render on NVIDIA+AMD+Intel | e2e parity vs PATHCPU on 3 vendors |
| **M2** ✅ | Ray-query RT kernel + AS build (`BuildRTAccel` in `vkintersectiondevice.cpp`) | 15/15 ray parity on MoltenVK; throughput TBD on native driver |
| **M3 decision** | Retire OpenCL everywhere; retire CUDA on NVIDIA if VK ≥ CUDA; keep Metal on macOS until MoltenVK RT ships stable | parity + perf evidence |

## Blocked by

The **current Metal HWRT hit-consumption bug must be fixed first** —
`MetalRTKernel` is the reference implementation the Vulkan RT kernel
design copies (AS layout, user-ID mapping, hit-buffer contract).

## Effort scale

Roughly **Metal backend ×1.5–2** (device layer + translation glue + RT
kernel + 3-vendor parity debugging). The clspv spike is the go/no-go:
if the `.cl` corpus compiles, the rest is plumbing we have already
written once for Metal.

## Implementation status (Apple Silicon)

Verified end-to-end on Apple M5 Pro via MoltenVK (2026-09):

- **Device layer**: `VulkanDevice` + `VulkanIntersectionDevice`
  (`src/luxrays/devices/vkdevice.cpp`, `vkintersectiondevice.cpp`).
  Physical storage buffers (`VK_KHR_buffer_device_address`), push
  constants for clustered POD kernel args, `pointer_ubo` /
  `pointer_pushconstant` marshalling, descriptor reflection via
  `clspv-reflection` + programmatic binding.
- **Kernels**: all **21 PATHOCL kernels** compile through locally
  patched `clspv` — per-kernel `internalize`+`globaldce` split-compile
  after stripping `llvm.global.annotations` (the annotation global roots
  every `spir_kernel`, so naive globaldce prunes nothing: 44 MB → ~1.5k
  lines per kernel, seconds instead of ~10 min/kernel). Every `.spv`
  passes `spirv-val` with `OpEntryPoint` present. Cache:
  `~/.luxcore/vkcache`.
- **Validation**: `vk_intersect_test` PASS 8/8 rays vs CPU BVH;
  PATHOCL 1280×720 Cornell render matches PATHCPU (8×8-blurred MAE
  0.745, mean Δ 0.17 — pure MC noise, identical structure).
- **fp64**: LuxCore sources contain bare double literals
  (`0.5 * bool`, `1e-5`, …) which OpenCL silently promotes but Metal
  forbids. Fixed at the clspv frontend with
  `-cl-single-precision-constant` — zero source churn.
- **Opt-in**: Vulkan devices are enumerated in `GetOpenCLDeviceDescs()`
  and `OCLRenderEngine`, but are only selected via an explicit
  `opencl.devices.select` mask — `opencl.gpu.use` never auto-picks them.
- **SuperBlendLuxCore**: `gpu_backend = "VULKAN"` preference, device
  filtering/selection strings, build-availability warning; the
  external-render runner re-derives the selection string in the child
  process via `BLC_GPU_BACKEND`.
- **Runtime toolchain** (2026-09): `dev-tools/vulkan-tools-install.sh`
  installs clspv + LLVM `opt`/`llvm-dis` + `clspv-reflection` +
  `libMoltenVK.dylib` into `~/.luxcore/vktools` (clspv-tree layout, so
  `GetOptPath` resolves `opt` relatively). `InitVulkanLibrary()` falls
  back to `dlopen(abs)+volkInitializeCustom` over `LUXRAYS_MOLTENVK` /
  `~/.luxcore/vktools/lib` / module-adjacent paths — GUI Blender
  enumerates `VULKAN_GPU` with no `DYLD_*` env.
- **Per-kernel cache granularity**: cache keys hash each kernel's
  pruned module (`vkk2-<kernel>-<khash>`), not the whole program — a
  source edit only recompiles kernels whose call graph changed.
- **SPIRV-Cross fixes** live on the writable fork
  `claudianus/SPIRV-Cross@luxcore-psb-msl-fixes` (`be1636d3`,
  `335eaf6c`); the MoltenVK build in `dev-tools/vkrt` references it.
- **Full regression**: `vulkan-regression.sh --full` PASS — Stage A
  `vk_intersect_test` 8/8 rays, Stage B 1280×720 PATHOCL render with
  deterministic centre-pixel assert (needs fix #5 below; without it
  `vkCreatePipelineLayout`/`pipeline` fails
  `VK_ERROR_INITIALIZATION_FAILED` inside MTLCompilerService).

### SPIRV-Cross / MoltenVK findings (patched locally)

`dev-tools/vkrt/MoltenVK/External/SPIRV-Cross/spirv_msl.cpp` — all
fixes verified by `xcrun metal` on the generated MSL:

1. **PSB pointer-to-array deref** — `OpAccessChain` on a physical-
   storage-buffer pointer whose pointee is an array needs `(*p)[i]`,
   not `p[i]`; the existing struct path skipped the wrap. Same for
   zero-index access chains (`(*p) = v`, not `p = v`).
2. **Array-of-pointers declaration** — `OpTypeArray` copies the element
   SPIRType, so `[8 x PSB-ptr]` types inherit `pointer=true`+PSB storage;
   `type_to_array_glsl` then wrongly suppressed the `[8]` suffix. Check
   `is_physical_pointer` (the op), not the inherited flag.
3. **Pointer-array by-ref params** — `constref` prepended `const` to the
   *pointee* (`const device uchar*(&)[8]`), which cannot bind a caller's
   `device uchar*[8]`. Suppressed for pointer-element arrays.
4. **Large-function `noinline`** — the big one. SPIRV-Cross emits every
   non-entry function `static inline __attribute__((always_inline))`
   (upstream design for small shaders). On our ~180k-line megakernel MSL
   this *forced* Metal's AlwaysInliner to flatten a 19k-line function
   into every call site → `MTLCompilerService` OOM (~19.6 GB). Plain
   `static` alone is not enough: the pipeline-state stage
   (`airntEmitPipelineImage`) re-inlines the graph anyway (34+ min).
   Emitting `static __attribute__((noinline))` for functions above a
   size threshold (instruction count via `SPIRFunction::blocks`/
   `SPIRBlock::ops`) survives into AIR and blocks pipeline-stage
   re-inlining: library ~15 s, pipeline ~184 s, vs OOM/∞ before.
   Small helpers keep `always_inline`; `static` keeps internal linkage
   so metallib symbol dedup still works.
5. **BDA pointer-to-array casts** — BDA pointers are declared
   `spvUnsafeArray<T,N>*` (the templated wrapper `type_to_glsl` emits
   for physical pointers), but `&member` on a physical-layout C array
   yields `T(*)[N]` — an incompatible pointer type that fails MSL
   (`cannot initialize 'spvUnsafeArray<float,4096>*' with
   'float(*)[4096]'`, surfaced at runtime as
   `VK_ERROR_INITIALIZATION_FAILED` during pipeline creation).
   `to_pointer_expression`/`to_enclosed_pointer_expression`/
   `to_ptr_expression` route through `bda_array_pointer_cast`, wrapping
   `&expr` in `reinterpret_cast<device spvUnsafeArray<T,N>*>` when the
   pointee is an array — layout-identical, so the cast is a no-op
   (`335eaf6c`).
6. **Module-scope PSB variables** — clspv's
   `-module-constants-in-storage-buffer` emits
   `OpVariable ... PhysicalStorageBuffer <constant-composite>` for
   kernel constant tables, referenced by `OpConvertPtrToU`. Compiler
   only registers Private/Workgroup/Output storage in
   `global_variables`, and SPIR-V 1.4+'s interface rule hides the rest,
   so `&_N` was emitted with no declaration (`undeclared identifier
   '_23'` in `AdvancePaths_MK_RT_DL`). `emit_resources` now emits them
   as program-scope `constant` globals, and `bda_array_pointer_cast`
   routes their address through `reinterpret_cast<ulong>` (MSL bans a
   direct `constant`→`device` pointer cast) — fork commits `f7e6f6da`
   + `69472361` on `SPIRV-Cross@luxcore-psb-msl-fixes`.

### Known limitations (this port)

- **Compile time dominates**: cold pipeline creation for all 21 kernels
  is tens of minutes (largest kernel ~3.5–8 min in AGX codegen). Two
  cache layers mitigate: per-kernel `.spv`+`.map` under
  `~/.luxcore/vkcache` (content-keyed on the pruned module — skips
  clspv entirely on hit) and a persistent `VkPipelineCache`
  (`vkpipe-<pipelineCacheUUID>.bin`, seeded at `Start()`, serialized at
  `Stop()`) which skips MoltenVK's SPIR-V→MSL→Metal codegen on warm
  runs (update test: 1350 s cold → 10 s warm). If the seed blob is
  rejected (a serialized entry whose stored MSL no longer compiles —
  the "poisoned cache" failure mode), Start() retries with an empty
  cache so one bad entry can't wedge every run; pipeline creation also
  retries ≤4× for cold-compile flakes. Fine for development, still
  not shippable UX for cold starts.
- **MoltenVK-specific**: path goes OpenCL C → clspv → SPIR-V →
  SPIRV-Cross → MSL → Metal. Native Vulkan drivers (NV/AMD/Intel) skip
  the MSL stage entirely — the SPIRV-Cross fixes above are Apple-only
  concerns.
- **HWRT implemented (M2)**: `VulkanIntersectionDevice` builds one BLAS
  per mesh + TLAS and runs a GLSL `GL_EXT_ray_query` compute kernel
  (`src/luxrays/devices/vkintersectiondevice.cpp`,
  `BuildRTAccel()`/`EnqueueTraceRayBuffer`). OpenCL C cannot express
  `OpRayQuery*`, so the traversal shader is compiled by
  `glslangValidator` (bundled in `~/.luxcore/vktools`) while shading
  kernels keep the clspv path. Hit mapping:
  `instanceCustomIndex`→meshIndex, `primitiveIndex`→triangleIndex,
  barycentrics (u,v)→(b1,b2) — byte-identical `RayHit` to the SW path.
  `LUXRAYS_VULKAN_RT=0` forces the SW traversal fallback; dataset
  edits (`Update()`) rebuild the AS set. Validated: `vk_intersect_test`
  15/15 rays HWRT *and* forced-SW, `spirv-val` clean
  (`OpTypeAccelerationStructureKHR`/`OpRayQuery*` present); 1280×720
  PATHOCL kitchen render (`scenes/kitchen/kitchen-agx-vulkan.cfg`,
  25 BLAS + TLAS, AgX + ACES 2.0 outputs visually verified);
  scene-edit rebuild verified via `dev-tools/vk_rt_update_test.py`
  (second `BLAS + TLAS built` logged after `EndSceneEdit`, session
  completes clean). Remaining note: edit path reports a small
  872-byte device-buffer leak on session teardown — minor, tracked.
- **MoltenVK RT is experimental**: AS support needs
  `enableExperimentalRayTracing` in the global `MVKConfiguration`
  (set programmatically via `vkSetMoltenVKConfigurationMVK` — the
  function is deprecated but `VK_EXT_layer_settings` cannot reach the
  *global* config it toggles; ABI-safe via the size round-trip). Perf
  unverified vs native drivers — MoltenVK's RT is functional, not
  tuned.
- Blender external-process render supports Vulkan via `BLC_GPU_BACKEND`
  re-derivation; in-process and viewport renders work through the
  normal `opencl.devices.select` path.

Regression: `dev-tools/vulkan-regression.sh` (Stage A `vk_intersect_test`,
seconds; `--full` adds a 1280×720 PATHOCL emissive-quad render with
deterministic centre-pixel assert + HWRT-mode check — Stage B — and a
scene-edit AS-rebuild test — Stage C, skipped without pysuperluxcore).

## References

- MoltenVK RT status: <https://github.com/KhronosGroup/MoltenVK/pull/2771> (WIP, opt-in)
- ChameleonRT multi-backend RT comparison: <https://github.com/Twinklebear/ChameleonRT> + NVIDIA forums thread 260558
- Tellusim RT API benchmarks: <https://tellusim.com/rt-perf/>
- clspv (OpenCL C → Vulkan SPIR-V): <https://github.com/google/clspv> + docs.vulkan.org OpenCL-on-Vulkan tutorial
- Slang targets & RT capabilities: <https://shader-slang.org/slang/user-guide/targets>, SIGGRAPH 2025 Slang BOF
- HIPRT: <https://gpuopen.com/hiprt/> + HIPRT paper (ACM 10.1145/3675378)
