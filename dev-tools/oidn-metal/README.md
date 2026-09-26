# OIDN Metal backend — CLT-only build patch (OIDN 2.5.1)

This directory holds the patch used to build `libLuxOpenImageDenoise_device_metal`
on machines that have **only the Xcode Command Line Tools** (no full Xcode).

## Why this exists

Upstream OIDN compiles its Metal kernels to a `.metallib` at build time with
`xcrun metal`/`metallib`, which ship only with full Xcode. LuxCore instead
compiles its own Metal kernels **at runtime** from embedded MSL source
(`newLibraryWithSource:`), which works with CLT alone. This patch applies the
same strategy to OIDN's Metal device module:

1. `cmake/metal_source.py` preprocesses `devices/metal/metal_kernels.metal`
   into a single self-contained MSL file:
   - `clang -E -D__METAL_VERSION__=310` selects the `OIDN_COMPILE_METAL_DEVICE`
     code path in `common/platform.h`.
   - An empty stub `metal_stdlib` on the include path lets the preprocessor
     resolve `#include <metal_stdlib>`; the real include is re-prepended to the
     output so the *runtime* Metal compiler resolves the standard library.
   - linemarkers and `#pragma` lines are stripped.
2. `cmake/oidn_metal.cmake` embeds the preprocessed source as a binary blob
   (instead of embedding a compiled `.metallib`). It calls `blob_to_cpp.py`
   directly — upstream's `oidn_generate_cpp_from_blob()` cannot take
   build-tree inputs (it maps paths via `oidn_get_build_path()`, which returns
   an empty dir for build-tree files, producing a broken `/metal_kernels.cpp`
   target path).
3. `devices/metal/metal_engine.mm` compiles the embedded source at runtime
   with `newLibraryWithSource:` (fastMath disabled and Metal 3.0 language
   version, matching upstream's `-fno-fast-math -std=metal3.0`).

## Applying

```bash
git clone --recursive --depth 1 --branch v2.5.1 \
    https://github.com/OpenImageDenoise/oidn.git
cd oidn
git lfs install && git lfs pull        # neural weights (.tza) are LFS objects
git apply /path/to/oidn-2.5.1-metal-runtime-compile.patch
cp /path/to/metal_source.py cmake/
```

Configure with the same options the LuxCoreDeps recipe uses, plus Metal:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DOIDN_DEVICE_CPU=ON -DOIDN_DEVICE_METAL=ON \
  -DOIDN_APPS=OFF -DOIDN_FILTER_RT=ON -DOIDN_FILTER_RTLIGHTMAP=ON \
  -DOIDN_API_NAMESPACE=lux -DOIDN_LIBRARY_NAME=LuxOpenImageDenoise \
  -DTBB_ROOT=<onetbb package dir>
cmake --build build --target OpenImageDenoise OpenImageDenoise_core \
      OpenImageDenoise_device_cpu OpenImageDenoise_device_metal -j
```

Important integration notes learned while validating:

- The device module list is compiled into **`libLuxOpenImageDenoise` (the API
  library)** — `Context::init()` in `core/context.h` is inline but instantiated
  in `api/api.cpp` under `#if defined(OIDN_DEVICE_*)` guards. Rebuilding only
  `*_core` or dropping in the `device_metal` dylib is **not** sufficient; the
  API library must be built with `OIDN_DEVICE_METAL=ON`.
- Device dylibs are `dlopen`ed at runtime from the directory containing the
  loaded core library (`modulePathPrefix` = `dladdr` dir). Make sure no stale
  build-tree `LC_RPATH` remains in the deployed dylibs
  (`otool -l | grep -A2 LC_RPATH`; remove with
  `install_name_tool -delete_rpath <path>`), or module resolution will look in
  the wrong directory.
- `OIDN_VERBOSE=3` prints module loading and device info — look for
  `Loaded module: 'libLuxOpenImageDenoise_device_metal.2.5.0.dylib'` and
  `Device    : <GPU name>\n    Type    : Metal`.

## Validation results (Apple M5 Pro, OIDN 2.5.1)

- Standalone `RT` filter, 512×512 HDR: **Metal 3.8 ms vs CPU 65.6 ms (~17×)**,
  identical denoise delta (mean|out-in| 0.0136 both).
- LuxCore `INTEL_OIDN` image pipeline (`cornell-denoise` style cfg):
  `Loaded module: device_metal`, `Type: Metal`, `Arch: applegpu_g17s`,
  correct denoised output with albedo+normal inputs.
- CPU fallback intact: `newDevice(CPU)` loads `device_cpu`, `newDevice(Default)`
  picks Metal automatically.

## Conan recipe integration (proven end-to-end)

The LuxCoreDeps recipe carries this patch plus two options:

- `oidn/*:with_device_metal=True` — builds `device_metal` (set in
  `conan-profiles/conan-profile-macOS-ARM64`)
- `oidn/*:metal_embed_source=True` — applies this patch in `build()`, so the
  package compiles **without Xcode** (CLT only)

`conan create` on OIDN **2.5.1** was verified on a CLT-only machine: the
produced `oidn/2.5.1@luxcore/luxcore` package's `device_metal` runs the RT
filter on Metal (~6.6 ms for 512×512). Recipe changes live in our
LuxCoreDeps fork (local clone at `../LuxCoreDeps`, commit `736f92b`).

## Path to upstream / LuxCoreDeps

Two repos must change to ship this properly:

1. **LuxCoreDeps** (`conan-local-recipes/recipes/oidn/all/conanfile.py`):
   `with_device_metal=True` for macOS, and add
   `f"{library_name}_device_metal.{version}"` to `cpp_info.libs` for macOS
   shared builds. Upstream CI builds on `macos-15` runners (full Xcode), so
   upstream does **not** need this CLT patch — the standard `xcrun metal`
   path works there.
2. **LuxCore** `src/slg/film/imagepipeline/plugins/intel_oidn.cpp`: request
   `DeviceType::Metal` on `__APPLE__` with CPU fallback (already in tree,
   commit `30dc89ab3`).

For our own dependency bundles (CLT-only machines), the fork's recipe
applies this patch via `metal_embed_source` so the package builds without
Xcode anywhere. Remaining step for shipping: push the fork, let CI produce
the full dep bundle (incl. `with_device_cpu=True` + ISPC), tag a dep
release, and point `build-system/build-settings.json`
`Dependencies.user/release` at it.
