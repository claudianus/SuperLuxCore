# OpenPBR SSS default depth and CPU coefficient evaluation

## Contract and scope

The Blender OpenPBR node must produce non-truncated subsurface transport with
untouched render settings. Authored native and Cycles bounce limits retain
their meaning. CPU coefficient evaluation may reuse an exactly identical
numeric result, without changing the remap, wavelengths, random sequence,
distance/phase sampling, PDF/MIS, roulette, clamping or denoising.

This is a bounded repair and optimization, not a claim of Cycles BSSRDF
equivalence, Octane quality, unrestricted convergence or general CPU speed.
The standalone imported Cycles BSSRDF compatibility work remains separate.

Visual gate: the 720p/64-target-SPP raw spectral area-light scene still has
substantial chromatic grain/fireflies compared with the independent Cycles
control. Automatic dense transport is still costly. The broad request for
Cycles-like responsiveness and Octane-like quality is not achieved by these
bounded fixes. Denoising, clamping and spectral defaults were not changed to
hide the remaining problem.

## Defect

SuperBlendLuxCore's `_convert_path` emitted `path.pathdepth.volume = 0` for
the unset native `depth_volume = -1`. `RenderConfig::ApplyAutoSSSDepth`
correctly respects an explicitly supplied volume limit, so it returned
without assigning the dense walk its own budget. Scattering and internal
boundary events then exhausted the ordinary diffuse/total budgets.

The adapter now leaves an unset negative default absent. An authored native
`-1` still emits the legacy sentinel; zero and positive native/Cycles limits
still export with the existing vertex-count convention. No explicit artist
limit is silently increased. Scenes without an SSS medium retain the core's
ordinary volume default. GPU Metropolis retains its existing memory guard.

The CPU furnace under identical surface limits reproduced mean RGB 0.164
with the old sentinel versus 0.797 with automatic SSS depth (16x16, 128 target
SPP). These progressive captures have slightly different completed sample
populations and are diagnostic, not an exact-output comparison.

## CPU optimization

`HomogeneousVolume::SSSCoeffs` previously solved the 16-iteration CB15 inverse
for every SigmaA, SigmaS and phase evaluation, even during the same walk.
One thread-local entry now retains the last complete numeric input and
result. Texture evaluation still happens before comparison, including the
current spectral evaluation. The key includes albedo, mfp, anisotropy, IOR
and profile. A changed input recomputes the original algorithm immediately.

The entry has constant size (no per-scene/path accumulation), contains no
scene/material pointers and needs no synchronization. This is exact numeric
memoization, not a radiance/irradiance cache or an approximate SSS model.

## Point-light distance sampler precedence

CPU Scene::Intersect selected equiangular sampling whenever eligible point,
spot or sphere lights existed, before checking the SSS parametrization.
That bypassed the chromatic, throughput-weighted SSS flight and used the
average-sigma proposal. The GPU already gives chromatic SSS precedence.

The CPU now keeps SSS on its chromatic path; ordinary non-SSS homogeneous
media retain equiangular sampling. In a unit furnace, adding only a zero-power
point light gave old CPU mean RGB (.296,.407,.323) at 256 target SPP; the
corrected result was (.799,.783,.774), consistent with the no-point control.
The same fixed 64-sample-per-pixel paths are now bit-identical with/without
that zero-power light. The former proposal has valid asymptotic weights but
severe finite-budget chromatic convergence failure, not an exposure issue.

## Verification

- `SuperBlendLuxCore/dev-tools/sss_depth_export_test.py`: eight production
  depth-export contracts, including explicit native and Cycles precedence.
- `dev-tools/sss_cpu_regression.py`: fixed-budget TILEPATHCPU before/after
  arrays and per-pixel SAMPLECOUNT are bit-identical in eight conditions:
  truncated, automatic, chromatic, spectral, textured, anisotropic, vdH and
  repeated CB15. Every pixel completes exactly 64 samples. Small fixtures
  establish exact-output preservation, not representative image quality.
- `SuperBlendLuxCore/dev-tools/sss_blender_test.py`: actual stock Blender
  5.2.2 LTS, native OpenPBR nodes and presets, RNA author-state contracts,
  full configuration observation, float EXR reopen and ACES 2.0 PNG output.
  Skin Light, Skin Dark and Marble include closed thin ellipsoids under
  backlighting. A matching Principled graph supplies an independent Cycles
  appearance control; its transport is not identical to native OpenPBR.

Evidence is in workspace `test-scenes/validation-2026-10-19/openpbr-sss`:
source/binary identities, before-source snapshots, baseline native,
`exact-parity.json`, raw arrays, logs, full wheels and Blender outputs.
See `report.json` there for current artifact/release/deployment gates.

The actual installed adapter received only the two relevant source blocks;
391 of 393 installed Python files were left byte-identical. A complete wheel
was rebuilt from the installed wheel plus the tested native payload, with a
new verified RECORD. Its cache and LOCAL source were updated together. The
previous wheel and settings are retained as a rollback reference. This local
native build includes the workspace's pre-existing core WIP, which remains
separate from these SSS changes and from a published release.

The observed small CPU tests became substantially cheaper, but the host had
other active applications and some functional runs overlapped. Their times
are not an uncontended ABBA result or a product speed ratio. Higher-quality
automatic transport can cost more than the former incorrectly truncated
result; compare optimization at the same physical path budget.
