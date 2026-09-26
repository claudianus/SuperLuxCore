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

### v2/v3: cluster index + ray-driven residency

- flags bit2 = cluster index: `LxmCluster[clusterCount]` (32B each:
  bbox + `firstTri`/`triCount`) at `clusterIndexOffset`, appended after
  the data sections. Clusters are fixed-stride (16 tri default) runs of
  the Morton-sorted triangles — the residency unit. `SaveProxy(name,
  stride)` / `Scene::SaveMeshClusterStride` expose the stride; measured
  on a 717k-tri terrain: stride 16 ~2.3x faster than 64 (cluster-leaf
  intersect is a linear scan; smaller strides win until the cluster
  count's BVH-leaf overhead dominates).
- flags bit3 = baked `Normal[triCount]` triNormals at
  `triNormalsOffset`; `LoadProxy` adopts it and constructs the mesh
  field-by-field (bypassing `Init`/`Preprocess`), so loading faults in
  **zero** geometry pages — `Preprocess` skips the triNormals scan when
  `triNormals.IsExternal()`.
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
6. Payload gap: today's vertex renumbering is global first-use order,
   so a cluster's referenced vertices are NOT contiguous. A v4 stream
   format needs per-cluster local vertex streams (cluster-referenced
   vertex subset packed next to its triangles) for self-contained
   uploads — CPU paging tolerates scatter, DMA uploads don't.
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
- `pyluxcore.Scene(props)` single-Properties overload is the
  resize-policy ctor (empty scene) — use `Scene()` + `scene.Parse()`.
  `session.Parse()` handles film props only; scene edits go through
  `scene.*` calls inside BeginSceneEdit/EndSceneEdit.
- Halt conditions are evaluated inside `Film::RunTests()`, which only
  runs during `UpdateFilm`/`UpdateStats` — a bare `WaitForDone()`
  never returns. Poll `HasDone()` + `UpdateStats()`.
- With no `film.imagepipeline*`/`film.imagepipelines*` defined the film
  applies `AutoLinearToneMap` + gamma 2.2 (`Film::CreateImagePipeline`
  fallback) — it normalizes the image mean to ~0.5, so furnace/energy
  tests that read `RGB_IMAGEPIPELINE` see ~0.51 regardless of material
  or light gain. Measure radiance via the raw `RGB` output or set
  `film.imagepipelines.0.0.type = NOP`.
