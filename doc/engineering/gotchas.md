# Build / platform / debugging gotchas

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Gotchas

- **cl2msl + `#define NAME value // comment`**: the trailing `//` is
  part of the macro EXPANSION — `return LPE_SYM_V;` becomes
  `return 6 // volume scattering vertex;` and swallows the rest of the
  line (ternary `:`, call args, `;`). Keep comments out of #define
  values in shared .cl headers — comment block above instead. Metal
  compile errors surface only as "Metal program compilation error";
  the real diagnostics are reachable by re-compiling the cached
  translation: `newLibraryWithSource` on the newest .msl in
  `~/Library/Caches/LuxCoreRender/metal/` (or `xcrun metal -c` — but
  that one hides some diagnostics the runtime hits).
- `clz`/`ctz` are not in the cl2msl function map — use a portable bit
  loop (`while (v >>= 1) ++i`) or extend cl2msl.py's preamble defines.
- Device uploads must be drained (`FinishQueue()`) before host arrays
  are swapped — CUDA HtoD and same-size OCL buffer reuse are async.
- Metal `AllocBuffer` uses `newBufferWithLength` + memcpy (no host
  aliasing); MetalRTAccel uses `newBufferWithBytes` — safe to spill
  source arrays after upload.
- PATHOCL on Apple Silicon: an empty `opencl.devices.select` picks
  BOTH OPENCL_GPU and METAL_GPU (same physical GPU) and crashes inside
  AGX OpenCL-over-Metal encode — pre-existing, unrelated to spilling.
  Select a single device.
- pybind11 `py::smart_holder` + non-owning reference returns: a method
  returning `const unique_ptr<T>&` (e.g. `RenderConfigImpl::GetProperties`)
  can only materialize by aliasing the parent's shared holder — it throws
  "Non-owning holder (load_as_shared_ptr)" when the Python wrapper itself
  is the non-owning one (`RenderSession.GetRenderConfig()` returns a
  `cref` to the session's member). Fix pattern used in
  `pysuperluxcore.cpp`: `GetProperties` returns `Clone()` (owned) and
  `GetRenderConfig` carries `py::keep_alive<0,1>` so the borrowed config
  wrapper keeps the session alive. Prefer `GetProperty(name)` (returns by
  value) for scalar reads.
- Vulkan on macOS needs MoltenVK loaded: `volkInitialize` only tries
  leaf-name dlopens (no DYLD_* env → silent zero-device enumeration).
  vkdevice falls back to `dlopen(abs path)+volkInitializeCustom` over
  `LUXRAYS_MOLTENVK`, `~/.luxcore/vktools/lib/libMoltenVK.dylib`, and
  module-adjacent paths. `dev-tools/vulkan-tools-install.sh` installs
  the clspv toolchain + MoltenVK into `~/.luxcore/vktools` (layout
  mirrors the clspv build tree — `opt`/`llvm-dis` resolve via
  `<clspv>/../third_party/llvm/bin`). `LUXRAYS_CLSPV` still wins if set.
- MoltenVK fork rebuild (after editing External/SPIRV-Cross —
  `MoltenVKShaderConverter/SPIRV-Cross` is a symlink to it, branch
  `luxcore-psb-msl-fixes` on github.com/claudianus/SPIRV-Cross):
  SPIRV-Cross is NOT rebuilt by `make macos` — it's a prebuilt static
  lib. Sequence:
  1) `xcodebuild build -project ExternalDependencies.xcodeproj
     -scheme SPIRV-Cross-macOS -destination "generic/platform=macOS" -quiet`
  2) `cp External/build/Intermediates/XCFrameworkStaging/Release/Platform/
     libSPIRVCross.a External/build/Release/SPIRVCross.xcframework/
     macos-arm64_x86_64/`
  3) `xcodebuild build -project MoltenVKPackaging.xcodeproj
     -scheme "MoltenVK Package (macOS only)"
     -destination "generic/platform=macOS" -quiet`
  4) install `Package/Release/MoltenVK/dynamic/dylib/macOS/
     libMoltenVK.dylib` → `~/.luxcore/vktools/lib/` + `codesign -s -`.
  `LUXRAYS_MVK_SHADER_DUMP=<dir>` dumps each compiled shader's .spv +
  generated .metal — required to see SPIRV-Cross codegen failures.
  Module-scope `OpVariable PhysicalStorageBuffer` (clspv
  `-module-constants-in-storage-buffer` constant tables) was the last
  MSL codegen hole: emitted as program-scope `constant` + `ulong`-hop
  cast (fork `f7e6f6da`,`69472361`).
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
- `Film.GetOutputFloat` rows are BOTTOM-UP (row 0 = bottom scanline) —
  `np.flipud(buf.reshape(H, W, 3))` before saving to PNG, or the image
  comes out vertically flipped.
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

