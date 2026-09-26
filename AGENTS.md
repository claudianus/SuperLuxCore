# Agent notes

## Out-of-core scene spilling (scene.spill.*)

`luxrays::SpillToFile()` (`include/luxrays/utils/memspill.h`) writes a
buffer to a file, maps it back `MAP_PRIVATE`, unlinks it immediately,
and returns the owner as `shared_ptr<void>`. Clean file-backed pages
are reclaimable under memory pressure; dirty pages become anonymous
COW. Demand-paged capacity, not free RAM — hot pages still occupy
memory.

Spill points (all gated by `scene.spill.enable`, size floor
`scene.spill.minbytes`, dir `scene.spill.dir` or a per-process
`superluxcore-geospill/<ts>-<ptr>` subdir under TMPDIR):

- **Geometry buffers** — `Scene::SpillGeometryBuffers()` runs BEFORE
  DataSet/BVH construction so accelerators (incl. Embree) bind the
  mapped addresses. Per-layer in `ExtMeshProp::SpillLayer`.
- **Image-map pixels** — `Scene::SpillImageMaps()` runs AFTER
  `imgMapCache.Preprocess` (resize policies/color conversion done).
  `ImageMapStorageImpl::pixels` is `shared_ptr<ImageMapPixel[]>` —
  indexed access ok, no vector API. Serialization uses `make_array`
  so mapped storage round-trips through .bcf.
- **CompiledScene staging** — `CompiledScene::SpillHostStaging()` via
  `SpillableArray<T>` (`include/luxrays/utils/spillablearray.h`).
  Called inside each OCL thread's init after `FinishQueue()`:
  later-starting devices re-read through the mappings. Mutating ops
  pull spilled arrays back to heap (edit -> CompileGeometry rebuild ->
  re-upload -> re-spill works).
- **Accelerator host nodes** — `Accelerator::SpillBVHNodes()`
  (BVHAccel/MBVHAccel), driven by `DataSet::SpillAcceleratorNodes()`
  in `PathOCLBaseRenderEngine::StartLockLess()` after all device
  threads started. `keepHot` protects the accelerator native threads
  traverse (EMBREE under ACCEL_AUTO); pure-GPU renders spill all.

## .lxm mesh proxy format

`ExtTriangleMesh::SaveProxy`/`LoadProxy` (`src/luxrays/geometry/exttrianglemeshfile.cpp`)
— V-Ray `.vrmesh`-style binary container: 128B fixed header +
64B-aligned raw sections (verts, tris, normals, UVs, colors, alphas,
vertAOV, triAOV). `LoadProxy` `MAP_PRIVATE`-maps the file and adopts
each section in place via `Buffer::Adopt`/aliasing `shared_ptr` — no
parsing, no heap copy; clean pages are demand-paged and reclaimable.
`.lxm` dispatches by extension in `Load`/`Save`; `scene.SaveMesh(name,
"x.lxm")` converts. Header flags bit1 = triangles Morton-sorted by
centroid + vertices renumbered first-use (spatial locality for paging;
loader is order-independent). Robust: bad magic/truncated/OOB counts
throw cleanly (`LxmCheckSizes` POD-layout guard).

### v2-v4: cluster index + ray-driven residency

- flags bit2 = cluster index: `LxmCluster[clusterCount]` (40B each:
  bbox + `firstTri`/`triCount` + `firstVert`/`vertCount`) at
  `clusterIndexOffset`, appended after the data sections. Clusters are
  fixed-stride (16 tri default) runs of the Morton-sorted triangles —
  the residency unit. `SaveProxy(name, stride)` /
  `Scene::SaveMeshClusterStride` expose the stride; measured on a
  717k-tri terrain: stride 16 ~2.3x faster than 64 (cluster-leaf
  intersect is a linear scan; smaller strides win until the cluster
  count's BVH-leaf overhead dominates).
- v4: vertices are renumbered *per cluster* (boundary verts
  duplicated), so `verts[firstVert..firstVert+vertCount)` +
  `tris[firstTri..firstTri+triCount)` is a self-contained upload
  payload — the >VRAM streamer's DMA unit. Older v2/v3 files load via
  an owned 32B→40B record expansion (firstVert=0,
  vertCount=hdr.vertCount = whole-mesh fallback).
- flags bit3 = baked `Normal[triCount]` triNormals at
  `triNormalsOffset`; `LoadProxy` adopts it and constructs the mesh
  field-by-field (bypassing `Init`/`Preprocess`), so loading faults in
  **zero** geometry pages — `Preprocess` skips the triNormals scan when
  `triNormals.IsExternal()`. **The section is written permuted by
  triPerm**: file triangle i is `srcTris[triPerm[i]]`, so an
  unpermuted normal dump silently misaligns normals↔triangles (this
  bug shipped in v3 bakes — old .lxm render with subtly wrong flat
  normals; rebake to fix).
- `EmbreeAccel`: a cluster-indexed mesh exports as
  `RTC_GEOMETRY_TYPE_USER` — `boundsFunc` returns stored cluster bounds
  (BVH build touches no vertex/triangle pages), `intersectFunc` walks
  the cluster's triangle range (pages fault in per-cluster on ray
  contact). Reports `primID` = real triangle index.
- `BVHAccel`/HW path is intentionally NOT cluster-aware: it exists to
  feed GPU kernels, which upload full geometry anyway, so clustered
  leaves would just break the kernel's leaf decode. CPU ray-driven
  residency lives entirely in `EmbreeAccel` (the native default).
- `ExtTriangleMesh::GetBBox` unions cluster bounds when present — no
  vertex scan.
- Measured: 1.44M-tri .lxm (87MB) → +0MB at map, +0MB at accel build,
  render faults only reached clusters (`dev-tools/cluster_residency_test.py`).

## >VRAM device streaming — design (not yet implemented)

Foundation already exists: the .lxm cluster index gives per-cluster
bounds + triangle ranges, and `BVHAccel` feeds GPU kernels a flat
triangle BVH. True >VRAM streaming needs a device-side cluster cache:

1. Device holds the cluster table (32B each — tiny) + a cluster-level
   BVH built from it, plus a fixed-capacity pool of cluster data slots.
2. Kernel traversal: at a cluster leaf, check a resident bitmap; if
   resident, walk the slot's triangles; if not, push the cluster id to
   a miss-request queue and defer the ray (ray state parked or
   re-queued next iteration).
3. A host streamer thread drains the miss queue, reads the cluster's
   triangle/vertex span straight from the .lxm mapping, async-uploads
   to a free/evicted slot, flips the resident bit.
4. Eviction: LRU over the slot pool; clusters are read-only so eviction
   is just "free the slot" — dirty pages never exist.
5. Backends: Metal sparse buffers (`MTLHeap` tile attach/detach) or
   CUDA VMM (`cuMemMap` physical chunks); OpenCL SVM is too limited —
   prefer host-side staging + regular buffers.
6. Payload format is DONE: .lxm v4 renumbers vertices per cluster, so
   `verts[firstVert..firstVert+vertCount)` + the cluster's triangle
   range is a self-contained DMA unit. Measured on a 717k-tri terrain
   at stride 16: +233% boundary duplication (360k -> 1.2M stored
   verts; fine-grained clusters share most of their verts) — the cost
   of self-contained payloads, still mmap'd so residency stays lazy.
7. `include/luxrays/utils/geomstream.h` — `ClusterResidencyPool`
   skeleton: fixed-capacity LRU slot pool with fill/evict callbacks a
   device backend subscribes to.

Unverified on this machine (unified memory — no discrete VRAM
pressure), so this is a design note for a dedicated-VRAM platform
port. The CPU cluster path above already delivers the same effect via
OS paging.

## Imagemap streaming decode

`ImageMap::Init` with a resize hint on a mip-less oversized image uses
a lazy `ImageBuf` (ImageCache tiles) + `ImageBufAlgo::resize` straight
to target size — the full-res buffer is never allocated (8192^2 PNG:
195MB -> 3MB resident). `FIXED`/`MINMEM` policies probe `GetSize()`
(header-only) then construct at target resolution. `.tx` with mips
keeps the mip-selection path; `oiio:UnassociatedAlpha` config is
forwarded to the streaming ImageBuf.

Windows: `SpillToFile`/`MapFileCopyOnWrite` use
`CreateFileMapping(PAGE_WRITECOPY)` + `MapViewOfFile(FILE_MAP_COPY)`
(POSIX `MAP_PRIVATE` equivalent); spill files use
`FILE_FLAG_DELETE_ON_CLOSE`, read-only .lxm assets fall back to
`FILE_MAP_READ`.

## Light BVH strategy (`lightstrategy.type = LIGHT_BVH`)

E&K'18 ("Importance Sampling of Many Lights with Adaptive Tree
Splitting", ACM TOG 37(4) — the technique behind Cycles' light tree).
`LightStrategyLightBVH` (`src/slg/lights/strategies/lightbvh.cpp`)
derives from `LightStrategyLogPower`: TASK_ILLUMINATE builds a
binned-SAH binary tree over direct-sampling-enabled lights; TASK_EMIT/
TASK_INFINITE_ONLY keep the flat log-power table, which also serves as
the device fallback.

- Node = `slg::ocl::LightBVHNode` (56B, `lightbvh_types.cl`): implicit
  layout, root at 0, left subtree in `[i+1, rightChildIndex)`. Per node:
  bbox, emission bounding cone (axis + thetaO), `energyFlat` (infinite/
  directional leaves — bypasses spatial bounds) + `energyLocal`.
- Importance bound at receiver: `I = E_flat + E_local/max(d^2,dmin^2)
  * cosSurf * cosOrient` — CPU `NodeImportance()` and GPU
  `LightBVH_NodeImportance` (`lightbvh_funcs.cl`) are bit-parity twins;
  keep them in sync.
- `SampleLightPdf` replays the root-to-leaf path via the
  `lightToLeaf[lightSceneIndex]` table — `lightSceneIndex` ==
  `lights[]` order == `lightDefs[]` order, so the same index works on
  both backends.
- GPU dispatch is buffer-presence driven (`if (lightBVHNodes)` before
  the DLSC check in `lightstrategy_funcs.cl`) — the call sites pass
  `NULL` when `lastOnlyInfiniteLights`/`onlyInfLights` is set, same as
  `dlscAllEntries`.
- Device buffers: `lightBVHNodesBuff` + `lightBVHLightToLeafBuff`
  (CompiledScene `lightBVHNodes`/`lightBVHLightToLeaf`
  SpillableArrays, uploaded in `InitLights`, bound in
  `SetAdvancePathsKernelArgs` right after the dlsc args — `KERNEL_ARGS`
  and `LIGHTS_PARAM_DECL` order must match).
- Regression: `dev-tools/e26_lightbvh_test.py` (unbiasedness vs
  LOG_POWER ref, same-spp RMSE bound, CPU/GPU parity, finiteness).
- clspv probe recipe in git history: cat the .cl files in
  `GetKernelSources()` order + `-D LUXRAYS_OPENCL_KERNEL
  -D LUXRAYS_OPENCL_DEVICE -D SLG_OPENCL_KERNEL -D RENDER_ENGINE_PATHOCL`.
  `Init` kernel reports a pre-existing POD-arg layout error on clspv —
  unrelated to light params (it takes none).

## Adaptive Robust Clamping (path.clamping.variance.*)

Firefly suppression in `VarianceClamping` (`src/slg/utils/varianceclamping.cpp`,
GPU twin `include/slg/utils/varianceclamping_funcs.cl` — keep them in
sync). Three orthogonal mechanisms on top of the user `maxvalue`:

- `path.clamping.variance.adaptive` (default 1): the margin is estimated
  from robust statistics of the 3x3 neighborhood of pixel means, read
  straight from the film channel buffer (no extra buffers). Bound per
  pixel: `T = max(ownMean, med) + max(sigma*mad, sqrtMax*(0.1+E))`.
  Spatially coherent bright content (sun glints, caustic patches) has a
  high neighborhood median/MAD so the bound relaxes; isolated fireflies
  sit in dark neighborhoods with tiny MAD and get clamped hard. Median/
  MAD is the online counterpart of DeCoro et al. PG'10 density-outlier
  rejection (breaks only at >50% contamination). <3 valid neighbors →
  legacy `[0, sqrtMax]` virgin bound. `adaptive=0` keeps the legacy
  fixed margin around the own-pixel mean.
- `path.clamping.variance.scope` = `all|indirect|direct` (default
  `indirect`, Cycles-style direct/indirect split). Scope enums:
  `CLAMP_ALL=0, CLAMP_INDIRECT=1, CLAMP_DIRECT=2` — the ints must match
  the CL kernel params. Under `indirect`, emission + first-vertex direct
  components are untouched and the beauty loses exactly the removed
  indirect share (beauty/AOV consistency). `SampleResult::AddEmission`/
  `AddDirectLight` fill the component fields regardless of AOV channel
  declaration, so the decomposition is always valid. Light-traced
  PER_SCREEN splats count as indirect (skipped under `direct` scope).
- `path.clamping.variance.sigma` (default 6): MAD multiplier; 6*MAD ~ 4σ
  for Gaussian neighborhoods.

Multi-group caveat: `directish` lives in group 0; `indirectY` is
computed over all groups, so with >1 radiance groups the threshold is
slightly permissive for group 0 (bookkeeping stays consistent via the
proportional `sB` scale on extra groups).

Regression: `dev-tools/e42_adaptive_clamp_test.py` (firefly suppression,
energy preservation, scope symmetry, legacy mode, CPU/GPU parity).
720p Blender-path visual: `SuperBlendLuxCore/dev-tools/clamp_visual_test.py`.

References: DeCoro et al., "A Memory Efficient Method for Variance
Estimation in Path Tracing" (PG 2010); Cycles direct/indirect clamp
split; Buisine et al. adaptive median-of-means; Zirr & Kaplanyan,
"Re-Weighting Firefly Samples" (CGF 2018).

Gotcha fixed along the way: upstream AOV clamp fallbacks read the
`*_REFLECT` channel as the expected value for `*_TRANSMIT` components
and the `else if` fallback repeated the same (dead) condition — CPU
now reads the proper TRANSMIT channel + aggregate fallback, matching
the GPU twin.

## Light linking (scene.{objects,lights}.X.linkgroups)

Receiver-based direct-illumination linking (Cycles semantics):
`scene.lights.X.linkgroups = "a,b"` puts the light in groups a,b
(`LightSource::linkMask`, 0 = global); `scene.objects.X.linkgroups` +
`.linkmode = include|exclude` sets the object's accept mask
(`SceneObject::linkAcceptMask`). A light contributes to a vertex iff
`light->IsLinkedTo(bsdf.GetLinkAcceptMask())` (mask intersect, or
light global). Names map to bits via `Scene::ParseLinkGroupMask` —
insertion order = bit order, and `Scene::ToProperties` re-emits names
in the same order so masks round-trip through .bcf.

- Filtered: NEE (`DirectLightSampling`), BSDF-sampled direct emitter
  hits (`DirectHitFiniteLight`/`DirectHitInfiniteLight` — must stay
  consistent with NEE or MIS weights leak light), and the FIRST
  surface vertex of light tracing (`RenderLightSample`,
  `BiDirCPURenderThread::TraceLightPath`, GPU light kernel).
- NOT filtered: indirect bounces (depth>=1) — a linked light still
  propagates through GI; objects receive it indirectly.
- `EyePathInfo::linkAcceptMask` carries the receiver mask to direct-hit
  tests; updated in `EyePathInfo::AddVertex` (CPU+GPU twins).
- Emissive mesh triangle lights inherit the owning object's link
  groups (`sceneobjectdefs.cpp`); env/infinite lights take
  `scene.lights.X.linkgroups` too.
- 64-bit masks use `u_longlong` host-side (NOT `u_int64_t` — POSIX
  only) and `ulong`/`~0ull` kernel-side (`~0ul` is 32-bit on Windows).
- Regression: `dev-tools/e41_lightlink_test.py` (8/8: include/exclude,
  global, mesh emitter, env, multi-group, CPU/GPU parity, roundtrip).

## Wavefront queues (LUXRAYS_WAVEFRONT_QUEUES=1)

Per-state task queues opt-in for PATHOCL (dense stays default).
Iteration: `BucketHistogram` (per-state x 3-lambda-bin counts) ->
`QueuePrefix` (device exclusive prefix -> `taskQueueBase` cursors +
`taskQueueTotals` + re-zeroes counts) -> `BuildQueues` (atomic-append
task indices) -> one kernel launch per non-empty state, guarded by
`WAVEFRONT_GUARD` against `taskQueueTotals[state]`.

Measured on cornell 720x720 PATHOCL Metal (30 s): dense ~9.5M
samples/s vs wavefront ~24M — wavefront wins ~2.5x from COMPACT
launches (dense launches every state kernel at taskCount and
early-outs internally; wavefront launches only queued lanes).

- The totals are read back once per iteration (72 B) to size the
  launches. Do NOT replace this with stale/async sizing: measured a
  ~6x regression when totals lagged by a resync period — tasks landing
  in a state's queue tail beyond the stale launch size sit unexecuted
  until the next resync (the guard protects lanes, not progress).
- Metal/Vulkan `EnqueueReadBuffer` ignores the blocking flag and calls
  `FinishQueue()` (shared storage needs a drained queue before the
  host memcpy) — every host read/write on those backends is a queue
  drain, so batch them; the QueuePrefix device-side prefix removed the
  old read-histogram + host-prefix + upload-bases round trip (2 syncs
  -> 1).
- `dev-tools/wavefront-regression.sh` needs `pipefail` (the
  compare|tee pipe swallowed the comparison exit code — printed a
  0.2556 reldiff vs tol 0.2 as PASS once) and requires WFDBG records
  to exist (an absent debug log would vacuously pass the oob/dup/
  badState/badLambda==0 check).

## Gotchas

- Device uploads must be drained (`FinishQueue()`) before host arrays
  are swapped — CUDA HtoD and same-size OCL buffer reuse are async.
- Metal `AllocBuffer` uses `newBufferWithLength` + memcpy (no host
  aliasing); MetalRTAccel uses `newBufferWithBytes` — safe to spill
  source arrays after upload.
- PATHOCL on Apple Silicon: an empty `opencl.devices.select` picks
  BOTH OPENCL_GPU and METAL_GPU (same physical GPU) and crashes inside
  AGX OpenCL-over-Metal encode — pre-existing, unrelated to spilling.
  Select a single device.
- Vulkan on macOS needs MoltenVK loaded: `volkInitialize` only tries
  leaf-name dlopens (no DYLD_* env → silent zero-device enumeration).
  vkdevice falls back to `dlopen(abs path)+volkInitializeCustom` over
  `LUXRAYS_MOLTENVK`, `~/.luxcore/vktools/lib/libMoltenVK.dylib`, and
  module-adjacent paths. `dev-tools/vulkan-tools-install.sh` installs
  the clspv toolchain + MoltenVK into `~/.luxcore/vktools` (layout
  mirrors the clspv build tree — `opt`/`llvm-dis` resolve via
  `<clspv>/../third_party/llvm/bin`). `LUXRAYS_CLSPV` still wins if set.
- Bool scene props must be typed: `scene.spill.enable = true` inside
  `SetFromString` parses to false (lexical_cast accepts only 0/1) —
  spill silently no-ops. Use `= 1` or a typed `Property(name, True)`.
- `.lxm`-loaded meshes set `buffersFromFileMapping` and are skipped by
  `SpillBuffers` — re-spilling would fault every mapped page in and
  rewrite them to fresh files.
- `.bcf`/`.bsc` serialization stores a file-backed `.lxm` mesh as a
  name+transform stub; on load the stub is re-mapped via
  `ExtTriangleMesh::LoadProxy` (missing file → clear runtime_error).
  Meshes whose buffers are NOT file-backed serialize in full as before.
- `pysuperluxcore.Scene(props)` single-Properties overload is the
  resize-policy ctor (empty scene) — use `Scene()` + `scene.Parse()`.
  `session.Parse()` handles film props only; scene edits go through
  `scene.*` calls inside BeginSceneEdit/EndSceneEdit.
- Halt conditions are evaluated inside `Film::RunTests()`, which only
  runs during `UpdateFilm`/`UpdateStats` — a bare `WaitForDone()`
  never returns. Poll `HasDone()` + `UpdateStats()`.
- `batch.halttime` measures SAMPLING time: `RenderEngine::Start()` and
  `EndSceneEdit()` call `film->RestartSampleClock()` after the render
  threads (re)start, so kernel compilation no longer eats the halt
  budget (a cold Metal build is ~100 s — previously a 25 s limit fired
  instantly and produced a near-empty denoised image). A resumed
  session likewise gets a fresh per-session clock.
- With no `film.imagepipeline*`/`film.imagepipelines*` defined the film
  applies `AutoLinearToneMap` + gamma 2.2 (`Film::CreateImagePipeline`
  fallback) — it normalizes the image mean to ~0.5, so furnace/energy
  tests that read `RGB_IMAGEPIPELINE` see ~0.51 regardless of material
  or light gain. Measure radiance via the raw `RGB` output or set
  `film.imagepipelines.0.0.type = NOP`.
- Integer film outputs (OBJECT_ID, MATERIAL_ID, CRYPTOMATTE) need
  `Film.GetOutputUInt` + a `np.uint32` buffer — `GetOutputFloat` throws
  "Unknown film output type". Outputs must also be declared
  (`film.outputs.N.type = OBJECT_ID`) or GetOutput fails "not available".
- PATHOCL device types on Apple Silicon are `METAL_GPU`/`VULKAN_GPU`,
  not `OPENCL_GPU` — device selection masks must match those names.
- Per-object luminance tests should compare the MEDIAN of the object's
  OBJECT_ID pixels, not the mean: the film pixel filter bleeds bright
  neighbours into silhouette-edge pixels (~0.01 luminance on "unlit"
  objects).
- `HardwareDevice::AllocBuffer(&ptr, ...)` overwrites a non-null `ptr`
  WITHOUT freeing — re-allocating an existing member leaks the old
  buffer and its `usedMemory` accounting (the "memory leak in LuxRays
  HardwareDevice" shutdown warning). Free first or keep init paths
  idempotent (`ThreadFilm::Init` now calls `FreeAllOCLBuffers()` up
  front). Conversely `FreeBuffer` on an uninitialized member crashes —
  every `HardwareDeviceBuffer*` member must be nullptr-set in the ctor.
  `InitFilm()` must not loop-Init after `IncThreadFilms()` — that call
  already inits the new film.

## OpenPBR / SSS debugging findings (e35)

- `GgxSampleVNDF(wo, alphaX, alphaY, u0, u1)` — callers must keep the
  alphas before the random draws. OpenPBR originally passed
  `(wor, u0, u1, alphaT, alphaB)`; the sampler then used the uniforms as
  roughness (~0.5 effective alpha) while eval computed pdfs with the
  intended alpha → ~57% energy loss on every glossy/BTDF vertex.
- `FresnelDielectricModulated` must test the *physical* TIR condition
  (`etaTI < 1 && 1-cosI^2 > etaTI^2`) before the specular_weight
  modulation shortcut — with `specularweight=0` the modulated eta is 1
  and the early return would otherwise report F=0 for directions the
  sampler's TIR fallback reflects at pdf>0 (energy loss).
- OpenPBR interior IOR convention: `hitPoint->interiorIorTexIndex`
  (GPU) / `GetInteriorVolume()->GetVolumeIOR()` (CPU) is ray-relative.
  When the material has no interior volume the fallback must be
  `specular_ior`, not 1 — otherwise eta=1 collapses the transmission
  half-vector and BTDF eval returns 0.
- eta = n(wi side)/n(wo side) with `wo.z>0` = entering. For internal
  rays (`wo.z<0`) nWo comes from the interior volume, nWi is the
  exterior — swapping them inverts refraction (~80% loss). On TIR the
  sampler must reflect off the microfacet (the direction is scored by
  the specular lobe's eval/pdf), never kill the path.
- Eval functions that take TIR-reflected directions must canonicalize
  `wor`/`wir` into the +z hemisphere before computing `wh = w_o + w_i`
  — raw internal directions give wh.z<0 and early-out on backfacing.
- Walter BTDF f*cosI has NO `|wi.z|` factor (see roughglass.cpp); an
  extra `|wi.z|` attenuates every transmitted path by ~0.6.
- Albedo-parametrized SSS volumes already reproduce `subsurface_color`
  as diffuse reflectance — the OpenPBR interface tint must be white
  (`sssAlbedoMedium`), else the color is applied twice. CPU reads it
  via `GetInteriorVolume()` + `IsSSSParametrized()`; GPU mirrors it via
  `mats[material->interiorVolumeIndex].volume.homogenous.sssAlbedoTexIndex`.
- `hitPoint.passThroughEvent` feeds THREE consumers (surface lobe pick,
  stochastic transparency, volume pick). `Scene::Intersect` must draw a
  FRESH random for the volume free-flight sample — reusing passThrough
  correlates the escape decision with the boundary lobe pick (~8%
  energy loss on a matched glass shell, +48% on interior NEE before the
  MIS fix). GPU mirrors this via `Rnd_InitFloat(passThrough, &volSeed)`.
- `scene.objects.X.transformation` takes 16 values in COLUMN-major
  order (Property::Get<Matrix4x4> reads v0,v4,v8,v12 as row 0). A
  row-major translation string silently lands in the projective row.
- Ninja multi-config: `ninja -C out/build pysuperluxcore` builds Debug only;
  the Release module needs `-f build-Release.ninja pysuperluxcore`.

## Huang hair (e36) findings

- `FresnelDielectricT` (and LuxCore's dielectric Fresnel helpers) return a
  POSITIVE transmitted cosine; Cycles' `fresnel_dielectric` returns a
  SIGNED negative one. Any ported Cycles refraction code must negate the
  cosine passed to `refract_angle`-style helpers — otherwise wt points
  above the surface and every TT/TRT visibility guard kills the path
  (furnace energy ~0.31 -> 0.97 after the fix).
- Cycles `reflect(i,n) = i - 2(i.n)n`, so their `-reflect(i,n)` already
  equals LuxCore's `ReflectDir(i,n) = 2(i.n)n - i`. Adding a '-' flips
  the reflected direction below the surface and all samples die.
- Huang'22 curve kernel -> fixed-surface-point conversion: `f*cos =
  kernel / (arc_i * cosMi)`. The R lobe then reduces exactly to the
  standard slanted-GGX BRDF `F*D*G*scale/(4*cosMi)` — verified against
  brute-force microfacet MC (the curve kernel's jac/arc factors are
  measure-change terms, they do not belong in the surface BSDF).
- `roughness` (artist param) IS the GGX alpha directly; the energy LUT
  axis is `sqrt(alpha)` (Cycles convention).
- Chiang's far-field model is only energy-conserving on `ribbon`
  tessellation (~0.90 furnace). On `solid` cylinders grazing azimuths
  lose ~40% — that is the known far-field limitation Huang'22 solves
  (Huang: 0.97 on the same geometry). Furnace tests must pick the
  tessellation that matches the model's domain.
- aspectratio<1 legitimately reads <1 in a furnace: the elliptical
  cross-section has a narrower projected silhouette, hits outside
  |h|>radius are transparent by design (same as Cycles).
- Huang eval uses deterministic Hammersley VNDF quadrature for TT/TRT
  (Evaluate must be a pure function of wi/wo); Sample uses the Cycles
  self-normalized estimator (f/pdf = eval, pdfW = 1, energy-proportional
  lobe pick).
