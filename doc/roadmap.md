# SuperLuxCore roadmap

Independent LuxCoreRender 2.11.2 fork targeting a production-grade,
GPU-first spectral renderer. Primary backend: Apple Metal HWRT; first
adapter: BlendLuxCore on current Blender LTS. Working rules: every
feature lands as reviewable, documented, tested commits (see
`features/README.md`); CPU/GPU parity where practical; conservative
claims backed by measured evidence.

## Done

| Track | Scope | State |
|---|---|---|
| Metal backend | Device + HWRT, cl2msl kernel translation, pipeline, HW film + OIDN, native curves | shipped (Apple-only; CPU/OCL fallbacks intact) |
| ReSTIR DI / MNEE / path guiding / spectral / samplers | see `features/README.md` index | shipped, CPU/OCL/Metal |
| Wavefront queues (M1+M2) | per-state task queues + λ-bucketed queues, opt-in `LUXRAYS_WAVEFRONT_QUEUES=1` | validated; default-off pending A/B benchmark |
| DEP-1/DEP-2 | deps refresh (openvdb 13, robin-hood removal), v2.3.0/v2.4.0 dep releases | done, CI green |
| A6-II/A6-III | persistent-scene incremental export, transform/material/geometry deltas, dupli-set refresh | done; `a6_persistent_scene_test.py` all PASS |
| A5 | dupli/particle + point-cloud transform motion blur | done |
| E9 deformation motion blur | vertex-motion series plumbing (Ph1), Metal HWRT descriptors + motion intersector fix (Ph2), swept-bound SW MBVH/OCL path (Ph3), Embree timesteps (Ph4), BlendLuxCore mesh export (Ph5) | done; `e9_parity_test.py` 4-backend parity PASS |

## In flight / next

| Track | Item | Notes |
|---|---|---|
| E9 Ph5b | Hair/curve deformation motion | strands tessellate to `ExtTriangleMesh` (SW/CPU) and emit Catmull-Rom CPs (Metal); needs per-step CP series → re-tessellated vertex series + `MTLAccelerationStructureMotionCurveGeometryDescriptor` on the curve-AS path |
| E9 Ph6 | Validation scenes | animated character mesh, GN-deformed geometry, armature-driven hair, divergence-stress scene; A/B vs static + backend parity |
| E9 leftovers | OptiX/CUDA motion geometry refresh | `OptixMotionGeometryDesc` vertex buffers; out of scope until the CUDA path is revived |
| E2 | ReSTIR PT/GI/PG + RIS visibility term | current DI-only, ~2× spatial-reuse inefficiency |
| E1 | OIDN Metal into dep bundle | validated locally; needs LuxCoreDeps `with_device_metal=True` recipe + dep release |
| Wavefront M3 | default-on decision | needs A/B benchmark matrix over scene × engine |
| Blender UX | V-Ray/Corona-level polish | persistent-scene cache + deltas landed; remaining: render stats UX, low-resource fallback profiles |
| Compatibility | Cycles shader-node / Geometry Nodes coverage | node reader exists; audit coverage vs Blender LTS |

## Standing gaps (honest list)

- Metal is Apple-only by design; OpenCL SW path is the cross-vendor
  fallback. CUDA/OptiX support is stale (post-E8 codepaths untested).
- Non-uniform motion step times are exact on MBVH/BVH/SW-OpenCL and
  approximated piecewise-uniformly on Metal HWRT and Embree.
- Strand (hair) motion blur not yet implemented — see E9 Ph5b.
- `PATHOCL` + `SOBOL` produced black frames in standalone tests once
  (unverified-path artifact); `TILEPATHOCL`/`TILEPATHSAMPLER` is the
  validated OCL config. Worth a dedicated triage before claiming PATHOCL
  sampler parity.
