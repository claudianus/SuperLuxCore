# Film hardware image pipeline + OIDN denoising

Status: implemented. The film tone-map / output image pipeline runs on the
render device (incl. Metal); OIDN runs on the **Metal device** on Apple Silicon
with automatic CPU fallback.

## What and why

Two hardware-acceleration items around the film/output stage:

1. **Hardware film pipeline** — the image pipeline (tonemap, gamma, plugin
   chain) executes as GPU kernels on the same device as the render, instead of
   downloading the film to the CPU each output.
2. **OIDN** — Intel's machine-learning denoiser removes residual Monte Carlo
   noise; production renders rely on it for usable output at moderate spp.

## Implementation

- `Film hardware image pipeline: run on the render engine's device` — the
  pipeline kernels are compiled and run on the render device (OpenCL or
  Metal), so film output stays on-GPU.
- `OIDN: use the Metal device on Apple Silicon, fall back to CPU` — on
  `__APPLE__` the plugin requests `oidn::DeviceType::Metal`; if creation
  fails it retries `DeviceType::CPU` (`src/slg/film/imagepipeline/plugins/
  intel_oidn.cpp`, commit `30dc89ab3`).

## Metal device backend

The vendored OIDN 2.5.0 dependency bundle previously shipped only the CPU
device binary. A Metal-enabled build has been produced and validated:

- Device dylib `libLuxOpenImageDenoise_device_metal.2.5.0.dylib` loads at
  runtime (`OIDN_VERBOSE=3` → `Loaded module`, `Type: Metal`,
  `Arch: applegpu_g17s`).
- The OIDN **API** library must be built with `OIDN_DEVICE_METAL=ON` — the
  device-module table is compiled into `api/api.cpp` (inline
  `Context::init()`), so adding the dylib alone does nothing.
- Denoising runs on GPU: 512×512 RT filter — **Metal 3.8 ms vs CPU 65.6 ms**
  (~17×), same output delta as CPU.
- `newDevice(Default)` selects Metal; `newDevice(CPU)` still loads
  `device_cpu`, so the LuxCore CPU fallback path works.
- The build was produced **without full Xcode** (CLT only) by compiling the
  embedded Metal shader source at runtime — see
  `dev-tools/oidn-metal/README.md` for the patch and rationale.

### Remaining integration step

The deployed bundle (`out/dependencies/.../oidn`) is still the upstream
package built with `with_device_metal=False`, plus a locally dropped
`device_metal` for validation. The reproducible path is **proven**: our
LuxCoreDeps fork's recipe (`with_device_metal=True` +
`metal_embed_source=True`, OIDN **2.5.1**) produces a working Metal package
via `conan create` **without Xcode** (see `dev-tools/oidn-metal/README.md`,
LuxCoreDeps commit `736f92b`). Shipping needs: push the fork, CI-build the
full dep bundle (with `device_cpu` too), tag a dep release, and point
`build-settings.json` `Dependencies` at it.

## References

- Intel Open Image Denoise — https://www.openimagedenoise.org (ML denoiser).
- OIDN device backends — https://github.com/OpenImageDenoise/oidn

## Platforms

Film HW pipeline: OpenCL + Metal. OIDN: Metal on Apple Silicon (validated),
CPU fallback everywhere; GPU where a device build exists (CUDA/ROCm/SYCL).
