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

## Component-decomposed recipe (`mode = "components"`)

The plugin can denoise each radiance component independently and
recombine, instead of filtering the beauty in a single pass
(`src/slg/film/imagepipeline/plugins/intel_oidn.cpp`):

- Channels used: `DIRECT_DIFFUSE`, `DIRECT_GLOSSY`, `INDIRECT_DIFFUSE`,
  `INDIRECT_GLOSSY`, `INDIRECT_SPECULAR`, `EMISSION`, plus `ALBEDO` and
  `AVG_SHADING_NORMAL` guides. All are requested automatically when the
  mode is parsed (no manual AOV setup needed).
- **Albedo demodulation** (`*.demodulate`, default on): the indirect
  diffuse component is divided by the albedo guide before filtering and
  multiplied back afterwards — the illumination-only signal is smooth,
  so the network can not blur texture detail. Demodulated inputs are
  filtered against a *neutral* albedo guide: feeding the real albedo
  back in re-injects the texture as hallucinated blotches.
- **Emission passthrough** (`*.emission.denoise`, default off): emitters
  are usually converged and denoising bleeds their silhouettes.
- **Firefly suppression** (`*.firefly.sigma`, default 0): per-component
  median+MAD outlier clamp. A pixel is clamped only when it exceeds the
  local robust limit *and* dominates its 5×5 ring (isolation test keeps
  spatially correlated texture intact). Intended for sparse fireflies;
  on dense-noise inputs prefer raising spp.
- **Exact recombination**: `out = Σ denoised components + residual`,
  where `residual = beauty − Σ raw components` covers channels the
  decomposition does not model, so total energy is preserved.

Config example:

    film.imagepipeline.0.type = INTEL_OIDN
    film.imagepipeline.0.mode = components
    film.imagepipeline.0.demodulate = 1
    film.imagepipeline.0.emission.denoise = 0
    film.imagepipeline.0.firefly.sigma = 0

Measured on `scenes/classroom` at 8 spp / 720p (MSE vs a 256 spp
reference): combined 0.0370, components 0.0363, components + firefly
σ=3 0.0318. Runs on PATHCPU and PATHOCL; the component buffers are
filled by the existing `SampleResult` channels so no render-side changes
are needed.

## References

- Intel Open Image Denoise — https://www.openimagedenoise.org (ML denoiser).
- Keller, "Path Tracing" SIGGRAPH 2019 course — albedo demodulation is a
  standard ingredient of production reconstruction filters.
- RenderMan denoise JSON config / V-Ray render elements — per-component
  (diffuse/specular, direct/indirect) denoising with recombination.
- OIDN device backends — https://github.com/OpenImageDenoise/oidn

## Platforms

Film HW pipeline: OpenCL + Metal. OIDN: Metal on Apple Silicon (validated),
CPU fallback everywhere; GPU where a device build exists (CUDA/ROCm/SYCL).
