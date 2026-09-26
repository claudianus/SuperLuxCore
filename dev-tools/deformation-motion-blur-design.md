# Deformation (vertex) motion blur — E9 design

Status: **Phase 1 (plumbing) + Phase 2 (Metal HWRT backend) +
Phase 3 (swept-bound software path) implemented.**
Roadmap item E9 — engine-level per-vertex motion blur for meshes and
curves. Adapter-side prerequisites (A5 step-collection infra) are done.
On Metal, meshes carrying a vertex series now render true deformation
blur via `MTLAccelerationStructureMotionTriangleGeometryDescriptor`;
all other backends route through the software MBVH path (swept bounds +
per-ray vertex interpolation). Embree uses native multi-timestep
geometry (Phase 4).

Phase-5 surface (implemented, BlendLuxCore repo):

- `ExportedMesh` records an export-time topology signature
  (`vert_count`, `loop_vertices` map) — only for objects with
  `enable_motion_blur` under an enabled motion-blur camera.
- `motion_blur.convert()` gained a vertex-step collection pass inside
  the existing `frame_set` shutter loop (`_collect_vertex_step` /
  `_sample_loop_points`): each step re-runs the mesh_converter vertex
  pipeline (`to_mesh` → `calc_loop_triangles` → `split_faces` →
  `co`/`vertex_index` foreach_get) on the evaluated object and keeps
  the loop-expanded `(N,3)` positions — the same domain `DefineMeshExt`
  exports `points` in. Samples dedupe per `mesh_key`, so objects
  sharing an instanced mesh evaluate it once per step. A step whose
  vertex/loop topology differs from the signature disables vertex
  motion for that mesh (static fallback + notice).
- `_build_vertex_motion` calls `Scene.SetMeshVertexMotion` per base
  shape name with the same `frame_offsets` schedule used by transform
  motion; identical step buffers are skipped. Objects whose final
  shape is a wrapper (subdiv/displacement) are excluded — wrappers
  build new meshes that cannot carry the base mesh's series.
- Validation: `dev-tools/e9_vertex_motion_e2e_test.py` renders a
  shape-key-deforming emissive quad headless (PATHOCL/Metal) — blurred
  footprint span 143px vs static 88px, non-opted-in object identical to
  static. All checks pass.

Phase-4 surface (implemented, `src/luxrays/accelerators/embreeaccel.cpp`):

- `ExportTriangleMesh` resolves the base mesh via
  `ExtTriangleMesh::FromMesh`; when the mesh carries a vertex-motion
  series the geometry gets `rtcSetGeometryTimeStepCount(stepCount)` and
  each step buffer is shared as a `RTC_BUFFER_TYPE_VERTEX` timestep
  (zero-copy, same shared-buffer pattern as the existing step-0 export).
  Step counts beyond `RTC_MAX_TIME_STEP_COUNT` throw, matching the
  transform-motion export. Instanced deforming meshes share the same
  multi-timestep geometry through the existing instance-scene path.
- `ExportMotionTriangleMesh` composes transform and deformation motion:
  each timestep's vertex buffer samples `GetVertexAtTime()` at the
  motion-system step time and applies that step's `local2World`.
- `Init` extends `minTime`/`maxTime` to cover vertex-series times so the
  `(ray.time - minTime) * timeScale` normalization matches the shutter
  range; `Intersect` now clamps the normalized time to [0,1] so
  out-of-range shutter times sample the boundary poses (same clamping
  as `GetVertexAtTime` on the BVH/MBVH/OpenCL paths).
- Limitation: Embree distributes timesteps uniformly over the scene
  time interval, so a non-uniform step-time series is approximated
  piecewise-uniformly — the same convention as the Metal HWRT path;
  exact non-uniform timing is preserved on the MBVH CPU and OpenCL SW
  paths which store real step times.
- Validation: `vertexmotion_test` grew 8 Embree asserts (pose hit/miss
  at t=0/0.5/1, no fabricated hit, static-leaf isolation, `meshIndex`
  attribution, clamping) — 50 checks total, all passing.

Phase-3 surface (implemented):

- Swept bounds: `ExtTriangleMesh::GetBBox()` now unions the base
  vertices with every motion step, so root-leaf bounds
  (`MBVHAccel::Init`/`Update` and `DataSet::UpdateBBoxes`) stay
  conservative. `BVHAccel::Init` widens each triangle's leaf-node bbox
  to the union over all steps — traversal can reach every pose, while
  misses still come from the interpolated triangle test.
- Kernel (`include/luxrays/accelerators/mbvh.cl`): under
  `MBVH_HAS_VERTEXMOTION` the accelerator gains three params —
  `leafVertMotionDescs` (one `VertMotionDesc` per leaf reference,
  indexed by `meshOffsetIndex`), the packed `vertMotionVerts`
  (step-major) and `vertMotionTimes` arrays. On leaf entry the
  descriptor is cached; per triangle the global page-encoded vertex
  index is decoded back to the leaf-local index
  (`v >> 29 * MBVH_VERTS_PAGE_SIZE + v & 0x1fffffff - staticVertOffset`
  — valid for both single- and multi-page layouts), the surrounding
  step pair is found by linear scan over the (small) step times, and
  `mix()` interpolates the three corners at `ray.time`. Static leaves
  (`vertCount == 0`) keep the original fetch path.
- Upload (`mbvhaccelhw.cpp`): step buffers are packed contiguously per
  unique leaf and shared between leaf references (instanced deforming
  meshes don't duplicate memory); `MBVH_VERTS_PAGE_SIZE` is defined so
  the kernel can decode indices. Buffers are allocated once at kernel
  construction — `Update()` only refreshes nodes/transforms.
- CPU parity: `MBVHAccel::Intersect` resolves the leaf base mesh once
  per leaf entry via the new `ExtTriangleMesh::FromMesh` helper
  (instance/motion wrappers share the base mesh's motion data) and
  samples `GetVertexAtTime()` per triangle. The flat
  `BVHAccel::Intersect` got the same treatment for
  `accelerator.type=BVH` scenes.
- Routing: `DataSet::Add` now flags `hasMotionBlur` for any mesh whose
  base `ExtTriangleMesh` carries a vertex series — vertex-motion-only
  scenes previously took the flat-BVH path and rendered statically.
- Validation: `vertexmotion_test` grew 20 asserts (swept bbox,
  MBVH hit/miss at t<0/0/0.5/1/>1, mixed static+motion leaves,
  occluder-inside-swept-bound attribution, nonuniform K=3);
  `dev-tools/e9_swaccel_vertex_motion_test.py` re-runs the Phase-2
  scene with `LUXRAYS_METAL_HWRT=0` (cl2msl software kernel) —
  all pose and sweep checks pass.

Phase-2 surface (implemented, `src/luxrays/devices/metalrtaccel.mm`):

- Motion-triangle geometry descriptor per vertex-motion leaf: all
  keyframes packed into one `MTLBuffer`, each `MTLMotionKeyframeData`
  referencing its slice (offset = step × vertexBytes), shared index
  buffer from the constant topology. `motionKeyframeCount` /
  `motionStartTime`/`motionEndTime` on the primitive AS descriptor.
- Motion instance AS: when any leaf has a motion system or vertex
  motion, instance descriptors switch to
  `MTLAccelerationStructureMotionInstanceDescriptor` (uniform type
  across the buffer). Transform keyframes are sampled uniformly over
  `[startTime, endTime]` (Metal distributes keyframes uniformly;
  LuxCore `MotionSystem` times may be nonuniform). Static leaves in a
  motion instance AS get a single keyframe with a non-degenerate [0,1]
  interval.
- Motion kernel variant: `intersector<instancing, instance_motion,
  primitive_motion, triangle_data[, curve_data]>` + timed
  `intersect(ray, as, r.time)` — the timed overload only exists on
  motion-tagged intersectors; on a plain `<instancing,...>` intersector
  the time argument silently binds the uint-mask overload (0.5 → 0 →
  every instance masked → all rays miss). This also fixed pre-existing
  object-transform motion blur on Metal, which was broken by the same
  overload resolution.
- Regression test: `dev-tools/e9_metal_vertex_motion_test.py` —
  emissive-marker quad in front of a static wall, narrow-shutter
  fixed-pose checks (t≈0/0.25/0.5/1) + full-shutter sweep extent for
  K=2/3/4 and nonuniform keyframe times.
- Debug hook: `LUXRAYS_METAL_DBG_TIME=<t>` compiles the motion kernel
  with a fixed ray time (isolates AS keyframe data from sampler-side
  time distribution).

Phase-1 surface (implemented):

- `ExtTriangleMesh::SetVertexMotion(times, stepVerts)` — `motionVertTimes`
  + `motionVertSteps` members. Validation: ≥2 steps, strictly increasing
  finite times, every step buffer has the mesh vertex count (constant
  topology). `GetVertexAtTime(i, t)` lerps between adjacent steps and
  clamps outside the range (MotionSystem convention). Positions only;
  per-step normals are not stored.
- Propagation: `ApplyTransform` transforms every step buffer;
  `CopyExt`/`Copy` keep the series iff `meshVertices` is not overridden
  (same rule as curve data); `Merge` keeps it iff every input has a
  series with identical times — partial presence or differing times
  throw (same convention as UV/AOV mismatches). `Delete` frees steps;
  serialization does not persist them (load clears → static fallback).
- Scene plumbing: `Scene::SetMeshVertexMotion` →
  `ExtMeshCache::SetMeshVertexMotion` (plain `TYPE_EXT_TRIANGLE` meshes
  only — set it on the base shape, not instance/motion wrappers) +
  `GEOMETRY_EDIT` so the next `Preprocess` rebuilds the DataSet.
- Public API: `luxcore::Scene::SetMeshVertexMotion(meshName, times,
  timesCount, verts, vertsCount)` — flat step-major float array —
  and the pyluxcore binding
  `Scene.SetMeshVertexMotion(name, times, [step (N,3) arrays])`.
- Properties: `<prefix>.motion.N.time` + `<prefix>.motion.N.vertices`
  parsed in `CreateInlinedMesh`, covering both `scene.objects.*` inlined
  meshes and `scene.shapes.* type=inlinedmesh`. The object-level
  transform-motion wrapper is now skipped when no step defines
  `.transformation` (vertex-only motion keeps the plain-mesh path —
  avoids pushing static transforms through the motion path).
- Unit test: `vertexmotion_test` (`dev-tools/e9_vertexmotion_test.cpp`,
  wired in `src/luxrays/CMakeLists.txt`) — 22 asserts covering
  validation, lerp/clamp, copy/merge/transform/serialization rules.
  Building it surfaced a pre-existing upstream bug —
  `TriangleMesh::save()` serialized `vertices.Count()` as the triangle
  count, corrupting every mesh where the two differ; fixed separately.

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

1. ~~ExtTriangleMesh vertex series + serialization/merge rules + scene
   property plumbing (no backend uses it yet — pure plumbing).~~ **Done**
   — see the Phase-1 surface list above.
2. ~~Metal motion geometry descriptor (primary GPU-first target).~~
   **Done** — see the Phase-2 surface list above.
3. ~~OpenCL swept-bound software path.~~ **Done** — see the Phase-3
   surface list above.
4. ~~Embree timestep path (CPU parity).~~ **Done** — see the Phase-4
   surface list above.
5. ~~BlendLuxCore mesh export.~~ **Done** — see the Phase-5 surface list
   above. Hair/strand export is still open: strands go through
   `DefineBlenderStrands` (curve control points, not triangle vertices)
   and need a core curve-point motion series first.
6. Validation scenes: animated character mesh, GN-deformed geometry,
   armature-driven hair — A/B vs static, plus a divergence-stress scene.

Open question: whether per-step normals are needed for smooth-shaded
deforming surfaces or whether shading-time recomputation suffices —
decide after the first visual tests.
