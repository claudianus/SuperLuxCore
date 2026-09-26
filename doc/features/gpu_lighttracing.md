# GPU Light Tracing — camera-projection splatting on the path engines

Status: **functional on Apple Metal (cl2msl), CPU-validated** — phases 1
(host plumbing), 2a (device `LightSource_Emit` ports + emit distribution
incl. lasers), 2b (device `Camera_GetSamplePosition`/`Camera_GetPDF` for
perspective and orthographic cameras) and 3 (`MK_LIGHT_INIT`/`MK_LIGHT_VERTEX`
state machine, deferred camera-connection splats into
`RADIANCE_PER_SCREEN_NORMALIZED`, light-path sampler dimension layout
matching `PathTracer::RenderLightSample`, Sobol dimension expansion, dense +
wavefront dispatch) are implemented. Caustic-class connections are gated on
`LightPathInfo::IsCausticPath` in mixed mode (the Metropolis
`addonlycaustics` contract: non-caustic splats would double-count against
the eye pass); under `path.lighttracing.only` the gate is bypassed so the
pass reproduces `LIGHTCPU` (full light-path output) for validation.

Validated at 1280×720 on a refractive-enclosure scene (LuxBall shell,
40.7k-vert cutout glass mesh + interior volume + inner object, two
spotlights, dark floor): GPU `lighttracing.only` vs CPU `LIGHTCPU`
agrees within ~1% mean per region (full frame 0.2%), matching lit-pixel
counts and caustic pool geometry — including splats on surfaces *inside*
the refractive shell (see "Connection rays through refractive enclosures"
below). Remaining: sampler variants, spectral, RTPATHOCL, Blender adapter
exposure.

This document describes how to add a
GPU light-subpath pass with camera projection and film splatting to the
`PATHOCL` / `RTPATHOCL` engines (OpenCL + Metal), and how the chosen
architecture keeps the door open to full GPU bidirectional path tracing
(BDPT) later. No code in this document is authoritative — it cites the
existing implementation and marks every proposed piece as such.

## What and why

Light tracing samples transport paths starting at light sources; every
path vertex is projected onto the camera and its contribution is splatted
into the film. It is the adjoint of eye-path tracing and the classic answer
to caustics: paths like `L S+ D E` are trivially reachable from the light
side but nearly unreachable by eye-side sampling. The goal of this feature
is caustics at GPU rates — i.e. the light-subpath work currently done by
CPU threads in `path.hybridbackforward.enable` mode moves onto the same
device that already runs the eye pass, plus it enables a future GPU light-
only debug/validation mode.

Two properties make this a good fit for the existing engine rather than a
new one:

- The light path needs the same machinery the eye path already has:
  compiled scene, light distributions, BSDF eval/sample, volumes, Russian
  roulette, ray/hit submission, spectral state.
- The light path's output is a *film splat*, and the only estimator that
  makes it useful (hybrid eye+caustic suppression) already exists in the
  GPU kernels behind `taskConfig->pathTracer.hybridBackForward`.

A future GPU BDPT additionally requires retaining light and eye vertices
and connecting them; that only makes sense with both populations in one
address space — another reason to build this inside `PATHOCL` rather than
as a standalone engine.

## References

- Dutré, Lafortune, Willems. **Monte Carlo Light Tracing with Direct
  Computation of Pixel Intensities.** Compugraphics '93. (Light tracing by
  camera projection — the estimator implemented here.)
- Veach. **Robust Monte Carlo Methods for Light Transport Simulation.**
  PhD thesis, 1997, ch. 10-11. (Path-space measure, camera PDF,
  area↔solid-angle conversion — the `fluxToRadianceFactor` used below.)
- Lafortune, Willems. **Bi-directional Path Tracing.** Compugraphics '93.
  (The future GPU BDPT target.)
- Georgiev, Křivánek, Davidovič, Slusallek. **Light Transport Simulation
  with Vertex Connection and Merging.** SIGGRAPH Asia 2012. (Light
  tracing inside a combined estimator; the "splatting" estimator class.)
- Keller. **Instant Radiosity.** SIGGRAPH 1997. (Light-particle splatting
  lineage.)
- Jensen. **Realistic Image Synthesis Using Photon Mapping.** 2001.
  (The competing light-transport record — our PhotonGI cache is the
  gather-based variant; this feature is the projection-based variant.)

## Baseline: the CPU light tracer

`LightCPURenderEngine` (`src/slg/engines/lightcpu/lightcpu.cpp`) adds the
`Film::RADIANCE_PER_SCREEN_NORMALIZED` channel (line 39) and disables
`hybridBackForwardEnable` in standalone mode (line 83, comment: otherwise
only caustic light paths would be traced). `LightCPURenderThread`
(`src/slg/engines/lightcpu/lightcputhread.cpp`) requests
`SCREEN_NORMALIZED_ONLY` samples of size `pathTracer.lightSampleSize`.

`PathTracer::RenderLightSample()` (`src/slg/engines/pathtracer.cpp:1205-1353`)
is the per-path reference:

1. Spectral wavelength draw (1213-1216), time draw →
   `Camera::GenerateRayTime` (1220-1221).
2. Light pick via `GetEmitLightStrategy().SampleLights` (1224-1226) — the
   *emit* strategy, default `LightStrategyLogPower`
   (`lightsourcedefs.cpp:40`), i.e. a `Distribution1D` over light power
   (`logpower.cpp:75`).
3. `light->Emit(...)` — dims 1-5 — returns flux and `lightEmitPdfW`
   (1232-1235); `lightPathFlux /= emitPdfW * lightPickPdf` (1240).
4. Per vertex (`sampleOffset = boot + depth*7`, 1257):
   `scene.Intersect(LIGHT_RAY | INDIRECT_RAY, &pathInfo.volume, ...)` →
   `BSDF` + `connectionThroughput` (1262-1268); miss → terminate; non-black
   `GetPassThroughShadowTransparency` without override → terminate (1277);
   `Camera::SampleLens(time, u6, u7)` → `pathInfo.lensPoint` (1291-1292);
   `ConnectToEye(...)` (1294); BSDF sample with dims +4/+5 (1313-1316);
   `pathInfo.AddVertex`; hybrid early-out: stop once the path is no longer
   specular-dominated (`IsSpecularPath` + `diffuse+glossyDepth > 1`,
   1322-1329); Russian roulette dim +6 (1331-1340); `flux *= bsdfSample`;
   `nextEventRay.Update` (1342-1345). Spectral results are projected to RGB
   once per sample (1349-1352).

`PathTracer::ConnectToEye()` (`pathtracer.cpp:1102-1199`) is the splat
reference:

- Rejects `bsdf.IsCameraInvisible() || bsdf.IsDelta()` (1110) — delta
  vertices cannot be connected deterministically.
- Orthographic camera: ray along `-cameraDir`, distance via plane
  projection, `Camera::ProjectToImage` (1120-1134). Other cameras: ray
  `lensPoint → vertex`, `Camera::GetSamplePosition` (1136-1144).
- `bsdf.Evaluate(-eyeDir)` (1149); visibility ray is traced *reversed*
  (vertex → camera, `LIGHT_RAY | CAMERA_RAY`, fresh copy of the path's
  `PathVolumeInfo`, `UpdateMinMaxWithEpsilon`, 1156-1171); on miss the
  vertex is visible.
- `Camera::GetPDF(eyeRay, eyeDistance, filmX, filmY, …,
  &fluxToRadianceFactor)` (1175), then
  `radiance[light.GetID()] = connectionThroughput * flux *
  fluxToRadianceFactor * bsdfEval` (1195) — the radiance-group index is the
  *light* group.
- `isCaustic` classification via `pathInfo.IsCausticPath` (1192).

Camera PDFs (`GetPDF`):

- Perspective — `cameraPdfW = 1 / (cosAtCamera³ · pixelArea)`,
  `fluxToRadianceFactor = cameraPdfW / eyeDistance²`
  (`perspective.cpp:255-276`).
- Orthographic — constant `cameraPdf = 1 / (xPixelWidth · yPixelHeight)`
  (set in `InitCameraData`, `orthographic.cpp:60-64`; `GetPDF` at
  `orthographic.cpp:183-191`).
- Environment — `1 / (2π² sin θ)`, `/ eyeDistance²`
  (`environment.cpp:145-154`).

`GetSamplePosition` does the inverse projection
(`perspective.cpp:136-171`): back-face and `clipHither/Yon` rejection,
lens-radius/focal-distance point projection, `Inverse(rasterToWorld)`
raster transform, subregion bounds check, arbitrary clipping-plane test.

Light sample layout (`pathtracer.cpp:1544-1549`):
`lightSampleBootSize = 9 (+1 spectral)`, `lightSampleStepSize = 7`.
Dims: 0 light pick, 1-5 emit, 6-7 lens, 8 time, [9 wavelength];
per-vertex: +0 passthrough, +1..3 connect (u0 consumed by the visibility
`Intersect`), +4/+5 BSDF sample, +6 RR.

Splatting + normalization are host-side today:
`FilmSampleSplatter::AtomicSplatSample` (`filmsamplesplatter.cpp:46-104`)
walks the `FilterLUTs` footprint and atomically adds `weight *
filterWeight` per covered pixel into
`channel_RADIANCE_PER_SCREEN_NORMALIZEDs[group]`. At output/merge time,
`GetPixelFromMergedSampleBuffers` (`filmchannels.cpp:313-335`) multiplies
the raw sums by `pixelCount / screenSampleCount` — i.e. the channel stores
raw radiance sums and normalization happens once, host-side. `Film::AddFilm`
already merges screen-normalized buffers plus their sample count
(`film.cpp:756-774`); `Film::SetSampleCount/AddSampleCount` take the screen
count as third argument (`film.h:69-78`).

**PATHOCL already runs hybrid today — on CPU.** With
`path.hybridbackforward.enable`, the engine film gets the screen channel
(`pathoclbase.cpp:255-258`), `lightSampleSplatter` is created
(`pathocl.cpp:153-155`), and each *native* thread runs a Metropolis light
sampler (`sampler.metropolis.addonlycaustics`,
`sampler.imagesamples.enable=false`) interleaving eye and light samples via
`HasToRenderEyeSample` (`pathoclnativethread.cpp:121-144,173`,
`pathtracer.cpp:1359+`). The GPU thread film meanwhile *removes* the
screen channel (`pathoclbaseoclthreadfilm.cpp:118-120`) — GPU threads
trace eye paths only. This feature replaces that CPU light population with
GPU light tasks; the native-thread path remains the fallback.

## Baseline: the GPU micro-kernel engine

All GPU path engines share `PathOCLBaseOCLRenderThread`. The renderer is a
micro-kernel state machine (`PathState` enum,
`pathoclbase_datatypes.cl:41-80`, states 0-14), where each task owns
`tasks[gid]`, `tasksState[gid]`, `rays[gid]`, `rayHits[gid]`,
`sampleResultsBuff[gid]`, `eyePathInfos[gid]`, and a slice of the sampler
buffers. `GPUTask` already carries scratch `tmpBsdf`/`tmpHitPoint`
(503-515); `GPUTaskState` carries `state`, `throughput`, the variable-size
`bsdf`, `seedPassThroughEvent`, `throughShadowTransparency` (296-312);
`GPUTaskStats` is just `sampleCount` (519-521).

Per iteration the host loop (`pathoclopenclthread.cpp:184-206`) issues:

1. `EnqueueTraceRayBuffer(raysBuff, hitsBuff, raySlotCount)` — one trace
   pass over every task's ray slot plus tail regions (ReSTIR visibility
   candidates at `visCandRayBase`, GI bounce/NEE at `giCandRayBase`,
   allocated at `pathoclbaseoclthreadinit.cpp:961-969`, bases at
   519/587-602). Masked slots (`RAY_FLAGS_MASKED`) exit inside the RT
   kernel.
2. Dense mode (`EnqueueAdvancePathsKernel`,
   `pathoclbaseoclthreadkernels.cpp:649-695`): 14 kernel launches, each
   scanning all `taskCount` lanes and early-outing on state mismatch.
   Ordering is deliberate: consumers precede producers for each ray slot
   (`MK_RT_NEXT_VERTEX` first, `MK_RT_DL` before `MK_DL_ILLUMINATE`,
   `MK_GENERATE_NEXT_VERTEX_RAY` late).
3. Wavefront mode (`LUXRAYS_WAVEFRONT_QUEUES=1`,
   `EnqueueAdvancePathsWavefront`, 697-833): histogram → blocking readback
   → host prefix bases → `AdvancePaths_BuildQueues` refill → per-state
   launches sized by `wavefrontQueueTotals[state]` over the dispatch table
   (802-818). `WAVEFRONT_NUM_STATES = 15`, `WAVEFRONT_NUM_LAMBDA = 3`
   (`pathoclbaseoclthread.h:48,53`); queues at init:615-630; lane→task
   remap via `WAVEFRONT_GID`/`WAVEFRONT_GUARD`
   (`pathoclbase_funcs.cl:4434-4454`); λ buckets read
   `sampleResultsBuff[gid].spectralHeroAlive`.

`Init` (`pathoclbase_funcs.cl:4490+`) masks the ReSTIR tail rays
(4505-4517), calls `Sampler_Init` (4543) + `GenerateEyePath` (4560); tile
engines bound-check `gid >= filmWidth*filmHeight*aa²` → `MK_DONE`
(4524-4525). TILEPATHOCL
forces `wavefrontQueues = false` (`tilepathoclthread.cpp:42-45`) and runs a
bounded `worstCaseIterationCount ≈ 2·maxDepth-1` loop (136-152);
RTPATHOCL uses the same tile machinery with a single full-frame tile.

`Scene_Intersect` (`scene_funcs.cl:37`) is the hit-consume workhorse: it
takes `rayType` flags (`EYE_RAY|INDIRECT_RAY`, `EYE_RAY|SHADOW_RAY`,
`LIGHT_RAY|…` for us), `throughShadowTransparency`, a `PathVolumeInfo`,
pass-through draw, ray/hit, output `BSDF`, and returns
`connectionThroughput` (volume transmittance + transparent-shadow
throughput). The shadow-ray consume in `MK_RT_DL`
(`pathoclbase_kernels_micro.cl:454+`) is exactly the pattern the
camera-visibility ray needs.

GPU-side hybrid suppression already exists: `MK_HIT_NOTHING` skips caustic
environment hits when `hybridBackForward.enabled`
(`pathoclbase_kernels_micro.cl:157-158`), and the direct-light code paths
skip caustic-path contributions the same way; the config fields are
`include/slg/engines/pathtracer_types.cl:113-115`, compiled at `compilepathtracer.cpp:43-44`.

What the GPU side is missing today:

- `LightSource::Emit` — `light_funcs.cl` implements `*_Illuminate` for 14
  light types but no `*_Emit`.
- The emit-strategy distribution — `CompileLightStrategy`
  (`compilelights.cpp:121-195`) uploads only the *illuminate* and
  *infinite-light* distributions, not `GetEmitLightStrategy()`'s.
- Camera inverse projection — `camera_funcs.cl` has `GenerateRay`,
  `PerspectiveCamera_LocalSampleLens` (107), `MotionSystem_Sample`
  (233-236), but no `GetSamplePosition`/`ProjectToImage`/`GetPDF`, and the
  GPU structs lack `pixelArea` (persp), `cameraPdf` (ortho) and world-space
  `dir`.
- A screen-normalized film channel on the GPU — stripped at
  `pathoclbaseoclthreadfilm.cpp:120`; the GPU `Filter` struct is only
  `{widthX, widthY}` (`include/slg/film/filters/filter_types.cl:72-74`) and
  `pixelFilterBuff` is a
  *sampling* distribution, not the eval-side `FilterLUTs`.
- `BSDF::IsCameraInvisible` — on GPU it's
  `sceneObjects[bsdf->hitPoint.sceneObjectIndex].cameraInvisible`
  (`sceneobject_types.cl:34`, `bsdf_types.cl:44`); trivially portable.
- `LightPathInfo` — GPU `EyePathInfo` (`include/slg/utils/pathinfo_types.cl:21-37`) has
  `depth`, `volume`, `lastBSDFEvent`, `isNearlyS/SD/SDS`; light paths need
  the same plus `lensPoint` and pending-splat bookkeeping.

## Design

### Architecture decision: extend PATHOCL/RTPATHOCL — no LIGHTOCL

A separate `LIGHTOCL` engine would duplicate `CompiledScene` upload,
camera/light/material compilation, kernel plumbing, film handling and the
intersection-device scaffolding, and would have to share the film across
two engines for hybrid compositing. The alternative — a second task
population inside the existing engine — costs a handful of new states and
buffers and keeps everything (including future BDPT vertex retention) in
one address space. **Recommended.**

The eye/light split follows the CPU hybrid contract
(`path.hybridbackforward.enable`): the eye pass drops caustic-class
contributions (gates already in the kernels), the light pass owns them and
early-terminates once a path leaves the specular domain
(`pathtracer.cpp:1322-1329`). `path.hybridbackforward.partition` maps to
the static task-count split (the CPU ratio test becomes a population
fraction — see *Open questions*).

### Task population and buffer layout

`totalTaskCount = eyeTaskCount + lightTaskCount`; all per-task buffers
(`tasks`, `tasksState`, `tasksDirectLight`, `sampleResults`, `taskStats`,
`samples`, `samplesData`, `eyePathInfos`) are sized to the total, and light
tasks occupy the tail range `gid ∈ [eyeTaskCount, totalTaskCount)`. The
`Init` kernel branches on `gid >= eyeTaskCount` → light-task init instead
of `GenerateEyePath`. This partition keeps every existing gid→pixel mapping
(samplers, `pixelFilterDistribution`, tile bound check) untouched — the
eye-task index space is identical to today.

Ray buffer: a light vertex needs two rays per bounce — the continuation
ray *and* the camera-visibility ray. Two layouts:

- **Dual-slot (recommended):** light tasks keep `rays[gid]` for the path
  ray plus a tail slot `rays[lightVisRayBase + (gid - eyeTaskCount)]` for
  the visibility ray — the ReSTIR `visCandRayBase` precedent
  (`pathoclbaseoclthreadinit.cpp:519`, slot math at `:961-969`). Both rays
  trace in the same pass →
  **one iteration per vertex** and, crucially, a single consume kernel
  reads both `rayHits` slots — no dense-dispatch ordering hazard (see
  below). Cost: `lightTaskCount` extra `Ray`+`RayHit` slots.
- **Single-slot (memory-tight):** `rays[gid]` alternates path ray /
  visibility ray across iterations, matching the eye path's
  vertex-ray/shadow-ray cadence — 2 iterations per vertex, and the
  `HIT↔EYE` cycle needs the `needsTrace`-style dense barrier used by MNEE
  (`pathoclbase_funcs.cl:3990-3995`) / ReSTIR GI
  (`pathoclbase_kernels_micro.cl:868-872`), because the two consume
  kernels' launch order cannot satisfy both directions of a 2-cycle.

Either way the ReSTIR/GI tail bases shift from `taskCount` to
`totalTaskCount`, and a single `EnqueueTraceRayBuffer` call covers both
populations — light and eye rays share each trace pass (masked slots exit
inside the RT kernel), one device round-trip, no extra submission.

New buffers:

- `lightPathInfosBuff` — `LightPathInfo` per light task:
  `PathDepthInfo depth`, `PathVolumeInfo volume`, `PathVolumeInfo
  connectVolInfo` (the connect ray needs a *copy* of the path volume —
  `pathtracer.cpp:1166`), `BSDFEvent lastBSDFEvent`, `isNearlyS/SD/SDS`
  flags, `float3 lensPoint`, and a pending-splat record
  `{filmX, filmY, float3 radiance, uint lightGroupID, uint pending}`.
  Sized `lightTaskCount`, indexed `gid - eyeTaskCount` (or reuse
  `eyePathInfos[gid]` storage for the shared `PathInfo` prefix and keep
  the extras in the new array — implementation choice).
- `emitLightsDistributionBuff` — `CompileDistribution1D` output of the
  emit strategy (`CompileLightStrategy` extension).
- `channel_RADIANCE_PER_SCREEN_NORMALIZEDs_Buff[group]` — `float3` per
  pixel per radiance group, matching the host `GenericFrameBuffer<3,0>`
  layout (`film.cpp:373-377`); atomic CAS accumulation
  (`atomic_funcs.cl`, same pattern as the pixel-normalized channel).
- `filterLUTsBuff` — upload the host `FilterLUTs` table already built by
  `FilmSampleSplatter` (`filmsamplesplatter.cpp:34-39`) so the GPU splat is
  weight-identical to CPU.

`taskConfig->pathTracer` gains a `lightTracing` block: `{enabled,
eyeTaskCount, lightTaskCount, lightSampleBootSize, lightSampleStepSize,
lightSampleSize}` (`pathtracer_types.cl`, populated in
`compilepathtracer.cpp`).

### Light-path state machine

**Recommended: fused consume, dual ray slots.** Two new `PathState`
values appended after `MK_RT_GI_RESOLVE` (`WAVEFRONT_NUM_STATES` grows
accordingly):

- `MK_LIGHT_INIT` — sample time + wavelengths, pick the light via
  `emitLightsDistribution`, call `LightSource_Emit`, write the emission ray
  to `rays[gid]`, initialize `LightPathInfo` (depth=0, volume from the
  light's volume, flags clear, `pendingSplat.valid = 0`), `throughput =
  flux/(emitPdfW·pickPdf)`, spectral hero λ into
  `sampleResultsBuff[gid].spectralW/heroAlive` (the wavefront λ-bucketer
  reads these fields — keep them valid for light tasks). Increment the
  light sample counter. → `MK_LIGHT_VERTEX`.
- `MK_LIGHT_VERTEX` — the per-vertex consume kernel, one iteration per
  bounce. Order of operations inside the kernel:
  1. **Resolve pending connect** — if `pendingSplat.valid`, consume
     `rayHits[lightVisRayBase + (gid-eyeTaskCount)]`:
     `Scene_Intersect(LIGHT_RAY | CAMERA_RAY | shadow-style consume, …)`
     against `connectVolInfo` into `task->tmpBsdf`/`tmpHitPoint` (the
     `MK_RT_DL` pattern); on miss → `Film_SplatLight(pendingSplat)`; clear
     `pendingSplat`.
  2. **Consume path hit** — `rayHits[gid]` via
     `Scene_Intersect(LIGHT_RAY | INDIRECT_RAY, …)` → BSDF +
     `connectionThroughput`; `throughput *= connThroughput`.
     Miss → terminate → `MK_LIGHT_INIT` (recycle) / `MK_DONE` (tile).
  3. **Camera connect** — if `!BSDF_IsDelta && !cameraInvisible`, run
     inverse projection → `filmX/filmY`, `BSDF_Evaluate(-eyeDir)`,
     `Camera_GetPDF` → `fluxToRadianceFactor`; write the visibility ray
     (vertex→lens, `eyeDistance - eps` mint/maxt, CPU `pathtracer.cpp:
     1156-1160`) into the tail slot and store `pendingSplat =
     {filmX, filmY, connThroughput·flux·factor·eval, lightID, valid}`.
     Otherwise write a masked slot-B ray and leave `pendingSplat` invalid.
  4. **Continue** — BSDF-sample + `AddVertex` + hybrid specular early-out
     (`pathtracer.cpp:1322-1329`) + RR + depth cap → write next path ray
     into `rays[gid]`; terminate → `MK_LIGHT_INIT` / `MK_DONE`.

The key correctness property: the kernel *reads* both ray-hit slots
(written by the previous trace pass) and *writes* both ray slots (traced
by the next pass). There is no consumer/producer ordering hazard inside
the dense advance pass — the state is a self-loop, so no
`needsTrace`-style barrier is needed at all. The splat for vertex *N* is
deferred one iteration — identical in spirit to the CPU
`connectToEyeCallBack` timing and required anyway because the visibility
test completes a pass after the ray is queued.

**Alternative: single-slot, split consume.** If the extra ray slot is too
expensive, `MK_LIGHT_VERTEX` splits into `MK_LIGHT_HIT` (consume path hit
→ queue vis ray) and `MK_LIGHT_EYE` (consume vis hit → splat → queue path
ray), matching the eye path's vertex-ray/shadow-ray cadence — 2 iterations
per vertex. Because `HIT→EYE→HIT` is a 2-cycle that launch order cannot
serialize, the consume kernels need the `pending`-flag trace barrier used
by `MneeState.needsTrace` (`pathoclbase_funcs.cl:3990-3995`) and
`RestirGIResult.needsTrace` (`pathoclbase_kernels_micro.cl:868-872`): set
when the ray is queued; in dense mode the consumer's first launch clears
the flag and skips, the next launch consumes. Under wavefront queues the
barrier is redundant (queues rebuild once per iteration) — the flag is
simply cleared, mirroring the GI code.

### Sampler dimensions for light tasks

Light tasks keep the same per-task sampler state (`samplesBuff`,
`samplesDataBuff`, `task->seed`) but use the *light* dimension layout.
Because `IDX_SCREEN_X/Y` (dims 0-1) are special-cased to `samplesDataBuff`
in every GPU sampler, light dims map `cpuDim → Sampler_GetSample(cpuDim +
2)` — for `RANDOM` any index ≥ 2 draws from the seed stream, for
`SOBOL`/`PMJ02` it indexes the direction tables. Consequences:

- The Sobol directions array (`InitSamplerSharedDataBuffer`,
  `pathoclbaseoclthreadinit.cpp:653`) must be sized
  `max(eyeSampleSize, 2 + lightSampleSize)` when light tracing is on.
- Light tasks must not call `Sampler_Init`/`Sampler_NextSample` (those pick
  a pixel); a light-path "next sample" only advances the Sobol/PMJ02 `pass`
  counter — small `LightSampler_Next` helper.
- `METROPOLIS` per-task `u[]` storage is sized by `eyeSampleSize`; light
  tasks under Metropolis are deferred (the CPU hybrid light sampler uses
  Metropolis only for image-luminance stabilization — RANDOM/SOBOL light
  dims are the v1 target).

### Camera inverse projection and PDF

New device functions in `camera_funcs.cl`, ported from the CPU
implementations, dispatched on `camera->type`:

- `Camera_GetSamplePosition(ray, &x, &y)` —
  `PerspectiveCamera::GetSamplePosition` (`perspective.cpp:136-171`) and
  `OrthographicCamera::ProjectToImage` (`orthographic.cpp:78`):
  back-face/clip reject, lens-focal projection, `worldToRaster`
  (`Matrix4x4_Invert` exists in `matrix4x4_funcs.cl:125`;
  `rasterToWorld = cameraToWorld ∘ rasterToCamera`), `y`-flip vs
  `filmHeight`, subregion bounds, arbitrary clipping plane
  (`enableClippingPlane`/`clippingPlaneCenter`/`clippingPlaneNormal` are
  already in `ProjectiveCamera`, `camera_types.cl:42-50`). Motion blur:
  `MotionSystem_Sample`/`SampleInverse` on `ray->time`, same as CPU.
- `Camera_GetPDF(eyeRay, eyeDistance, filmX, filmY, &fluxToRadianceFactor)`
  — perspective `1/(cos³·pixelArea)/d²`, ortho constant, env `1/(2π²sinθ)/d²`.
- `Camera_SampleLens` for DoF — `PerspectiveCamera_LocalSampleLens` +
  `cameraBokehDistribution` already exist for the eye path; reuse.

`compilecamera.cpp` additionally uploads `pixelArea` (persp), `cameraPdf`
(ortho). The world-space `dir` and `worldToRaster` transform are **not**
stored: both pushed `sizeof(Camera)` past the Apple cl2msl per-kernel
buffer-argument encoding limit (`AGX::ComputeContext::prepareForEnqueue`
crash at dispatch — the same budget class as the `GPUTaskConfiguration`
growth crash noted under Risks). Instead:

- `worldToRaster` is derived on the device as
  `rasterToCamera⁻¹ ∘ cameraToWorld⁻¹` (`Transform_InvApplyPoint` twice),
  which requires `rasterToCamera.mInv`/`cameraToWorld.mInv` to be
  uploaded — `compilecamera.cpp` previously copied only the forward `m`
  half of every camera `Transform`, leaving `mInv` as garbage (upstream
  never inverted these on-device, so the missing upload was latent).
  Both halves are now uploaded for all camera branches.
- The world-space camera direction is derived as
  `cameraToWorld × (0,0,1)` (camera forward is +Z in camera space).

v1 support matrix: **perspective + orthographic**;
environment is a small follow-up (the PDF is trivial, the projection is
spherical mapping); **stereo is rejected** at init for v1
(`STEREO_PERSPECTIVE` could delegate to the wrapped camera later).

Unsupported camera → the engine logs and disables the light pass (eye-only
render continues), never silently wrong splats.

### Light emission on the device

Port `LightSource::Emit` per GPU light type — 14 CPU implementations
(`src/slg/lights/*light.cpp`): triangle (area), point, spot, sphere,
mappoint, mapsphere, projection, laser, sun, distant, sharpdistant, sky2,
infinite, constantinfinite. Scope v1 to the dominant set for caustics —
triangle/point/spot/distant/sun/sky2/infinite — and emit a per-light
"unsupported" fallback (skip the light in the emit distribution, counted
and logged once) rather than silently biasing.

Emission selection reuses `Distribution1D_SampleContinuous` (the same
device helper the illuminate path uses) over the new
`emitLightsDistribution` upload.

**Transform convention caveat (bug found + fixed):** spot lights do not
store a plain `lightToWorld`. `compilelights.cpp` copies
`SpotLight::alignedLight2World` into `light2World.m` — the local variable
is misleadingly named `alignedWorld2Light`. The device `SpotLight_Emit`
samples the cone in *local* space and must therefore apply
`Transform_ApplyVector` (`light2World.m`); the original port used
`Transform_InvApplyVector` (`light2World.mInv`, i.e. world→light) which
slewed the emission cone — one spotlight produced no output at all, the
other only its direct pool with all refracted contributions missing.
`SpotLight_Illuminate` correctly uses `InvApplyVector` because it maps
world directions *into* spot space — do not copy that call direction into
`Emit`. (CPU reference: `SpotLight::Emit` uses `alignedLight2World`,
`SpotLight::Illuminate` uses `Inverse(alignedLight2World)` —
`spotlight.cpp`.)

### Caustic focus cache (guided emission)

A learned, GPU-resident emission guide for hard caustic setups — the
regime where the productive solid angle of an emitter is tiny (a small
refractor far from the light) and native emission wastes ~99% of rays.
Related work: *focal path guiding* and 3D-Gaussian online photon guiding
for caustics (both learn where caustic-generating geometry lives and
steer particles toward it). Our variant is deliberately minimal:

- **Store**: per-light ring of `LIGHT_FOCUS_K` (=32) `float4` entries —
  the world position of the *first delta-specular vertex* of each path
  whose camera connect splatted (i.e. a verified productive refraction
  entry point) + the aim radius in `.w`. Writes are
  `atomic_inc(&count[light]) % K`, one credit per path.
- **Read**: `MK_LIGHT_INIT`, for `TYPE_POINT`/`TYPE_SPOT` only, with
  probability `focusRatio` re-aims the emitted direction into a uniform
  cone around `normalize(hotspot - origin)` whose angular radius comes
  from `focus.radius * worldRadius` (per-entry `.w` is stored for future
  per-hotspot adaptation).
- **Unbiasedness (important subtlety)**: the emitted direction is a
  *one-sample mixture* `q(d) = (1-g)·p_native(d) + g·p_aim(d)`. Every
  emission — including the ones left on the native branch — must be
  weighted by `q`, i.e. `emissionPdfW` is always overwritten with the
  mixture. Weighting only the guided draws by `q` while native draws keep
  `p_native` under-weights all directions outside the aim cones by
  `1-g` (measured: a systematic few-% darkening before the fix). The aim
  pdf itself must be the true mixture density of the slot procedure:
  `(1/N)·Σ_k [d inside cone_k]·UniformConePdf(cosMax_k)` plus
  `(fDegenerate/N)·p_native` for slots whose hotspot lies inside the aim
  sphere (they fall back to a native-density draw).
- Flux for the redirected direction is re-evaluated per emitter
  (`PointLight`: `emittedFactor/4π`; `SpotLight`:
  `emittedFactor·falloff(localDir)/|cosθ_local|`); a rim sample leaking
  outside a spot cone gets `falloff = 0` and dies cleanly.

Results (1280×720, `path.lighttracing.only`, point light moved to a far
corner so the glass sphere subtends ~1% of the emission sphere): at
16 spp the caustic pool forms as a coherent pattern instead of sparse
speckles, caustic-region lit pixels +9.5% and std −8%; walls/floor keep
their means (256 spp: all regions within ~1% of the unguided run — the
mixture is unbiased) at mildly higher flat-region variance, the expected
price of redirecting half the emissions.

Properties: `path.lighttracing.focus.enable` (default true),
`path.lighttracing.focus.ratio` (0..0.9, default 0.5),
`path.lighttracing.focus.radius` (fraction of `worldRadius`, default
0.01 — shrink toward `hotspot cluster size / worldRadius` for tight
aiming; too small under-covers the caustic-forming surface).

#### Distant-light casters (origin steering)

Distant-family lights (`distant`, `sharpdistant`) have no position to
aim *from* and their direction is (nearly) fixed, so the hotspot ring
cannot steer them. Instead the emit **origin** is focused: every
delta-specular object in the scene is a potential caustic *caster*, and
its bounding sphere projects to a disc on the light's emit plane.
Sampling origins on that disc sends ~100% of the guided rays through the
caster — the uniform-emission coverage problem disappears regardless of
how small the caster is relative to the scene disc.

- Casters are collected at compile time (delta + SPECULAR materials),
  stored as `(center.xyz, boundingRadius)` float4s appended to the
  `lightFocus` buffer after the per-light rings, count in
  `pathTracer.lightTracing.focusCasterCount`. CPU mirrors this in
  `PathTracer::LightFocusEmitDistant`.
- With probability `g` a caster is picked proportional to its disc area
  `r^2` and the origin is resampled inside its projected disc; the
  direction (including `distant` cone jitter) is untouched.
- Mixture pdf on the final origin: `(1-g)*nativeArea/(pi*envR^2) +
  g*coverN/(pi*sumR2)` — folded into `emissionPdfW` as a ratio, so
  unfocused draws are correctly down-weighted by `1-g` and focused draws
  carry their share of the inflated density. Unbiased.
- Emit-plane gotcha: `o = wc - R*(dir + d1*x + d2*y)` maps unit-disc
  coords to the perpendicular offset with a **negative** sign — the
  caster disc center sits at `-perp(C - wc)/R`, not `+perp/R`.
- `TYPE_SHARPDISTANT` previously had no device `Emit` at all (zero
  weight in the emit distribution); the port is a fixed direction +
  disc origin (u0/u1 are the disc coords, pdf `1/(pi*envR^2)`).

**Camera-side caveat (measured)**: focusing fixes only the emission
half. A caustic whose receiver is seen *through* the refractor (sheet /
window / basin) cannot splat — the camera connect ray hits the caster
itself and the vertex is rejected. That is the LMNEE path's job; enable
`path.mnee.enable` for those views. `scenes/mnee_dir/dirsheet.scn`
renders 0.00 in the shadowed footprint without LMNEE and ~0.30-0.47 with
it, focus making no measurable difference there (the sheet is large
enough that uniform emission already covers it).

### Manifold-guided emission

The focus cache steers emission toward *specular hotspots* — the first
delta vertex of a camera-connected path — which targets the **portal** a
photon must refract through to form a caustic. Manifold-guided emission
(MGE) closes the loop from the other side: every *successful camera
connection*, including LMNEE specular-manifold solves, records the
**receiver** position (the light-path vertex that connected to the
lens), and emission is steered toward those camera-productive positions.

#### Why the receiver, not the portal

For an LMNEE connect `light → … → x0 (diffuse) → s1 … sn (specular) →
lens`, the photon must *land* on the receiver `x0`; the `x0 → lens`
manifold is then solved by the connect machinery. Aiming emission at
`x0` deposits photons on a receiver that is known to reach the camera
(possibly only through the solved specular chain). Aiming at the
specular `si` would be wrong — `si` lies on the receiver→camera half,
not the emitter→receiver half. So the productive emission target is the
receiver position `x0` of each camera connect, manifold or plain.

This directly targets the refracted-view hard case: an interior surface
that can only reach the camera *through* glass is invisible to ordinary
emission (a plain connect is blocked by the shell), but once LMNEE
solves it once, its receiver is recorded and subsequent emission is
preferentially deposited there — bootstrapping the very paths that are
slowest to discover.

#### Estimator and unbiasedness

Identical one-sample mixture to the focus cache. With aim ratio `g`, the
direction (and, for area emitters, the surface position) is drawn from
`q = (1−g)·p_native + g·p_aim`. Because the returned weight is divided by
the **full mixture** `q` — including the `g·p_aim` term evaluated on the
native draw and the `(1−g)·p_native` term on the guided draw — every
emitted direction carries its true density under the mixture, so the
estimator stays unbiased for any `0 ≤ g < 1` and any `p_aim > 0` over
the native support. Recording adds no bias: it only changes which
positions future emission is aimed at, and the mixture pdf already
covers the whole ring.

#### Mechanism

* On a camera connect (plain or LMNEE/chain), the receiver vertex
  position is appended to the per-light `lightFocus` ring alongside the
  existing first-delta hotspots. Both are just world-space "productive"
  positions; the aim cone treats them uniformly.
* Emit-side guidance is extended from `POINT`/`SPOT` to area (`TRIANGLE`)
  emitters: the sampled surface point is kept (native position pdf), the
  outgoing direction is re-aimed into the hotspot cone, and the
  direction pdf is recomputed for the redirected direction and folded
  into the mixture. This removes the previous gap where triangle/area
  lights — the most common studio emitter — received no guidance at all.

#### Design notes / failure modes

* The same ring holds portals (first-delta) and receivers; a scene with
  both gets a population-weighted mixture, which is the desired
  behaviour. A portal target is the transit point a productive path
  refracted/reflected through (aiming there sends fresh photons along a
  known-good route); a receiver target is a camera-visible diffuse
  surface point. Both are just world-space "productive" positions.
* Each ring entry stores its own aim radius in `.w`, set adaptively at
  record time to the distance to the nearest ring entry already present
  (`FocusAimRadius`, clamped to `[focus.radius, worldRadius]`). A dense
  caustic cluster yields a tight cone; a spread-out receiver field
  yields a broad one — so `focus.radius` now acts as the *tightest*
  allowed cone rather than a single global width, and no separate
  per-target radius has to be tuned.
* Stale entries (moved/deleted geometry, animation) are simply aims that
  no longer connect — they cost a little emission but never bias, and
  ring replacement naturally ages them out.
* Validation (`scenes/cornell/mge-recvonly.scn`, a pure-receiver case
  where the light reaches the wall directly but the camera sees it only
  through a glass pane, so the ring fills with receivers alone): guided
  emission cut the wall region's variance to **~0.4×** the unguided
  value (~2.5× reduction) at 512 spp / 1280×720, unbiased (mean ratio
  ~1.0). `mge-cavity.scn` exercises the mixed portal+receiver path.
* Benefit is largest when productive receivers are hard to reach by
  native emission (isotropic point emitters, receivers seen only through
  an occluder). When a large area light already floods the receiver,
  guidance adds little — the mixture pdf still keeps it unbiased, so the
  feature is safe to leave on by default.
* **Known limitation:** the aim cone is a straight-line cone to the
  receiver. When the *light* can only reach the receiver through a
  refractive interface too (light and camera on the same side of a glass
  pane, receiver on the far side), the emitted ray refracts off-axis and
  does not land on the aimed receiver — guidance then mis-aims and can
  raise variance. Covering that case needs an emission-side manifold
  solve (refract the aim direction onto the receiver), which is future
  work. It never biases the result either way — it only wastes a little
  emission.

### Connection rays through refractive enclosures

A light path readily refracts *into* a glass shell, but splats on
interior surfaces were lost because the camera-visibility ray is a
straight line that gets blocked by the enclosing delta surface. Two
switches combine to fix this, both now wired on GPU **and** CPU
(`ConnectToEye`, `pathtracer.cpp`) for parity:

- The connection march runs `Scene_Intersect` with `SHADOW_RAY` added to
  `LIGHT_RAY | CAMERA_RAY`, so surfaces whose material has
  `transparency.shadow` non-black are passed through (accumulating the
  shadow-transparency into `connectionThroughput`), exactly like an
  eye-path shadow ray.
- On the *path* ray, a shadow-transparent hit normally terminates the
  light path (the estimator hands such surfaces to direct-light
  sampling). `transparency.shadowoverride` (`material_types.cl`,
  `BSDF_GetPassThroughShadowTransparencyOverride`) suppresses that
  termination so the path refracts onward — this is precisely the flag's
  intended use.

With `transparency.shadow = 1` + `transparency.shadowoverride = 1` on the
shell material, vertices on objects *inside* the enclosure connect and
splat on both CPU and GPU. This is still a straight-line connection —
it does not solve general `D…S…`-specular visibility (that needs manifold
NEE / refractive connection solving, see *Manifold camera connect* below),
but it covers the dominant "decorative glass shell around diffuse content"
case.

### Manifold camera connect (LMNEE, design)

The straight connect still fails whenever a *non*-shadow-transparent delta
surface sits between the vertex and the lens: a glossy/diffuse vertex
behind real glass, underwater objects seen from air, mirrors relaying a
view. The eye direction solves the symmetric problem with GPU MNEE
(`MK_MNEE_NEXT_VERTEX`): when a shadow ray is blocked by a specular
occluder, a Newton solver walks the occluder surface until the
half-vector constraint `h = wi + eta·wo` (generalized for refraction)
residual converges. LMNEE mirrors it with the endpoint roles swapped:

- `x0` = the light-path vertex whose direct connect was blocked
  (`taskState->bsdf`), `y` = `lpi->lensPoint` instead of a light
  position. The solver math (`Mnee_StepAndWriteProposal`, `Mnee_Residual`,
  `Mnee_GeometricTerm`, `MneeChain_*`) is endpoint-agnostic — both enter
  as bare `float3` — so the whole Newton/Thomas/Jacobian machinery is
  reused verbatim.
- Trigger: in `MK_LIGHT_VERTEX` stage A, the queued visibility ray hit a
  delta occluder (`task->tmpBsdf`, same slot eye MNEE reads) instead of
  reaching the lens. The pending splat is consumed and converted into a
  solve state on the same task (`taskDirectLight->mnee` — the buffer is
  allocated for every task, light included, so no extra memory).
- Trace slot: the `lightVisRayBase + lightIndex` slot multiplexes
  connect ray → seed/proposal rays → final `xn→lens` shadow ray, one
  trace per iteration. The light path stalls (`lpi->mneeActive`) while
  solving; `rayHits[gid]` is untouched so the path resumes transparently.
- Solve-end weight (mirroring `pathtracer_mnee.cpp:804`): eye MNEE
  assembles `bsdfEval0 · specFactor · G · lightRadiance2 · (plainHV ?
  r12² : 1) / pickPdf`. `G = dw0_dx1 · |det(dx_first/dy)|` is the
  endpoint-agnostic manifold Jacobian — direction-invariant, so the
  same `G` serves both traversal directions.
- **Endpoint weight correction** (validated — see *Results*): the
  naive mapping `lightRadiance2 → fluxToRadianceFactor` is wrong.
  `fluxToRadianceFactor = cameraPdfW / d²_seg2` is the *straight*
  connect's flux→radiance conversion — it bakes in the free-propagation
  `1/d²` falloff of the endpoint segment. For a manifold connect the
  Jacobian `G` already encodes that segment's area-measure conversion
  (for refraction it carries the full `1/d²`; for reflection the
  `d²_seg2` factor supplies it, exactly as `pathtracer_mnee.cpp:787-806`
  notes `directPdfW2` is applied only when `eta == 1`). Using
  `fluxToRadianceFactor` therefore double-counts `1/d²` and darkens the
  splat by a factor that grows with endpoint distance (~5× observed at
  slab depth ≈ 5). The correct endpoint "emission" — symmetric to the
  light's `lightRadiance2` — is the camera's emitted-importance
  solid-angle density `cameraPdfW`, with `directPdfW2 → d²_seg2`
  (pinhole lens point is a delta, same measure convention as a point
  light). So
  `camWeight = cameraPdfW · (plainHalfVector ? d²_seg2 : 1)`,
  `pickPdf → 1`. Contribution splats via `Film_SplatLight` at the film
  position of the *solved* arriving direction `xn→lens`
  (`Camera_GetSamplePosition`), not the straight-line projection.
- Segmentation: straight connects (unoccluded) and manifold connects
  (blocked by delta) cover disjoint path-space regions — no double
  counting, no bias. A failed solve contributes zero, exactly like a
  blocked straight connect today.
- **Black-eval probe** (CPU `ConnectToEye` + `MK_LIGHT_VERTEX` share the
  rule): the receiver's straight-line `BSDF_Evaluate(-eyeDir)` can be
  black even when a valid manifold connect exists — a matte surface whose
  normal faces *away* from the lens (pedestal top seen only through a
  glass sphere) fails the reflect/transmit side test. Both sides queue
  the visibility ray whenever `mnee.enabled` regardless of the eval, and
  only the unblocked splat is gated on `!evalBlack` — a probe that turns
  out visible contributes nothing, a probe blocked by a delta occluder
  starts the solve. Without this the wedge of receivers facing away from
  the camera is never attempted (`lt-dispersion.scn` upper disc).
- **Single→chain fallback**: when the single-vertex LMNEE machine exits
  unsolved (Newton stall, proposal miss, iteration cap), the same light
  path retries with `LMneeChain_Start` — the straight `x0 → lens`
  discovery re-walk rebuilds the occluder topology from scratch (CPU:
  `LMNEEMultiConnectToEye` after `LMNEEConnectToEye` failure).
- **Chain line-search ordering** (both chain machines, eye and light
  side): the CPU solver commits the accepted `trial[]` vertices *before*
  doubling `beta`. On the GPU the commit re-projects `TrialPos = v.p −
  beta·(dpdu·dx.x + dpdv·dx.y)` *on the fly* during `MS_COMMIT`, so beta
  must stay at the value that produced the validated trial until the last
  vertex committed — doubling it at trial-acceptance made every commit
  re-projection target a 2×-displaced, off-surface point and miss, which
  killed every chain solve. Beta is raised at the end of `MS_COMMIT`
  instead.

### Multi-interface chains (glass slab, lens, closed dielectrics)

A single-vertex solve handles one refracting/reflecting interface, but
real glass is a *slab*: the solved first-vertex→lens segment still
travels through the material and re-blocks on the exit face. LMNEE
detects this — `pendingSplat.fromMnee` marks a solved-manifold segment,
and a re-block on it (rather than a fresh connect) hands off to the
multi-vertex chain solver instead of restarting the single-vertex solve:

- `LMneeChain_Start` casts a straight discovery ray `x0 → lens` and
  records every mirror/glass delta interface it crosses (`chainMatType`,
  `chainVtx[].eta`), up to `path.mnee.maxspecular`. Non-delta or
  unsupported occluders abort (contribute zero). ≥2 vertices trigger
  `MNEE_PHASE_MS_DISCOVER → MS_JACPERT → MS_TRIAL → MS_COMMIT → MS_POST`.
- The Newton/Thomas/Jacobian core (`MneeChain_Residual*`, `MneeChain_Jac*`,
  `MneeChain_ThomasSolve*`, `MneeChain_LightJac`) is reused verbatim —
  endpoint-agnostic, `lightPos` carries the lens point. `MneeChain_LightJac`
  differentiates the last specular constraint w.r.t. the lens position,
  giving `dxDy` and hence `G` for the chain.
- `pendingSplat.fromMnee` is a 3-state hand-off: `0` fresh connect → try
  `LMnee_Start`, fall back to `LMneeChain_Start`; `1` single-vertex
  solved but its segment re-blocked → `LMneeChain_Start` directly; `2`
  chain-solved but still blocked (occluder has more interfaces than
  `maxspecular`) → drop, no re-solve. This prevents both missed slabs
  and infinite re-solve loops.
- `LMneeChain_SolveEnd` applies the same corrected endpoint weight
  `cameraPdfW · (plainHalfVector ? d²_seg2 : 1)` using the chain's last
  vertex→lens distance, then queues the final segment through
  `pendingSplat` for normal Stage-A visibility resolution.

### Film splatting and normalization

New device function `Film_SplatLight(filmX, filmY, lightGroupID,
radiance3)` mirroring `AtomicSplatSample`:

- `FILTER_NONE`/box: bounds-check against `filmSubRegion`, atomic CAS add
  of `radiance` into `filmScreenRadianceGroup[group][pixel]` — always
  atomic (splats from many tasks collide; independent of
  `film->usePixelAtomics`, which only governs the eye path's writes).
- General filters: walk the footprint exactly as `filmsamplesplatter.cpp:
  73-101`, weight = uploaded `FilterLUTs` lookup, per-pixel atomic add.
- `KERNEL_ARGS_FILM` gains a `__global float **filmScreenRadianceGroup`
  pointer array (same pattern as `filmRadianceGroup`,
  `film_types.cl:110`). A null slot already encodes "channel absent", so
  no `hasChannelRadiancePerScreenNormalized` flag is needed — adding one
  to `Film` pushed the `taskConfig` constant struct past the Apple
  cl2msl encode limit and crashed `MK_HIT_NOTHING` in
  `AGX::ComputeContext::prepareForEnqueue` (see "Risks" below).
- `ThreadFilm::Init` stops removing the channel when the light pass is on
  (`pathoclbaseoclthreadfilm.cpp:118-120`), allocates the GPU channel
  buffers, and `RecvFilm` transfers them — `Film::AddFilm` and
  `GetPixelFromMergedSampleBuffers` then handle merge + normalization
  unchanged (`pixelCount / screenSampleCount`, `filmchannels.cpp:315`).
- Sample counting: light tasks bump `taskStats[gid].sampleCount` once per
  completed light path; the host splits the sum by gid range
  (`[0,eyeTaskCount)` → per-pixel count, `[eyeTaskCount,…)` → screen count)
  and calls `SetSampleCount(total, eyeCount, lightCount)` /
  `AddSampleCount(…)` — the third argument is already the screen count
  (currently hard-wired `0.0`, `pathoclopenclthread.cpp:167`,
  `tilepathoclthread.cpp:155-157`).

This keeps the estimator's normalization identical to CPU: raw sums in the
channel, single host-side scale. Nothing is baked into the splat weight.

`isCaustic` AOV bookkeeping (`sampleResult.isCaustic` on CPU) has no GPU
field; v1 writes radiance only — no alpha/depth/normal/AOV writes from
light splats (CPU splat writes data channels unfiltered; acceptable
divergence, noted for later).

Clamping: CPU light samples go through `VarianceClamping` with
`sqrtVarianceClampMaxValue`. GPU v1 applies a direct magnitude cap on the
splat radiance using the same property value (the full film-statistics
clamp needs convergence buffers; documented difference).

### Scheduling: dense and wavefront

Dense mode: two launches appended to `EnqueueAdvancePathsKernel`
(`MK_LIGHT_VERTEX`, `MK_LIGHT_INIT`). **Ordering is load-bearing:**
`MK_LIGHT_VERTEX` must be enqueued *before* `MK_LIGHT_INIT` within the same
pass. VERTEX consumes `rayHits[gid]` produced by tracing `rays[gid]` — the
ray written by the *previous* pass's INIT. Running INIT first overwrites
`rays[gid]` with a fresh emission ray, so VERTEX pairs the old hit with the
new ray (BSDF built at `newRay.o + oldT·newRay.d`): the light pass
degenerates to zero-hit garbage and all splats vanish. This was a real bug
found by instrumented debugging — the earlier claim that the self-loop has
"no ordering constraint" was wrong, because INIT both writes `rays[gid]`
and resamples `rays` for tasks that terminated in VERTEX. The wavefront
dispatch table is ordered the same way for consistency (queue membership
is frozen per pass there, so it is not strictly required). Cost: 2 extra
`taskCount` lane-scans per iteration, proportional overhead only. The
single-slot variant adds a third launch plus the pending-flag barrier
described above.

Wavefront mode (`PATHOCL` only — tile engines disable it): the new states
get queue columns automatically; light tasks compact into their own
queues, which is precisely where a divergent, splat-heavy workload
benefits most. `taskLambdaBuff`/`spectralHeroAlive` λ-bucketing applies to
light tasks unchanged.

### Engine coverage

- `PATHOCL` — full support (dense + wavefront).
- `RTPATHOCL` — full support; the tile is the whole film so splats are
  never clipped; the `Init` tile bound-check gets a second branch
  (`gid >= eyeTaskCount && gid < total` → light init instead of
  `MK_DONE`), and `worstCaseIterationCount` covers the light cadence
  (fused: ~`maxDepth` iterations per path + emit; single-slot:
  `2·maxDepth` — verify against the existing `2·maxDepth-1` bound either
  way).
- `TILEPATHOCL` — splats are clipped to the tile's film subregion (the
  same check as `filmsamplesplatter.cpp:53`); fraction of wasted connects ≈
  `1 - tileArea/filmArea`. v1: allowed but wasteful; recommend documenting
  as PATHOCL/RTPATHOCL-first. Tile-space coordinate offset
  (`tileStartX/Y` in `TilePathSamplerSharedData`) must be subtracted
  before splat since camera projection yields camera-film coordinates.

### Memory cost (order-of-magnitude)

Per light task ≈ `GPUTask` + `GPUTaskState` + `LightPathInfo` (~100-160 B)
+ sampler slots + 1-2 ray/hit slots (~2×~90 B) — roughly 0.5-1 KB. At
`taskCount` ~1M with 25% light partition → +~250 MB worst case; typical
`taskCount` is much smaller. Screen channel: `groups × pixels × 12 B`
(1 light group @4K ≈ 95 MB; 8 groups ≈ 765 MB — same scale as the existing
pixel-normalized group buffers). Filter LUT: KBs.

## Interaction with other estimators

- **Eye path / hybrid suppression**: enabling the light pass implies the
  eye-side caustic suppression (`hybridBackForward.enabled` gates already
  in the kernels). Running light tracing *without* suppression
  double-counts caustics — the two are bound together, matching the CPU
  `path.hybridbackforward` contract.
- **MNEE** solves `DS+` connections eye→light and overlaps the light
  pass's caustic family. On CPU the combination is not explicitly
  prevented; for GPU v1 the two features are documented as mutually
  exclusive (`path.mnee.enable` + light tracing → warning, MNEE off).
- **PhotonGI caustic cache** gathers caustic photons at eye vertices —
  same overlap. CPU already gates it under hybrid
  (`IsCausticEnabled() && (!hybrid || depth != 0)`); GPU mirrors the gate.
  Keep both estimators' contributions disjoint; no photon↔splat mixing in
  v1.
- **ReSTIR DI/GI**: orthogonal — light tasks perform no NEE, reservoirs
  untouched. Ray-tail bases shift; that's an init-time constant.
- **Spectral**: light tasks carry `spectralW`/`spectralHeroAlive` in
  `sampleResultsBuff[gid]` (needed by `Scene_Intersect` wavelength
  propagation and wavefront λ-bucketing); splat projects spectral→RGB
  before accumulation, as CPU does once per sample
  (`pathtracer.cpp:1349-1352`).
- **Denoiser/AOVs**: screen-normalized splats bypass the BCD denoiser's
  per-sample statistics exactly as on CPU (the splatter calls
  `AtomicAddSample*`, not `AddSample`); the hardware image pipeline sees
  the merged film normally. RAYCOUNT can count connect rays if desired.

## Properties (proposed)

- `path.lighttracing.enable` (bool, default false) — GPU light pass;
  requires/implies `path.hybridbackforward.enable` semantics on the eye
  side. Engines: `PATHOCL`, `RTPATHOCL`.
- `path.lighttracing.taskfraction` (0..1, default ~0.25) — share of the
  task population assigned to light paths; the static analogue of
  `path.hybridbackforward.partition`.
- `path.hybridbackforward.glossinessthreshold` — reused for caustic
  classification and light-path early termination.
- `path.maxdepth`, `path.russianroulette.*` — shared with the eye path
  (same depth accounting).
- `lightstrategy.type` — the emit strategy is already a property
  (`lightsourcedefs.cpp:197-198`); the GPU consumes its uploaded
  distribution.
- Debug: `path.lighttracing.only` (fraction=1, light-only image) for
  validation against `LIGHTCPU`.
- `path.lighttracing.focus.enable` / `.ratio` / `.radius` — caustic
  focus cache (guided emission); see "Caustic focus cache" above.

## Phased implementation plan

1. **Host plumbing** — `lightTracing` config block, buffer allocation,
   `totalTaskCount`, `Init` kernel branch, `ThreadFilm` screen channel +
   buffers + `RecvFilm`, sample-count split. Everything conditional on the
   flag; zero diff when off.
2. **Emission** — emit distribution upload; `LightSource_Emit` ports for
   the v1 light set; `MK_LIGHT_INIT`.
3. **Transport** — `MK_LIGHT_VERTEX` consume kernel (`Scene_Intersect`
   reuse on both ray slots), BSDF sample/RR/depth, specular early-out,
   pending-splat bookkeeping.
4. **Camera connect** — `Camera_GetSamplePosition`/`GetPDF`/ortho
   `ProjectToImage`, lens sampling, clipping, motion blur; new camera
   fields uploaded in `compilecamera.cpp`.
5. **Splat** — `Film_SplatLight` (none/box first, filter LUT second),
   atomic accumulation, normalization via existing merge.
6. **Wavefront** — extend `WAVEFRONT_NUM_STATES`, dispatch table, λ
   bucketing validation.
7. **Hardening** — Metal via cl2msl (atomics, no OpenCL-only builtins),
   sampler variants, volumes, spectral, RTPATHOCL tile path.

Each phase is independently testable (see below); keep commits scoped per
the fork's one-feature-per-chunk convention.

## Test scenes / validation

Validated so far (Apple Metal, 1280×720, dense dispatch):

- **Cornell + glass sphere** (`scenes/caustic-area` style): GPU
  `lighttracing.only` reproduces the CPU `LIGHTCPU` image — caustic pool
  under the sphere, color-bleed walls — and mixed mode adds the caustic
  contribution on top of eye-only.
- **Refractive enclosure, two spotlights, dark floor** (LuxBall shell:
  40.7k-vert cutout glass mesh with interior volume + inner object,
  `transparency.shadow`/`shadowoverride` enabled): per-region means GPU vs
  `LIGHTCPU` (film filter off): full frame 63.3/63.2, shell interior
  52.6/52.1, each spotlight's caustic pool within 0.2%; lit-pixel counts
  match (~571k/573k). Remaining RMSE is independent-run Monte Carlo
  noise, not bias. Isolated per-spotlight renders confirmed each emission
  cone after the `SpotLight_Emit` transform fix.
- **LMNEE single-interface** (`scenes/cornell/lmnee-quad.scn`: half-frame
  single-face glass quad, right half of the film sees the room *through*
  the glass). 1280×720 PATHOCL light tracing vs eye-path reference,
  right/refracted half: lit pixels 447,514 (dense coverage), Pearson
  spatial correlation vs eye = 0.976, mean ratio vs eye = 1.11 (within
  Monte Carlo variance). Correct refracted structure (green wall, box)
  confirms the solved direction, not the straight-line one.
- **LMNEE two-interface chain** (`scenes/cornell/lmnee-slab.scn`:
  half-frame thin glass slab — entry + exit faces, `maxspecular=2`).
  The single-vertex solve re-blocks on the exit face → `fromMnee=1` →
  `LMneeChain_Start` discovers both faces (`n=2` measured), solves the
  coupled chain. Right/refracted half: Pearson vs eye = 0.978, mean
  ratio = 0.78 — matching the *direct*-connect left-half ratio (0.82),
  i.e. the manifold branch contributes at the same rate as ordinary
  light tracing. Endpoint-weight fix verified: `fluxToRadianceFactor`
  gave a ~5× under-darkened right side (mean ≈ 13–15); `cameraPdfW`
  restores it to eye parity.
- **LMNEE mirror** (`scenes/cornell/lmnee-mirror.scn`: slab rotated
  −40° about X so it reflects the ceiling emitter). Right half renders
  the reflected geometry at correct structure (Pearson = 0.908); mean
  ratio 0.55 vs eye, tracking its own left-half direct-connect ratio
  (0.49) — the residual gap is scene-level light-tracing variance, not
  an endpoint-measure bug. The bright *emitter reflection* path
  (light→mirror→lens) is a specular-bounce path, not a delta-occluded
  connect, and is a known coverage gap (see *Risks*).
- **Diffuse-depth parity vs `LIGHTCPU`** (`scenes/cornell/lmnee-open.scn`,
  a pure-diffuse Cornell with no occluders — every connect is a plain
  light→lens splat). Per-`path.maxdepth` full-frame mean, GPU
  `lighttracing.only` vs `LIGHTCPU` (both `film.filter.type = NONE`,
  512 spp): depth 1 → 1.003, depth 2 → 1.000, depth 3 → 1.0002,
  depth 4 → 0.9997. Exact across all bounces.
- **Found & fixed — hybrid diffuse cut in `.only` mode**: enabling
  `path.lighttracing.enable` force-enables `hybridBackForwardEnable`
  (`pathtracer.cpp`), which on the light path terminates any
  non-nearly-specular vertex once `diffuse+glossy depth > 1`. That is the
  correct hybrid partition (eye tasks own the diffuse term) but it was
  also applied in `lighttracing.only` mode (`eyeTaskCount == 0`), where
  there are no eye paths — so the light path is the sole estimator and
  every depth≥3 diffuse connect was dropped. Symptom: `maxdepth` 1 and 2
  matched `LIGHTCPU` exactly but depth 4 read ~0.886× (the deep indirect
  term vanished: GPU depth-4 ≈ depth-2). Fix: gate that early-out on
  `lightTracing.eyeTaskCount > 0` (`pathoclbase_kernels_micro.cl`), so it
  only fires when eye tasks actually exist to carry the diffuse
  contribution. Regression:
  `dev-tools/lighttracing-depth-parity.sh`.
- **Filter caveat for comparisons**: the CPU splatter walks the film
  filter footprint (`FilmSampleSplatter` + `FilterLUTs`) while the GPU
  splat is a point write — comparing GPU `.only` against default-filter
  `LIGHTCPU` shows a spurious ~7-8% offset plus edge ringing. Always
  compare against `film.filter.type = NONE` (or implement the LUT walk,
  below) for structural validation.

Planned:

- **CPU parity, light-only**: extend to `scenes/causticcube/` and a
  simple analytic scene (single point light + diffuse wall: expected mean
  radiance computable). Compare per-pixel statistics (mean/RMSE over N
  passes), not just images.
- **Hybrid parity**: `PATHCPU + path.hybridbackforward.enable` (CPU eye +
  CPU light) vs `PATHOCL + light tracing` (GPU eye + GPU light) — same
  images within noise; caustic regions specifically.
- **Normalization check**: integrate the screen channel over the film for
  a known-flux emitter; verify `pixelCount/screenSampleCount` scaling gives
  the CPU-consistent magnitude (regression for the double-normalization
  hazard).
- **Cameras**: perspective (baseline), orthographic, DoF on/off, motion
  blur, clipping plane, stereo → graceful disable, environment (if in
  scope).
- **Samplers**: RANDOM vs SOBOL light dims — convergence direction and
  no-correlation check.
- **Atomics contention**: bright small emitter + large task count → no
  NaN/Inf, energy conserved; compare CAS cost under
  `LUXRAYS_WAVEFRONT_QUEUES` on/off.
- **Platforms**: OpenCL and Metal outputs bit-comparable within float
  tolerance on the same scene; `dev-tools/parity-regression.sh`-style
  gates.
- **Convergence**: RMSE vs CPU reference over passes on a caustic scene;
  `film.haltspp`/`convergence` behavior unchanged for the eye channel.

## Risks / open questions

- **Divergence** — light paths scatter arbitrarily; dense dispatch pays
  `taskCount`-wide scans for sparse light states (wavefront mitigates;
  recommend wavefront for production).
- **Splat contention** — a hot caustic concentrates atomics on few pixels;
  CAS-retry cost on Apple GPUs needs measurement; mitigation options:
  per-workgroup pre-accumulation or tile-binned splat lists (future).
- **Eye/light fraction** — static partition vs the CPU's dynamic
  `HasToRenderEyeSample` ratio; a fixed task split is simpler and stable
  under wavefront, but adaptive rebalancing is a possible follow-up.
- **Metropolis light sampler** — CPU hybrid uses it for luminance
  stabilization; deferred on GPU (Sobol light dims instead).
- **Volume edge cases** — connect rays run through `connectVolInfo`
  copies; homogeneous/heterogeneous media need targeted tests.
- **TILEPATHOCL** — subregion-clipped splats waste work; document the
  limitation rather than silently dropping splats.
- **Emitter-reflection coverage gap (LMNEE)** — LMNEE solves
  receiver→delta-chain→lens connects: the delta surfaces are
  *occluders* between a non-delta receiver and the lens. A path where
  the emitter's own image is reflected into the lens
  (light→mirror→camera, i.e. the camera sees the light source in a
  mirror) is a specular *bounce* terminating at the lens, not a blocked
  connect — the mirror vertex's straight shadow-ray connect has a
  delta BSDF (~0) toward the lens. Capturing it needs either the
  light path to actually terminate on the lens (endpoint hit) or an
  emission-side manifold seed. Not yet handled; mirror LMNEE currently
  reproduces the diffuse-through-mirror subset.
- **Apple cl2msl encode limit (measured)** — `MK_LIGHT_INIT`/`MK_LIGHT_VERTEX`
  carry `KERNEL_ARGS` + a 10-arg `KERNEL_ARGS_LIGHT` tail; they are
  compiled only when `lightTaskCount > 0` (unconditional compilation +
  dispatch crashed `AGX::ComputeContext::performEnqueueKernel` even with
  the feature off). Independently, growing the `Film` struct embedded in
  `GPUTaskConfiguration` by a single dead `int` field was enough to crash
  `MK_HIT_NOTHING` at enqueue: keep `taskConfig` fields minimal — every
  byte of the constant struct counts against the same per-kernel
  buffer/constant budget. Diagnose with `LUXRAYS_OCL_TRACE_ENQUEUE=1`
  (per-dispatch `clFinish`); the crashing kernel is the last
  `[OCL-ENQUEUE]` without a matching `-DONE`.

## Future: toward GPU BDPT

v1 deliberately discards light vertices after splatting. GPU BDPT needs,
in addition to everything above:

- **Vertex retention** — a `lightVerticesBuff[taskCount × maxDepth]`
  (position, direction, throughput, BSDF or compact descriptor, geometric
  PDFs both directions) and symmetric eye-vertex storage; the
  `LightPathInfo`/state layout here is designed to grow into it.
- **Subpath connections** — a `MK_BDPT_CONNECT` phase pairing eye×light
  vertices (the GPU `Scene_Intersect` shadow-ray consume already exists);
  the CPU `ConnectToEyeCallBack` hook (`pathtracer.cpp:1293`) is the CPU
  seam for exactly this.
- **MIS weights** — path PDFs in area measure, camera/light sampling PDFs
  (the `Camera_GetPDF` port in this design is reused directly), and the
  Veach weighting bookkeeping.
- **Scheduling** — light subpaths must complete before connecting:
  wavefront queues make this a pipeline of queue phases rather than a
  retrofit.

None of that is in v1 scope; the task/state/buffer separation chosen here
is what makes it reachable without restructuring.

## Platforms

CPU (reference), OpenCL GPU, Metal GPU — same code path via cl2msl;
`EnqueueTraceRayBuffer` is backend-agnostic
(`hardwareintersectiondevice.h:71-72`; Metal impl
`metalintersectiondevice.mm:110`). Wavefront mode is `PATHOCL`-only today.
