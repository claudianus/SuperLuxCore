# SuperLuxCore — fork feature documentation

This directory documents every feature added to this fork (LuxCoreRender
2.11.2 -> SuperLuxCore), one self-contained page per feature. Each page states
**what** it is, **why** it is useful, **references** (papers / sources), the
**properties / API** it adds, **test scenes**, **how it was validated**, and
which **platforms** it runs on — the evidence the upstream review guidelines
ask for.

> The fork point is `e9ced7e7a`. The post-fork history has been rewritten
> into **18 clean, self-contained commits** (one per feature/subsystem, with
> what+why+references in each message) so each feature is a reviewable chunk —
> the granularity upstream asks for before a PR. The full squashed change is
> provably identical to the integrated development history (verified via
> `git diff` against the pre-rewrite tree).

## Feature index

| Feature | Doc | Key commits | Test scene(s) | Platforms |
|---|---|---|---|---|
| Metal backend + HWRT | [metal-backend.md](metal-backend.md) + [../metal_backend_design.md](../metal_backend_design.md) | `562a3bf5f` device+HWRT, `9ffadfb12` cl2msl, `ca83e8ceb` pipeline, `30dc89ab3` film | luxball, cornell | **Apple only** |
| ReSTIR DI | [restir-di.md](restir-di.md) | `db4600a18` | manylights | CPU/OCL/Metal |
| MNEE | [mnee.md](mnee.md) | `8363ad339` | causticcube | CPU/OCL/Metal |
| Path guiding | [path-guiding.md](path-guiding.md) | `8687ffc37` | interior | CPU/OCL/Metal |
| Spectral transport | [spectral.md](spectral.md) | `7d8896fc9` | cornell-spectral | CPU/OCL/Metal |
| Samplers | [samplers.md](samplers.md) | `0a62ebb7b` | convergence | CPU/OCL/Metal |
| Blackbody + Whitenoise | [textures.md](textures.md) | `5d38878b6` | cornell/bb-test, whitenoise-* | CPU/OCL/Metal |
| Hair + Disney | [hair.md](hair.md) + [disney.md](disney.md) | `a8785faa2` | strands, hairmat-test, cornell-disney | CPU/OCL/Metal |
| Strand AOVs | [hair.md](hair.md) | `584fabbd1` | strands, strandu-test | CPU/OCL/Metal |
| Metal native curves | [../dev-tools/metal_curve_design.md](../dev-tools/metal_curve_design.md) | `d32bfe3cd` | scenes/strands/hair.scn + BlendLuxCore adapter A/B (parity < MC noise) | **Apple only** (Metal HWRT) |
| Lights plumbing | [restir-di.md](restir-di.md) | `4e40c8d4a` | manylights | CPU/OCL/Metal |
| Film HW pipeline + OIDN | [oidn-film.md](oidn-film.md) | `30dc89ab3` | any render | OCL/Metal; OIDN=Metal validated* |
| Blender adapter | [blender-adapter.md](blender-adapter.md) | BlendLuxCore repo — motion blur `43dc7674`, `35b47f18`; persistent-scene export (A6-II) `c40f585b` + frame-change fix `c78fb7de` + regression `ee166cdd`, `590cb0ac`; material+geometry deltas (A6-III) `2579a019`, `c28f40f0` | .blend scenes; `BlendLuxCore/dev-tools/a6_persistent_scene_test.py` | all; Metal opt = Apple |
| Wavefront task queues (M1+M2, opt-in) | [../dev-tools/wavefront-design.md](../dev-tools/wavefront-design.md) | `85a122a1e` cl2msl fix, `9c59522fe` M1, `f424a8552` M2 λ-bucketed queues (branch `feature/wavefront-queues`) | cornell, cornell-spectral | OCL/Metal; `LUXRAYS_WAVEFRONT_QUEUES=1` |
| Deformation motion blur (E9, scoped) | [../dev-tools/deformation-motion-blur-design.md](../dev-tools/deformation-motion-blur-design.md) | `bc26dbd45` design doc; `5b1e23fa1` plumbing; `d4dc25e22` Metal HWRT; `d1eb37198` SW MBVH; `c16e19453` Embree; BlendLuxCore `c9193f0a` export | `vertexmotion_test`; `dev-tools/e9_*_vertex_motion_test.py`; BlendLuxCore `dev-tools/e9_vertex_motion_e2e_test.py` | Metal HWRT + SW MBVH (OCL/cl2msl) + Embree timesteps; mesh export (hair pending) |

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

- **ReSTIR** is DI-only (no PT/GI/PG) and the RIS target lacks a visibility
  term (~2x spatial-reuse inefficiency) — opt-in, roadmap E2.
- **OIDN Metal** validated locally (device module built + `Type: Metal`
  confirmed in `INTEL_OIDN` pipeline, ~17× over CPU) but **not yet in the
  dependency bundle** — requires LuxCoreDeps recipe `with_device_metal=True`
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
- **Wavefront queues (B2/E3 M1)**: opt-in per-state task queues for the
  PATHOCL micro-kernel state machine (`LUXRAYS_WAVEFRONT_QUEUES=1`).
  Validated on OpenCL + Metal vs dense (Monte-Carlo-noise-level parity,
  queue integrity clean, SOBOL/RANDOM/METROPOLIS). By design a task
  advances one state hop per iteration — same per-sample result, more
  iterations. Dense stays default until the A/B benchmark pass (M2
  λ-bucketing scope). See `dev-tools/wavefront-design.md` §M1 status.
- Opt-in / experimental stages are **default-off** until regression coverage
  lands.
