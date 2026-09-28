# PhotonGI caustic beams — implementation notes

Design + gotchas found while landing `path.photongi.caustic.volumebeams`
(Stage B2 of `doc/features/caustics-sota.md`). Read this before touching
`tracephotonsthread.cpp`, `photongicache.cpp`, `pgicvisibility.cpp`.

## Dataflow

```
TracePhotonPath (per traced photon, per in-medium flight)
  → newCausticBeams  (thread-local PhotonBeam chunks)
  → AddPhotons (metropolis currentPhotonsScale + uniformCount scaling)
  → PhotonGICache::causticBeams
  → PGICBeamIndex (boost::geometry rtree over segment AABBs)
  → ConnectCausticBeams at volume eye vertices
```

`PhotonBeam` stores `p0`, normalized `d`, `length`, `lightID`, `alpha`
(flux per chunk, pre-attenuated to chunk start) and the `Volume` used for
`TransmittanceEstimate`.

## Estimator

Point-photon kernel is 4/3πr³. For a beam, the kernel integrated along
the beam direction is the **segment/ball overlap length**: solve the
quadratic on beam parameter `t = s ± sqrt(r² − d²)` (`s` = unclamped
projection of the query point onto the beam line, `d²` = squared
point-line distance), then clamp to `[0, length]` — this also culls
end-caps, so do **not** clamp `s` before solving (clamping moves the
interval center and changes the overlap).

Contribution: `alpha · T(p0→mid) · Evaluate(−d)/pdf · overlap/length`,
sum divided by `causticPhotonTracedCount · 4/3πr³`. Kernel-consistent
with the point estimator; e54 gate 4b asserts beams/points within 2×
(measured ≈1.16 on the ms scene).

## Chunking is a performance requirement, not an approximation

Long flights (light→glass→far wall) have AABBs that hit *every* query
box → the rtree degenerates. Chunks of `max(8·r, len/16)` keep AABBs
local. Because each chunk carries `beamFlux·(chunkLen/segLen)` the sum
over chunks equals the unsplit segment exactly, and piecewise
transmittance is more accurate. Before chunking: 160×90@16spp took 50s;
after: 1.7s (≈30×).

## Gotchas

- **`pathInfo.volume` is empty for world-medium rays.** `Scene::Intersect`
  falls back to `scene->GetDefaultWorldVolume()` internally; beam
  deposits must mirror that fallback (`flightVolume`) or world-fog
  flights deposit nothing.
- **Volumes default `photongi.enable=false`** (`parsevolumes.cpp:207`).
  `IsPhotonGIEnabled` (query/receiver gate) therefore admits medium
  vertices only when `caustic.volumeBeams` is on; a separate
  `IsVisibilityEnabled` keeps the *upstream* semantics for
  visibility-particle generation. Coupling the two floods a world-scale
  volume with ~4M particles and the visibility pass never finishes.
- **`visibilityParticlesKdTree` can be null.** In pure-medium scenes all
  visibility particles may be excluded; guard every kd-tree access and
  let photon tracing proceed (beams don't need particles). Update()
  likewise must not bail on an empty map when beams are enabled.
- **R-tree query box vs `intersects`** returns candidates whose AABB
  touches the box; the quadratic overlap test does the real culling —
  keep it cheap (early `d2 >= r2` exit).
- `PhotonGICacheParams` is Boost-serialized: field additions need a
  `BOOST_CLASS_VERSION` bump + `version >= N` guard (currently v7).
- `dynamic_observer_cast` (observer_ptr) is the cast helper for
  `VolumeConstPtr`/`Material` checks — plain `dynamic_cast` does not
  compile on observer pointers.
- `pgic_funcs.cl`/`pathtracer_types.cl` mirror the receiver eligibility
  with `causticVolumeBeams`. GPU beam traversal landed with the flat-BVH
  index (below) — same estimator math as the CPU query.

## GPU index (flat IndexBvh)

`PGICBeamIndex` no longer uses Boost R-tree: segment AABBs are expanded
by the lookup radius, fed to `BuildIndexBVH` (Embree builder → flat
`luxrays::ocl::IndexBVHArrayNode` array). The same node array serves
CPU `Query()` and the device upload in `CompiledScene::CompilePhotonGI`
(`pgicCausticBeams` + `pgicCausticBeamsBVHArrayNode`).

Kernel side (`PGICBeamBvh_ConnectAllNearEntries`) repeats the CPU math
one to one: point→line projection, quadratic chord `[lo,hi]` clamped to
the segment, `beam.alpha * T * bsdfEval * (hi-lo)/length` with the same
`1/(N·4/3πr³)` normalization.

- **`Volume_TransmittanceEstimate` requires `__global` storage** for
  both the ray and the scratch `HitPoint` — private/local structs fail
  the Metal compile (address-space overload resolution). The caller
  passes `&rays[gid]` + `&tasks[gid].tmpHitPoint`; the helper backs the
  ray up once and restores it after the traversal.
- **`half` is a reserved MSL type** — naming a `float` local `half`
  breaks the cl2msl output. Use `halfChord` etc.
- `VSTORE3F` has no private-address overload: write `Ray` fields
  component-wise or use `VLOAD3F`/`VADD3F` only where the pointed-to
  storage lives in global space (kernel radiance accumulation is fine).
- CPU and GPU measured within noise: fog+glass Cornell, 32 spp —
  CPU beams 0.0079 / points 0.0069 (+14 %); GPU beams 0.00797 /
  points 0.00708 (+12.6 %).
