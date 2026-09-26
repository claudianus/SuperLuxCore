# Metal Native Curve Primitives (E7) — Design

Status: implementing. Roadmap item E7/A3 — native `MTLAccelerationStructure`
curve primitives for hair/fur, replacing the triangle tessellation path on
Metal HWRT (~29% gap vs dense-mesh hair).

## References

- `MTLAccelerationStructureCurveGeometryDescriptor` (Metal 3 / macOS 13+):
  controlPointBuffer(+stride), radiusBuffer(+stride), indexBuffer,
  segmentControlPointCount, segmentCount, curveType, curveBasis, curveEndCaps.
- `raytracing::intersector<instancing, triangle_data, curve_data>`:
  `hit.type == intersection_type::curve`, `hit.primitive_id` = segment index,
  `hit.curve_parameter` = position u along the segment, `hit.distance` = ray t.
- Cycles Metal backend (`kernel/device/metal/bvh.h`): same pattern —
  curve keys as float4 (xyz + radius), segment→curve lookup, Catmull-Rom
  re-evaluation in shading for tangent/normal.

## Design overview

The strands shape already tessellates cyHair polylines into a triangle
`ExtTriangleMesh`. We keep that tessellation (CPU/software-BVH path and
light sampling are unchanged) and additionally store **curve data** on the
mesh. The Metal HWRT kernel builds a *curve* primitive AS for such meshes;
the CPU path continues to intersect the tessellated triangles.

### Curve data layout (host `ExtTriangleMesh`)

- `curveCps`: `float4[]` — `{pos.x, pos.y, pos.z, radius}` per control point,
  object space. Each strand contributes `P+2` cps (endpoints duplicated) so
  that segment `j` reads cps `[j, j+1, j+2, j+3]` — uniform Catmull-Rom
  (identical to `CatmullRomSpline` in strands.cpp and Metal's
  `MTLCurveBasisCatmullRom`).
- `curveSegIndices`: `uint[]` — per-segment starting control-point index
  (**mesh-local**). This is exactly the Metal index-buffer layout.
- `curveCpAttrs`: `float4[]`, 2 per cp —
  `[2i] = {colR, colG, colB, alpha}`, `[2i+1] = {uvU, uvV, strandU, strandIndexBits}`.
  Attributes interpolate linearly between the two inner cps of a segment,
  matching the ribbon tessellation's vertex interpolation.

### GPU data layout (CompiledScene)

All meshes' curve data is concatenated into three global buffers appended to
`KERNEL_ARGS`/`EXTMESH_PARAM`:

- `curveCps` (float4), `curveSegIndices` (uint, **globalized** cp indices),
  `curveCpAttrs` (float4 ×2/cp).
- `ocl::ExtMesh` gains `curveSegsOffset` (into `curveSegIndices`;
  `NULL_INDEX` ⇒ no curves) and `curveSegsCount`.

### Hit encoding

`RayHit` layout is unchanged. The Metal kernel writes:

- `triangleIndex = 0x80000000 | segmentIndex` (high bit = curve flag;
  never set by the software BVH)
- `b1 = curve_parameter` (u), `b2 = 0`
- `t = hit.distance`, `meshIndex = instance_id` (unchanged)

Consumers branch on the flag inside the `ExtMesh_*`/`HitPoint_*` accessors —
call sites (path kernels, `BSDF_Init`, `HitPoint_Init`) stay untouched.

### Shading evaluation (kernel side)

For a curve hit on segment `s` at parameter `u` (cp indices `s0..s0+3`):

- **geometry/shade normal**: `N = normalize(P_world - C(u))` where `C(u)` is
  the Catmull-Rom centerline evaluated in object space then transformed by
  `localToWorld` (same transform handling as triangle meshes).
- **interpolated normal** = geometry normal (no per-cp normals).
- **default UV** = `mix(uv[cp1], uv[cp2], u)` from `curveCpAttrs`.
- **color/alpha** = `mix` of cp attrs.
- **vertex AOV** (hair): indices 4/5/6 → analytic Catmull-Rom derivative
  tangent components (object space); 7 → `mix(strandU)`; 0 → strand-random
  hash recomputed from `strandIndex` (identical hash to strands.cpp).
- **differentials**: `dpdu` = tangent vector, `dpdv` = shading normal;
  `dndu/dndv = 0` (approximation; adequate for hair).

### Metal AS build

- Per curve-carrying leaf: one `MTLCurveGeometryDescriptor` —
  `controlPointBuffer` = packed float3 positions (`stride 12`),
  `radiusBuffer` = packed float radii (`stride 4`) in a *separate*
  buffer (Cycles' Metal BVH uses the same split layout; the earlier
  float4 + `.w`-lane alias relied on undocumented stride semantics),
  `indexBuffer` = mesh-local seg indices, `segmentControlPointCount = 4`,
  `curveType = Round`, `curveBasis = CatmullRom`, `curveEndCaps = None`.
- Instance AS unchanged (mixed triangle/curve primitive ASs are supported).
- `LUXRAYS_METAL_CURVES=0` opt-out mirrors `LUXRAYS_METAL_HWRT`.
- **Motion (E9 Ph5b)**: strand meshes carrying `curveCpsMotionSteps`
  build a `MTLAccelerationStructureMotionCurveGeometryDescriptor`
  instead — `controlPointBuffers`/`radiusBuffers` are per-step slices of
  two packed buffers (float3 positions, float radii — same layout as the
  static path) exposed through `MTLMotionKeyframeData` entries;
  `controlPointCount`/`segmentCount` shared
  across keyframes by construction (the recipe rejects step poses that
  change them). On macOS < 14 or when the mesh also takes the
  motion-triangle path, strands fall back to tessellated motion
  triangles. Validated by `dev-tools/e9_strand_motion_test.py` (HWRT
  motion sweep matches the CPU MBVH render pixel-for-pixel).

## Limitations / notes

- **CPU↔Metal parity for hair is approximate, not bit-exact**: the CPU path
  shades tessellated ribbon/solid triangles while Metal shades smooth
  Catmull-Rom tubes. Silhouettes/attributes match to tessellation accuracy;
  this is the intended quality improvement, not a defect. Gate checks on hair
  scenes should compare vs the tessellated baseline visually/statistically.
- **Curve meshes as triangle lights**: the hit→light reverse lookup
  (`lightIndexByTriIndex`) maps segment index → wrong triangle. Hair emission
  is rare; documented v1 limitation (forward light sampling via the
  tessellation still works).
- **Non-uniform object scale**: curve radii are scaled by the mean axis scale
  on `ApplyTransform`/`Merge` (Metal supports a single radius per cp; a tube
  under non-uniform scale is not representable).
- `Merge` propagates curve data only when **every** input mesh carries it —
  the merged curve AS replaces the whole merged mesh, so a partially-curved
  group would silently lose its non-curve members' native primitives. Mixed
  groups fall back to triangles for everything.
- `Copy`/`CopyExt` propagate curve data only when the vertex set is
  unchanged; callers overriding vertices (subdiv, displacement, ...) produce
  geometry the original curves no longer match, so those meshes fall back to
  triangle rendering.
- cyHair linear polylines become Catmull-Rom curves — same spline model the
  adaptive tessellation already uses.

## Mesh data lifecycle

| Path | Curve data |
|---|---|
| `strands.cpp` refinement | emitted (all tessellation types) |
| `ApplyTransform` | cps transformed, radii × mean axis scale |
| `Merge` (group bake) | kept iff all inputs curved; indices globalized |
| `Copy`/`CopyExt` | kept iff vertices not overridden |
| Serialization (save/load) | **not stored** — loaded scenes render triangles |

Serialization intentionally excludes curve vectors: adding fields would break
the binary scene format in both directions (old files lack them; new fields
are unreadable by old code). A deserialized strands mesh simply falls back to
its tessellated triangles.

## Validation (2026-09, M5 Pro, Debug build)

Scene: `scenes/strands/hair.scn` — 10k strands, 150k curve segments,
180k control points, 9.29M tessellated triangles. Both paths render to
completion with visually identical output (alpha/color attributes intact).

| Path | Samples @30s | Notes |
|---|---|---|
| `LUXRAYS_METAL_CURVES=0` (triangles) | 847 | 9.29M tris, refine 0.25s |
| native curves (default) | 651 | 9MB curve buffers, refine 0.20s |

Native curves are ~24% slower in raw sample throughput on this scene —
hair.scn uses extreme tessellation (62 tris/segment), which favors the
triangle HW-RT path. The native path's value is memory (~1/100 primitive
footprint), exact curve surfaces (no facets), and refinement speed; it
is expected to pull ahead on dense production hair where tessellation
quality must rise for close-ups and memory pressure dominates.

### Bug found by fallback testing

`LUXRAYS_METAL_CURVES=0` crashed in `ExtTriangleMesh::Merge`:
`GetAlpha` asserted `vertIndex < alphas.GetLayerSize()` with layer size 0.

Root cause: `ExtMeshProp(Layer)` (shared_ptr ctor) stored the pointer
without setting `_size`. Every mesh built via the raw `shared_ptr` ctor
path (strands alphas, scene/parseshapes UVs/cols/alphas) had
`HasAlphas()==true` but `GetLayerSize()==0` — release builds silently
read with no bound check; debug builds asserted.

Fix: `ExtTriangleMesh::Init` normalizes any installed layer that carries
a pointer but `_size==0` to the vertex count (per-vertex attribute
contract). Covers all raw-ctor call sites uniformly.

### Blender adapter integration (2026-09, SuperBlendLuxCore headless)

Headless Blender 5.2.1 render through the real adapter path:
`hair_curves` datablock → `convert_hair_curves` →
`Scene.DefineBlenderCurveStrands` (3000 segments / 3600 control points,
200 strands × 15 segments) → PATHOCL on `MetalIntersect`.

- Native run logged `Metal HWRT: native MTLAccelerationStructure path
  active` — curve geometry entered the Metal AS, render completed.
- `LUXRAYS_METAL_CURVES=0` run rendered the same scene through the
  triangle path to completion.
- Parity check at 256 spp: pixel diff native-vs-tri (mean 12.50) was
  *smaller* than native-vs-native across seeds (mean 15.74) — output
  differences are below Monte Carlo noise, so the paths are statistically
  equivalent.
- Parentless `hair_curves` correctly skipped UV/color export (warning
  path exercised); per-strand `radius` attribute folded into
  diameter/taper as designed.

### Primitive-parity investigation (2026-11, M5 Pro, Debug build)

An earlier observation ("native curves render hair darker") was
re-investigated quantitatively. Findings:

- A standalone Metal probe (same descriptor + data) verified the AS
  semantics directly: a segment index `i` draws the Catmull-Rom span
  between `cps[i+1]` and `cps[i+2]`; duplicated end control points
  (LuxCore's `[p0,p0,...,p_{n-1},p_{n-1}]` padding, identical to
  Cycles' emission) render correctly; `hit.distance` is the true
  round-tube surface distance; `curve_parameter` is the segment-local
  `u`.
- With the tessellation at `solid.sidecount=16` (a 16-gon tube
  approximating the round primitive), the hit coverage is
  pixel-identical to native curves (ALPHA mean 0.6745 vs 0.6746) —
  the curve AS geometry is exact.
- The residual brightness delta (~6% on `hair.scn`'s glossy2
  material) is a *shading* difference: native hits get the true
  radial tube normal while the tessellation carries faceted/
  interpolated polygon normals. The mean normal field matches
  (AVG_SHADING_NORMAL means equal to ~0.2%); per-pixel angles differ
  up to ~80° near silhouette edges, which shifts the specular lobe.
  This is the intended primitive upgrade — round tubes are the
  mathematically correct interpretation of strand thickness.
- With the default `ribbon` tessellation the coverage also matches
  (0.6746 vs 0.6745) on this scene; earlier "missing coverage"
  observations were traced to a stale binary, not the AS.
- The default `solid` tessellation uses `sidecount=3`, whose faces sit
  at the inradius `r·cos(60°)=0.5r` — native tubes are ~2x thicker
  than a 3-gon tessellation of the same strand. This is a tessellation
  undershoot, not a curve defect; raising `sidecount` converges the
  triangle path to the native result.

Conclusion: native curves are geometrically exact and shading-correct.
CPU/OpenCL parity is bounded by the tessellation being a different
primitive (flat ribbon or faceted N-gon). `LUXRAYS_METAL_CURVES=0`
remains the bit-comparable fallback.

### NaN continuation-ray root cause (gpuAddress residency)

The darker-hair symptom turned out to be a real bug, not a primitive
difference: on dense hair scenes, ~45% of BSDF continuation rays were
generated with NaN directions, collapsing INDIRECT_DIFFUSE ~20x while
DIRECT_DIFFUSE stayed correct.

Chain: `BSDF_Sample` returned NaN directions <- `bsdf->frame` was NaN
<- `Frame_Set` normalized a zero `dpdu` <- `Curve_GetTangentObj` read
zero control points <- the kernel's `curveCps`/`curveSegIndices`/
`curveCpAttrs` pointer-table entries dereferenced to zeros even though
the host wrote the correct `gpuAddress` and the MTLBuffer held correct
data.

Root cause: buffers reached through the `KernelPtrsN` pointer table are
referenced only by `gpuAddress` — they are never `setBuffer`-bound, so
Metal does not automatically make them resident for the dispatch. On
Apple Silicon the pages usually stay mapped anyway (unified memory),
which is why earlier members (lights, rayHits, film) worked — but the
curve buffers, written once at scene init and only ever touched via
`gpuAddress`, lost residency and reads returned zeros.

Fix (`metaldevice.mm` `EnqueueKernel`): every table-referenced buffer
is now declared `[encoder useResource:buf usage:Read|Write]` before
dispatch — the documented requirement for gpuAddress-indirect access.
Known platform limitation (verified on the standalone probe, not
render-path specific): Metal's built-in curve intersector is
front-face only — a ray whose origin lies inside a tube reports a
miss for that tube's exit surface (it still hits OTHER tubes
normally; the miss is only the containing primitive's back wall).
Cycles' Metal backend has the same constraint (it also uses the
built-in intersector + filter functions for curves). Consequence for
us: a continuation ray spawned just inside a neighbour strand it
overlaps escapes through the wall instead of hitting it — a slight
energy loss on deeply overlapping dense hair, not a correctness
break. If interior hits ever matter (e.g. refractive strand
materials), the fix is a custom intersection function via
`intersectionFunctionTableOffset` like Cycles' triangle path, at the
cost of losing the hardware curve test.

Post-fix measurements on the dense opaque-hair scene (160x120, 48spp,
matte+sky+sun):

| AOV | Metal curve | Metal tess-16 | OpenCL tess |
|---|---|---|---|
| DIRECT_DIFFUSE | 0.00145 | 0.00140 | 0.00139 |
| INDIRECT_DIFFUSE | 0.0332 | 0.0325 | 0.0324 |

Residual ~3% delta is the expected round-tube-vs-16-gon normal field
difference. e22 gained a T5 INDIRECT_DIFFUSE gate to catch a
recurrence of exactly this failure mode.

Throughput on the same dense scene (10s wall, PATHOCL, 65536 tasks):
native curves 37.8M samples/s vs solid-16 tessellation 36.5M samples/s
(~+3.4%) — the exact primitive is also marginally faster: 150k curve
segments vs ~2.4M triangles for the 16-gon tube.

A defensive fallback was also added in `HitPoint_Init` (hitpoint_funcs.cl):
a degenerate curve segment whose tangent is (numerically) zero or
parallel to the shading normal now rebuilds `dpdu` from an arbitrary
axis perpendicular to `shadeN` — the tube is rotationally symmetric
about its axis, so any perpendicular is valid — instead of letting
`normalize(0)` poison the frame.
