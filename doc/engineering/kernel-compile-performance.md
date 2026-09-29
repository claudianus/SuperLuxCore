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
- `sccache` 0.17 measured and REJECTED under the PCH build (see
  "Structural options #5" update): CMake implements PCH via `-Xclang
  -include-pch/-pth`, which sccache cannot cache — 612/614 calls were
  pass-through ("Can't handle UnknownFlag arguments with -Xclang").
  And even without PCH, a touched header changes every dependent TU's
  preprocessed hash → incremental builds always miss; a compiler cache
  only pays off for clean/branch-switch rebuilds. `luxmake config` now
  wires a launcher only when `SUPERLUXCORE_CCACHE=1` is set explicitly.

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

## Vulkan `CompileProgram`: parallel opt prune + the `system()`
serialization trap (2026-09-27)

### The discovery: `system()` serializes across threads on macOS

`VulkanDevice::CompileProgram` runs three per-program stages: clspv
frontend (`.bc`), per-kernel `opt -passes='internalize,globaldce'`
prune (`.pruned.bc`), per-kernel `clspv -x ir` + clspv-reflection
(`.spv`+`.map`). The clspv stage already had a worker pool and the opt
loop got the same pool shape (`std::atomic<u_int> next`/`failed`,
`nWorkers = min(8, todo)`) — **but measured `system()` behaviour on
macOS kills both pools**: Libc holds a process-wide lock spanning the
whole child lifetime, so N threads calling `system()` run their
children strictly one-at-a-time.

Measured: 4 threads × `system("sleep 3")` = **12.0s wall** (children'
own timestamps fully disjoint); the same via `posix_spawn` +
per-thread `waitpid(pid)` = **2.0s**. The opt pool's `.pruned.bc`
mtimes under `system()` show a uniform ~2.5s drip = serial rate, and
the clspv-ir stage had silently been serial all along — that is the
"clspv program compile (cold ~11min) still serial" noted in the
previous session: the worker pool existed but never actually
parallelized.

### Fix (vkdevice.cpp)

- New `RunShellCmd(cmd)`: `posix_spawn("/bin/sh", {"sh","-c",cmd})` +
  `waitpid` on the child's own pid in each worker thread — same wait
  status contract as `system()` (nonzero = failed), real concurrency.
- Opt loop split into: (1) collect missing `.pruned.bc`, (2) worker
  pool of `min(8, toPrune.size())` running `RunShellCmd`, failures
  collected per-kernel (name+rc, reported in kernel order), (3) serial
  hash/`kernelBasePaths`/`todo` bookkeeping on the main thread.
- The existing clspv-ir pool switched to `RunShellCmd` too — same one-
  word fix, same latent bug.
- Serial single-shot `system()` calls left alone (clspv frontend,
  llvm-dis, glslangValidator).

### Measured (M5 Pro, cold `SUPERLUXCORE_CACHE_DIR`, PATHOCL = 25
kernels + MergeSampleBuffers 4)

| stage | before (`system()`) | after (`posix_spawn`) |
|---|---|---|
| opt prune, 25× `opt` on a 248MB `.ll` | ~62s (serial drip) | **21.4s** |
| clspv -x ir + reflection, 25 kernels | **607.6s** | **191.5s** |
| **CompileProgram total** | **~678s (~11.3min)** | **~228s (~3.8min), −450s** |

Controlled micro-bench (same 25 opts, idle machine): serial 64.0s,
`xargs -P8` 11.4s (5.6×); solo opt 2.3s / ~1GB RSS. Per-kernel opt
cost is dominated by parsing the 248MB annotation-stripped `.ll` once
per kernel — memory-bandwidth heavy, so 8-way scales ~3× in-app not
8× (same contention pattern as the 20-way Metal pipeline phase).
Outputs verified byte-identical: all 25 `.pruned.bc` produced by the
parallel pool cmp-equal the serially-produced ones on the same input;
cross-run diffs are only the cache-dir path embedded in the module
(`source_filename`), pre-existing behaviour.

Regression: `dev-tools/vulkan-regression.sh --full` ALL PASSED both runs (Stage A 15/15 HWRT+SW, Stage B centre 4.0000 vs 4.0±0.20, Stage
C AS rebuild). A direct `LUXRAYS_VULKAN_RT=0 vk_intersect_test` run
also exercised the SW-traversal CompileProgram path on the new code.

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
- Vulkan posix_spawn pools: serial-via-system() cold
  `/tmp/vk_optpar_full.log` + cache `/tmp/slc_vk_opt/luxcore/vkcache`,
  real-parallel cold `/tmp/vk_optpar2_full.log` + cache
  `/tmp/slc_vk_opt2/luxcore/vkcache`; serial opt reference outputs in
  `/tmp/opt_serial_test/`, `/tmp/opt_run2_serial/`; system() probe:
  `/tmp/test_system_par{,2}.c`
- cl2msl profile (cProfile, top-25 cumulative): `/tmp/cl2msl_profile.txt` —
  ~85% of translation time is `propagate_gid()` inside
  `rule_kernel_buffer_attrs()` (regex `re.search` over function bodies).

---

# C++ incremental-rebuild fan-out (header hygiene)

Separate topic from GPU kernels: wall time after `touch`ing a project
header, Ninja Multi-Config `out/build`, Release config, post-PCH baseline
(`src/pch/stable_pch.hpp` already covers luxrays/boost/std — 446 TUs).

## Method

`ninja -C out/build -f build-Release.ninja -t deps` dumps the `.ninja_deps`
log; count, per project header, how many Release `.o` files list it as a
dependency. A per-TU "root" model (the .cpp's direct includes + dep-headers
unreachable from other dep-headers) attributes each dependency to the
carrier header(s) that deliver it — this tells you which include edge to
cut instead of guessing.

## Top-20 fan-out headers (Release TUs, 598 total)

| TUs | header |
|----:|--------|
| 446 | src/pch/stable_pch.hpp (PCH) |
| 403 | include/luxrays/utils/utils.h |
| 400 | include/luxrays/luxrays.h (+ luxrays_types.cl) |
| 386 | include/luxrays/utils/serializationutils.h |
| 370 | include/luxrays/core/geometry/vector.h |
| 369 | include/luxrays/core/color/color.h |
| 368 | include/luxrays/utils/observer_ptr.h |
| 366 | include/luxrays/utils/exportdefs.h |
| 365 | include/luxrays/utils/properties.h |
| 359 | include/luxrays/utils/strutils.h |
| 355 | include/luxrays/core/geometry/point.h + utils/buffer.h |
| 351 | include/luxrays/core/geometry/normal.h |
| 347 | geometry/{bbox,bsphere,matrix4x4,uv}.h + epsilon.h |
| 344 | include/luxrays/utils/proputils.h |
| 342 | geometry/{matrix4x4op,transform,triangle,quaternion}.h |
| 340 | include/luxrays/core/trianglemesh.h |
| 324 | include/slg/usings.h |
| 309 | include/slg/slg.h |
| 302 | include/slg/bsdf/bsdfevents.h |
| 279 | include/slg/film/filters/filter.h |
| 262-271 | imagemap.h / mapping.h / hitpoint.h / texture.h / colorspace / sdl |
| 197 | include/slg/materials/material.h |
| 172-178 | volume.h / light.h / sceneobject.h / bsdf.h / extmeshcache.h |

(every boost header sits at 454 — all-or-nothing via the PCH.)

## Why material.h fans out to 197 TUs

material.h sits in a dense cluster with bsdf.h / volume.h / light.h /
trianglelight.h / sceneobject.h / scene.h / materialdefs.h — all of them
include it **legitimately**: `Volume : public Material`, `bsdf.h` calls
`material->*()` inline, `light.h` calls `lightMaterial->*()` inline,
`materialdefs.h` needs `dynamic_cast<MaterialConstRef>`, `scene.h` owns a
`MaterialDefinitions` member by value. Measured sole-carrier contribution
of each gateway is small: sceneobject.h→material.h alone delivers **0**
unique TUs (every consumer also has light.h/bsdf.h/volume.h in its set).
The fan-out is an emergent property of ~40 mid-level headers each
correctly pulling the cluster — per-edge cuts give <10%.

## Applied (kept — pure include hygiene, zero semantics)

- `cameras/camera.h`: dropped `volumes/volume.h` — only `VolumeConstPtr`/
  `VolumeConstRef` used; `slg/usings.h` already provided the aliases.
- `film/sampleresult.h`, `utils/pathdepthinfo.h`: `bsdf/bsdf.h` →
  `bsdf/bsdfevents.h` (they only need the `BSDFEvent` typedef).
- `samplers/sampler.h`: added `luxrays/core/namedobject.h` (its base class
  was only ever satisfied transitively!) + `using luxrays::ocl::Seed` for
  sampler_types.cl.
- `utils/pathinfo.h`: fwd `class BSDF` + `using luxrays::ocl::Normal`.
- `utils/pathvolumeinfo.h`: fwd `class BSDF` + `bsdf/hitpoint.h`
  (`HitPoint` is a `typedef struct HitPoint_t` — cannot be fwd-declared
  cleanly; hitpoint.h is luxrays-only and reaches neither material.h nor
  texture.h).
- `scene/scene.cpp`, `imagemap/resizepolicies/calcoptsize.cpp`: explicit
  `bsdf/bsdf.h` (both instantiate/use `BSDF` — were piggybacking on the
  sampleresult.h transitive include).

Hidden coupling discovered: `*_types.cl` files inside `namespace slg::ocl`
depend on `using luxrays::ocl::*` declarations injected by *other* headers
(hitpoint.h, mapping.h). Removing a cluster header exposed which headers
were secretly not self-contained — fixed with explicit usings where needed.

## Result

`touch include/slg/materials/material.h && ninja … pysuperluxcore`:

- TUs: **202 tasks → 185 (181 compile steps), −8.1%**
- wall: 33.0s → ~35s (within noise; system had parallel load)
- `texture.h` touch: 261 → 245 TUs (−6.1%), wall ~35→37s

Verdict: <15% gain — kept only the hygiene-level cleanups (all safe).
Further per-edge trimming is exhausted: remaining material.h TUs
genuinely use Material/BSDF definitions.

## Structural options for a deeper cut

1. **Split `material.h` into `material_types.h` + `material_base.h` +
   `material.h`.** (NOT implemented) `MaterialType` /
   `MaterialEmissionDLSType` enums + `Material`'s vtable-pure interface
   (decls only, no inline) vs. the members-heavy class body. Most
   consumers call `material->IsLightSource()` etc. — they'd still need
   the full class, so payoff is modest; the real win would be a
   *handle-level* header exposing only `MaterialRef`/`MaterialType`
   (used by scene.h/renderengine.h declarations). Est: 10-20% of
   material.h-touch TUs.
2. **`BSDF` in its own light-weight TU boundary.** (NOT implemented)
   bsdf.h is the biggest cluster multiplier (171 TUs): it must
   inline-call Material, so any TU creating a BSDF needs everything.
   Moving BSDF's hot inline accessors to a `bsdf_inline.h` leaf and
   keeping `bsdf.h` decl-only lets light paths (pathtracer etc.) still
   inline while edit/serialize TUs skip the material cluster. Est:
   20-40 TUs; touches the hottest shading code — measure perf
   before/after.
3. **Break `scene.h`'s by-value ownership — DONE.** `Scene::texDefs`/
   `matDefs`/`objDefs`/`lightDefs` are now `TextureDefinitionsUPtr` etc.
   (`std::unique_ptr`, allocated in `Scene::Init()`), so scene.h needs
   only the `DECLARE_SUBTYPES` forward declarations from `slg/usings.h`
   and drops `lightsourcedefs.h`/`texturedefs.h`/`materialdefs.h`/
   `sceneobjectdefs.h`. `~Scene()` was already out-of-line so
   unique_ptr-of-incomplete-type is safe; Scene was already
   non-copyable (`std::mutex trashMtx`) so no copy-ctor work was
   needed. Accessor API unchanged (`return *texDefs`); scene.h also had
   to honestly include `extmeshcache.h`, `imagemapcache.h` (by-value
   members were piggybacking on the defs' transitive includes!) and
   `bsdf/bsdfevents.h` (`NONE` default arg).
   Measured (`touch include/slg/scene/scene.h && ninja … pysuperluxcore
   luxcoreconsole`, deps via `ninja -t deps`):
   - TUs pulling the defs cluster via scene.h: materialdefs.h
     **140 → 13**, texturedefs.h **140 → 5**, sceneobjectdefs.h
     **140 → 19**, lightsourcedefs.h **140 → 32** — all survivors are
     genuine consumers (src/slg/scene/*.cpp, pathoclbase/compile*,
     light strategies, luxcoreimpl).
   - A TU including ONLY scene.h no longer sees material.h/texture.h/
     light.h/sceneobject.h/volume.h/trianglelight.h/lightstrategy.h at
     all (verified with `clang -M` on a minimal TU).
   - scene.h-touch rebuild: 145 tasks both before/after (the TU count
     is unchanged — each TU just compiles less), wall **26.2s → 23.3s
     (−11%)**, user **4m36s → 4m08s (−10%)**.
   - 42 TUs needed explicit defs includes added (8 scene-internal
     files also had `.` → `->` member-access conversions) — including
     latent include bugs this exposed: `lightsourcedefs.cpp` and
     `sceneobjectdefs.cpp` never included their own headers (they
     compiled only because scene.h supplied the class decls).
   `parity-regression.sh`: PASS. No new warnings.
4. **`observer_ptr`/`reference_wrapper` audit.** (NOT implemented) Many
   headers hold `std::reference_wrapper<T>`/`observer_ptr<T>` members
   that only need fwd decls — same pattern as the camera.h fix, but
   applied systematically with IWYU tooling.
5. **Compiler cache — MEASURED, opt-in only.** sccache 0.17 installed
   and benchmarked 2026-09-27: 612/614 compile calls non-cacheable
   ("Can't handle UnknownFlag arguments with -Xclang" — CMake passes
   PCH through `-Xclang -include-pch`, unsupported by sccache). The two
   cacheable TUs were the PCH-less C sources. It also cannot help the
   common incremental case (touched header → new preprocessed hash →
   guaranteed miss); its win would only be clean rebuilds and
   Debug↔Release/branch switches, at the cost of dropping PCH.
   Kept as opt-in: `SUPERLUXCORE_CCACHE=1` makes `luxmake config` pass
   `CMAKE_{C,CXX}_COMPILER_LAUNCHER` when sccache/ccache is on PATH.

## Windows / NVRTC: PTX-vs-SASS (cubin) cold start on RTX 5060 (sm_120)

Measured on a 16-core Windows box, GeForce RTX 5060 (8GB, driver
616.92 / CUDA 13.4 UMD), ~100k-line PathOCL program,
`--use_fast_math`, NVRTC 12.8:

| path | NVRTC compile | load | total first-run | notes |
|---|---|---|---|---|
| PTX (`compute_120`) | ~minutes | driver PTX→SASS JIT ~20min inside `cuModuleLoadDataEx` | ~24min | warm runs hit `%APPDATA%\NVIDIA\ComputeCache` (driver-managed) → ~172ms program compile |
| CUBIN (`sm_120`, `--split-compile=4`) | 485s (~12GB peak, embedded ptxas) | `cuModuleLoadData` ≈ instant | ~8min | 64.6MB cubin on disk; no driver JIT at all |

- `--split-compile=0` (default) makes embedded ptxas OOM-*abort the
  process* on this kernel — no fallback possible, the call never
  returns. `=4` was the sweet spot; higher counts did not help further.
- PTX JIT ~20min is *not* NVRTC compile time — it's the driver
  translating PTX inside `cuModuleLoadDataEx`, and it happens on every
  cold start until the NVIDIA compute cache kicks in. CUBIN output
  bypasses that stage completely and survives driver updates (cache key
  is content-based, not driver-based).
- SASS is therefore the default when RAM ≥ 20GB and NVRTC exposes
  `nvrtcGetCUBIN`; `LUX_CUDA_SASS=0` forces PTX, PTX failure-free
  fallback preserved. OptiX always stays on PTX
  (`optixModuleCreateFromPTX` rejects cubin — `forcePTX` flag on
  `CompilePTX`/`ForcedCompilePTX`).
