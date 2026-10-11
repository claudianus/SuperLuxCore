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

## Studio comparison design

The retired website images and their presentation recipes were removed. The
new portfolio uses product, interior, textile, architectural and optical scenes
with new art direction and 32 credited CC0 Poly Haven model, PBR texture and HDRI families.
[The reproduction notes](studio-readme.md) and [image manifest](provenance.json)
disclose native settings, sample counts, source credits and fingerprints.

The three official upstream 2.11.2 comparisons show the added diffraction
shader against a silver-metal alternative, Disney multiple-scattering energy
compensation, and Huang fiber scattering against a labelled Glossy2 surface
alternative. Geometry, lighting, sampling, exposure and ACES 2.0 match within
pairs. Detail views use the same crop on both sides. They are material examples,
not speed benchmarks or a claim that the baseline artwork is the best possible
upstream artistic setup.

Separate optical artwork uses spectral CPU hybrid light transport to show
Sellmeier dispersion and caustics. Beauty images use the real OCIO ACES 2.0
view and native OIDN; albedo, normals and Cryptomatte are actual technical data
views. There are no AI generated beauty images, baked light or per-image
normalization. The visual toolkit distinguishes artwork context from actual
render passes and does not infer GPU performance or blanket compatibility from
these illustrations.
