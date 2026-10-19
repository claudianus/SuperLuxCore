# Metal startup: avoid unreachable PSOs and unrelated prefill

## Root cause and bounded change

The retained diagnostic log separated the stages: the offline `.metallib` was
ready at 7.595 s, but the 19-kernel pipeline set completed at 159.902 s. Total
kernel preparation was 157.940 s. Several expensive micro-kernels completed
near 140-152 s, including ReSTIR visibility, GI and PT states that were disabled.
The process sample shows Metal compiler work, not a Python GIL deadlock.

`InitKernels()` now creates ReSTIR visibility/GI/PT PSOs only when their exact
existing dispatch-reachability conditions are true. The all-disabled case
creates 14 rather than 19 PSOs. Enabling a feature still creates all of its
handlers. A selection mask participates in the in-session kernel identity, so
later feature changes cannot silently reuse an incomplete set. Disabling a
feature releases its old handlers after the render threads have stopped.

This does not change kernel source, MSL, floating-point options, material
specialization, sample counts, integrators or estimator defaults. In particular
it does not adopt size optimization or a new noinline boundary: this translator
has historical numerical problems with some such boundaries.

The Blender worker also skips generic `KernelCacheFill()` for explicit and
default automatic Metal selection. Automatic detection mirrors the core's
GPU-use gate and CUDA precedence; Vulkan remains explicit-only. That routine renders matte test scenes with PATHOCL
and RTPATHOCL, without copying the actual spectral/options/type configuration.
Its `.ck` existence test is not a Metal binary-archive lookup. The real session
already compiles its exact program on the worker. Other backend fill policies
remain covered by a separate regression.

## Evidence and scope

`review_optional_kernels_test.py` compiles and executes the actual C++ selection
block over 64 gate combinations, checking 8 selection identities, exact handler
membership and release of disabled handlers. `review_metal_prefill_test.py`
checks the actual worker function for Metal, OpenCL and Vulkan device selection.

`run_review_metal_pso.py` executes a sequential ABBA diagnostic with the same
prepared MSL, same optimization flags and unique kernel entry-point namespaces.
It retains archive misses, PSO time and archive-add wait separately. Results:

| Selection | PSO set time |
|---|---:|
| A1: 19 kernels | 26.532 s |
| B1: 14 kernels | 23.539 s |
| B2: 14 kernels | 22.968 s |
| A2: 19 kernels | 28.081 s |

Archive-add wait was tiny compared with PSO generation. macOS internal compiler
caches were not erased, so these are namespaced PSO-set diagnostics, not a
reproduction of the historical 157-second fully cold startup. Do not report
157 -> 23 s as an improvement. The mean set-time reduction in this diagnostic
is about 14.8%; renderer/Blender startup and heavy-scene responsiveness require
their own receipts.

All evidence, admission records, source/binary fingerprints and Blender results
are under workspace `dev-tools/reviews/2026-10-16/metal-startup/`. Rendering
artifacts use `test-scenes/validation-2026-10-16/metal-startup/`.

## Signed spectral RGB in the viewport smoothing filter

Real CPU Junk Shop resize/orbit validation exposed a separate existing display
bug. Finite raw RGB, albedo and normal outputs became NaN only after the viewport
pipeline. `review_viewport_smooth_test.py` executes the actual filter body and
reproduces overflow for a center RGB of `(-0.04, -0.04, -0.04)` next to brighter
pixels. The previous signed color-distance scale makes the exponential grow
without bound for center channel sum below -0.1; normalization then gives
`inf/inf`. Spectral RGB may legitimately contain negative channels.

The scale now uses the magnitude of the center channel sum. This bounds the
color weight without clamping radiance or changing sampling, spectral transport,
raw film data, converged display values or final-render pipelines. Ordinary
nonnegative-center filtering is bit-exact against the previous body. The test
also covers the zero-scale boundary, negative and HDR RGB, and raw/converged
preservation. The native module was rebuilt and CPU/Metal Blender checks were
repeated for its new binary fingerprint; old binary receipts remain distinct.

## Final candidate and measured scope

- Installed native SHA256: `927269ffad2caf49a5368441d2c414a9cd054ca1547c669c96d46e41057a8c2b`.
- Worker SHA256: `b0b3ad6ffa99347cd5a0a3263724e1a3b45c6de92831c1dd1bff9819e1ddb879`.
- Framebuffer SHA256: `888d08dae84eecfb3dfa12473ee1aaa59e73e590cb4c58e26e9d61e123146911`.
- Engine viewport SHA256: `34c24baef79a06ca4e2f784d487be8a49fdb78bc72f2049877b45d6e2872e653`.
- Translator SHA256: `8fdcc4918de6b5b3b686c1c290f37aca4cd2abf308abe1662a337b2a7d298215`.

All seven final correctness cases pass on that native candidate: optional
Metal states, heavy CPU/Metal, 720p viewport CPU/Metal and 720p final CPU/Metal.
The CPU heavy scene passed twice after the signed-RGB smoothing fix. Its paused
raw radiance still includes negative values (minimum about -9.92), while raw,
guides, cached/fresh pipelines and isolated NOP/infill/smooth/temporal chains
all contain zero nonfinite values. Each successful heavy run drained its workers
and exited normally. The final optional-device receipt records `METAL_GPU` and
zero native CPU helper devices.

| Backend | Final film | Draw p95 / max | Timer gap p95 / max | Observed orbit uploads | Observed orbit p95 | Last resize to matching full-size upload |
|---|---:|---:|---:|---:|---:|---:|
| CPU | 3815 x 2292 | 5.318 / 18.201 ms | 30.075 / 315.839 ms | 22/30 | 130.752 ms | 1.873 s |
| Metal | 4218 x 2292 | 3.907 / 36.199 ms | 35.837 / 268.735 ms | 19/30 | 147.065 ms | 3.470 s |

These are authorized shared-run observations, with performance eligibility
explicitly false. Actual viewport widths differ, so this is not a CPU-versus-
Metal speed comparison. Only the observed subset of orbit actions contributes
to generation-upload latency; rapid actions may coalesce or have no observed
upload before the next action. No compositor presentation timestamp is measured.
Both final resize checkpoints use the exact current film dimensions.

UI draw calls remain short, but full-size fresh images still take seconds.
The Metal screenshot retains the existing sparse/blocky low-sample interactive
preview. Neither screenshot establishes heavy-scene convergence or equal-error
CPU/Metal quality. Geometry, textures, bounce settings, sampling and denoiser
defaults were not reduced for this measurement.

Both final 1280 x 720, 32-SPP spectral ACES2 images and saved 32-bit raw EXRs
pass. Reopening the EXRs reproduces the live RGB means exactly; HDR maxima are
about 44.74 (CPU) and 43.34 (Metal). The largest mean-channel change from the
retained prior fixture is about 0.059%. This is a stochastic regression smoke,
not an estimator or convergence-quality bound. GPU texture upload differs from
its source by at most 0.000244141; stale denoise rejection and 30 material-preview
draws with zero replacements pass. ACES2 PNGs, reopened raw-EXR previews, updated
blue-material previews and both actual Junk Shop screenshots were inspected.

Install/wheel/dependency and actual MSL/metallib/archive fingerprints are saved
in `validated-install-fingerprints.json` and `validated-program-fingerprints.json`.
The isolated profile is removed after verification; the unique validated wheel,
original prior wheel, raw EXRs, PNGs, logs and JSON evidence are retained. The
same-thread automatic follow-up is paused. No commit, push or release was made.
