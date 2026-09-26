# SuperLuxCore — fork feature documentation

This directory documents every feature added to this fork (LuxCoreRender
2.11.2 -> SuperLuxCore), one self-contained page per feature. Each page states
**what** it is, **why** it is useful, **references** (papers / sources), the
**properties / API** it adds, **test scenes**, **how it was validated**, and
which **platforms** it runs on — the evidence the upstream review guidelines
ask for.

> The fork point is `e9ced7e7a`. The first post-fork batch was rewritten
> into **18 clean, self-contained commits** (one per feature/subsystem, with
> what+why+references in each message). On 2026-09-27 a **second squash
> cycle** rebuilt the ~270 granular commits after them into **52 themed
> commits** covering the 9/19–9/26 landing wave (GPU light tracing, ReSTIR
> DI/GI, volume tracking, guiding M4/M5/P5, Vulkan HWRT, vertex
> connection/merging, Light BVH, AOV stack, .lxm out-of-core, Phase S
> shaders) — each squashed commit message lists its constituent original
> commits, and its tree is the real end-of-group tree (final tip verified
> bit-identical to the pre-rewrite branch, kept locally as
> `backup/pre-rewrite-2`). Hashes below refer to the rewritten history.

## Feature index

| Feature | Doc | Key commits | Test scene(s) | Platforms |
|---|---|---|---|---|
| Metal backend + HWRT | [metal-backend.md](metal-backend.md) + [../metal_backend_design.md](../metal_backend_design.md) | `562a3bf5f` device+HWRT, `9ffadfb12` cl2msl, `ca83e8ceb` pipeline, `30dc89ab3` film; primitive-AS residency `fd3f6047146d89e5eb57976f52b2977ea877dff9` | luxball, cornell, lightinstances | **Apple only** |
| ReSTIR DI | [restir-di.md](restir-di.md) | `db4600a18`; visibility target `b349776cb82259f6dcaef00633395ae4ba8f5512`/`b349776cb82259f6dcaef00633395ae4ba8f5512`; screen-space merge `b349776cb82259f6dcaef00633395ae4ba8f5512`; shift `b349776cb82259f6dcaef00633395ae4ba8f5512`/`b349776cb82259f6dcaef00633395ae4ba8f5512`; clamp `b349776cb82259f6dcaef00633395ae4ba8f5512`; vis-aware merge `b349776cb82259f6dcaef00633395ae4ba8f5512` | manylights | CPU/OCL/Metal |
| ReSTIR GI | [../../dev-tools/restir-gi-design.md](../../dev-tools/restir-gi-design.md) | CPU `10ed0383e3f5b2af9eb5748659f54769e7fa6bfe`; spatial `10ed0383e3f5b2af9eb5748659f54769e7fa6bfe`; GPU `10ed0383e3f5b2af9eb5748659f54769e7fa6bfe`; e19 10/10 CPU+GPU | cornell | CPU/OCL/Metal |
| MNEE | [mnee.md](mnee.md) | `8363ad339` | causticcube | CPU/OCL/Metal |
| Path guiding | [path-guiding.md](path-guiding.md) | `8687ffc37` | interior | CPU/OCL/Metal |
| Spectral transport | [spectral.md](spectral.md) | `7d8896fc9` | cornell-spectral | CPU/OCL/Metal |
| Samplers | [samplers.md](samplers.md) | `0a62ebb7b` | convergence | CPU/OCL/Metal |
| Blackbody + Whitenoise | [textures.md](textures.md) | `5d38878b6` | cornell/bb-test, whitenoise-* | CPU/OCL/Metal |
| mathfunc texture | [textures.md](textures.md) | `13afd4f29610461a758589d015db855b7824d620` | `dev-tools/e10_mathfunc_test.py` (14 checks) | CPU/OCL/Metal |
| Gabor noise texture | [textures.md](textures.md) | `c34efab7109c63d2ce13ed62e9d9f400680d8e21` | `dev-tools/e11_gabor_test.py` (14 checks) | CPU/OCL/Metal |
| Hair + Disney | [hair.md](hair.md) + [disney.md](disney.md) | `a8785faa2` | strands, hairmat-test, cornell-disney | CPU/OCL/Metal |
| Strand AOVs | [hair.md](hair.md) | `584fabbd1` | strands, strandu-test | CPU/OCL/Metal |
| Metal native curves | [../dev-tools/metal_curve_design.md](../dev-tools/metal_curve_design.md) | `4a90e7057573da31442615f36fa213c53ec3d9bd`; float3 packing `fd3f6047146d89e5eb57976f52b2977ea877dff9`; gpuAddress residency fix `fd3f6047146d89e5eb57976f52b2977ea877dff9` | scenes/strands/hair.scn + `dev-tools/e22_metal_curve_test.py` (coverage/parity/indirect gates) | **Apple only** (Metal HWRT) |
| Lights plumbing | [restir-di.md](restir-di.md) | `4e40c8d4a` | manylights | CPU/OCL/Metal |
| Film HW pipeline + OIDN | [oidn-film.md](oidn-film.md) | `30dc89ab3` | any render | OCL/Metal; OIDN=Metal validated* |
| Blender adapter | [blender-adapter.md](blender-adapter.md) | SuperBlendLuxCore repo — motion blur `43dc7674`, `35b47f18`; persistent-scene export (A6-II) `c40f585b` + frame-change fix `c78fb7de` + regression `ee166cdd`, `590cb0ac`; material+geometry deltas (A6-III) `2579a019`, `c28f40f0` | .blend scenes; `SuperBlendLuxCore/dev-tools/a6_persistent_scene_test.py` | all; Metal opt = Apple |
| Wavefront task queues (M1/M2/M3a, opt-in) | [../dev-tools/wavefront-design.md](../dev-tools/wavefront-design.md) | `ae418f869dbf49168410de865b79c992cf748760` cl2msl fix, `ae418f869dbf49168410de865b79c992cf748760` M1, `ae418f869dbf49168410de865b79c992cf748760` M2 λ-bucketed queues, `37cc04b4baa4b492334e8391a2fd2b0a4534e815` M3a device prefix (branch `feature/wavefront-queues`) | cornell, cornell-spectral | OCL/Metal; `LUXRAYS_WAVEFRONT_QUEUES=1` |
| Deformation motion blur (E9, scoped) | [../dev-tools/deformation-motion-blur-design.md](../dev-tools/deformation-motion-blur-design.md) | `cf0db726245c23a186d09f5ff70c4db65fee0051` design doc; `25472b8a50a2c925c6c44b62719a5f428339bf4d` plumbing; `25472b8a50a2c925c6c44b62719a5f428339bf4d` Metal HWRT; `25472b8a50a2c925c6c44b62719a5f428339bf4d` SW MBVH; `25472b8a50a2c925c6c44b62719a5f428339bf4d` Embree; SuperBlendLuxCore `c9193f0a` export | `vertexmotion_test`; `dev-tools/e9_*_vertex_motion_test.py`; SuperBlendLuxCore `dev-tools/e9_vertex_motion_e2e_test.py` | Metal HWRT + SW MBVH (OCL/cl2msl) + Embree timesteps; mesh export (hair pending) |
| Adaptive caustic partition | [adaptive-caustic.md](adaptive-caustic.md) | `038414538411bab9e404da0464cb7a8e39126346` | `scenes/cornell/caustic-roughglass.scn`; `dev-tools/e25_adaptive_caustic_test.py`, `e25_render_compare.py`, `e25_caustic_channel.py` | CPU/OCL/Metal |
| GPU light tracing + LMNEE/MGE | [gpu_lighttracing.md](gpu_lighttracing.md) | `45caf34594bf414508cfbd01d9f90f4df5becd51` tasks, `45caf34594bf414508cfbd01d9f90f4df5becd51` Emit, `45caf34594bf414508cfbd01d9f90f4df5becd51` camera, `45caf34594bf414508cfbd01d9f90f4df5becd51` state machine, `038414538411bab9e404da0464cb7a8e39126346` splat, `45caf34594bf414508cfbd01d9f90f4df5becd51` LMNEE, `038414538411bab9e404da0464cb7a8e39126346` MGE, `345431a45c794de4d25b19bc67f2d78458d8ef3d` lt-only natives | `dev-tools/e23_*` LT parity; `e45_lighttracing_only_test.py`; cornell | OCL/Metal |
| Path guiding M4a–P5 | [path-guiding.md](path-guiding.md) + [path-guiding-m4-design.md](path-guiding-m4-design.md) | `065f895d771fdf7a287a42b5601a62e770fc7743` SD-tree, `065f895d771fdf7a287a42b5601a62e770fc7743` RIS, `065f895d771fdf7a287a42b5601a62e770fc7743` portal, `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a` GPU field, `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a` GPU RIS, `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a` GPU portal, `00804937b0f880ff2421a497591a27a6e2a531ba` P5 (`path.guiding.*` + BIC-K + fallback) | `dev-tools/e26_*`, `e27_*`, `e43_*` | CPU/OCL/Metal |
| Volume tracking (E6) | [volume-tracking.md](volume-tracking.md) | `ba793a22faccfe31fbe8b4606f6f2092b85da3dc` null-collision, `ba793a22faccfe31fbe8b4606f6f2092b85da3dc` residual, `ba793a22faccfe31fbe8b4606f6f2092b85da3dc` minorant, `ba793a22faccfe31fbe8b4606f6f2092b85da3dc`/`ba793a22faccfe31fbe8b4606f6f2092b85da3dc` equiangular MIS, `ba793a22faccfe31fbe8b4606f6f2092b85da3dc` HG, `ba793a22faccfe31fbe8b4606f6f2092b85da3dc` contribution-aware, `632e8928e5b2887d1220f806c8a5c2d6609beba5` flight | `dev-tools/e34_volume_guiding_parity.py`, `pyunittests/pysuperluxcoreunittests/tests/materials/testvolumetracking.py` | CPU/OCL/Metal |
| Light BVH strategy | `../../dev-tools/lightstrategy-audit.md` + [../engineering/light-bvh.md](../engineering/light-bvh.md) | `6c0006bc4ab8fc8a6139873ba6160812290b448d` (`lightstrategy.type=LIGHT_BVH`, E&K'18) | `dev-tools/e26_lightbvh_test.py`, e37 audit | CPU/OCL/Metal |
| GPU vertex connection (M6) | [gpu_vertexconnection.md](gpu_vertexconnection.md) | `bc63643146f7d9a7a916125c0d33057acb862c86`, MIS lift gate `345431a45c794de4d25b19bc67f2d78458d8ef3d` | `dev-tools/e38_vc_smoke.py`, `e44_vc_shadowtransparency_test.py` | OCL/Metal |
| GPU vertex merging (M7) | [gpu_vertexconnection.md](gpu_vertexconnection.md) | `b780648e47854f103980f4f634c6eb27bbf47cd7`, scale fix `d4a5282f3bb85d8f1e95782f1ab8b14eb1e73e1f`, radius `d4a5282f3bb85d8f1e95782f1ab8b14eb1e73e1f` | `dev-tools/e38_*`, `e39_bidir_focus.py` | OCL/Metal |
| Vulkan backend + HWRT | [vulkan-backend-assessment.md](vulkan-backend-assessment.md) + `../../dev-tools/vkrt/` | M0/M1 spike; M2 `ef7a4ca6054e3ab2403e5261865748c866570f44` BLAS/TLAS+ray_query, `13208166ff248a7249f513d9a74c83242d4d6509` maxVertex, `ef7a4ca6054e3ab2403e5261865748c866570f44` AS rebuild, `bfc37876b0ce54827d49746a13cb94107860a3b8` pipeline cache | `dev-tools/vk_intersect_test.cpp` (C++ target), `vk_rt_update_test.py`, `vulkan-regression.sh --full` (720p PATHOCL render) | MoltenVK (experimental — native drivers unvalidated) |
| Cryptomatte AOV | [../engineering/cryptomatte.md](../engineering/cryptomatte.md) | `aabfdb3e5fbe98e75617309a30920ca27f0b8f7a` (`CRYPTOMATTE_OBJECT`/`_MATERIAL`, MurmurHash3 manifest) | `dev-tools/e40_cryptomatte_test.py` + visual | CPU/OCL/Metal |
| LPE AOV | [../engineering/lpe.md](../engineering/lpe.md) | `7a1622a86dfefb044b17c7d4f3f332bee69b668f` (`film.lpe.N.expression`, bounded NFA ≤8) | `dev-tools/e42_lpe_test.py` + `e42_lpe_visual.py` | PATHCPU/PATHOCL only (no BIDIR/LT) |
| Adaptive robust clamping | [../engineering/adaptive-clamping.md](../engineering/adaptive-clamping.md) | `8d05a4ef1793ca9ccc98549dae78af2156bbe078` (`path.clamping.variance.*`) | `dev-tools/e42_adaptive_clamp_test.py` | CPU/OCL/Metal |
| Light linking | [../engineering/light-linking.md](../engineering/light-linking.md) | `8d05a4ef1793ca9ccc98549dae78af2156bbe078` 64-bit groups, `8d05a4ef1793ca9ccc98549dae78af2156bbe078` DuplicateObject fix | `dev-tools/e41_lightlink_test.py` | CPU/OCL/Metal |
| VARIANCE / MOTION_VECTOR / temporal accumulate | [aov-audit-temporal-denoise.md](aov-audit-temporal-denoise.md) | `a7c3ae282f0eea9ba80c3191e1175da34c28b7b9` channels, `17b020b312e0c2aaf92c119dab53ef1c6073acbf` TA, `17b020b312e0c2aaf92c119dab53ef1c6073acbf` components | `dev-tools/e2x` film tests | CPU/OCL/Metal |
| GGX opt-in (S0) + Heitz'16 | [ggx-multibounce.md](ggx-multibounce.md) | `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a` metal2 MB + 6-material `distribution=ggx` series | `dev-tools/e33_glossy2_ggx_parity.py`, `scenes/ggxoptin/` | CPU/OCL/Metal |
| OpenPBR (S1) | [disney.md](disney.md) / material defs | OpenPBR material + parser + BLC node; Metal fix `fe11c2ee900d1c19119e488f8f6ea0f865b5cc39` | `dev-tools/e31_openpbr_parity.py` | CPU/OCL/Metal |
| Random-walk SSS (S2/E35) | material defs / [../engineering/openpbr-sss-findings.md](../engineering/openpbr-sss-findings.md) | `62366b35b964de43395f36874cfc888110493bc6` albedo/mfp | `dev-tools/e35_sss_albedo.py`, `e35_visual_demo.py` | CPU/OCL/Metal |
| Huang hair (S3) | [hair.md](hair.md) | `4cfae33296cc73c7f1d29e97877fce716a0266d6` Chiang/Huang model select | `dev-tools/e36_hair_huang.py`, `e36_visual_demo.py` | CPU/OCL/Metal |
| .lxm proxy + scene.spill out-of-core | [../engineering/out-of-core.md](../engineering/out-of-core.md) | `fe11c2ee900d1c19119e488f8f6ea0f865b5cc39` v1, `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a` cluster idx, `62c7c06687c99b453cfa4584cc1cacc0500b93e1` v4, `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a` stride, `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a` residency pool, spill `c92c2f3407e9eb55fa6e4c649a3c49b409fa867a`+ | SuperBlendLuxCore `dev-tools/lxm*_test.py`, `cluster_residency_test.py`, `imagemap_stream_test.py`, `*spill*_test.py` | all (POSIX+Win COW) |
| Diffraction grating (CD rainbow) | [diffraction.md](diffraction.md) | this branch | `dev-tools/e46_diffraction_test.py` (T1–T4), `scenes/diffraction/cd-rainbow.scn` | CPU/OCL/Metal |

> Engine/API plumbing and misc integration: `06b8b826d`, `8601eaa12`.
> Example scenes: `64aad5c47`. This documentation: `481fea0d2`.
> Maintenance: `cf0db726245c23a186d09f5ff70c4db65fee0051` ExtMeshProp sizeless-layer fix (E7 fallback
> regression), `cf0db726245c23a186d09f5ff70c4db65fee0051` robin_hood→tsl::robin_map, `cf0db726245c23a186d09f5ff70c4db65fee0051` dead
> vendored assets (~47MB). Regression automation:
> `dev-tools/wavefront-regression.sh` (`ae418f869dbf49168410de865b79c992cf748760`, strands_hair case
> `cf0db726245c23a186d09f5ff70c4db65fee0051`) and `dev-tools/parity-regression.sh` (`03dc67b74`, CPU/GPU
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
- ~~**Known portability fragility**~~ — fixed in `cf0db726245c23a186d09f5ff70c4db65fee0051`:
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
  tessellation on the Metal HWRT path (commit `4a90e7057573da31442615f36fa213c53ec3d9bd`, gated on
  macOS 14+ + `LUXCORE_METAL_CURVES`). Known v1 limits: curve meshes as
  triangle lights mis-map in the hit→light reverse lookup; strand AOV
  parity vs the tessellated baseline is approximate by design.
- **PATHCPU-vs-PATHOCL brightness difference** on bright emissives — root
  cause found and fixed in commit `4a90e7057573da31442615f36fa213c53ec3d9bd`. It was not an emission
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
  queue integrity clean, SOBOL/RANDOM/METROPOLIS). M3a (`37cc04b4baa4b492334e8391a2fd2b0a4534e815`)
  moved the queue prefix on-device — measured ~2.5x over dense on the
  cornell 720²/30s Metal benchmark, reversing the earlier dense-wins
  verdict on that scene. The per-iteration totals readback (72B) is
  still required for launch sizing (stale sizing measured ~6x worse).
  Dense stays default; re-benchmark on more workloads before promoting.
  See `dev-tools/wavefront-design.md` §M3a.
- Opt-in / experimental stages are **default-off** until regression coverage
  lands.
