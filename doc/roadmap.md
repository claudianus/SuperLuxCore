# SuperLuxCore roadmap

> **Historical snapshot (2026-09-24/25 era).** The canonical, continuously
> updated roadmap is the workspace `ROADMAP.md` (one level above this repo).
> This file is kept as a feature-level snapshot — entries below marked
> "(superseded)" have since landed or changed. Do not treat it as the
> live plan.

Independent LuxCoreRender 2.11.2 fork targeting a production-grade,
GPU-first spectral renderer. Primary backend: Apple Metal HWRT; first
adapter: SuperBlendLuxCore on current Blender LTS. Working rules: every
feature lands as reviewable, documented, tested commits (see
`features/README.md`); CPU/GPU parity where practical; conservative
claims backed by measured evidence.

## Done

| Track | Scope | State |
|---|---|---|
| Metal backend | Device + HWRT, cl2msl kernel translation, pipeline, HW film + OIDN, native curves | shipped (Apple-only; CPU/OCL fallbacks intact) |
| ReSTIR DI / MNEE / path guiding / spectral / samplers | see `features/README.md` index | shipped, CPU/OCL/Metal; GPU ReSTIR visibility-weighted target + MNEE manifold seed cache (validated: 92% hit, −9.5% Newton iters/solve, unbiased; e17 rewritten non-vacuous — old scenes set transparency.shadow=1 so MNEE never ran) |
| Wavefront queues (M1+M2+M3a) | per-state task queues + λ-bucketed queues + device-side QueuePrefix, opt-in `LUXRAYS_WAVEFRONT_QUEUES=1` | M3a (`37cc04b4b`) measured ~2.5x on cornell 720²/30s Metal; dense stays default pending wider re-benchmarks (see below) |
| DEP-1/DEP-2 | deps refresh (openvdb 13, robin-hood removal), v2.3.0/v2.4.0 dep releases | done, CI green |
| A6-II/A6-III | persistent-scene incremental export, transform/material/geometry deltas, dupli-set refresh | done; `a6_persistent_scene_test.py` all PASS |
| A5 | dupli/particle + point-cloud transform motion blur | done |
| E9 deformation motion blur | vertex-motion series plumbing (Ph1), Metal HWRT descriptors + motion intersector fix (Ph2), swept-bound SW MBVH/OCL path (Ph3), Embree timesteps (Ph4), SuperBlendLuxCore mesh export (Ph5), strand/hair control-point motion incl. Metal motion-curve AS + Blender adapter (Ph5b) | done; `e9_parity_test.py` 4-backend parity + `e9_strand_motion_test.py`/`e9_strand_motion_e2e_test.py` PASS |
| E9 Ph6 | Validation scenes | done — GN-deformed mesh (`e9_gn_vertex_motion_e2e_test.py`), particle-hair (`e9_psys_strand_motion_e2e_test.py`), topology-change fallback (`e9_topology_change_test.py`) all PASS |
| GPU light tracing + LMNEE | PATHOCL/RTPATHOCL light-path splatting (dense + wavefront), caustic focus cache, light→camera manifold NEE for delta occluders incl. multi-interface chains (glass slab/lens, mirror) with correct camera endpoint weighting | shipped; `doc/features/gpu_lighttracing.md`, `dev-tools/lighttracing-depth-parity.sh`. Fixed a deep-path deficit where the hybridBackForward diffuse cut wrongly truncated light paths in `lighttracing.only` mode (eyeTaskCount==0) — now depth-1..4 == `LIGHTCPU` within 0.03% |
| MGE (manifold-guided emission) | records each solved-manifold camera connect's receiver into the per-light focus ring and steers emission toward those camera-productive points; per-entry aim radius (tight portals / broad receivers); extends guided emission to triangle area emitters (joint position×direction pdf) | shipped; `mge-recvonly.scn` pure-receiver test cuts receiver-region variance to ~0.39× unguided; mixture pdf keeps all emitter types unbiased |

## In flight / next

| Track | Item | Notes |
|---|---|---|
| **P0 — user-escalated** | Viewport interactivity, GPU caustics (light tracing+splat), spectral fidelity (JH2019), Cycles .blend direct-render fidelity | mid-review escalation; see workspace `ROADMAP.md` Phase P0 |
| E9 leftovers | OptiX/CUDA motion geometry refresh | `OptixMotionGeometryDesc` vertex buffers; out of scope until the CUDA path is revived |
| E2 | ReSTIR PT/GI/PG + RIS visibility term | E2a: visibility-weighted target on GPU + CPU (GPU candidate shadow-ray tail + MK_RT_RESTIR resolve, exact-sample reuse; CPU: inline accelerator trace + winner-sample return through `SampleLightsBSDF`, own-cell merge only under vis; `restir.visibility.enable`; e16 8/8 Metal, e18 8/8 CPU, no wedge at 512K tasks). E2b: GPU spatial reuse moved to screen-space neighbour-pixel merge with same-surface gate — replaced the world-space hash grid that merged unrelated surfaces (e14 6/6; spots RMSE ~1.0x vs ~1.3x before); pre-spatial store + representative-winner gate fixed the merge-feedback explosion. E2c: reconnection shift — reservoirs store the winning sample's light-surface draws (`lsU/lsV/lsP`) and neighbour merges replay that same emitter point, so pi_new/pi_old tracks only the shading change (e14 6/6, e16 8/8). E2d: visibility-aware spatial merge — under the visibility target the 2 merge-candidate rays ride the same candidate tail trace, folding real V into pi_new (replaces the V-free approximation; e16 8/8, no wedge at 512K tasks). G1 GI (CPU): `path.restir.gi.*` — per-pixel first-bounce reservoir, K BSDF candidates with proxy target `f·cos·(L̂+eps)` (one-NEE L̂ + 5% support floor — restores unbiasedness where the binary-V probe reports 0 on lit geometry), temporal merge with Jacobian-corrected reconnection shift + binary-V test, and G1-b pixel-neighbour spatial reuse (same-surface gate + pre-spatial store), RIS weight `W = wSum/(M·π̂)` on the continuation throughput. G2 (GPU): same estimator on PATHOCL/TILEPATHOCL via a 2K+1-slot tail (candidate bounce + NEE + temporal-visibility rays) and MK_RT_GI_BOUNCE/RESOLVE states; reservoir merges use a seqlock pass stamp (INVALID-first publish) + vSeq visibility-ray pairing + representative-winner gate — unsynchronized reads compounded wSum into ~1e15x hot pixels on Metal (OpenCL ordering masked the same window). e19 10/10 CPU+GPU (unbiased, bounded RMSE, no-explosion tripwire). Still DI+GI — ReSTIR PT/PG remain the larger follow-ups |
| E1 | OIDN Metal into dep bundle | done: `with_device_metal=True` + `metal_embed_source` in SuperLuxCoreDeps `conan-profile-macOS-ARM64`, `oidn-2.5.1-metal-runtime-compile.patch`, `device_metal` dylib used by `intel_oidn.cpp` (Metal device preferred, CPU fallback). Published: `claudianus/SuperLuxCoreDeps` release `v2.4.0` (pinned by `build-settings.json`) ships `libLuxOpenImageDenoise_device_metal.2.5.1.dylib` in the macOS-ARM64 bundle — verified in the public asset |
| Wavefront M3 | material bucketing | **(superseded)** — M3a device-side QueuePrefix landed (`37cc04b4b`); cornell 720²/30s Metal dense ~9.5M vs wavefront ~24M samples/s (~2.5x). The −7~−17% verdict below was pre-M3a. Dense stays default pending wider workload re-benchmarks; see `dev-tools/wavefront-design.md` |
| Blender UX | V-Ray/Corona-level polish | persistent-scene cache + deltas, auto light strategy/clamp/device, low-VRAM profile (`opencl.task.count` cap), quality presets, ReSTIR visibility toggle, convergence stat row (incl. PATHOCL via `batch.haltthreshold`) all landed; remaining: incremental polish |
| Compatibility | Cycles shader-node / Geometry Nodes coverage | audited vs Blender 5.2.1 (97 node branches); Math/VectorMath nearly complete via `mathfunc` (trig/exp/log/hyperbolic/invsqrt/floormod + smooth-min/max); BsdfHair/RayPortal/PointInfo/VectorRotate/VectorTransform/EeveeSpecular/Squeeze/Gabor mapped (native `gabornoise` texture); IES light nodes map to mappoint/mapsphere iesblob (parity-tested vs native IES path); residual gaps are scene-query nodes (Raycast/CameraData/LightFalloff/Script) — warn+neutral fallback, see SuperBlendLuxCore `doc/cycles_node_coverage.md` |

## Standing gaps (honest list)

- ~~**MNEE only handles delta *point* lights.**~~ **(superseded)** — the
  directional/distant endpoint landed (`038414538`/`038414538`): constant-`wo`
  sun endpoint + direction-space Jacobian, so directional lights go through
  the manifold solve. Remaining true gap: `TYPE_TRIANGLE` area emitters and
  environment lights still bypass the manifold path (MGE focus cache +
  guided emission cover part of that need).
- Metal is Apple-only by design; OpenCL SW path is the cross-vendor
  fallback. CUDA/OptiX support is stale (post-E8 codepaths untested).

- ~~Critical bug found 2026-10 (PATHOCL texture eval corruption)~~ —
  **fixed 2026-10.** Root cause was not texture evaluation: PATHOCL's
  bucketed samplers (RANDOM/SOBOL/PMJ02) assigned buckets via an atomic
  counter and every task sharing a bucket swept `pixelOffset`
  0..bucketSize-1 in lockstep, so each sample wave covered only
  `bucketCount` morton-clustered positions. With the default task count
  (≈512K) far exceeding small film pixel counts and `batch.haltspp`
  halting after a few waves, ~3/4 of pixels received zero samples —
  a deterministic `..##` mask (dark pixels had RAYCOUNT=0).
  Fix: per-task staggered cyclic bucket sweep (`bucketCycleStart`, see
  `include/slg/samplers/sampler_types.cl`); wave-1 coverage now spreads
  across the film while every visited bucket is still swept completely.
  Verified on dense+wavefront PATHOCL, RANDOM/SOBOL/PMJ02, 64²/32×16/
  256², 6+ repeats — `dev-tools/e12_pathocl_eval_corruption.py` is the
  regression test. Also fixed: `atan2(0,0)` in the Gabor phase output
  returned garbage on Metal fast-math paths (explicit guard added).
- Non-uniform motion step times are exact on MBVH/BVH/SW-OpenCL and
  approximated piecewise-uniformly on Metal HWRT and Embree.
- `PATHOCL` + `SOBOL` black frames were reported once in an old build;
  re-verified 2026-09 on cornell.scn and a minimal emissive scene —
  output matches `PATHOCL+RANDOM` and `TILEPATHOCL`+`TILEPATHSAMPLER`
  within sampling noise. Considered resolved; keep an eye on it if a
  scene-specific reproducer shows up.
