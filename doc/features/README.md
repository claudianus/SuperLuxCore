# SuperLuxCore — fork feature documentation

This directory documents every feature added to this fork (LuxCoreRender
2.11.2 -> SuperLuxCore), one self-contained page per feature. Each page states
**what** it is, **why** it is useful, **references** (papers / sources), the
**properties / API** it adds, **test scenes**, **how it was validated**, and
which **platforms** it runs on — the evidence the upstream review guidelines
ask for.

> The fork point is `e9ced7e7a`. The first post-fork batch was rewritten
> into **18 clean, self-contained commits** (one per feature/subsystem, with
> what+why+references in each message). The table below covers those plus
> the ~250 feature commits landed since then on `feature/wavefront-queues`
> (GPU light tracing, ReSTIR DI/GI, volume tracking, guiding M4/M5/P5,
> Vulkan HWRT, vertex connection/merging, Light BVH, AOV stack, .lxm
> out-of-core, Phase S shaders) — these newer commits keep the same
> one-feature-per-commit discipline and are indexed with their real hashes.
> A second squash cycle is planned before upstream submission.

## Feature index

| Feature | Doc | Key commits | Test scene(s) | Platforms |
|---|---|---|---|---|
| Metal backend + HWRT | [metal-backend.md](metal-backend.md) + [../metal_backend_design.md](../metal_backend_design.md) | `562a3bf5f` device+HWRT, `9ffadfb12` cl2msl, `ca83e8ceb` pipeline, `30dc89ab3` film; primitive-AS residency `99513eaf8` | luxball, cornell, lightinstances | **Apple only** |
| ReSTIR DI | [restir-di.md](restir-di.md) | `db4600a18`; visibility target `df93221fe`/`c4068c653`; screen-space merge `79a5f3b8f`; shift `b7b9c56ed`/`a77a32c5e`; clamp `2b2ee77bf`; vis-aware merge `b7b8a66ea` | manylights | CPU/OCL/Metal |
| ReSTIR GI | [../../dev-tools/restir-gi-design.md](../../dev-tools/restir-gi-design.md) | CPU `be6c6d656`; spatial `4fa074565`; GPU `afc202ac6`; e19 10/10 CPU+GPU | cornell | CPU/OCL/Metal |
| MNEE | [mnee.md](mnee.md) | `8363ad339` | causticcube | CPU/OCL/Metal |
| Path guiding | [path-guiding.md](path-guiding.md) | `8687ffc37` | interior | CPU/OCL/Metal |
| Spectral transport | [spectral.md](spectral.md) | `7d8896fc9` | cornell-spectral | CPU/OCL/Metal |
| Samplers | [samplers.md](samplers.md) | `0a62ebb7b` | convergence | CPU/OCL/Metal |
| Blackbody + Whitenoise | [textures.md](textures.md) | `5d38878b6` | cornell/bb-test, whitenoise-* | CPU/OCL/Metal |
| mathfunc texture | [textures.md](textures.md) | `5b7eefe2b` | `dev-tools/e10_mathfunc_test.py` (14 checks) | CPU/OCL/Metal |
| Gabor noise texture | [textures.md](textures.md) | `c8e96ee28` | `dev-tools/e11_gabor_test.py` (14 checks) | CPU/OCL/Metal |
| Hair + Disney | [hair.md](hair.md) + [disney.md](disney.md) | `a8785faa2` | strands, hairmat-test, cornell-disney | CPU/OCL/Metal |
| Strand AOVs | [hair.md](hair.md) | `584fabbd1` | strands, strandu-test | CPU/OCL/Metal |
| Metal native curves | [../dev-tools/metal_curve_design.md](../dev-tools/metal_curve_design.md) | `d32bfe3cd`; float3 packing `b85bfa07a`; gpuAddress residency fix `a728e02bf` | scenes/strands/hair.scn + `dev-tools/e22_metal_curve_test.py` (coverage/parity/indirect gates) | **Apple only** (Metal HWRT) |
| Lights plumbing | [restir-di.md](restir-di.md) | `4e40c8d4a` | manylights | CPU/OCL/Metal |
| Film HW pipeline + OIDN | [oidn-film.md](oidn-film.md) | `30dc89ab3` | any render | OCL/Metal; OIDN=Metal validated* |
| Blender adapter | [blender-adapter.md](blender-adapter.md) | SuperBlendLuxCore repo — motion blur `43dc7674`, `35b47f18`; persistent-scene export (A6-II) `c40f585b` + frame-change fix `c78fb7de` + regression `ee166cdd`, `590cb0ac`; material+geometry deltas (A6-III) `2579a019`, `c28f40f0` | .blend scenes; `SuperBlendLuxCore/dev-tools/a6_persistent_scene_test.py` | all; Metal opt = Apple |
| Wavefront task queues (M1/M2/M3a, opt-in) | [../dev-tools/wavefront-design.md](../dev-tools/wavefront-design.md) | `85a122a1e` cl2msl fix, `9c59522fe` M1, `f424a8552` M2 λ-bucketed queues, `88f425ffe` M3a device prefix (branch `feature/wavefront-queues`) | cornell, cornell-spectral | OCL/Metal; `LUXRAYS_WAVEFRONT_QUEUES=1` |
| Deformation motion blur (E9, scoped) | [../dev-tools/deformation-motion-blur-design.md](../dev-tools/deformation-motion-blur-design.md) | `bc26dbd45` design doc; `5b1e23fa1` plumbing; `d4dc25e22` Metal HWRT; `d1eb37198` SW MBVH; `c16e19453` Embree; SuperBlendLuxCore `c9193f0a` export | `vertexmotion_test`; `dev-tools/e9_*_vertex_motion_test.py`; SuperBlendLuxCore `dev-tools/e9_vertex_motion_e2e_test.py` | Metal HWRT + SW MBVH (OCL/cl2msl) + Embree timesteps; mesh export (hair pending) |
| Adaptive caustic partition | [adaptive-caustic.md](adaptive-caustic.md) | `f9954cbc1` | `scenes/cornell/caustic-roughglass.scn`; `dev-tools/e25_adaptive_caustic_test.py`, `e25_render_compare.py`, `e25_caustic_channel.py` | CPU/OCL/Metal |
| GPU light tracing + LMNEE/MGE | [gpu_lighttracing.md](gpu_lighttracing.md) | `2fce509e1` tasks, `09081cc54` Emit, `41a9e03f5` camera, `e76d03298` state machine, `8f90fe099` splat, `23c56e61d` LMNEE, `7dfecaaa3` MGE | `dev-tools/e23_*` LT parity; cornell | OCL/Metal |
| Path guiding M4a–P5 | [path-guiding.md](path-guiding.md) + [path-guiding-m4-design.md](path-guiding-m4-design.md) | `bc11fb8bc` SD-tree, `9816e5834` RIS, `25b9e04f9` portal, `6308bb7e1` GPU field, `9d675aa02` GPU RIS, `065623c8e` GPU portal, `aafe53bfd` P5 (`path.guiding.*` + BIC-K + fallback) | `dev-tools/e26_*`, `e27_*`, `e43_*` | CPU/OCL/Metal |
| Volume tracking (E6) | [volume-tracking.md](volume-tracking.md) | `a1d665d43` null-collision, `99b90a188` residual, `62b02cc21` minorant, `7aa868bbf`/`52011710d` equiangular MIS, `6501b098b` HG, `6cebebadb` contribution-aware, `3663a4444` flight | `dev-tools/e34_volume_guiding_parity.py`, `pyunittests/pysuperluxcoreunittests/tests/materials/testvolumetracking.py` | CPU/OCL/Metal |
| Light BVH strategy | `../../dev-tools/lightstrategy-audit.md` + [../engineering/light-bvh.md](../engineering/light-bvh.md) | `8beac5718` (`lightstrategy.type=LIGHT_BVH`, E&K'18) | `dev-tools/e26_lightbvh_test.py`, e37 audit | CPU/OCL/Metal |
| GPU vertex connection (M6) | [gpu_vertexconnection.md](gpu_vertexconnection.md) | `4badd28c3` | `dev-tools/e38_vc_smoke.py` | OCL/Metal |
| GPU vertex merging (M7) | [gpu_vertexconnection.md](gpu_vertexconnection.md) | `d41d53fb2`, scale fix `6f3dbcddb`, radius `50cd9f817` | `dev-tools/e38_*`, `e39_bidir_focus.py` | OCL/Metal |
| Vulkan backend + HWRT | [vulkan-backend-assessment.md](vulkan-backend-assessment.md) + `../../dev-tools/vkrt/` | M0/M1 spike; M2 `da59a1d13` BLAS/TLAS+ray_query, `5c8547418` maxVertex, `fc0625d14` AS rebuild, `76a132516` pipeline cache | `dev-tools/vk_intersect_test.cpp` (C++ target), `vk_rt_update_test.py`, `vulkan-regression.sh --full` (720p PATHOCL render) | MoltenVK (experimental — native drivers unvalidated) |
| Cryptomatte AOV | [../engineering/cryptomatte.md](../engineering/cryptomatte.md) | `eedefb2dc` (`CRYPTOMATTE_OBJECT`/`_MATERIAL`, MurmurHash3 manifest) | `dev-tools/e40_cryptomatte_test.py` + visual | CPU/OCL/Metal |
| LPE AOV | [../engineering/lpe.md](../engineering/lpe.md) | `283dd3ee0` (`film.lpe.N.expression`, bounded NFA ≤8) | `dev-tools/e42_lpe_test.py` + `e42_lpe_visual.py` | PATHCPU/PATHOCL only (no BIDIR/LT) |
| Adaptive robust clamping | [../engineering/adaptive-clamping.md](../engineering/adaptive-clamping.md) | `b7b78178d` (`path.clamping.variance.*`) | `dev-tools/e42_adaptive_clamp_test.py` | CPU/OCL/Metal |
| Light linking | [../engineering/light-linking.md](../engineering/light-linking.md) | `c5d529195` 64-bit groups, `1a811b64e` DuplicateObject fix | `dev-tools/e41_lightlink_test.py` | CPU/OCL/Metal |
| VARIANCE / MOTION_VECTOR / temporal accumulate | [aov-audit-temporal-denoise.md](aov-audit-temporal-denoise.md) | `74aa37f11` channels, `0ef11800d` TA, `5f8502d51` components | `dev-tools/e2x` film tests | CPU/OCL/Metal |
| GGX opt-in (S0) + Heitz'16 | [ggx-multibounce.md](ggx-multibounce.md) | `554fce7d3` metal2 MB + 6-material `distribution=ggx` series | `dev-tools/e33_glossy2_ggx_parity.py`, `scenes/ggxoptin/` | CPU/OCL/Metal |
| OpenPBR (S1) | [disney.md](disney.md) / material defs | OpenPBR material + parser + BLC node; Metal fix `2274d641b` | `dev-tools/e31_openpbr_parity.py` | CPU/OCL/Metal |
| Random-walk SSS (S2/E35) | material defs / [../engineering/openpbr-sss-findings.md](../engineering/openpbr-sss-findings.md) | `26340ee73` albedo/mfp | `dev-tools/e35_sss_albedo.py`, `e35_visual_demo.py` | CPU/OCL/Metal |
| Huang hair (S3) | [hair.md](hair.md) | `f1935c607` Chiang/Huang model select | `dev-tools/e36_hair_huang.py`, `e36_visual_demo.py` | CPU/OCL/Metal |
| .lxm proxy + scene.spill out-of-core | [../engineering/out-of-core.md](../engineering/out-of-core.md) | `b2c383d56` v1, `addcf1feb` cluster idx, `dd00ac0c0` v4, `99d62338a` stride, `25a24e38b` residency pool, spill `8abe2f9d7`+ | SuperBlendLuxCore `dev-tools/lxm*_test.py`, `cluster_residency_test.py`, `imagemap_stream_test.py`, `*spill*_test.py` | all (POSIX+Win COW) |
| Diffraction grating (CD rainbow) | [diffraction.md](diffraction.md) | this branch | `dev-tools/e43_diffraction_test.py` (T1–T4), `scenes/diffraction/cd-rainbow.scn` | CPU/OCL/Metal |

> Engine/API plumbing and misc integration: `06b8b826d`, `8601eaa12`.
> Example scenes: `64aad5c47`. This documentation: `481fea0d2`.
> Maintenance: `45310f556` ExtMeshProp sizeless-layer fix (E7 fallback
> regression), `3925d3248` robin_hood→tsl::robin_map, `861a5ea24` dead
> vendored assets (~47MB). Regression automation:
> `dev-tools/wavefront-regression.sh` (`abfe20a23`, strands_hair case
> `f89210cf5`) and `dev-tools/parity-regression.sh` (`03dc67b74`, CPU/GPU
> centre-value gate for scenes/parity).

## How the criteria are met

| Upstream criterion | Where |
|---|---|
| Descriptions + references (not hallucination) | Each feature doc has a *What/why* + *References* section. |
| Documentation (API / feature docs) | The *Properties* and *Implementation* sections list new SDL properties, materials, textures, node mappings. |
| Test / example scenes | `scenes/` — committed per feature (see below). |
| Demonstrate correctness | *Validation* sections: parity vs CPU, vs the previous path, convergence checks — not just a screenshot. |
| Platform compatibility | *Platforms* row per feature; Metal is explicitly Apple-only with CPU/OpenCL fallbacks. |
| Buildable on pipelines | See *Build / CI* below. |
| Clear chunks per feature | Each feature is a set of scoped commits; the index maps them. |

## Test / example scenes

| Scene | Exercises |
|---|---|
| `scenes/cornell/bb-test.scn` | blackbody textured temperature -> volume fire emission |
| `scenes/cornell/whitenoise-test.scn`, `whitenoise-float.scn` | whitenoise colour + scalar output |
| `scenes/cornell/hairmat-test.scn` | hairmat BSDF |
| `scenes/strands/strandu-test.scn`, `scenes/strands/hair.scn` | strand-u / strand-random AOVs |
| `scenes/cornell/fire-test.scn` | volume fire emission (Planck) |
| `scenes/cornell/cornell-spectral*.scn` | spectral hero-λ transport |
| `scenes/cornell/cornell-disney.scn` | Disney transmission |
| `scenes/manylights/scene.scn` | ReSTIR DI many-light direct illumination |
| `scenes/causticcube/` | MNEE specular caustics |
| `scenes/media/vol-*.scn` | OpenVDB heterogeneous volumes |

The new example scenes ship a matching `.cfg` render config. Reference
scenes (upstream `scenes/`) render with the standard config and the
feature enabled via a property — e.g. `scenes/manylights/scene-restir.cfg`
sets `light.strategy.type = RESTIR_DI`.

## Build / CI

The upstream GitHub workflows (`.github/workflows/sample-builder.yml`,
`wheel-builder.yml`, `sample-releaser.yml`, `wheel-releaser.yml`,
`wheel-publisher.yml`) are unchanged. Notes:

- **Metal code is Apple-only** and fully compiled out elsewhere: the CMake
  `if(APPLE)` block only builds `luxrays_metalobj` on macOS, and every Metal
  call site is behind `#if defined(__APPLE__) && !defined(LUXRAYS_DISABLE_METAL)`
  in `src/luxrays/core/context.cpp`. Linux/Windows pipelines build the
  portable CPU + OpenCL paths unaffected; `LUXRAYS_DISABLE_METAL` is an
  opt-out even on Apple.
- ~~**Known portability fragility**~~ — fixed in `268475781`:
  `luxrays_metalobj` now links the Conan imported targets
  (`OpenImageIO::OpenImageIO`, `OpenEXR::OpenEXR`, `Imath::Imath`,
  `TBB::tbb`) instead of hardcoded `armv8`/`Release` `full_deploy`
  paths, so dependency version/arch/config bumps no longer break the
  Metal object build. Verified Release + Debug.
- cl2msl-translated kernels are C99-compatible (no C++/STL in `.cl`), keeping
  both the OpenCL and Metal compilations working.
- A regression/benchmark matrix over scene x engine x feature is a roadmap
  item; until then each feature's *Validation* section records the manual
  evidence.

## Honest status / known gaps

- **ReSTIR GI** runs on CPU and GPU (PATHOCL/TILEPATHOCL, OpenCL +
  Metal): first-bounce reservoir, temporal + gated spatial reuse, and
  the seqlock/vSeq concurrency guards the GPU merge needs — e19 10/10
  (CPU+GPU). ReSTIR PT/PG remain open — opt-in, roadmap E2.
- **OIDN Metal** ships in the public SuperLuxCoreDeps `v2.4.0` bundle
  (`with_device_metal=True`, `device_metal` dylib verified) and is
  wired into the `INTEL_OIDN` pipeline (~17× over CPU).
  + dep release rebuild. See `dev-tools/oidn-metal/` — roadmap E1.
- **Metal curves**: native Catmull-Rom primitives now replace the hair
  tessellation on the Metal HWRT path (commit `d32bfe3cd`, gated on
  macOS 14+ + `LUXCORE_METAL_CURVES`). Known v1 limits: curve meshes as
  triangle lights mis-map in the hit→light reverse lookup; strand AOV
  parity vs the tessellated baseline is approximate by design.
- **PATHCPU-vs-PATHOCL brightness difference** on bright emissives — root
  cause found and fixed in commit `10ecf93ec`. It was not an emission
  evaluator bug but a **Metal HWRT static-scene intersection leak**: the
  timed `intersect(ray, as, time)` overload returns no intersection on a
  non-motion instance AS, so PATHOCL on Metal leaked camera rays to the
  environment (~62% on the minimal parity scene). Fixed by gating the timed
  overload behind `useMotionTime`. CPU/OpenCL were always correct.
  Regression scenes: `scenes/parity/` (emissive-direct → `(4,4,4)`,
  whiteenv → `(0,0,0)`, both exact on Metal now). Any residual CPU↔GPU
  delta on complex scenes should be re-measured post-fix.
- **Wavefront queues (B2/E3 M1/M2/M3a)**: opt-in per-state task queues for the
  PATHOCL micro-kernel state machine (`LUXRAYS_WAVEFRONT_QUEUES=1`).
  Validated on OpenCL + Metal vs dense (Monte-Carlo-noise-level parity,
  queue integrity clean, SOBOL/RANDOM/METROPOLIS). M3a (`88f425ffe`)
  moved the queue prefix on-device — measured ~2.5x over dense on the
  cornell 720²/30s Metal benchmark, reversing the earlier dense-wins
  verdict on that scene. The per-iteration totals readback (72B) is
  still required for launch sizing (stale sizing measured ~6x worse).
  Dense stays default; re-benchmark on more workloads before promoting.
  See `dev-tools/wavefront-design.md` §M3a.
- Opt-in / experimental stages are **default-off** until regression coverage
  lands.
