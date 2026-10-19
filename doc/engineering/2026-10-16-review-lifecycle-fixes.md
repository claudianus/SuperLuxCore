# 2026-10-16 review: memory and render lifecycle

The workspace review in `../dev-tools/reviews/2026-10-16/findings.json`
identified ten defects and two initially unreproduced risks. The runtime
fixes preserve the transport estimator, sampling defaults and shader payloads.

- **F01/F09:** `SpillableArray` refreshes its data pointer after reserve.
  Byte spill is refused for non-trivially-copyable elements. CPU indirect
  radiance photons therefore remain on the heap; their owning
  `SpectrumGroup` cannot safely be copied into an mmap payload. Trivial
  geometry/GPU staging/point-photon arrays can still spill.
- **F02:** PATHCPU rebuilds PhotonGI before restarting edited render
  threads. Cached PATHOCL/TILEPATHOCL sessions rebuild cache, compiled
  scene and deposit state together. RTPATHCPU retains its separate
  camera-preserving edit protocol.
- **F04/R01:** Metal uses explicit MRC ownership for library/archive,
  descriptors and layout-probe temporaries. A readback retains its chosen
  command buffer under the tracking mutex, so a concurrent drain cannot
  destroy it before `waitUntilCompleted`.
- **F10:** orthographic footprint is measured from ray origins and does
  not scale with camera distance. Perspective differential behavior is
  retained; the radius probe also forwards the correct shutter end.
- **R02:** loaded PhotonGI payloads rebind synchronization to the current
  render thread count. Saving does not replace a live cache's barrier.
- **Additional archive regression:** `FilmSamplesCounts` allocates the
  deserialized thread count before loading slots and rebuilds the atomic
  total. The old loader wrote a multi-thread archive into its default
  one-slot allocation and reported a zero sample count. Existing archive
  field order/version is unchanged; pending batches restart empty.

## Regression commands

Run from this repository with bytecode disabled where practical:

```
python3 dev-tools/review_spill_test.py
python3 dev-tools/review_film_counts_test.py --baseline
python3 dev-tools/review_film_counts_test.py
python3 dev-tools/review_metal_lifetime_test.py
python3.13 dev-tools/review_lifecycle_test.py
python3.13 dev-tools/review_lifecycle_test.py edit-metal
python3.13 dev-tools/e94_serialize_roundtrip_test.py
```

The baseline counter test intentionally runs the original class from HEAD
under ASan and expects an overflow. The Metal interleaving test executes
the actual method bodies with a gated mutex and Objective-C test objects;
it is a deterministic lifetime regression, not a GPU driver stress test.
Native cache edits and paired 1280x720 Blender CPU/Metal images provide
separate real-device evidence. See the workspace fix report and logs for
the exact candidate hashes, outcomes and limits. These diagnostics make no
equal-error performance or general production-quality claim.
