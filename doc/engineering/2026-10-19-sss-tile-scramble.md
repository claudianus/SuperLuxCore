# Local Cycles sampler port: bounded SSS screen

The local Blender5.2 checkout at `d13f752e3b9c4f8c261cda552b1021f8bcc0382c`
was read directly, including `kernel/sample/sobol_burley.h`,
`kernel/sample/util.h`, `kernel/tables.h` and `util/hash.h`.
CPU TILEPATHSAMPLER uses the legacy high-dimensional Sobol sequence with
two shared Cranley-Patterson shifts. Cycles uses padded, shuffled Owen
sequences. This difference was tested rather than assumed to explain the
large SSS gap.

The private CPU screen compared the existing sequence, per-dimension
blue-noise randomization, existing full-dimensional Owen, and a port of
the actual Cycles2D primitive with semantic pairs for camera/light/phase
draws. Physical coefficients, PDFs, refraction, roulette and sample
counts were retained. The adapted semantic layout is not a claim of
bitwise equivalence to the entire Cycles renderer's random dimension map.

The port matched the actual extracted local Cycles functions in 2109440
primitive/semantic checks; 12288 exact dyadic strata checks passed.
The numerical binaries were removed automatically.

320x180 RGB144 CPU4, seeds17/53/97, raw unregularized-v2 controls:

| Sequence | Mean SSS relative RMSE | Three-seed variance | Mean process CPU s |
| --- | ---: | ---: | ---: |
| Existing | .455994 | 2.445995 | 22.3692 |
| Per-dimension blue-noise | .455994 | 2.423963 | 21.6948 |
| Full-dimensional Owen | .441964 | 2.304697 | 22.6559 |
| Cycles padded2D primitive | .452078 | 2.402025 | 22.3991 |

The reference is the independent finite4096SPP native image and has noise.
These are shared-workstation screens, not certified timing ratios or an
equal-time Cycles comparison. Full-dimensional Owen is a modest lead for
future multi-scene evaluation; the directly ported padded primitive did
not resolve the gap. No default or installed artifact was changed from
this single scene. The production sampler was restored byte exactly,
the private header removed, and consumed native overlays deleted.

Evidence: `../test-scenes/validation-2026-10-19/sss-tile-scramble/`;
`comparison.json`, `padded-comparison.json`, `padded-unit.json`,
`prototype/` and `disposition.json`. No learned radiance replacement,
clamp, denoiser, similarity transform or diffuse exit was adopted.
