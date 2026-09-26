# Out-of-core memory: spilling, .lxm proxies, streaming

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

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

