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
