# E90 finding: SIGSEGV in progressive PhotonGI caustic update (updatespp > 0)

Found by `dev-tools/e90_caustic_stress_test.py` (2026-09-28, Apple M5 Pro,
Release build of pysuperluxcore cp313). All five new caustic stress scenes
crash deterministically during the first periodic cache update.

## Symptom

`path.photongi.caustic.enabled = 1` + `path.photongi.caustic.updatespp > 0`
→ `SIGSEGV` (`EXC_BAD_ACCESS`, e.g. `KERN_INVALID_ADDRESS at 0x1265e8`)
inside

```
slg::PGICPhotonBvh::ConnectAllNearEntries(slg::BSDF const&)
slg::PhotonGICache::ConnectWithCausticPaths(slg::BSDF const&)
slg::PathTracer::RenderEyePath(...)
```

`updatespp = 0` is unaffected (all scenes render fine).

## Root cause — dangling `allEntries` after the shadow-cache swap

`PhotonGICache::UpdateWorker` builds the replacement BVH over the
*shadow* array (`photongicache.cpp`):

```cpp
updateCausticPhotonsBVH = new PGICPhotonBvh(&updateCausticPhotons, ...);
```

`IndexBvh` stores `const SpillableArray<T> *allEntries` — a pointer to the
array *object*, not to its data.

`PhotonGICache::ApplyPendingUpdate` then installs that BVH as live and
moves the shadow array into the live one:

```cpp
delete causticPhotonsBVH;
causticPhotonsBVH = updateCausticPhotonsBVH;   // allEntries = &updateCausticPhotons
causticPhotons = std::move(updateCausticPhotons); // ...leaves ptr=nullptr
```

`SpillableArray::operator=(SpillableArray&&)` (`spillablearray.h:53-59`)
steals `ptr`/`count` and leaves the source with `ptr = nullptr`. So the
live `causticPhotonsBVH->allEntries` points at the emptied
`updateCausticPhotons`, and the next eye-path leaf visit does
`nullptr[entryIndex]` → SEGV (crash address ≈ `entryIndex*sizeof(Photon)`
matches: `0x1265e8 ≈ 25k entries × 48B`).

## Why the pre-existing scenes don't trip it

The crash needs (a) a swapped-in non-null BVH, i.e. the update must
produce >= 1 caustic photon, and (b) a subsequent `ConnectWithCausticPaths`
on a surface/volume BSDF:

- `cornell-vol-caustic-ms.scn` has no diffuse receiver at all → no surface
  caustic queries → never dereferences the dangling array.
- `caustic-roughglass.scn` @ updatespp=4 survived locally — most likely
  its update re-trace stores 0 photons (update BVH stays null, the
  `else if (causticPhotonsBVH)` guard in `ConnectWithCausticPaths` holds).

All e90 scenes have matte receivers + lights that reliably produce caustic
deposits, so they hit the dangling pointer within ~1 update period.

## Fix applied

`IndexBvh` gained `SetEntries(const SpillableArray<T> *)` and
`ApplyPendingUpdate` rebinds the adopted BVH to the live array right
after the move:

```cpp
causticPhotons = std::move(updateCausticPhotons);
if (causticPhotonsBVH)
    causticPhotonsBVH->SetEntries(&causticPhotons);
```

Node entry *indexes* stay valid across the move (the contents moved
along); only the container address went stale. `PGICBeamIndex` stores
entry indexes instead of a container pointer, so beams never had the
hazard.

Takeaway: `IndexBvh` holds an *object* pointer, not a data pointer —
any move/swap of the backing `SpillableArray` must be followed by
`SetEntries` on every BVH built over it.

## Repro

```
python3.13 dev-tools/e90_caustic_stress_test.py
# or any single scene: PATHCPU, path.photongi.caustic.enabled=1,
# path.photongi.caustic.updatespp=4
```
