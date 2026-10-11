# What SuperLuxCore adds after the LuxCoreRender fork

Snapshot: **11 October 2026**, engine source `adba86f43`, native **2.11.27**.
The comparison starts at **LuxCoreRender 2.11.2 / `e9ced7e7a`**.
[All 509 post-fork commits](commit-audit.json) are inventoried with their dates,
subjects, changed paths and overlapping change areas. Every commit was included;
current source and feature documents were checked for the claims marketed here.
This is not a line-by-line review of every historical diff. A change inventory
includes fixes, reversions, experiments and tools, not 509 new shipped features.

Upstream is still developing. Its observed head on this date was
[`de8707e7232`](https://github.com/LuxCoreRender/LuxCore/commit/de8707e7232cd6c03692a92755e776e6df6ff9d1).
These comparisons describe the fork point, not all later upstream releases.

| Artist benefit | Added or changed implementation | Source and present scope |
|---|---|---|
| Use Apple GPU hardware directly | Native Metal compute, hardware ray tracing, instancing, motion and native curves | [Metal](../../doc/features/metal-backend.md), [HWRT landing](https://github.com/claudianus/SuperLuxCore/commit/562a3bf5f). Supported Apple hardware; no universal CPU/GPU speed ratio claimed. |
| Rough metal retains more energy | Disney/roughglass/carpaint multiple-scatter compensation; conductor GGX random-walk option | [Compensation](../../doc/features/multiscatter.md). Opt-in per material. A fresh original-binary Disney pair illustrates the change. |
| Reflection and refraction within one Principled material | Integrated Disney dielectric transmission, alongside OpenPBR surface controls | [Transmission landing](https://github.com/claudianus/SuperLuxCore/commit/a8785faa2). The upstream Disney baseline is opaque, but upstream already has separate glass materials. |
| Express wavelength-dependent light | CPU/BIDIRCPU/GPU hero-wavelength transport, spectral upsampling, Sellmeier glass and diffraction | [Spectral](../../doc/features/spectral.md). RGB vs hero wavelength is an internal feature study; upstream already has glass dispersion. |
| More paths for concentrated light | MNEE/LMNEE/MGE, GPU light tracing, SSP, adaptive partitions, progressive PhotonGI work and volume photon beams | [Caustic coverage](../../doc/features/caustics-sota.md), [GPU beam index](https://github.com/claudianus/SuperLuxCore/commit/bf3c3531a), [GPU SSP](https://github.com/claudianus/SuperLuxCore/commit/2f86563f3). Scene/engine coverage differs. Upstream already has bidirectional and PhotonGI caustics. |
| Richer soft surfaces and fibers | OpenPBR random-walk SSS, measured SSS controls, Charlie sheen, Chiang/Huang hair and native curves | [Hair](../../doc/features/hair.md), [curve deformation motion](https://github.com/claudianus/SuperLuxCore/commit/5e473bf8c). Core support does not establish full Blender hair deformation or Cycles SSS equivalence. |
| More choices for hard sampling problems | ReSTIR DI/GI/PT/PG, Light BVH, guiding, PMJ02, blue-noise Sobol and noise-based stopping | [GPU PT](https://github.com/claudianus/SuperLuxCore/commit/312e5d6ff), [PG](../../doc/engineering/restir-pg-design.md). PT is opt-in with bounded-bias reuse; PG has neutral measured cases. No blanket speed claim. |
| More consistent work while editing | Film reset after scene edits, nested-material refresh, persistent/delta export, viewport coverage and temporal controls | [Edit reset](https://github.com/claudianus/SuperLuxCore/commit/131c84733), [Blender adapter](../../doc/features/blender-adapter.md). Supported Cycles nodes are reused with the renderer's own light transport. |
| More control in finishing | Object/material Cryptomatte, light linking, path-engine LPE, variance/motion channels and Metal-capable OIDN | [Passes](../../doc/features/aov-audit-temporal-denoise.md), [OIDN pooling](https://github.com/claudianus/SuperLuxCore/commit/eaf91d5e4). LPE excludes bidirectional/light-tracing engines. |
| More practical distribution and resource handling | Four-platform native and Blender bundles, modern CUDA compilation, bounded kernel compilation and memory/resource fixes | [CUDA SASS](https://github.com/claudianus/SuperLuxCore/commit/841af8310), [bounded compile](https://github.com/claudianus/SuperLuxCore/commit/485cba4d9). Local image review is macOS ARM64; other platforms have CI build evidence. |

Important present-day distinctions: ReSTIR PT and SSP have CPU **and** GPU
paths. Regularization can automatically use a conservative decaying schedule
in eligible caustic scenes, with explicit control. Wavefront automatic promotion
was withdrawn; dense scheduling remains the automatic default. Vulkan remains
experimental. The website reflects current behavior rather than treating every
historical implementation as a default feature.

## Fresh comparison design

All old website and diagnostic pictures were removed. Eleven unique images were
newly authored and actually rendered with [the procedural scene generator](artist-scenes.py).
The collection uses sculptural studio lighting, physical geometry/materials,
1600 × 900 output, renderer OIDN and the real OCIO **ACES 2.0** display view.
No generative image, baked lighting or per-image normalization is used.

- **Rough-metal energy:** original official upstream 2.11.2 vs current 2.11.27,
  identical Disney colors/roughness and studio setup, added multibounce on the
  fork side. This demonstrates an added material model, not a speedup.
- **Principled transmission:** identical scene; the fork's integrated
  transmission changes how the lit backdrop appears through the braid.
- **Diffraction:** current fork RGB approximation vs hero-wavelength transport.
  Both are legitimate modes; this is not an upstream comparison or a claim
  that every spectral frame is visibly superior.

Exposure is equal within every pair. OIDN is used for presentation on both
engines; versions/devices can differ. Pair records disclose observed sample
counts, configuration and native-library hashes. An original upstream M5
OpenCL-discovery workaround disables only optional device discovery in memory;
CPU renderer bytes are unchanged. These images are feature illustrations,
not final unbiased convergence evidence or performance measurements.
