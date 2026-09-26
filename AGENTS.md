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
`luxcore-geospill/<ts>-<ptr>` subdir under TMPDIR):

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
- `pyluxcore.Scene(props)` single-Properties overload is the
  resize-policy ctor (empty scene) — use `Scene()` + `scene.Parse()`.
  `session.Parse()` handles film props only; scene edits go through
  `scene.*` calls inside BeginSceneEdit/EndSceneEdit.
- Halt conditions are evaluated inside `Film::RunTests()`, which only
  runs during `UpdateFilm`/`UpdateStats` — a bare `WaitForDone()`
  never returns. Poll `HasDone()` + `UpdateStats()`.
