# CPU SSS transport and convergence, 2026-10-19

The OpenPBR subsurface path now uses a joint direction/free-flight proposal
for optically thick, isotropic homogeneous SSS. Physical absorption,
scattering, phase functions, material interfaces and the existing path budgets
remain unchanged. There is no radiance cache, denoiser, contribution clamp or
replacement of the refractive bulk model with a diffusion surface closure.

## Transport contract

`path.sss.guiding.enable` defaults to true on the CPU implementation. A
10-percent classic phase proposal and two opposite Dwivedi proposals form a
nine-component mixture across the three transport lanes. At each vertex the
phase density is marginalized, then the next flight is sampled from its
conditional mixture. Collision and escape weights both use that actual
density. NEE and emitter-hit MIS use the same marginal angular proposal.
The eigenvalue fit affects sampling probabilities only.

The last entry normal supplies the proposal axis. An already-required global
intersection estimates the opposite boundary; no geometry is omitted. The
entry-directed guide is used only when every lane satisfies
`thickness * sigma_t / v >= 6`. Thin or unknown domains, nonzero phase
anisotropy, emitting volumes and camera-inside paths without an entry axis
retain the classic walk. Other path-guiding, ReSTIR or portal proposals also
retain their existing integration. Spatial texture inputs are evaluated at
each vertex; a purely absorbing texture region cannot evaluate an unset
sampled direction. Metal retains its classic SSS proposal.

The direction/free-flight construction follows the importance-sampling
principle in [Wrenninge, Villemin and Hery, 2017](https://graphics.pixar.com/library/PathTracedSubsurface/paper.pdf).
The numerical eigenvalue fit is also used by Cycles. This implementation
does not use Cycles' deep-walk similarity approximation or its white diffuse
exit closure.

## Additional repairs

- CPU and Metal NEE shadow rays update their volume stack when their outgoing
  direction crosses the geometric boundary. A transmitted connection used to
  keep absorbing through empty exterior space. The absorbing-slab test
  doubles exterior light distance and compensates inverse-square intensity:
  the old far/near result was 0.367876, the repaired result is approximately
  0.999989. The remaining slab attenuation agrees with `exp(-0.1)`.
- SSS roulette uses the accumulated packet's largest contribution and divides
  survivors by that exact probability. It no longer repeatedly kills a
  dominant chromatic lane according to local mean albedo. General fog retains
  its former policy. CPU and Metal implement the same SSS roulette rule.
- Absorption/scattering coefficients are evaluated together when both are
  needed. Exact numerical coefficient memoization remains bounded per thread.
- Sobol byte tables initialize only dimensions actually queried. All 249,600
  tested values match the eager implementation in plain, blue-noise and Owen
  modes. Repeated `RequestSamples` releases previous allocations; ASan passes.
  Preparation-only peak RSS in the standalone test was 91.1 MB versus 5.9 MB.
  This is not a claim about whole-render peak RAM or steady-state speed.
- CPU and tiled GPU sampling now respect `renderengine.seed`. The default 131
  sequence is preserved. The earlier tiled sampler ignored the requested
  seed, so the legacy native 4096-SPP reference requested 181 but actually used
  sequence 131. Repaired quality runs use independent seeds 17, 53 and 97.

## Validation and limits

The shared workstation is an Apple M5 Pro, 18 CPU cores and 48 GiB RAM.
CPU comparisons use four threads, stock Blender 5.2.2 LTS (`d13f752e3b9c`),
320 by 180 pixels, BOX-equivalent filtering, fixed complete sample budgets,
raw unclamped linear EXR and disabled denoising/adaptive sampling. The fixture
contains Skin Light, Skin Dark and Marble bodies and thin closed parts under
two area lights. Authored geometry, optical inputs and lighting match across
engines. Cycles' nonlocal BSSRDF and native OpenPBR bulk transport differ, so
each renderer is compared to its own independent 4096-SPP reference.

The first unrestricted guide worsened thin-part error and was rejected.
Thickness eligibility restored the original thin transport. At similar wall
time, three independent RGB seeds improved both bulk and thin-part RMSE.
Spectral comparisons also retain the spectral model and measure interseed
variance separately. The early 256-SPP local-RR control had SSS RMSE 2.962;
accumulated roulette reduced it to 1.371 in that seed. Do not extrapolate this
single-scene result to production performance, Octane equivalence or all
hardware. Cycles still has a substantial convergence advantage in this
fixture. Raw images and finite-reference uncertainty remain visible.

The final native payload (`3b8b6b63...`) was repeated sequentially in ABBA
order, with no owned build or validation render overlapping it:

| Mode | Classic budget / mean render wall | Guided budget / mean render wall |
| --- | --- | --- |
| RGB | 64 SPP / 9.325 s | 100 SPP / 9.926 s |
| Spectral | 81 SPP / 13.625 s | 100 SPP / 12.377 s |

These are Blender render-operator wall times including scene/session
preparation and EXR writing, excluding Blender process startup. The RGB base
legs were 8.716 and 9.935 s: this is a similar-time study, not an exact equal-time
or general speedup claim. Guided RGB uses 56.25 percent more samples with
6.4 percent more mean wall time; guided spectral uses 23.5 percent more
samples with 9.2 percent less mean wall time in this fixture.

Final guided RGB repeats at seeds 17/53/97 reduce SSS-region RMSE by
20.2/15.6/24.0 percent relative to the earlier classic 64-SPP controls.
Bulk improves 16.7--25.8 percent and thin parts 10.1--27.4 percent; those
controls use an earlier source fingerprint, disclosed in `convergence.json`.
The same-final-binary seed-17 control independently confirms a 20.2 percent
SSS RMSE reduction. Three-seed spectral screening reduces SSS interseed
variance from 7.331 to 5.465, but its timing overlapped functional work and
is not used for the performance comparison.

At seed 17, final guided RGB has SSS relative RMSE 62.7 percent in 9.937 s;
Cycles RGB at 441 SPP has 3.21 percent in 9.907 s. Each uses its own finite
reference. Cycles at 64 SPP takes 1.446 s with 9.47 percent relative RMSE.
These results expose a large remaining convergence gap; neither the guide
nor the shipped spectral output is claimed to match Cycles/Octane quality.
`near-time-rgb.png` shows the unfiltered, common ACES 2.0 display images.
The product spectral default is preserved; RGB is a controlled comparison.

Native gates cover default/explicit depth, chromatic and spectral transport,
textured and absorbing regions, anisotropy fallback, both albedo profiles,
zero-power lights, an interior area emitter, analytic shadow attenuation and
cancel/recovery. Tiled CPU and Metal cases finish every pixel's requested
sample count. CPU zero-light equality is exact; GPU kernel specialization
rounding is checked with an explicit tolerance (observed maximum 1.19e-7).
The final wheel is tested with its own dylibs in stock Blender, including
raw-film/EXR equality. Cancellation stopped the owned CPU session in 25 ms
and two fresh recovery films were bit-identical.

Evidence, source snapshots, manifests and `convergence.json` are under
`test-scenes/validation-2026-10-19/sss-transport` in the workspace. Packaging
keeps a complete wheel, native payload, cache and installed RECORD coherent.
Factory-startup validation now uses an explicitly isolated config/extensions
root: extension reconciliation can remove inactive dependency packages.
All 393 installed adapter Python sources are preserved. The native artifact
is conditioned on the pre-existing local core WIP; this is a local developer
delivery, not a clean published release or a claim of broad GPU certification.

## Reproduction

```sh
python3.13 dev-tools/sss_cpu_regression.py --module out/build/src/pysuperluxcore/Release \
  --out /tmp/sss-cpu --tiled --size 16 --spp 256 --threads 4 \
  --cases auto,chromatic,spectral,textured,anisotropic,vdh,point,interior,blacktextured
python3.13 dev-tools/sss_shadow_medium_test.py --module out/build/src/pysuperluxcore/Release \
  --out /tmp/sss-shadow
python3.13 dev-tools/sss_cancel_recover.py --module out/build/src/pysuperluxcore/Release \
  --out /tmp/sss-recovery
```

Use `--sss-guiding off` for a CPU classic-walk control. `--engine TILEPATHOCL`
with `--tiled` selects a pure Metal exact-budget smoke when available.
The adapter's `sss_matched_benchmark.py`, `sss_compare_run.py` and
`sss_compare_analyze.py` retain the Blender study and source/binary identities.
