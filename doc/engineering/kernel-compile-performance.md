# GPU kernel-compile performance (cold-start)

Measured on Apple M5 Pro (18 cores), `dev-tools/e47_charlie_sheen_parity.py`,
PathOCL program = 3 base + 17 micro kernels (+ conditional light / VC /
merge-hash sets).

## Baseline (sequential `GetKernel` loop, log `/tmp/e47_cold.log`)

- **Metal total 111.1s** = cl2msl translation ~19s + `newLibraryWithSource`
  3.8s + sequential PSO creation ~88s (each `AdvancePaths_MK_*` ~7s, one at a
  time).
- **Apple OpenCL total 83.1s** = `clBuildProgram` 11.6s + sequential
  `clCreateKernel` ~72s (again ~7s per heavy kernel — Apple's OpenCL-on-Metal
  translator does real codegen per kernel, it is not a handle lookup).
- => ~160s of the ~194s cold run were per-kernel backend compiles.

## Design

- `HardwareDevice::HasThreadSafeKernelCreation()` (default false). True for
  `OpenCLDevice` (clCreateKernel/clGetKernelWorkGroupInfo are thread-safe per
  spec; only clSetKernelArg is not) and `MetalDevice` (PSO creation is
  thread-safe; see below). CUDA/Vulkan stay sequential.
- `PathOCLBaseOCLRenderThread::InitKernels()` collects one job list (base +
  micro + the same conditional light/VC/merge sets as before), then either
  launches every job with `std::async(std::launch::async)` — joining ALL
  futures before rethrowing the first exception so no worker outlives the
  program — or runs them sequentially. All logging happens on the calling
  thread after the join (SLG_LOG is not thread-safe).
- `MetalDeviceProgram::archiveMutex` serializes
  `addComputePipelineFunctionsWithDescriptor:` — MTLBinaryArchive is not
  thread-safe; the add is near-instant, the PSO compile stays outside the
  lock (same split as Blender Cycles' Metal backend).
- `MTLDevice.shouldMaximizeConcurrentCompilation = YES` (macOS 13.3+);
  `maximumConcurrentCompilationTaskCount` reports 18 on the M5 Pro.
- `MetalDevice::GetKernel()` runs its whole body in an `@autoreleasepool`:
  the async workers are raw std::threads with no NSAutoreleasePool.
- `SUPERLUXCORE_CACHE_DIR` env: `luxrays::GetCacheDir()` returns it verbatim
  (=> `$DIR/ocl_kernel_cache`, `$DIR/cuda_kernel_cache`) and the Metal
  translation/archive dir becomes `$DIR/metal` — honest cold/dev/CI runs
  without touching the user's real caches.

## Measured after-numbers (cold at every level)

Runs 1, 6, 7 of `e47` with a fresh `SUPERLUXCORE_CACHE_DIR` AND a cold
`com.apple.metal` OS shader cache (see gotcha below):

| run | Metal compile | OpenCL compile | wall | max RSS |
|---|---|---|---|---|
| baseline | 111.1s | 83.1s | ~194s | — |
| 1 | 31.46s | 40.50s | 74.51s | 667MB |
| 6 | 31.35s | 46.85s | 80.99s | 663MB |
| 7 | 32.58s | 40.63s | 75.90s | 689MB |

- Metal ≈ **3.4x faster**; OpenCL ≈ **2x faster**. Metal cold leg is now
  cl2msl-bound (translation ~15.7s + ~15.7s wall for all 20 parallel PSOs).
- Warm run (all caches hot, same `SUPERLUXCORE_CACHE_DIR`): Metal leg
  **74ms**, OpenCL leg **8.7s** (Apple's clBuildProgram binary-load cost —
  previously invisible behind the 72s sequential kernel loop), wall 11.4s.
- App-cold / OS-warm (fresh `SUPERLUXCORE_CACHE_DIR` but warm system shader
  cache): Metal ~13s (cl2msl only), OpenCL ~0.1s.

## Gotchas

- **Apple's shader caches are uid-scoped, not HOME-scoped.** The OS-level
  Metal/OpenCL caches live in
  `$DARWIN_USER_CACHE_DIR/com.apple.metal` (e.g.
  `/var/folders/6l/<hash>/C/com.apple.metal`). Neither `HOME=` nor
  `MTL_SHADER_CACHE_PATH=` redirects them. Consequence: a "fresh
  SUPERLUXCORE_CACHE_DIR" run is only app-cold — kernel compiles still hit
  the OS cache (~20ms/kernel). A *truly* cold run needs a new kernel source
  (first build after .cl edits) or moving `com.apple.metal` aside for the
  run and merging it back afterwards (it is fully regenerable).
- **Never kill a running ninja.** It truncates `out/build/.ninja_deps`
  ("ninja: warning: premature end of file; recovering") and the next build
  becomes a full rebuild. Run builds in the background and poll instead.
- `clCreateKernel` on Apple OpenCL is CPU-heavy codegen, not a lookup:
  17-20 concurrent workers saturate the cores, so per-kernel elapsed times
  inflate (up to ~30s each) while the wall drops 2x. Judge by the
  "Kernels compilation time" line, not per-kernel ms.

## Artifacts

- Cold logs: `/tmp/e47_par_1.log`, `/tmp/e47_par_6.log`, `/tmp/e47_par_7.log`
- Warm log: `/tmp/e47_warm2.log`; baseline: `/tmp/e47_cold.log`
- cl2msl profile (cProfile, top-25 cumulative): `/tmp/cl2msl_profile.txt` —
  ~85% of translation time is `propagate_gid()` inside
  `rule_kernel_buffer_attrs()` (regex `re.search` over function bodies).
