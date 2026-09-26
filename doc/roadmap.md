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
| Wavefront queues (M1+M2) | per-state task queues + λ-bucketed queues, opt-in `LUXRAYS_WAVEFRONT_QUEUES=1` | validated; A/B done — stays opt-in (see below) |
| DEP-1/DEP-2 | deps refresh (openvdb 13, robin-hood removal), v2.3.0/v2.4.0 dep releases | done, CI green |
| A6-II/A6-III | persistent-scene incremental export, transform/material/geometry deltas, dupli-set refresh | done; `a6_persistent_scene_test.py` all PASS |
| A5 | dupli/particle + point-cloud transform motion blur | done |
| E9 deformation motion blur | vertex-motion series plumbing (Ph1), Metal HWRT descriptors + motion intersector fix (Ph2), swept-bound SW MBVH/OCL path (Ph3), Embree timesteps (Ph4), BlendLuxCore mesh export (Ph5), strand/hair control-point motion incl. Metal motion-curve AS + Blender adapter (Ph5b) | done; `e9_parity_test.py` 4-backend parity + `e9_strand_motion_test.py`/`e9_strand_motion_e2e_test.py` PASS |
| E9 Ph6 | Validation scenes | done — GN-deformed mesh (`e9_gn_vertex_motion_e2e_test.py`), particle-hair (`e9_psys_strand_motion_e2e_test.py`), topology-change fallback (`e9_topology_change_test.py`) all PASS |

## In flight / next

| Track | Item | Notes |
|---|---|---|
| E9 leftovers | OptiX/CUDA motion geometry refresh | `OptixMotionGeometryDesc` vertex buffers; out of scope until the CUDA path is revived |
| E2 | ReSTIR PT/GI/PG + RIS visibility term | current DI-only, ~2× spatial-reuse inefficiency |
| E1 | OIDN Metal into dep bundle | validated locally; needs LuxCoreDeps `with_device_metal=True` recipe + dep release |
| Wavefront M3 | material bucketing | decided: not pursued — wavefront loses on every tested workload (dense-vs-wavefront −7~−17%, re-verified 2026-09 cornell 512²/30s: 13.4M vs 12.2M spp/s ≈ −9%); see `dev-tools/wavefront-design.md` M2 status |
| Blender UX | V-Ray/Corona-level polish | persistent-scene cache + deltas landed; remaining: render stats UX, low-resource fallback profiles |
| Compatibility | Cycles shader-node / Geometry Nodes coverage | audited vs Blender 5.2.1 (97 node branches); Math/VectorMath nearly complete via `mathfunc` (trig/exp/log/hyperbolic/invsqrt/floormod + smooth-min/max); BsdfHair/RayPortal/PointInfo/VectorRotate/VectorTransform/EeveeSpecular/Squeeze mapped; residual gaps are scene-query nodes (Raycast/CameraData/LightFalloff/IES/Gabor/Script) — warn+neutral fallback, see BlendLuxCore `doc/cycles_node_coverage.md` |

## Standing gaps (honest list)

- Metal is Apple-only by design; OpenCL SW path is the cross-vendor
  fallback. CUDA/OptiX support is stale (post-E8 codepaths untested).
- Non-uniform motion step times are exact on MBVH/BVH/SW-OpenCL and
  approximated piecewise-uniformly on Metal HWRT and Embree.
- `PATHOCL` + `SOBOL` black frames were reported once in an old build;
  re-verified 2026-09 on cornell.scn and a minimal emissive scene —
  output matches `PATHOCL+RANDOM` and `TILEPATHOCL`+`TILEPATHSAMPLER`
  within sampling noise. Considered resolved; keep an eye on it if a
  scene-specific reproducer shows up.
