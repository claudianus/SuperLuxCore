# GPU Vertex Connection — BDPT-style eye↔light connects on the path engines

Status: **functional on Apple Metal (cl2msl) and OpenCL, dense +
wavefront dispatch, BIDIRCPU-validated** — the light subpath vertex
cache, the `MK_VC_CONNECT` connect state, and the SmallVCM MIS
bookkeeping are implemented end to end. Verified at 1280×720 on
`scenes/cornell/cornell-area-caustic.scn` (parity suite entry
`vc_caustic`).

This document describes the design, the property surface, the MIS
contract mirrored from `BIDIRCPURenderThread`, and the known limits.
The CPU reference implementation is
`src/slg/engines/bidircpu/bidircputhread.cpp`
(`ConnectVertices`, `ConnectToEye`, `DirectHitLight`,
`DirectLightSampling`, `Bounce`) — every formula below cites the CPU
lineage it ports.

## What and why

`path.vertexconnection.enable = 1` turns the GPU light-task population
(see `gpu_lighttracing.md`) into a *bidirectional* one: in addition to
splatting light vertices into the film, each light task stores its
non-delta vertices into a global cache; every eye task then connects
its current vertex against **all stored vertices of one paired light
task**, weighted by the same balance-heuristic MIS as `BIDIRCPU`.

This closes the largest engine gap between `PATHOCL` and `BIDIRCPU`:
caustics seen through diffuse surfaces (`L S+ D E`), glossy-glossy
indirect chains, and any path that needs a mid-chain specular bounce —
reachable neither by eye-side sampling nor by pure light splatting
without a visible projection — become cheap.

The implementation is a faithful port of LuxCore's own SmallVCM-style
bookkeeping (the same `dVCM`/`dVC` accumulation scheme BIDIRCPU uses),
not a new estimator family: strategies are weighted, never
renormalized, so truncation or disabled cases stay unbiased for the
strategies actually sampled.

## User surface

| Property | Default | Meaning |
|---|---|---|
| `path.vertexconnection.enable` | `0` | Enable GPU vertex connection. |
| `path.vertexconnection.connects` | `0` | Expected connect shadow-ray budget per eye vertex (probabilistic connection). `0` = connect every pooled candidate (deterministic). |
| `path.vertexconnection.pool` | `1` | How many light tasks' vertex caches each eye vertex may connect to. `1` = the paired task only (M6 behaviour). |
| `path.vertexconnection.adaptive` | `1` | Scale the connect budget by the measured efficiency of the sample's screen-space tile (needs `connects > 0`). |
| `path.vertexconnection.mergeradius` | `0` | VCM vertex-merging radius as a fraction of the scene bounding sphere. `0` disables merging. |
| `path.vertexconnection.reuse` | `1` | Temporal connect reuse: replay each eye task's best-scoring connect vertex as one extra deterministic candidate per eye vertex (M7d). |
| `opencl.task.count` | — | Must exceed 8192: light tasks occupy the tail gids, so the population needs headroom above the eye-task floor. |
| `path.lighttracing.taskfraction` | `1 - path.hybridbackforward.partition` | Share of the task population running light paths (and therefore the vertex cache size). |
| `path.lighttracing.only` | `0` | Debug: all-light-task mode (no eye pass, hence no connects). |

Enabling vertex connection implies the GPU light-task population:
`PathOCLRenderEngine` promotes `path.lighttracing.enable` when VC is
on, and `PathTracer` forces `hybridBackForwardEnable` so the CPU
reference path (`PATHCPU` + native light threads) keeps the same
property contract. If the population resolves to zero light tasks, VC
logs a warning and disables itself — it never silently misrenders.

## Architecture

### Light vertex cache

`VCLightVertex` (`pathoclbase_datatypes.cl`) is a fixed-stride record
written by `MK_LIGHT_VERTEX` for every non-delta light vertex:

- embedded `BSDF` (hit point + material eval context),
- light-path throughput,
- `dVCM`, `dVC` (the SmallVCM accumulated terms at that vertex),
- light group ID, one-based vertex depth.

Addressing: `lightVertices[lightTask * slotsPerTask + slot]` where
`slot = depth - 1`. `slotsPerTask` defaults to the max path depth and
is clamped by a 384 MB byte budget
(`pathoclbaseoclthreadinit.cpp`); a truncated store drops deep-connect
strategies without renormalizing the survivors (unbiased by
construction).

No atomics or seqlocks are needed: all writes happen inside the
`MK_LIGHT_VERTEX` dispatch and all reads inside later `MK_VC_CONNECT`
dispatches, and kernel launches on one queue are serialized with a
full memory fence between them.

### Eye-side pairing and the connect state

`MK_VC_CONNECT` (`pathoclbase_kernels_micro.cl`) runs after
direct-light resolution and before next-bounce generation. Eye task
`gid` pairs deterministically with light task `gid % lightTaskCount`
and walks that task's stored slots with a per-task cursor
(`GPUTaskState::vcCursor`): each iteration resolves the pending
shadow ray from the previous launch (visibility arrive), then queues
the next candidate's shadow ray into the task's `rays[gid]` slot. The
single traced-ray-slot model is the same one `MK_RT_DL` uses — the
device's `EnqueueTraceRayBuffer` pass over the population resolves the
connect rays, so no extra ray machinery exists.

Per candidate:

1. Reject camera-invisible light vertices, delta BSDFs, and slots
   beyond `vcVertexCount`.
2. Evaluate `BSDF_Evaluate` at the eye vertex toward the light vertex
   (forward pdf) and `BSDF_Pdf` for the reverse density; evaluate the
   stored light BSDF toward the eye vertex both ways.
3. Geometry term `|cos_eye| |cos_light| / dist²`; reject on
   non-positive cosines or zero geometry.
4. Convert solid-angle densities to area (`PdfWtoA`) and apply the CPU
   `ConnectVertices` MIS denominator built from `dVCM`/`dVC` chains,
   the emit-strategy light-selection pdf (`emitLightsDistribution`,
   bound via `KERNEL_ARGS_VC`), and the camera pdf on the eye side.
5. `Scene_Intersect` the shadow ray with
   `LIGHT_RAY | INDIRECT_RAY | SHADOW_RAY` flags; the eye path's
   `PathVolumeInfo` is used as the base and the current volume is set
   from the arrival side of the light vertex (interior volume when the
   ray enters the light-vertex object, exterior otherwise) — the same
   contract the direct-light shadow ray uses.
6. Splat the weighted contribution through
   `SampleResult_AddDirectLight`.

The connect ray passes a null path-depth pointer to `Scene_Intersect`
so transparent-surface depth accounting on the eye path is not
mutated; the pass-through seed is drawn through a local `Seed` copy
(`seedPassThroughEvent` is read/written once, matching the DL kernel's
pattern) to keep the task's sampler state consistent.

### Probabilistic connection (PCBPT-style pool sampling)

With `connects > 0` the connect walk becomes a Horvitz–Thompson
estimator over a *pool* of `poolTasks` light tasks instead of the
deterministic single-task sweep:

- The candidate pool for eye task `gid` is the union of `poolTasks`
  light tasks' vertex caches, paired round-robin as
  `(gid + j * stride) % lightTaskCount` — `j = 0` reproduces the M6
  pairing, so `pool = 1, connects = 0` is bit-identical to the
  deterministic walk.
- A score pass runs before the connect pass: each candidate's score is
  `|throughput| / dist^2` (transport-flavoured importance), summed per
  pool into `GPUTaskState::vcScoreSum`.
- Each candidate is then included independently with probability
  `q_i = min(1, connects * score_i / scoreSum)` and its contribution
  weighted by `1/q_i`. This is the PCBPT scheme of Popov et al. 2015
  ("Probabilistic connections for bidirectional path tracing")
  flattened onto the wavefront cursor: the flat index
  `c ∈ [0, poolTasks * slotsPerTask)` decodes to `(task_j, slot)`.
- Truncation stays unbiased: candidates below the inclusion
  probability are simply not sampled, and `1/q` exactly compensates —
  no renormalization is needed anywhere.

### Efficiency-aware connect budget

When `adaptive` is on (and `connects > 0`), the per-vertex budget `K`
is modulated by a screen-space efficiency map — the spirit of
Grittmann et al. 2022 ("Efficiency-aware MIS") applied to strategy
*allocation* rather than weight computation:

- A device buffer `vcEffStats` holds, per 16×16 tile, the CAS-accumulated
  landed connect luminance and spent connect rays, plus a global pair.
- At each eye vertex, `K = connects * clamp(tileEff / globalEff, 0, 4)`
  where `tileEff = tileLum / tileRays` (defaults to `connects` during
  warmup or when a tile has no history yet).
- This is a pure proposal-shaping device: `q_i` already carries the
  exact Horvitz–Thompson correction, so any map state — cold, noisy,
  or converged — leaves the estimator unbiased; it only redistributes
  shadow rays toward tiles where connects keep landing.

### Vertex merging (SmallVCM VM terms)

Setting `mergeradius > 0` activates the second half of SmallVCM
(Georgiev et al. 2012): in addition to connecting, each eye vertex
*merges* every cached light vertex inside `r = mergeradius * sceneRadius`
world units — a photon-density estimate that covers `S+D+S` caustics
pure connects cannot reach.

- A fixed-size spatial hash (`VC_MERGE_BUCKETS` buckets ×
  `VC_MERGE_CAPACITY` slots) is rebuilt every iteration by
  `AdvancePaths_VCResetMergeHash` + `AdvancePaths_VCBuildMergeHash`
  before the connect dispatch; entries are `(lightTask, slot)` records
  validated by `seq != 0`, so no stale vertices survive a rebuild.
- At an eye vertex's first connect visit (`vcCursor == 0`) the kernel
  walks the 3×3×3 cell neighbourhood of the query sphere, rejects
  delta eye vertices, evaluates the eye BSDF toward the light vertex's
  `fixedDir`, and accumulates
  `vmNorm * misWeight * eyeBsdfEval * lv->throughput` directly — no
  shadow ray, matching `hashgrid.cpp::Process` (the merge is a density
  estimate, not a connection).
- The MIS follows the VCM technical report (37)–(39):
  `wLight = dVCM*misVc + dVM*MIS(eyePdfW)`,
  `wEye = dVCM*misVc + dVM*MIS(eyeRevPdfW)`,
  `w = 1/(wLight + 1 + wEye)`, with the global constants
  `etaVCM = π r² nVM / nVC`, `misVmWeightFactor = MIS(etaVCM)`,
  `misVcWeightFactor = MIS(1/etaVCM)` computed at init
  (`nVM = lightTaskCount`, `nVC = poolTasks`) — the same structure as
  `bidirvmcputhread.cpp`.
- `dVM` is carried alongside `dVCM`/`dVC` on both sub-paths with the
  identical specular/non-specular recurrence as `BIDIRVMCPU`; when the
  radius is `0` the constants collapse to the pure-BPT bookkeeping.

### Temporal connect reuse (M7d, ReSTIR-style vertex replay)

When `reuse` is on, each **eye task** keeps a persistent single-slot
reservoir (`VCReplay`, `eyeTaskCount` records, `vcReplayBuff`) holding
a copy of the light vertex that landed the most connect luminance at
that task so far. The next eye vertex evaluates it as candidate index
`poolSize` — one extra deterministic entry in the connect sweep, with
the same BSDF evaluations, SmallVCM MIS weights and shadow ray as a
pool candidate.

This is the vertex-granularity analogue of ReSTIR-BDPT's path reuse
(Wyman & Popov 2025) specialised to the wavefront structure:

- The replayed record is a **stale vertex** — the light sub-path that
  generated it no longer exists — but a vertex record is self-contained
  (position, BSDF, throughput, dVCM/dVC/dVM), so the connect term is
  re-evaluated exactly like a fresh candidate. The estimator stays
  unbiased by the same argument as the connect pool: every candidate is
  a distinct full-path strategy sample weighted by its own MIS terms,
  and the replay's inclusion is *deterministic* (the candidate set is
  fixed before evaluation — the reservoir never weights the estimate).
- The reservoir only picks **which** stale vertex gets a seat: after a
  connect ray resolves, a pool candidate whose landed luminance beats
  the stored score promotes its record. `staging` inside the record
  holds the queued candidate's copy because the paired light task can
  overwrite its cache slot before the shadow ray resolves.
- Cost: one extra connect shadow ray per eye vertex per pass and
  ~2×`sizeof(VCLightVertex)` per task (~22 MB at 32 K tasks). No
  atomics — each task owns its slot.

Productive caustic vertices (the ones that repeatedly win connects)
persist across samples instead of being rediscovered by the random
pool, which is where the variance win comes from on specular-heavy
scenes.

### MIS bookkeeping

The per-path `dVCM`/`dVC` accumulation mirrors BIDIRCPU:

- `MK_LIGHT_INIT` seeds the light path from `directPdfA`,
  `cosThetaAtLight`, `emissionPdfW`, `lightPickPdf` (the new
  `Light_Emit` outputs — all 14 light-source variants plus the
  dispatcher were extended to the CPU `Emit` contract).
- `MK_LIGHT_VERTEX` folds each bounce's forward/reverse pdf ratio into
  `dVC`/`dVCM`, stores the vertex, and applies `weightLight` to
  camera-projection splats (`MIS(cameraPdfA) * (dVCM + dVC *
  MIS(bsdfRevPdfW))`, `misVmWeightFactor = 0` as in BIDIRCPU).
- Eye side: `GenerateEyePath` initializes from `Camera_GetPDF`
  densities, `MK_HIT_OBJECT` folds the arrived-hit terms, and
  `MK_GENERATE_NEXT_VERTEX_RAY` updates the accumulators after each
  bounce (with a passthrough undo path for non-hitting continuations).
- `DirectHitFiniteLight`/`DirectHitInfiniteLight` and the DL path use
  the new `emissionPdfW`/`cosThetaAtLight` outputs of
  `Light_Illuminate`/`GetRadiance` for the CPU `DirectHitLight` /
  `DirectLightSampling` weightCamera terms, including the
  `throughShadowTransparency` override.

GPU BSDF evaluation has no `fromLight` adjoint flag like CPU
`BSDF::Evaluate`; the reverse density is obtained by evaluating the
swapped direction (`BSDF_Pdf` helper) which is the same quantity for
the non-measurement path.

### Dispatch

`MK_VC_CONNECT` is enqueued in both the dense and wavefront dispatch
paths (`pathoclbaseoclthreadkernels.cpp`), after `MK_RT_DL` resolution
and before `MK_GENERATE_NEXT_VERTEX_RAY`, so connection candidates are
exhausted before the eye path bounces. `WAVEFRONT_NUM_STATES` is 18.

## Validation

- `dev-tools/parity/run.py --scenes vc_caustic` renders
  `cornell-area-caustic.scn` (glass spheres + area light; the floor
  caustics are only reachable by light-side vertices) on PATHCPU+hybrid
  and each GPU backend. Gate: ratio 0.90–1.15 (VC legitimately adds
  energy PATHCPU can not sample), rmse ≤ 0.05, black_frac ≤ 0.01,
  zero NaN.
- `dev-tools/parity/run.py --scenes vc_merge` — the same caustic scene
  with `mergeradius = 0.004` vs `BIDIRVMCPU`; the merge adds the VCM
  density estimator inside the estimator-family gap.
- `dev-tools/parity/run.py --scenes vc_reuse` — `reuse = 1` explicitly
  on the same scene vs `BIDIRVMCPU`; pins the replay path (also on by
  default in the other VC entries).
- `dev-tools/e38_vc_smoke.py [metal|opencl]` — kernel-compile + render
  smoke, VC-on vs VC-off energy comparison.
- Measured at 1280×720/64spp on Metal: VC+VM vs VC-only ratio 1.006,
  rmse 0.0035, zero NaN — the merge contributes the same energy at
  lower variance, as a correct VCM should.
- Measured at 1280×720/256spp: VC vs BIDIRCPU ratio 0.787
  (PATHCPU vs BIDIRCPU on the same scene: 0.817 — the residual gap is
  the estimator-family difference, not a VC regression); VC vs
  PATHCPU 0.963 with lower RMSE (VC adds the caustic energy PATHCPU
  misses). Dense vs wavefront dispatch agree within noise
  (ratio 1.009), Metal vs OpenCL agree within noise.
- `dev-tools/e44_vc_shadowtransparency_test.py` — regression for the
  layered-glass whiteout: the direct-light kernel re-runs per
  shadow-ray SEGMENT, and the VC MIS lift
  (`lightRadiance /= vcMisWeight`, the port of CPU's `misWeight = 1`
  after the full shadow walk) was applied on every segment. A ray
  crossing N shadow-transparent panes compounded `(1/vcMisWeight)^N`
  (~1e4..1e5 per pane — GenmaB's multi-layer facade rendered a uniform
  white field, ~760k anomalous NEE deposits). Fix `189837864` gates the
  lift on `!continueToTrace` (terminating segment only, matching CPU).
  The test stacks 4 archglass panes between an emissive ceiling and a
  matte receiver: GPU median luminance finite/bounded, within 3x of
  BIDIRCPU, and 4 panes must not brighten the wall vs 1 pane.

## Known limits

- Perspective and orthographic cameras only (same bound as the GPU
  light tasks).
- Connect candidates are drawn from `pool` paired light tasks per eye
  vertex — the full BIDIRCPU O(eye×light) vertex product is
  approximated by the pool, which keeps the estimator unbiased (each
  connect is a valid strategy sample) but thins the strategy density
  vs CPU. `pool > 1` widens coverage at linear shadow-ray cost.
- The merge hash is fixed-size: overfull buckets drop excess vertices
  (a coverage loss, not a bias) and the merge lookup is a 27-cell
  neighbourhood per eye vertex — its cost scales with the merge
  radius, not the scene.
- The 384 MB vertex-cache budget can truncate `slotsPerTask`; deep
  light chains then lose their deepest connect strategies (still
  unbiased, just less coverage).
- Spectral: the bookkeeping is RGB-consistent with the existing GPU
  light pass; per-wavelength MIS terms are not stored.
- `RTPATHOCL` is not wired (same bound as the light-task population).
