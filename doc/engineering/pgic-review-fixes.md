# PhotonGI review fixes — concurrency invariants

Fixes applied after the strict code review of the C1–C8/B1′ caustic
work (commits `27353b84d`, `f08766a75`, `93f9d7f6f`). The patterns
below are invariants — violating them re-introduces the bugs.

## Live vs shadow fields (PhotonGICache)

The deferred-update design keeps TWO copies of generation state:

| Live (query-visible) | Shadow (worker-private) |
|---|---|
| `causticPhotons` | `updateCausticPhotons` |
| `causticBeams` | `updateCausticBeams` |
| `causticBeamsIndex` | `updateCausticBeamsIndex` |
| `causticPhotonTracedCount` | `updateCausticPhotonTracedCount` |
| `causticPhotonsBVH` | `updateCausticPhotonsBVH` |

Rule: **the worker may ONLY write `update*` fields.** Render threads
read live fields outside the barrier (e.g. `ConnectCausticBeams`
normalizes by `causticPhotonTracedCount`); a worker writing the live
member mid-update is both a data race and a visible brightness skew
until the swap lands. `TracePhotons()` takes destination pointers +
a `dstCausticTracedCount` ref for exactly this. The traced count is
seeded from the live value at launch (ingest generations accumulate)
and published by `ApplyPendingUpdate()` inside the barrier completion.

## updateThread lifecycle

`updateThread` (std::jthread) has three access sites: create
(`Update`, thread 0), join+reset (`ApplyPendingUpdate`, barrier
completion), join+reset (`FinishUpdate`, EVERY render thread at
shutdown). `updateThreadMutex` serializes all three.

Ordering subtlety in `ApplyPendingUpdate`: `updateThread.reset()`
runs BEFORE `updateInFlight = false`. Clearing the flag first lets
thread 0 launch a new worker between the two statements; the
completion step would then join the NEW jthread and park the whole
barrier for a full trace duration.

`FinishUpdate`'s own barrier dance can also run the completion step,
which is why thread-0 gating alone was insufficient — the mutex is
the synchronization point, not the thread index.

## threadTaskConfig (per-thread GPUTaskConfiguration)

`PathOCLBaseOCLRenderThread::threadTaskConfig` is snapshotted from
`renderEngine->taskConfig` at the top of `InitGPUTaskBuffer()` —
after `InitGPUTaskConfiguration()` (engine side, pre-spawn) and before
any per-thread mutation. Rules:

- **All post-snapshot reads/writes use `threadTaskConfig`** — kernel
  arg setup, enqueue gating (`vertexConnect.vertexCount`,
  `restir.visCandCount`, `restirGI.giCandCount`), VCM merge schedule,
  PGIC refresh, deposit retire. `InitFilm` and `InitPhotonGI` are the
  only exceptions (they run BEFORE the snapshot and read immutable
  compiled-config fields from the engine struct).
- The init code writes device-dependent values (`slotsPerTask`,
  `vertexCount`, `visCandCount` clamps, ray-tail offsets). On the
  shared struct these were both a race and could ship a different
  device's `taskCount`-derived offsets.
- Uploads to `taskConfigBuff` from `threadTaskConfig` use `CL_TRUE`:
  a `CL_FALSE` DMA could read the mutable source while the same
  thread writes the next field (torn config).
- PGIC refresh preserves `depositEnabled`/capacities across
  `pgicCfg = compiledPathTracer.pgic` (the compile struct resets them).

## Deposits are PATHOCL-only

`pgicDepositWanted` requires `(GetType() == PATHOCL)`: only
`pathoclopenclthread.cpp` calls `DrainPGIC()`. TilePathOCL/RTPATHOCL
run `MK_LIGHT_*` kernels (light tracing, VC) but never drain, so
deposit mode there would fill device buffers nobody reads while
`ingestOnly` disables the CPU re-trace — an empty caustic cache
forever. `lightTaskCount = Min(taskCount - 8192u, …)` is guarded
against `taskCount <= 8192` in both engines (unsigned wrap made
`eyeTaskCount` ~4G → zero light tasks but ingest still armed).

## CPU/GPU parity notes

- `PhotonGICache_IsDirectLightHitVisible` must mirror
  `IsDirectLightHitVisible` — it now calls the canonical
  `EyePathInfo_IsCausticPath` (const-qualified signature) so the
  `depth.depth > 1` rule can't drift.
- `directPdfW == 0` must yield zero contribution in BOTH
  `PGICPhotonBvh::ConnectCacheEntry`/`pgic_funcs.cl` (volume branch)
  and `ConnectCausticBeams`/`PGICBeamBvh_ConnectAllNearEntries`.
- `PhotonBeam(p0 == p1)` keeps `d = 0` (not NaN); the overlap test
  culls it via `length == 0`.

## Verification

- `dev-tools/parity-regression.sh`: 4/4 PASS (CPU + Metal).
- `dev-tools/e90_caustic_stress_test.py`: all PASS — CPU/GPU means
  within ~2% on 5 scenes; progressive vs one-shot pgic within ~1%.
- `dev-tools/e54_media_caustic_test.py`: 8/8 gates PASS incl. GPU
  beam/point agreement (0.0079 vs 0.0070) and partition disjointness.
- 720p GPU visual: focused-caustic-ring renders a correct ring
  caustic, no NaN/black output.
