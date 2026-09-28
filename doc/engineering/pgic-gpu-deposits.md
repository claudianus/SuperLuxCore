# GPU photon deposits (B1′) — implementation notes & gotchas

PhotonGI caustic generation on GPU: light tasks double as photon
paths. Feature doc: `doc/features/progressive-caustics.md` (B1′).
Regression: `dev-tools/e54_media_caustic_test.py` (gate 4c/4d),
`dev-tools/e90_caustic_stress_test.py`.

## Pipeline

```
MK_LIGHT_VERTEX (kernel)
  lpi->isNearlyS && lpi->firstVertSeen            (pre-vertex state)
  -> atomic_inc(pgicDepositCounters[k])           (append slot)
  -> pgicDepositPhotons[] / pgicDepositBeams[]    (ocl::Photon / PhotonBeam)
DrainPGIC (render thread, queue-synced film window + update boundary)
  -> read counters+records, reset cursors
  -> PhotonGICache::IngestTracedPhotons()         (staging, mutex)
UpdateWorker (background, ingestOnly mode)
  -> absorb staging into shadow buffers under ONE combined
     photon+beam room = maxSize - (existing photons + beams)
  -> rebuild photon BVH + beam index
ApplyPendingUpdate (barrier completion)
  -> swap + BVH SetEntries rebind + RecompilePhotonGI
render loop (every thread, pass-counter detection)
  -> refresh taskConfig.pgic (preserving deposit fields) + upload
  -> InitPhotonGI() + SetKernelArgs()
```

## Key gotchas found while building this

### Apple kernel-argument limit

Kernels with many buffer arguments can fail to translate/dispatch on
Apple OpenCL-on-Metal even when the feature is off. Deposit buffers are
therefore bound ONLY on `MK_LIGHT_VERTEX` via the
`KERNEL_ARGS_PGIC_DEPOSIT` tail (same reason `KERNEL_ARGS_LIGHT`
exists). `SetAdvancePathsLightKernelArgs` returns the next arg index so
the tail binds right after it. `MK_LIGHT_INIT` does NOT get the tail.

### Metal shared-storage buffers

Metal buffers live in shared storage — the host "read" is a
`FinishQueue` + memcpy of a heap pointer, so the small synchronous-copy
limit does not apply to deposit drains. That is what makes the
piggyback design free of DMA constraints.

### taskConfig fields go stale across generations (fixed)

`CompilePhotonGI` rewrites `compiledPathTracer.pgic`
(`causticPhotonTracedCount` normalization, `causticLookUpRadius`) at
every swap, but `taskConfig.pathTracer.pgic` — what the device actually
reads — was only copied once at `InitGPUTaskConfiguration`. Devices
therefore normalized by the generation-1 traced count and radius
forever. The render loop now refreshes `taskConfig.pgic` from the
compiled copy after each swap (per-thread, detected via
`GetCausticPhotonPass()`), preserving the three deposit session fields
`CompilePhotonGI` resets.

### Second OCL thread kept a stale cache (fixed)

`Update()` returns true only for `threadIndex == 0`, and only that
thread ran `InitPhotonGI()`/`SetKernelArgs()`. With 2 OpenCL render
threads on one device (the default on Apple Silicon), thread 1's device
buffers held generation-1 cache data forever — a real CPU/GPU-visible
divergence. The pass-counter refresh fixes it: every thread re-uploads
its own device state when the pass bumps.

### Ingest staging vs. shadow-buffer lifetime

`IngestTracedPhotons` appends into mutex-guarded staging vectors; the
update worker moves them into `updateCausticPhotons`/`updateCausticBeams`
at build time. In `ingestOnly` mode `Update()` seeds the shadow buffers
from the LIVE arrays each generation (they accumulate), which is why
`ApplyPendingUpdate` had to rebind the BVH (`IndexBvh::SetEntries`) —
see `e90_pgic_update_crash.md`.

### Volume index bridging

Device beams store the `mats[]` array index; `IngestTracedPhotons`
resolves it back through `scene->GetMaterials().GetMaterial(idx)` +
`dynamic_cast<const Volume *>` before creating the CPU `PhotonBeam`.
Non-volume entries are dropped (defensive — the kernel only emits for
`mats[idx].type == HOMOGENEOUS_VOL`).

### Deposit-task retirement

`DrainPGIC` clears `taskConfig.pgic.depositEnabled` and re-uploads
`taskConfigBuff` once the combined cache is full
(`IsCausticFull()` — photons+beams vs `maxSize`, CPU semantic) or
4,194,304 light paths produced nothing (mirrors the CPU tracer's
empty-scene early-out). `MK_LIGHT_INIT` then parks the tail tasks in
`MK_DONE` — they stop tracing entirely, not just stop appending.

### Deposit buffers persist across updates

`InitPhotonGI()` runs per generation; the append buffers are allocated
once (capacity is session-fixed) so records written between drains are
never dropped to a realloc. Only when `depositEnabled` flips off do they
free.

### luxcoreconsole vs pysuperluxcore freshness

`ninja -C out/build -f build-Release.ninja pysuperluxcore` relinks the
Python module but NOT `samples/luxcoreconsole/Release/luxcoreconsole` —
console tests silently ran a day-old binary until the target was built
explicitly. `LuxCore` is a symlink to `SuperLuxCore`, so
`CMAKE_HOME_DIRECTORY` pointing at `.../LuxCore` is the same tree.
