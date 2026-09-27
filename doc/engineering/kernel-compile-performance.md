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
  spec; only clSetKernelArg is not), `MetalDevice` (PSO creation is
  thread-safe; see below) and `VulkanDevice` (see the Vulkan section — the
  shared pipeline cache is internally synchronized, merges are mutex-gated).
  CUDA stays sequential.
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
  (=> `$DIR/ocl_kernel_cache`, `$DIR/cuda_kernel_cache`), the Metal
  translation/archive dir becomes `$DIR/metal`, and the whole per-user
  Vulkan tree becomes `$DIR/luxcore/{vkcache,vktools}` — honest cold/dev/CI
  runs without touching the user's real caches.

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

## cl2msl translator optimization (`propagate_gid`, 2026-09-27)

`propagate_gid` (threads `gid` through the helper call graph) was ~85%
of translation time: the upward fixpoint re-searched every function
body for every needing callee (~376k `re.search` over the 3.4MB
source) and pass 2 ran one `re.finditer` per needing name over the
whole text (~22k full-text scans total).

Rewrite (same output, verified byte-identical on the e47 PathOCL
program — emitted .msl and layout .json both `cmp`-clean):

- one `_fn_spans` walk; each body is comment-stripped once and all its
  callee candidates extracted with a single compiled pattern
  `(?<![\w:.$])([A-Za-z_]\w*)\s*\(`
- reverse edges callee → callers built once; fixpoint becomes a
  worklist walk over edges (same least fixpoint, O(edges) not
  O(names × needs × body size))
- pass 2 is ONE whole-text `finditer` filtering matches to `needs`;
  the per-call-site logic (paren walk, signature-skip via fresh spans,
  last-arg / SAMPLER_PARAM / empty-call rules) is unchanged

| metric | before | after |
|---|---|---|
| cl2msl wall (e47 PathOCL input, unprofiled) | 28.6s | 2.06–2.24s |
| cProfile total | 36.6s | 3.25s |
| `propagate_gid` cumulative | 30.98s | 1.48s |
| `re.search` calls | ~398k | ~27k |

New cold-run e47 numbers (fresh `SUPERLUXCORE_CACHE_DIR` + cold
`com.apple.metal`, logs `/tmp/e47_cl_{1,2}.log`):

| run | Metal compile | OpenCL compile | wall |
|---|---|---|---|
| before (runs 1/6/7) | 31.4–32.6s | 40.5–46.9s | 74.5–81s |
| cl1 | 20.25s | 42.52s | 66s |
| cl2 | 21.10s | 41.98s | 66s |

The Metal leg is no longer translator-bound: ~2.3s cl2msl + ~2.9s
`newLibraryWithSource`/archive + ~15s parallel PSO compiles.
PATHOCL-METAL_GPU and PATHOCL-OPENCL_GPU both PASS; the .msl the run
produced is byte-identical to the verified standalone output.
New profile: `/tmp/cl2msl_profile_new.txt` (top entry is now
`rule_kernel_buffer_attrs` at 2.2s, mostly `_fn_spans`/`re.sub`).

## Dev/test env knobs (2026-09-27)

- `SUPERLUXCORE_SERIAL_KERNELS` (set, non-empty): escape hatch in
  `PathOCLBaseOCLRenderThread::InitKernels()` — forces the sequential
  kernel-compile loop for debugging/driver issues. Verified on e47
  (fresh `SUPERLUXCORE_CACHE_DIR`): "Compiling 20 kernels sequentially"
  vs "in parallel", both runs PASS.
- `SUPERLUXCORE_BACKENDS=cpu,opencl,metal` (comma subset, default all):
  render-leg scoping in `dev-tools/e21_backend_parity_test.py`,
  `e47_charlie_sheen_parity.py`, `e48_sss_cb15_parity.py`,
  `e49_sellmeier_dispersion.py`. The parity ref is the first available
  leg (cpu when listed). This is the dev-loop mitigation for OpenCL's
  ~42s cold cost — skip it during iteration, parity still testable on
  demand.
- `SUPERLUXCORE_SCENES=<tag,…>` (e21 only): scene-tag subset.
- Verified: `SUPERLUXCORE_BACKENDS=cpu,metal SUPERLUXCORE_SCENES=cornell
  e21` runs exactly PATHCPU + PATHOCL-Metal, ALL PASS.

## e21 parallel-compile causality verdict (2026-09-27)

Question: are the mtl-vs-ocl pixel-tail gate failures (media, bump,
luxball) caused by parallel `clCreateKernel`? Ran
`SUPERLUXCORE_SCENES=media,bump,luxball` on the same Debug build,
serial vs parallel (kernel content is identical either way):

| leg | serial >30%-diff (p50/p99) | parallel >30%-diff (p50/p99) |
|---|---|---|
| media | 8.94% (0.992/2.192) | 2.79% (1.000/1.458) |
| bump | 2.08% (1.000/1.472) | 1.22% (1.000/1.228) |
| luxball | 2.67% (0.999/3.428) | 1.94% (1.000/1.405) |

Same three scenes fail in both modes with the same signature — p50
≈ 1.0 (bulk pixel parity) plus a >30%-diff tail above the 1% gate;
means agree within ~0.5% everywhere. Tail magnitudes move run to run
(PATHCPU/GPU sample ordering is nondeterministic), so the spread is
noise on top of a real, persistent Metal-vs-OpenCL per-pixel tail.
**Verdict: parallel kernel creation is NOT responsible.** The tail is
a genuine backend divergence (or stochastic sampler tail) on these
scenes, not a compile-order artifact. Left as-is per findings; no
workaround applied.

## Apple OpenCL residual cold cost

After parallel clCreateKernel the OpenCL leg still costs ~42s cold:
`clBuildProgram` (~12s) plus per-kernel codegen that is largely
serialized driver-internally despite 17-20 concurrent callers
(per-kernel elapsed inflates under contention; the wall gain was the
2x seen above — the remaining serialization is inside Apple's
OpenCL-on-Metal translator and is not fixable from our side).
Mitigation: `SUPERLUXCORE_BACKENDS` scoping during dev.

## C++ TU header fan-out (PCH, 2026-09-27)

Different axis from the rest of this doc: C++ translation-unit compile
time, not GPU-kernel cold start. A typical TU parses ~2000+ includes
(mostly libc++ + Boost) before reaching project code; e.g. touching
`include/slg/materials/material.h` recompiles ~197 TUs.

Mechanism: `src/pch/stable_pch.hpp` — a private umbrella of *stable*
headers only (C library, libc++, frequently-used Boost) under
`#ifdef __cplusplus`. Applied per target via
`target_precompile_headers(<t> PRIVATE
${PROJECT_SOURCE_DIR}/src/pch/stable_pch.hpp)` on `luxrays`, `slg-core`,
`slg-film`, `luxcore`, `luxcore_static`. CMake generates
`cmake_pch.hxx` + `.pch` per target per config and clang consumes it via
`-Xclang -include-pch`. PRIVATE means it never leaks into dependent
targets' compile lines. No `REUSE_FROM` — the targets compile with
different flags, so each gets its own PCH object. No LuxCore headers in
the umbrella: they change under development and would invalidate the
whole PCH per edit. ObjC++ `.mm` sources are already isolated in
`luxrays_metalobj` (no PCH attached); the C source
`deps/volk/volk.c` inside `luxrays` consumes a no-op C-mode PCH thanks to
the `__cplusplus` guard.

Measured (Release, Ninja Multi-Config, Apple M5 Pro, healthy
`.ninja_deps`; incremental = touch header → `ninja -f build-Release.ninja
pysuperluxcore`):

| touch | edges | baseline wall | PCH wall | paired TU CPU |
|---|---|---|---|---|
| materials/material.h | 197 | 36.99s | 29.6–30.05s | 782,996 → 625,720 ms (−20.1%) |
| textures/texture.h | 261 | —¹ | 34.84s | —¹ |

¹ The pre-PCH texture.h number was taken while `.ninja_deps` was still
corrupt (see gotcha below — every build degenerated toward a full
rebuild), so no trustworthy baseline exists for it; ~84s was observed
in that contaminated regime.

- Median per-TU compile: 1787ms → 1143ms (−36%).
- Warning delta: zero — 3278 warnings both runs, identical 123-type set.
- Gates: `dev-tools/parity-regression.sh` ALL PASS (CPU + GPU);
  `e47_charlie_sheen_parity.py` METAL_GPU + OPENCL_GPU both PASS.
- Decision: **kept** (−20% wall/CPU vs the 15% keep gate).

Failed experiment — do not retry blindly: adding
`embree4/rtcore*.h`, `Imath/*.h`, `OpenImageIO/image_span.h` to the
umbrella (each pulled by ~1170 TUs via `bvhbuild.h`/`imagemap.h`) made
things *worse*: paired TU CPU 626,816 → 640,775 ms (+2.2%). A fatter PCH
costs more to mmap/symbol-load per TU than the extra parsing saves.
Keep the umbrella to libc++ + Boost.

Caveats:

- The first build after `stable_pch.hpp` or compile flags change pays one
  extra edge per target (the `.pch` itself, ~2–4s) — amortised
  immediately.
- `.ninja_deps` corruption predates this work: a killed ninja truncated
  the deps log mid-record ("premature end of file; recovering"), making
  every subsequent build ignore deps and rebuild ~everything. Fixed by
  truncating at the last valid record. See the never-kill-ninja gotcha.
- `ccache` is not installed on this machine; PCH and ccache are
  orthogonal (PCH cuts parse, ccache skips whole TUs) — installing it
  would stack on top of this win.

## Vulkan (MoltenVK) parallel `GetKernel` + persistent pipeline cache (2026-09-27)

### Thread-safety audit of `VulkanDevice::GetKernel()`

- `vkCreateDescriptorSetLayout/PipelineLayout/ShaderModule`,
  `vkCreateBuffer`, `vkAllocateMemory`, `vkBindBufferMemory`,
  `vkMapMemory`, `vkCreateComputePipelines`: all take only `VkDevice` +
  per-call parameters; device-scope calls are implicitly synchronized per
  spec §3.6 ("all commands support being called concurrently ... certain
  parameters ... are defined to be externally synchronized").
- `kern->*` fields: `VulkanDeviceKernel` is `make_unique` per call; every
  handle (pipeline, layouts, descPool, buffers, module) is owned by that
  kernel and freed in `~VulkanDeviceKernel`. Program/SPIR-V inputs are
  per-call too.
- `cmdPool`/`openCmd`/`queue` (externally synchronized objects) are NOT
  touched by `GetKernel()`: module-constant and POD blobs are
  HOST_VISIBLE+HOST_COHERENT and filled via `vkMapMemory` memcpy — no
  command buffer or queue submit, so no upload lock is needed.
- `pipeCache` (persisted to `vkcache/vkpipe-<uuid>.bin`): the only shared
  object — see below.
- `GetKernelWorkGroupSize()` returns the baked `kern->localSizeX`; no
  device calls.

### Pipeline cache: the spec reading that decided the design

- `VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT` is an opt-OUT,
  not an enabler: "all commands that modify the created VkPipelineCache
  will be externally synchronized" — the implementation "may skip any
  unnecessary processing needed to support simultaneous modification".
  It transfers the sync duty to the app; it does NOT make concurrent
  access safe.
- `vkCreateComputePipelines` VUID only requires external sync of
  `pipelineCache` when the cache was created with that flag. Ours is
  flag-less ⇒ concurrent creates are legal per spec.
- MoltenVK reality (`MVKPipeline.mm`): the flag maps to
  `_isExternallySynchronized`; a flag-less cache serializes the whole
  `getShaderLibrary()` under `_shaderCacheLock` — including the
  SPIR-V → MSL → MTLLibrary compile. Sharing one cache would be
  spec-legal but would serialize exactly the work we want in parallel.
- `vkMergePipelineCaches`: dstCache needs external sync unless created
  with `VK_PIPELINE_CACHE_CREATE_INTERNALLY_SYNCHRONIZED_MERGE_BIT_KHR`;
  MoltenVK's merge iterates the shared `_shaderCache` unlocked.

### Design implemented (all documented inline in vkdevice.cpp `GetKernel`)

1. Warm-hit probe on the shared persistent `pipeCache` with
   `VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT`
   (`pipelineCreationCacheControl` — core in 1.3, the EXT struct covers
   older drivers), under `pipeCacheMutex` shared_lock: a hit builds the
   pipeline from a lookup-only pass, and readers still overlap.
2. Miss → compile on a private UNSEEDED `VkPipelineCache`: its internal
   lock is uncontended ⇒ true parallel compiles. Seeding it with the
   persisted blob was rejected — MoltenVK eagerly loads/recompiles every
   cached MSL at `vkCreatePipelineCache`, so a ~129MB seed would replay
   per worker.
3. Success → `vkMergePipelineCaches` into the shared cache under
   `pipeCacheMutex` unique_lock (exclusive writer ⇒ satisfies the
   dstCache VUID; can never overlap a probe). Next run is warm.
4. Any failure → the original 4× MoltenVK-nondeterminism retry loop on
   the shared cache (also a shared reader) — retry semantics preserved.

`HasThreadSafeKernelCreation()` now returns true for `VulkanDevice`.

### Measured (Apple M5 Pro, `dev-tools/vulkan-regression.sh --full`,
`scenes/parity/emissive-direct`, PATHOCL = 20 kernels)

| run | kernel-compile phase | notes |
|---|---|---|
| cold parallel (fresh `SUPERLUXCORE_CACHE_DIR=/tmp/slc_vk_cold`) | **1,271,162ms (~21.2min)** | per-kernel sum 3,711s (contention-inflated); longest pole `MK_MNEE_NEXT_VERTEX` 616s |
| cold sequential — TRUE cold (`SUPERLUXCORE_SERIAL_KERNELS=1`, SPV-seeded but `vkpipe`-cold `/tmp/slc_vk_seq2`, `com.apple.metal*` moved aside) | **1,845,818ms (~30.8min)** | per-kernel sum = wall (uncontended) ⇒ parallel wall win ≈ **1.45×** |
| app-cold sequential (same but OS shader cache warm) | **757,727ms (~12.6min)** | `com.apple.metal` is uid-scoped, NOT redirected — same gotcha as the Metal runs; MSL→MTL lib compiles hit the OS cache |
| warm parallel (same cache root as cold parallel) | **823ms** | all 20 probes warm-hit; MergeSampleBuffersOCL 1ms |

Honest read: 20-way parallel creation still overlaps real work
(the MTLCompiler-service XPC pipeline accepts concurrent requests), but
contention roughly doubles the summed per-kernel cost — the serial run's
own per-kernel numbers are ~2× the app-cold-warm run's and ~half the
parallel-contended ones. Judge all of it by "Kernels compilation time".

Cold parallel per-kernel ms (from `emissive-vk.log`):
`Film_Clear` 77 · `InitSeed` 50 · `Init` 1003 · `RT_NEXT_VERTEX` 344511 ·
`HIT_NOTHING` 631 · `HIT_OBJECT` 432537 · `RT_DL` 342398 · `RT_RESTIR`
249893 · `RT_GI_BOUNCE` 344232 · `RT_GI_RESOLVE` 314889 · `DL_ILLUMINATE`
376506 · `DL_SAMPLE_BSDF` 286382 · `MNEE_NEXT_VERTEX` 616014 ·
`GENERATE_NEXT_VERTEX_RAY` 398762 · `SPLAT_SAMPLE` 836 · `NEXT_SAMPLE`
138 · `GENERATE_CAMERA_RAY` 894 · `BuildQueues` 51 · `BucketHistogram` 49 ·
`QueuePrefix` 49.

Validation: ALL VULKAN REGRESSION CASES PASSED on every run — cold
parallel, warm parallel, OS-warm serial, true-cold serial (Stage A
intersect 15/15, Stage B centre pixel 4.0000 / 3.9844 / 4.0000 / 4.0000
vs 4.0±0.20, Stage C AS rebuild). No crashes, no validation errors, no
new MoltenVK warnings; parallel workers sampled inside MTLCompiler XPC
waits (no deadlock).

### `SUPERLUXCORE_CACHE_DIR` → `luxcore/` Vulkan tree

- `luxrays::GetVulkanLuxCoreDir()` (vkdevice.cpp): `$DIR/luxcore` when the
  env is set, else `~/.luxcore`; `GetVulkanLuxCoreDirs()` appends the real
  `~/.luxcore` as a read-only fallback so the `vktools` toolchain (clspv,
  glslangValidator, libMoltenVK.dylib) still resolves from a benchmark
  root that has no tools of its own.
- Applied at: clspv lookup, MoltenVK dylib candidates, the persistent
  `pipeCachePath`, `CompileProgram`'s `.bc/.spv` dir, and
  vkintersectiondevice.cpp's glslangValidator + `vkrt-intersect-*` shader
  cache. `MkdirP` replaces the single-level `mkdir` (env roots can nest).
- Verified: the env-scoped cold run wrote only under
  `/tmp/slc_vk_cold/luxcore/vkcache` (~680MB); `~/.luxcore/vkcache` and
  `~/.luxcore/vktools` were untouched (the real `vkpipe-*.bin` mtime
  predates the run — only the earlier non-env Stage A wrote it).

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
- Vulkan: cold parallel `/tmp/vk_par_cold_full.log`, warm
  `/tmp/vk_warm_render.log` (+ `emissive-vk.png` in `/tmp/vk_warm_work/`),
  sequential app-cold `/tmp/vk_seq_full.log`, sequential true-cold
  `/tmp/vk_seq_oscold_full.log`
- cl2msl profile (cProfile, top-25 cumulative): `/tmp/cl2msl_profile.txt` —
  ~85% of translation time is `propagate_gid()` inside
  `rule_kernel_buffer_attrs()` (regex `re.search` over function bodies).
