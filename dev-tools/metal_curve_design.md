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
  `controlPointBuffer` = leaf cp float4s (`format Float3, stride 16`),
  `radiusBuffer` = same buffer (`offset 12, format Float, stride 16`),
  `indexBuffer` = mesh-local seg indices, `segmentControlPointCount = 4`,
  `curveType = Round`, `curveBasis = CatmullRom`, `curveEndCaps = None`.
- Instance AS unchanged (mixed triangle/curve primitive ASs are supported).
- `LUXRAYS_METAL_CURVES=0` opt-out mirrors `LUXRAYS_METAL_HWRT`.

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
