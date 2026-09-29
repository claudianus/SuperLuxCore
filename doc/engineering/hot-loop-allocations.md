# Hot-loop allocation audit (PATHCPU)

Measured with `sample(1)` on a long `prism-conservatory` PATHCPU render
(Apple M5 Pro, Release). After the RTTI round (d872cbbb4), allocator
traffic concentrated under PhotonGI lookups and light-path result
construction.

## Fixed

- **`SpectrumGroup` is a `std::vector<Spectrum>` wrapper** —
  `Add(lightID, s)` auto-grows via `group.resize(i + 1)`, so every
  connect emitted a heap allocation (and a realloc cascade when photon
  lightIDs arrived out of order). Now:
  - `PhotonGICache::ConnectWithCausticPaths` pre-sizes the group to
    `scene->GetLightSources().GetSize()` once per query.
  - `PGICPhotonBvh::ConnectAllNearEntries` / `ConnectCacheEntry` and
    `PhotonGICache::ConnectCausticBeams` take the group by reference and
    accumulate in place — no per-photon/beam temporaries.
  - Math unchanged: the trailing `result /= scalar` normalizes a
    zero-initialized pre-sized group; absent light groups stay zero
    exactly as with auto-grow.
- **`PathTracer::RenderLightSample`** now `reserve(maxPathDepth.depth + 2)`
  after `clear()` — `AddLightSampleResult`'s `resize(size + 1)` used to
  realloc + move the whole `SampleResult` array once per light-path
  vertex (each `SampleResult` owns a radiance `SpectrumGroup`).

## Result

- prism-conservatory PATHCPU: 0.256 → 0.31 Ms/s (+21% on top of the
  RTTI round; ~72% cumulative vs the original 0.18 Ms/s baseline).
- e90 caustic stress, e91 pgic-update smoke, e54 media-caustic: PASS.

## Gotchas

- `SpectrumGroup::Add` keeps its auto-grow contract — the pre-size is a
  capacity hint, not a bound, so a lightID outside the light-defs count
  would still be handled correctly.
- `PGICPhotonBvh` is boost-serialized (`BOOST_CLASS_EXPORT_*`): do NOT
  add data members without a serialization version bump — the pre-size
  therefore lives at the caller, not in the BVH.
- `PathVolumeInfo` is already allocation-free (`std::array` volume
  list) — do not regress it to a vector.
