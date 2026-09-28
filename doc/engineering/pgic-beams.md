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
  with `causticVolumeBeams`; GPU still runs **point** queries — CPU is
  the beam path until a GPU segment index exists.
