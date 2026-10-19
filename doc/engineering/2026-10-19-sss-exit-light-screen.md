# Rejected SSS tangent-exit light-direction proposals

This investigation targeted the remaining refractive SSS light-connection variance without replacing the physical boundary by a diffuse BSSRDF exit. Both CPU prototypes were removed from production source after their raw-error screens failed. The installed complete wheel was not changed during this investigation.

## Estimators tested

The first prototype mixed bounded von Mises-Fisher light-direction lobes with the existing classic/front/back Dwivedi distribution. Light axes approximated refraction through the entry/opposite tangent planes, but all continuation rays still used global intersection and the original phase, extinction and boundary BSDF. Light selection was power/distance weighted. Its exact joint mixture folded light-direction density into the classic-extinction distance components.

The second prototype weighted each light by a throughput/channel-weighted diffusion attenuation to the approximate exit. It also coupled front/back light directions to their corresponding Dwivedi distance stretch. Its proposal extension lived in a CPU-only private header so it did not enlarge shared path/volume state. Neither prototype used a radiance clamp, denoiser, cached lighting replacement or modified physical coefficients.

Mathematical reference: Wenzel Jakob, [Numerically stable sampling of the von Mises Fisher distribution on S2](https://www.mitsuba-renderer.org/~wenzel/files/vmf.pdf). Both prototypes used normalized finite mixture PDFs and independently tested direction/conditional-flight weights; a valid local estimator is not sufficient evidence of useful full-path variance.

## Evidence and rejection

Evidence root: `test-scenes/validation-2026-10-19/sss-exit-light/`.

`proposal_probe.cpp` / `probe.json` test the second prototype; `first-prototype-header.h`, `first-prototype-pathtracer.cpp`, `probe-v1.json` retain the first. Source and module hashes, process receipts, raw buffers and both rejected source snapshots are retained. Disposable module overlays and temporary probe binaries were removed after consumption.

- Direction density quadrature: .999999; exit-lobe density quadrature: .999998.
- 2,097,152 joint direction/distance samples: all three normalized physical transport moments within five estimated standard errors of 1. This finite test does not prove correctness for every scene/parameter combination.
- With the proposal disabled, 14 CPU fixtures / 112 buffers remain bit-exact. Both prototype libraries' disabled artist-scene controls are bit-exact with the prior installed-matching library when rendered with the same single-thread configuration (`old-library-control.json`, `v2-off-old-control.json`). Cross-thread artist buffers have sparse differences, so exactness claims are limited to the matched thread configuration.
- Other Blender activity prevented an uncontended speed gate. These sequential single-thread runs are functional/quality screens. Reduced rays or shorter recorded time is not an adopted speed improvement.

All artist screens use the same unclamped/unregularized exported scene, 144 fixed samples, seed 17 and the independent native 4096-sample reference from `sss-unregularized/`.

| Variant | Relative SSS RMSE | Mean recorded ray-count buffer |
| --- | ---: | ---: |
| Prior / proposal off | .454491 | 1013.02 |
| First prototype, 50% light share | 1.951606 | 525.44 |
| Second prototype, 25% light share | 1.059156 | 575.58 |
| Second prototype, 50% light share | 11.045000 | 523.56 |

The tangent-exit heuristics do not predict the useful complete refractive paths well enough. Repeated directional reweighting produced rare large full-path contributions. The observed raw errors contradict adoption, despite fewer traced rays. Neither variant was promoted or exposed as an artist setting. No weight cap was added to hide the failure.

The final installed-state audit found the site package had disappeared while the selected immutable wheel path and its complete cached wheel were unchanged. The cause was not established. `restore_selected_dependency.py` restored and byte-verified all 192 files from that selected wheel, preserving adapter source and settings (`dependency-restore.json`). This restores the existing tested native payload, not either experimental candidate.

## Next meaningful implementation boundary

Investigate learning the incident continuation field from actual completed SSS paths, retaining the exact bulk/boundary estimator and positive-support mixture with the existing Dwivedi proposal. Existing `PathGuidingCache` already fits returned-radiance direction fields, supports isotropic full-sphere queries and bounds tree storage. However, enabling the generic guide currently disables the SSS dedicated loop and the Dwivedi proposal. `TILEPATHCPU` also creates a default cache without honoring the warm table/load/save/settings path implemented by `PATHCPU`. These are concrete integration boundaries to resolve and test; generic guiding being available is not evidence of an SSS improvement.

Any pilot learning cost must be included in equal-time comparisons. Training targets or proposal fits may be approximate; radiance weights/transport must remain exact. Preserve current defaults until raw quality, mean preservation, bounded memory, CPU/Metal and installed-Blender gates establish the actual candidate.
