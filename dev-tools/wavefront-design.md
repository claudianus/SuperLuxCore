# Wavefront completion + λ-alignment (B2 / E3)

Status: **M1 implemented** (opt-in, `LUXRAYS_WAVEFRONT_QUEUES=1`;
validated on OpenCL + Metal — see "M1 status" below). M2/M3 pending.
Related: roadmap Phase B item B2, engine goal E3.

## Current state

`PATHOCL` on GPU already runs a **micro-kernel state machine**
(`include/slg/engines/pathoclbase/kernels/pathoclbase_kernels_micro.cl`):
per-iteration, `EnqueueAdvancePathsKernel()` launches 11 kernels —
`MK_RT_NEXT_VERTEX`, `MK_HIT_NOTHING`, `MK_HIT_OBJECT`, `MK_RT_DL`,
`MK_DL_ILLUMINATE`, `MK_DL_SAMPLE_BSDF`, `MK_MNEE_NEXT_VERTEX`,
`MK_GENERATE_NEXT_VERTEX_RAY`, `MK_SPLAT_SAMPLE`, `MK_NEXT_SAMPLE`,
`MK_GENERATE_CAMERA_RAY` — each over the **full `taskCount` range**.
Each kernel reads `tasksState[gid].state` and early-outs on mismatch.
Tasks recycle forever (`...→ MK_NEXT_SAMPLE → MK_GENERATE_CAMERA_RAY
→ MK_RT_NEXT_VERTEX → ...`), so the alive set ≈ `taskCount` at all
times. `MK_DONE` + `RAY_FLAGS_MASKED` only appear in
TILEPATHOCL/RTPATHOCL.

Per iteration the host loop (`pathoclopenclthread.cpp`) does:

1. `EnqueueTraceRayBuffer(rays, rayHits, taskCount)` — one launch over
   every task slot; masked/inactive rays exit inside the RT kernel but
   still occupy lanes.
2. 11 MK kernel launches, each scanning `taskCount` slots.
3. `DrainGuide()` (path-guiding record drain).

## What wavefront adds here (and what it doesn't)

Already realised by the MK split: per-kernel register allocation /
occupancy (each MK kernel is a separate `__kernel`), no mega-kernel
divergence in the executed work.

Not yet realised:

1. **Per-state launch sizing.** Each MK launch currently scans all
   `taskCount` slots and discards non-matching ones. With per-state
   queues, kernel `S` launches over `count[S]` lanes only. Saves
   `taskCount × 11` state reads + queue-scan overhead per iteration.
2. **Queued ray dispatch.** Only tasks that produced a ray this
   iteration occupy RT lanes (index-remapped trace over a ray queue).
3. **λ-alignment.** Hero-wavelength 3-bin spectral transport: bucket
   queue append by `λ` bin so same-λ tasks land in adjacent lanes →
   coherent `spectralW` / bin progress per warp. No sort needed —
   bucketed append is O(1) per task.
4. **(Optional) material grouping.** Same bucketing by
   `bsdf.materialIndex` improves `mats[]` / eval-op cache locality.

## Design — per-state device queues (M1)

Buffers (device, allocated once at `taskCount`):

- `queueBuf[NUM_STATES][taskCount]` — uint task indices.
- `queueCount[NUM_STATES]` — uint counters (atomic).
- `rayQueue[taskCount]` + `rayCount` — indices of tasks whose
  `rays[taskIndex]` needs tracing this iteration.

Kernel protocol (replaces `taskState->state = X` transitions):

- Each MK kernel `S` is launched over `count[S]` lanes:
  `const uint taskIndex = queueBuf[S][gid];` — all task/rays/film
  indexing switches from `gid` to `taskIndex`. `taskState->state`
  remains the authoritative record.
- A kernel that transitions a task to state `T` executes
  `queueBuf[T][atomic_add(&queueCount[T], 1)] = taskIndex` (bucketed by
  λ bin once M3 lands — see below). Kernels that emit a ray also append
  to `rayQueue`.
- Host per iteration: read `queueCount[]` + `rayCount` (single small
  readback), launch `EnqueueTraceRayBuffer` over `rayCount` with
  `rayQueue` indirection, then launch each non-empty state kernel in
  the same topological order as today. Zero counters (tiny fill kernel
  or clear-on-read — see "Counter reset" below).
- Tasks produced into a state whose kernel already ran this iteration
  are processed next iteration — one state-step per task per
  iteration, i.e. a true wavefront (same convergence total, denser
  launches).

### Counter reset

Cheapest correct scheme: each state kernel, after processing its
queue, leaves the counter intact; the host-side snapshot for
iteration `i+1` then counts "queued but not yet consumed". Simpler
alternative used by Cycles-style implementations: keep **two counter
banks** (write-bank for appends, read-bank snapshotted by the host
kernel launch); a tiny `ResetCounters` kernel zeroes the write bank at
iteration start after the host has read the previous bank. Decide
during implementation; both are O(NUM_STATES) work.

### RT indirection

`EnqueueTraceRayBuffer(rayBuff, hitBuff, count)` currently assumes
`rays[gid]`/`rayHits[gid]` dense. Add an optional index-buffer variant
(`EnqueueTraceRayBufferIndexed(rayBuff, hitBuff, indexBuff, count)`)
to `HardwareIntersectionDevice` — implemented for the OpenCL and Metal
paths (CUDA/OptiX paths keep dense fallback: pass identity or keep
dense launch while the queue path is OCL/Metal-only initially, behind
a flag).

### SampleResult / film indexing

`sampleResultsBuff[gid]` and per-pixel film splat use the task index —
with `taskIndex` indirection this stays correct automatically, since
splat resolves through `eyePathInfos[taskIndex]` (pixel indices), not
the slot. Verify `filmNoise`/denoiser per-pixel fields too.

## M2 — λ-aligned append

- Store the task's sampled λ bin index in `GPUTaskState` (already
  carried in `SampleResult`/spectral state — check `spectralW`
  plumbing; add a `u_char lambdaBin` if not persisted at task level).
- Queue layout per state: `NUM_λ` sub-buckets
  (`queueBuf[S]` split into λ segments with per-bucket counters, or a
  per-state bucketed append: `bucketBase[S][λ]` + counter array).
  Append computes `λ` once — near-zero cost vs. a sort.
- Effect: warps become λ-coherent → `spectralW` uniform per warp, and
  the 3-bin spectral accumulators converge in lockstep, cutting
  spectral sample variance (see ROADMAP E3 argument).

## M3 — material grouping (optional, eval only after M1/M2 measured)

Same bucket mechanism keyed on `taskState->bsdf.materialIndex`
(low-bits bucket, e.g. 8 buckets). Only if profiling shows material
fetch incoherence.

## Risks / invariants

- **Iteration semantics**: with snapshot counters, a task advances
  ≥1 state per iteration instead of potentially several — convergence
  per-sample is identical; per-iteration latency changes only.
- **Ordering**: MK_SPLAT_SAMPLE does film writes — wavefront preserves
  per-task order, so film correctness holds; verify TILEPATHOCL tile
  ownership unchanged (tiles still own their task slots).
- **MNEE sub-state machine** (`MK_MNEE_NEXT_VERTEX` ↔ RT): the RT
  round-trip is already queue-shaped (needsTrace flag). Keep the
  existing ping-pong; it maps naturally onto ray queue + its own state
  queue.
- **PhotonGI / DLS / ELVC / ReSTIR / path-guiding drains**: these read
  task arrays by slot — indirection is contained to kernels; drains run
  on `taskStatsBuff`/record buffers unchanged.
- **Flag gate**: compile-time `WAVEFRONT_QUEUES` (kernel arg +
  `-D` define) + runtime `renderengine` property
  (`pathocl.wavefront.queues = 0/1`) so PATHOCL keeps a proven fallback
  — required for A/B benchmarks and upstream reviewability.

## Validation plan

- Pixel-parity: `scenes/parity/*` + cornell/luxball set — PATHOCL
  queues on/off must produce identical images (same seed schedule;
  state-step reordering must not alter RNG consumption per task — the
  per-task `Seed` is stored in `tasks[]`, so this holds by
  construction).
- Perf: sample/s and iteration latency on Metal + OCL, low-spp and
  high-spp regimes (queue wins grow with render duration).
- λ-coherence metric: average λ-bin run-length in queue order
  (debug counter) + spectral AOV variance on dispersive scene.

## Milestones

| M | Scope | Gate |
|---|---|---|
| M1 | Queue buffers + counter plumbing, all 11 MK kernels indexed via `taskIndex`, indexed RT dispatch (OCL+Metal), runtime flag | PATHOCL only, TILE/RTPATH keep dense path |
| M2 | λ-bucketed append + coherence metric | ✅ implemented (see below) |
| M3 | Material bucketing (eval) | M2 measured |

## M1 status (implemented, opt-in)

What landed, vs. the table above:

- **Done**: `taskQueueBuff` (`WAVEFRONT_NUM_STATES × taskCount` u32)
  + `taskQueueCountBuff` allocated only under
  `LUXRAYS_WAVEFRONT_QUEUES=1`; `AdvancePaths_BuildQueues` refills
  queues once per iteration from `taskState->state` (authoritative
  source — no per-transition instrumentation); each MK kernel launches
  over its compacted queue (count rounded up to `workGroupSize`;
  `WAVEFRONT_GUARD` exits overflow lanes before touching stale queue
  slots); `gid` is the task index — `SAMPLER_PARAM` threads it into
  every sampler/eval helper that touches task-persistent arrays;
  TILEPATHOCL/RTPATHOCL stay dense (their iteration model needs
  multi-state-per-iteration); `rayCount` accounting preserved
  (BuildQueues +1/task, MK_RT_NEXT_VERTEX's +1 gated to dense);
  `LUXRAYS_WAVEFRONT_DEBUG=1` dumps per-iteration queue integrity
  (oob/dup/badState counters).
- **Deferred**: RT dispatch is still dense (`EnqueueTraceRayBuffer`
  over `taskCount`); one host↔device sync per iteration for queue
  counts (cheap, but prevents GPU-only scheduling); per-state launches
  still serialize on the host queue.
- **Validated** (Apple M5 Pro, `scenes/cornell/cornell.scn`,
  512² / 32spp): OpenCL + Metal both compile all kernels incl.
  `AdvancePaths_BuildQueues`; wavefront renders converge to the same
  image as dense (byte diff = Monte-Carlo noise scale); WFDBG shows
  `oob=0 dup=0 badState=0` every iteration; SOBOL/RANDOM/METROPOLIS
  all render correctly; SAMPLECOUNT distribution matches dense (no
  checkerboard); dense path unchanged when flag off.
- **cl2msl fix** (`src/slg/utils/cl2msl.py`): `propagate_gid` is now
  idempotent — signatures already carrying `gid` (via
  `SAMPLER_PARAM_DECL` or explicit) are skipped; call-site `, gid`
  append detects a real trailing `gid` argument (depth-aware last-arg
  scan, previously dead `\)$` regex on a paren-less segment);
  dependency detection ignores block/full-line comments so dead
  debug comments no longer seed `gid` propagation.

Known semantics difference vs. dense (by design): a task advances at
most one state hop per iteration, so wall-clock iterations per sample
increase — per-sample results are identical. Enabling wavefront by
default needs an A/B benchmark pass first (M2 scope).

## M2 status (implemented, opt-in)

λ-bucketed append, keeping the queue memory flat:

- **Scheme**: two device passes + a host prefix inside the one
  existing per-iteration sync. `AdvancePaths_BucketHistogram` counts
  each live task into `taskQueueCount[state][λ]` (36 u32) and caches
  its hero-λ bin in `taskLambda[task]` (u32); the blocking count
  readback (unchanged cadence) lets the host exclusive-prefix
  λ-contiguous segment bases per state into `taskQueueBase` (36 u32,
  uploaded); `AdvancePaths_BuildQueues` then appends via an atomic
  cursor on `taskQueueBase`, so each flat per-state queue segment ends
  up λ0|λ1|λ2 grouped. No third kernel launch and no extra sync vs.
  M1; queue stays `NUM_STATES × taskCount`.
- **λ source**: `SampleResult::spectralHeroAlive` bits [3..4] (hero
  bin drawn per sample in `InitSampleResult`), clamped to
  `SLG_SPECTRAL_BINS-1`. Non-`SLG_SPECTRAL` builds bucket everything
  into λ0 — the layout degenerates to the M1 flat append order.
- **MK kernels**: unchanged except `WAVEFRONT_GUARD` bounds lanes by
  `Σλ taskQueueCount[state][λ]` (launch sizes come from host-side
  totals). `WAVEFRONT_GID` indirection untouched — λ grouping emerges
  purely from placement order.
- **Coherence metric** (WFDBG): per-iteration `badLambda` (queue
  entries whose cached λ ≠ their segment), `lamTrans` (λ transitions
  in launch order) and `lamRunLen` (queued/lamTrans). Measured on
  Apple M5 Pro: `badLambda=0`, `lamTrans` 1–18 per iteration vs.
  ~350k expected under random λ ordering, `lamRunLen` up to 524288.
- **Validated**: `scenes/cornell/cornell.scn` non-spectral renders
  identical to dense (λ degenerates to M1); `cornell-spectral.scn`
  (dispersive prism + laser, `path.spectral.enable=1`) at 128spp
  converges to dense statistics (identical mean/center-band, nz
  pixel count within Monte-Carlo scatter). OpenCL and Metal both
  compile `AdvancePaths_BucketHistogram` via cl2msl and render.
- **A/B benchmark** (Apple M5 Pro, PATHOCL, wall clock): dense vs
  wavefront across four scenes, best-of trailing `Avg. samples/sec`:

  | Scene | Divergence profile | Dense | Wavefront | Δ |
  |---|---|---:|---:|---:|
  | cornell.scn (512², 30s) | trivial | ~13.0M | ~10.8M | **-17%** |
  | classroom.scn (640×480, 30s) | interior, many materials | 1371 sp | 1277 sp | **~-7%** |
  | luxball-carpaint (640×480, 15s) | multi-lobe BSDF | ~8.0M | ~7.4M | **~-8%** |
  | cornell-spectral (512², 30s, λ on) | laser + prism dispersion | ~11.6M | ~9.8M | **~-15%** |

  Wavefront loses on every workload tested on this hardware. The
  per-iteration histogram + queue rebuild + host sync and the
  one-state-hop-per-iteration cadence cost more than the coherence
  gains buy at workgroup 64 — and the deficit shrinks with scene
  divergence (classroom -7% vs cornell -17%), consistent with the
  coherence model, but never inverts. λ bucketing (M2) does not help
  even on a dispersive scene because that scene is path-coherent
  anyway. **Wavefront stays opt-in**; enabling it by default would
  need a workload where it demonstrably wins (e.g. extreme material
  divergence, or a backend where divergence is costlier), or a
  redesign that removes the per-iteration host sync (fused
  histogram+place, persistent mega-kernel).
- **Deferred**: λ bucketing is currently all-or-nothing per state;
  a per-state λ-only launch split (extra parallelism when a state is
  dominated by one λ) is a possible follow-up, as is reusing the
  same histogram→prefix→place pipeline for M3 material buckets.

## Regression test

`dev-tools/wavefront-regression.sh` automates the dense-vs-wavefront
check: it renders `cornell.scn` (non-spectral, 64spp) and
`cornell-spectral.scn` (128spp) in dense mode and with
`LUXRAYS_WAVEFRONT_QUEUES=1 LUXRAYS_WAVEFRONT_DEBUG=1`, asserts all
queue-integrity counters (`oob`/`dup`/`badState`/`badLambda`) stay
zero, and requires the mean radiance of both outputs to agree within
20% (Monte-Carlo tolerance — RNG consumption order differs by design,
so pixel equality is not expected). Usage:

```
dev-tools/wavefront-regression.sh [path-to-luxcoreconsole]
```

Exit 0 = pass. Failure preserves logs under `/tmp/wf-regression-failed*`.
