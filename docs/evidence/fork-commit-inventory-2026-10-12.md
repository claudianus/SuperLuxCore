# SuperLuxCore fork commit inventory / 2026-10-12

This record compares the fork against LuxCoreRender from common ancestor `e9ced7e7a283249d83f1edb49b6f6290f3e416e7` (LuxCoreRender 2.11.2) through fork `bb48fedc098e26f29fec1ed7d54c982d791c07f3` and upstream `4209d6537445bda2e2add8d1131211cb8990dbfa`. It enumerates **517 fork commits** after the baseline, **51 upstream commits** after the same baseline, and keeps the website commits separate from the **509 engine commits**.

## Divergence snapshot

| scope | commits | endpoint |
|---|---:|---|
| SuperLuxCore after baseline | 517 | `bb48fedc098e26f29fec1ed7d54c982d791c07f3` |
| engine inventory through audit head | 509 | `adba86f43eb8c9dc9b95c6efc8293c62d877195d` |
| website/docs after engine audit | 8 | current fork |
| LuxCoreRender after baseline | 51 | `4209d6537445bda2e2add8d1131211cb8990dbfa` |

The fork-only count against the current upstream head is **517** and the common ancestor is still `e9ced7e7a`. Commit count is change history, not feature count; one commit may touch multiple areas, and the inventory includes fixes, reversions, experiments, tooling and documentation.

## Area counts in the engine audit

| area | commits (multi-label) |
|---|---:|
| `light_transport` | 148 |
| `maintenance` | 97 |
| `volumes_textures` | 86 |
| `engine_infrastructure` | 80 |
| `hardware` | 74 |
| `film_workflow` | 60 |
| `build_release` | 58 |
| `materials` | 58 |

## Visual promotion candidates

| candidate | evidence commits | what can be shown | claim boundary |
|---|---|---|---|
| **cd-diffraction** | `0fcc35b5c` | CD diffraction grating and holographic foil material controls — *macro CD spectrum / foil rainbow split, matched silver baseline* | feature is opt-in; do not imply upstream lacks all diffraction or claim speed |
| **spectral-film** | `7d8896fc9`, `e4a522380`, `2ba93f900` | hero-wavelength transport, RGB-to-spectrum upsampling, unbiased spectral film projection — *prism/glass dispersion with RGB vs spectral crop* | opt-in and scene-dependent; upstream has glass dispersion |
| **openpbr-energy** | `17b020b31`, `7028de5dc`, `2357ace5e`, `6db0ab072`, `8733f61be` | OpenPBR surface controls and rough-interface/multiscatter handling — *black chrome roughness/energy strip* | scope varies by lobe/engine; no blanket physical superiority |
| **hair-curves** | `a8785faa2`, `4cfae3329`, `4a90e7057`, `5e473bf8c` | Chiang/Huang strand scattering, native Metal curves and curve deformation motion — *textile/groom close-up with strand motion crop* | Blender hair deformation export remains scoped |
| **sss-bssrdf** | `62366b35b`, `b85510b04`, `b62c2d681`, `2db47e4e3`, `447159b8c`, `c2915a73a` | random-walk and experimental Cycles BSSRDF CPU/device/adjoint paths — *skin/wax slab with radius/profile inset* | experimental and not full Cycles equivalence |
| **mnee-caustics** | `8363ad339`, `038414538`, `940664923`, `60dc8ec1f` | MNEE/MGE focused specular chains with seed retention and dispersion endpoints — *glass-to-metal caustic hero plus path diagram* | scene-dependent; no universal convergence/speed claim |
| **gpu-light-tracing** | `45caf3459`, `de65c58aa`, `2f86563f3`, `bf3c3531a`, `f60d2b838` | GPU light tracing, SSP tails and volumetric photon-beam traversal — *jewellery/fog caustic progression* | engine/path coverage differs |
| **restir-guiding** | `db4600a18`, `b349776cb`, `10ed0383e`, `69104a64d`, `312e5d6ff`, `8687ffc37`, `065f895d7`, `ef17fe5b9` | ReSTIR DI/GI/PT and path guiding choices on CPU/GPU paths — *many-light noise crop and directional field* | bounded-bias/opt-in and neutral measurements; no universal speed |
| **volume-transport** | `ba793a22f`, `632e8928e`, `0dd8d6ee7`, `f60d2b838` | null-collision, majorant, residual/equiangular/HG volume transport and volume caustics — *smoke/fog phase-angle and beam inset* | medium/path limits vary |
| **vdb-cycles** | `f683200cb`, `110db3bf0` | named VDB fields with Cycles linear/cubic reconstruction and VolumeInfo example — *named-field volume with interpolation slider* | load-order and installed-package scope must be labelled |
| **cycles-textures** | `13afd4f29`, `8658f747e`, `977bac67a`, `5c5c190a9`, `30f984e71`, `1522b49e3`, `d56203208`, `64edbf2d9`, `0503acc89`, `63e3c6f3f`, `da63a777a` | expanded Cycles texture/HitPoint coordinate and utility node coverage — *single node graph gallery with output swatches* | per-node support, not complete Cycles parity |
| **normal-displacement** | `d72ff9178`, `e73ef9307`, `00e96a78a`, `839f1c6fe`, `114890a55`, `f4fe6afab`, `4256c562a`, `48c4659d0` | Bump/Wave/White Noise/Mapping/image/Normal Map/vector-displacement semantics and tangent preservation — *surface close-up with normal/UV/displacement technical passes* | adapter support remains scoped |
| **production-passes** | `30dc89ab3`, `eaf91d5e4`, `7132270fa`, `1fbf83977`, `aabfdb3e5`, `7a1622a86`, `a7c3ae282`, `ba65bbc9c` | GPU film/OIDN, residual feedback, Cryptomatte, LPE, variance/motion/bevel/viewdepth passes — *beauty → denoise → noise → Cryptomatte/LPE carousel* | LPE excludes bidir/light-tracing and no denoiser superiority claim |
| **interactive-editing** | `f9dece2d9`, `53d340f0c`, `58daaa1b7`, `131c84733`, `48c8c6217`, `6d3aca68f`, `940b8916f`, `dd6d1c029` | viewport coverage/temporal controls and scene-edit film/material refresh — *edit-render timeline with stale-film vs reset* | workflow behavior, not every Blender edit or beauty default |
| **metal-native** | `562a3bf5f`, `ca83e8ceb`, `9ffadfb12`, `4a90e7057`, `25472b8a5` | native Apple Metal compute/HWRT, CL-to-MSL translation, curves and motion blur — *Apple GPU pipeline diagram + native curve close-up* | Apple hardware support matrix; no blanket speed ratio |
| **resource-build** | `af913f2e2`, `c92c2f340`, `2871e485b`, `d8a591011`, `f0c1600e0`, `841af8310` | out-of-core scene spill, cluster residency, compiler/build pipeline improvements — *large-scene memory flow and cold-start timeline* | measurement is environment-specific; build time != render time |
| **distribution** | `c3bf662b1`, `62d76e587`, `3011b9127`, `bc504f6c1`, `d9f0e6a8e`, `af7b918f7` | pysuperluxcore naming, Blender 5.2/Python 3.13 wheels and Windows/CUDA portability — *platform/download matrix* | packaging/CI evidence rather than render quality |

## Upstream-only work after the same baseline

The current upstream branch is active. These changes must not be presented as fork-only additions. The 51 commits consist primarily of the GroupByEquivalence refactor, parallel Simplify experiment and revert, transmit AOV variance clamp, parallel kernel compilation/cache work, OIDN firefly prefilter, and SPIR-V pre-bake/runtime work. See the JSON for every SHA, subject and changed path.

## Evidence and limits

- Full machine-readable ledger: [`fork-commit-inventory-2026-10-12.json`](fork-commit-inventory-2026-10-12.json).
- Existing 509-entry audit: [`commit-audit.json`](commit-audit.json).
- Scoped current comparison: [`fork-analysis.md`](fork-analysis.md).
- Visual cards should state the artist-visible outcome and backend/engine scope. Use “supports”, “adds a control”, or “keeps the path available” unless matched quality/time evidence supports a stronger claim.
- Do not turn area counts into feature counts, and do not use artwork alone as proof of universal compatibility, SOTA, or speed.

