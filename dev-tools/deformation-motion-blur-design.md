# Deformation (vertex) motion blur — E9 design

Status: scoped. Roadmap item E9 — engine-level per-vertex motion blur for
meshes and curves. Adapter-side prerequisites (A5 step-collection infra)
are done; this document covers the engine side.

## Why this is an engine task

LuxCore's `MotionSystem` (`include/luxrays/core/geometry/motionsystem.h`)
interpolates `Transform`s only — `InterpolatedTransform` decomposes two
matrices and samples between them. There is no per-vertex time data
anywhere in the mesh model, BVH, or scene properties. Scenes that deform
vertices (character animation, FLIP/cloth caches, GN-evaluated geometry,
hair driven by armature) currently render with zero motion blur.

## Data model

`ExtTriangleMesh` gains an optional vertex time series:

- `motionTimes` — N shutter times (already the convention used by
  `MotionSystem` and by the `motion.N.*` scene properties).
- `motionVerts[N]` — N vertex buffers, one per step. Positions only;
  normals are recomputed at shading time (or per-step normals if a cheap
  cross-product normal is insufficient — decide during implementation).
- Same linear-interpolation model as transform motion: at ray time t,
  vertex position is the lerp between the surrounding steps.
- Topology is constant across steps — Blender-side export enforces this
  (vertex-count mismatch falls back to the center step, same policy as
  the A5/point-cloud instancing paths).

Memory cost: N× the vertex array. For hair (strands.cpp) the curve
control points get the same treatment — `curveCPs` becomes
`curveCPs[N]`; tessellated fallback gets `motionVerts` the same way.

## Per-backend path

| Backend | Mechanism |
|---|---|
| Metal HWRT | `MTLAccelerationStructureMotionTriangleGeometryDescriptor` and `MTLAccelerationStructureMotionCurveGeometryDescriptor` — per-keyframe vertex buffers, HW interpolates. Gate on `device.supportsMotionBlur`; falls back to static otherwise. Extends the E7 curve-AS path directly. |
| Embree (CPU) | `rtcSetGeometryTimeStepCount` + `RTC_BUFFER_TYPE_VERTEX` per timestep — native multi-segment motion blur for meshes and curves. |
| OpenCL (SW) | No HW motion support: build the BVH over each triangle's *swept* AABB (union of all step positions), and interpolate verts to `ray.time` inside the leaf intersection test. Same asymptotic traversal, wider bounds. The .cl kernels already carry `ray.time` for transform motion. |
| OptiX/CUDA | `OptixMotionGeometryDesc` vertex buffers — E8-era refresh handles this; out of scope for the first implementation. |

## Scene/API surface

- `Scene::DefineMesh`/`DefineMeshExt` gains an optional
  `(times, vertsPerStep)` argument, or a `motion.N.vertices` property —
  properties version preferred since everything else is property-driven
  (`motion.N.time`, `motion.N.transformation`).
- `Mesh::ApplyTransform`/`Merge`/`CopyExt` must propagate the vertex
  series under the same rules the E7 curve data uses (kept iff vertices
  are not overridden; merge keeps motion iff all inputs carry it).
- Serialization: same stance as E7 curve data — v1 does not persist
  vertex series; deserialized scenes render static.

## BlendLuxCore side (adapter, mostly done)

- The A5 step loop in `export/motion_blur.py` already re-evaluates the
  depsgraph at every shutter step — deformation export adds
  `foreach_get("co")` on the evaluated mesh per step (vectorized, same
  fast path used for hair point collection).
- Gate: `enable_motion_blur` on the object; topology check per step.
- Hair: `depsgraph` particle/curve positions per step — same collection
  point as A5's dupli matrices.

## Phasing

1. ExtTriangleMesh vertex series + serialization/merge rules + scene
   property plumbing (no backend uses it yet — pure plumbing).
2. Metal motion geometry descriptor (primary GPU-first target).
3. OpenCL swept-bound software path.
4. Embree timestep path (CPU parity).
5. BlendLuxCore mesh + hair export.
6. Validation scenes: animated character mesh, GN-deformed geometry,
   armature-driven hair — A/B vs static, plus a divergence-stress scene.

Open question: whether per-step normals are needed for smooth-shaded
deforming surfaces or whether shading-time recomputation suffices —
decide after the first visual tests.
