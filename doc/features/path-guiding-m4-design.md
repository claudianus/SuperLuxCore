# Path Guiding M4 — SOTA unbiased indirect transport: research survey & design

Status: **design doc — 구현 완료** (M4a→M4e/M5/P5까지 랜딩; 현행 상태는
`path-guiding.md`의 P5 섹션 참조). 이하 M4a stabilized 시점 기록 —
variance-aware target + persistent statistics, CPU; GPU transitional via
coarse-table snapshot). **M4b RIS product guiding implemented** — K-candidate
resampling against t(ω)=f|cos|·L̂(ω) with consistent MIS density p̂=t/Ẑ
on both the direct-light and emitter-hit partners; proven unbiased on the
raw linear radiance channel, gated off by default (`LUX_PG_RISK`, 0=off).
See "Measured findings" — the variance on our regression scenes lives in
NEE binary visibility, not bounce-direction choice, so direction-level
guiding (pure or product) cannot reach it; the next leverage is on the
NEE side, not the proposal side.
Scope: replace the current M1–M3 flat-bin `PathGuidingCache` with a
production-grade adaptive guiding field, and extend guiding from
*direction-only* to *all local sampling decisions* — the full zero-variance
program (Herholz 2019) adapted to LuxCore's CPU + GPU-first architecture.

## Why the current field cannot work

The M1 field is a uniform 16³ grid × 128 flat directional bins (GPU: 8³×32).
Measured status (see path-guiding.md): unbiased but **variance-neutral**.
Diagnosed root causes, all confirmed by the literature:

1. **Uniform spatial grid** cannot resolve peaked indirect fields
   (window-lit rooms, caustic-adjacent indirect). Inside a coarse cell the
   field is ~uniform → guided proposal degenerates to cosine sampling.
   → SOTA answer: adaptive spatial subdivision (SD-tree, Müller 2017).
2. **Flat directional bins** are neither peaked enough nor smooth.
   → SOTA answer: parametric lobes (vMF mixtures: Ruppert 2020 PAVMM,
   Dodik 2022 tangent-space Gaussians / SDMM).
3. **L_i-only target** ignores the BSDF. On glossy surfaces the product
   f·|cos|·L_i differs wildly from L_i.
   → SOTA answer: product guiding (Herholz 2016, Diolatzis 2020 LTC,
   Dodik 2022 SDMM, OpenPGL cosine/HG product lobes).
4. **Mean-radiance target** assumes the rest of the walk is perfect.
   → SOTA answer: variance-aware target ∝ √(second moment)
   (Rath 2020 — "trivial modification, large gains on glossy").
5. **RR is unguided** (throughput-only). → adjoint-driven RR
   (Vorba & Křivánek 2016 ADRRS; OpenPGL 0.7 GRR ships this).
6. Volume: direction-only guiding measured neutral — the dominant variance
   is in *distance/scattering* decisions.
   → SOTA answers: guided product distance sampling + guided VSP
   (Herholz 2019 zero-variance suite; Xu et al. 2024 VSPG — learn the
   per-region *volume scattering probability* and feed it to delta
   tracking, which we already have as `tracking=delta`+majorant grid).

## The SOTA reference set

**Must-read, in dependency order:**

| Paper | What it gives us |
|---|---|
| Müller et al. 2017 (PPG, EGSR) | SD-tree: alternating-axis spatial binary tree, refine where sample count high; per-leaf directional quadtree; iterative rounds. Our field's direct ancestor — we implement its *full* version. |
| Ruppert et al. 2020 (PAVMM, SIGGRAPH) | Parallax-aware vMF mixtures, robust EM fitting. OpenPGL's surface field. |
| Dodik et al. 2022 (SDMM, CGF/Eurographics) | 5D spatio-directional Gaussian mixtures + kd-tree accel; **closed-form product sampling with arbitrary BSDFs** via tangent-space Gaussians. The Hyperion gen-2 / OpenPGL lineage. |
| Rath et al. 2020 (variance-aware, SIGGRAPH) | Learn the target that minimizes *variance of the whole estimator*, not E[radiance]. Trivial diff on our Record path. |
| Herholz et al. 2019 (ZV volume guiding, SIGGRAPH) | Guide **all four** volume decisions: scatter/no-scatter, distance ∝ T·L̂ (incremental — no tabulated pdf), direction ∝ phase·L̂ (vMF product), RR/splitting ∝ adjoint. ~10× error reduction. |
| Vorba & Křivánek 2016 (ADRRS, SIGGRAPH) | Guided RR/splitting from cached adjoint estimate; synergizes with (not fights) direction guiding. |
| Diolatzis et al. 2020 (LTC product guiding, EGSR) | Cheaper product route: represent BSDF lobes as LTCs, product with directional field. Good glossy-surface gains. |
| Xu et al. 2024 (VSPG, SIGGRAPH Asia) | Learn per-region volume scattering probability; resampling-based distance sampler on top of delta tracking. Directly applicable — we already have majorant+delta tracking. |
| Reichardt/Green/Li/Manzi 2025 (Hyperion gen-2, SIGGRAPH course) | **Wavefront integration**: guiding without path history — the exact problem our PATHOCL micro-kernel architecture has. 36pp chapter. |
| OpenPGL 0.7.x (Intel, Apache 2.0) | Reference implementation of the above stack (surface PAVMM + cosine product, volume vMF + HG product, GRR, radiance caches). CPU/SIMD only — use as *oracle* and algorithm source, not a dependency. |

**Secondary / watch-list:**

- Rath et al. 2025 (EGSR) *Neural Resampling with Optimized Candidate
  Allocation* — learn 5D L_i on GPU, sample product via RIS; built to
  offload guiding to idle GPU from a CPU renderer. For us (GPU-first) the
  neural field could live entirely on-device. Long-term option.
- Alber, Hanika, Dachsbacher 2025 (I3D) *Real-Time Markov Chain Path
  Guiding* — vMF mixture controlled by Markov chain over multi-resolution
  hash grids; unbiased via continuous MIS; GPU-native, handles volumes.
  Cheapest GPU-native alternative to a full SD-tree.
- Ikkala et al. 2026 (TOG) *RCPG* — radiance cascades as a product-guiding
  field with exact per-direction pdf; world-space, GPU-native, MIS/ReSTIR
  compatible. Interesting longer-term field representation.
- Lin et al. 2022 (GRIS / ReSTIR PT) and Zeng et al. 2025 (ReSTIR PG) —
  unbiased path reuse; extracting guiding distributions from resampled
  paths. Keep as research track, not the main vehicle: guiding gets most
  of the benefit at a fraction of the complexity/risk.
- Many-light side: Conty & Kulla 2018 (adaptive light tree), Yuksel 2019
  (stochastic lightcuts), Hyperion *cache points* (DigiPro 2024: spatial
  structure over *shading points* with learned occlusion — a superset of
  our DLSC idea). Complementary to guiding, separate milestone.

## Proposed architecture (M4)

Keep everything already proven in our codebase: the one-sample MIS
scaffolding, training rounds with frozen read side, lock-free CAS
accumulation, GPU record/drain/swap pipeline, `Record/Sample/Pdf`
interface. Replace the field's interior.

### Field representation — SD-tree v2

- Spatial: binary tree (Müller-style alternating-axis splits) over the
  scene bbox. Split criterion upgradeable: start with record count
  (Müller), then evaluate **variance/flux-weighted refinement** (split
  where E[x²]−E[x]² or total flux is high — our differentiating
  contribution; variance-driven splits target the noise, not the traffic).
- Directional per leaf: small **vMF mixture** (K≤4–8 components,
  EM-fitted at round swap on CPU from accumulated sufficient statistics —
  S0, S1, and per-component moment accumulation during the round).
  Rationale: we already ship a single-lobe vMF fit (volume M3b) and it is
  trivially serializable to GPU (mu, kappa, weight per component — flat
  array, no pointers). Flat bins remain as the cold-cell fallback.
- Both sides (read=frozen field used for sampling/pdf, write=raw
  sufficient statistics) preserved; tree rebuilt at each swap from the
  write side — no incremental tree mutation races.

### Product sampling (M4b)

- Diffuse/HG vertices: closed-form product — append a cosine lobe
  (surface) or HG lobe (volume) into the leaf mixture
  (OpenPGL's approach; vMF×cosine has a cheap accurate approximation,
  per-component renormalization via fitted tables).
- Glossy vertices: **RIS product** — draw K candidates from the field,
  resample ∝ f(ω)·|cos|·L̂(ω)/p_field(ω); final pdf = RIS weight.
  Unbiased by construction (Talbot 2005 / GRIS), no BSDF model fitting,
  works for *all* our materials incl. Disney/velvet/hair. K adaptive
  (Rath 2025's optimized candidate counts idea, non-neural version:
  K ∝ leaf confidence).

### Variance-aware target (M4b)

Record `localValue` as today, plus its square; leaf target ∝ √(E[x²])
(Rath 2020's ESML). Trivial change to `Record()` + statistics.

### Guided RR (M4c)

Leaf stores the adjoint estimate ≈ outgoing-radiance integral (total
field mass); RR survival ∝ throughput·adjoint estimate instead of
throughput alone (ADRRS / OpenPGL GRR). Unbiased; kills paths in dark
regions early — pure speed win where guiding is enabled.

### Volume (M4d)

- Distance: Herholz 2019 incremental guided product distance sampling
  (∝ T(t)·L̂(t,ω)) — needs a *spatially varying in-scattered* field
  along the ray; our leaf vMFs already give L̂(x,ω) queries → evaluate
  at march/DDA steps and use as a control variate on delta tracking.
- VSPG (Xu 2024): per-region learned scattering probability q(x)
  modulating the real-collision probability inside delta tracking;
  resample accepted collisions ∝ q. This is the piece our fog
  measurements say matters most (distance/transmittance dominates).
- Direction: existing isotropic path + vMF already in place; upgrade to
  phase·L̂ product (vMF product with HG lobe — OpenPGL approach).

### GPU path (M4e)

- Frozen snapshot: flattened breadth-first tree array + per-leaf vMF
  array → single `SnapshotTable`-style upload per round (extends the
  existing coarse-table path; no new drain machinery needed).
- Kernel: leaf lookup = bounded descent loop (no recursion); Sample/Pdf
  = few vMF evals — Metal/OpenCL friendly, all branches cheap.
- Wavefront note (Hyperion ch.): per-path guiding *state* must live in
  the task state, not the field — our persistent `EyePathInfo` per task
  already carries what the field needs (previous vertex pos/dir), so the
  record path needs no wavefront restructuring.

### Milestones

| Step | Deliverable | Gate |
|---|---|---|
| M4a | SD-tree + per-leaf vMF mixture, CPU, unbiased MIS unchanged | guided/unguided mean ≈ 1.0 on pg-indirect + Cornell; per-pixel variance A/B must show real reduction (unlike M1) |
| M4b | variance-aware target + cosine-lobe product (diffuse) + RIS product (glossy) | glossy-heavy scene A/B; e26 cosine test stays green |
| M4c | guided RR/splitting | equal-time MSE A/B |
| M4d | volume: guided product distance + VSPG q(x) | fog/god-ray scenes: mean ≈ 1.0, variance down vs delta-only |
| M4e | GPU flatten/upload + kernel Sample/Pdf | CPU/GPU parity, e26/e23 green |
| M4f | Blender exposure (guiding quality preset, field viz AOV) | artist UX pass |

Risk register: vMF EM fitting cost per swap (bounded: only warm leaves,
K small); RIS candidate cost on GPU (cap K, adaptive); field memory on
GPU (leaf cap + coarse fallback like current COARSE_GRID).

## Honest expectation

PPG-class SD-tree + vMF + product guiding is the *validated* SOTA
(Cycles/OpenPGL, Hyperion, Corona all ship variants). Expected gain on
peaked-indirect scenes: ~2–5× effective samples; on uniform-indirect
scenes it must remain neutral (MIS guarantees). The differentiating
research hooks are (a) variance-driven spatial refinement, (b) RIS
product without per-material model fitting, (c) tight delta-tracking
integration for volumes — all three are publishable-shaped deltas on top
of the reference stack, not reinvented wheels.

## Measured findings (M4a, post-implementation)

Synthetic validation of the estimator is clean: on ideal peaked fields
the one-sample MIS mixture cuts variance 1.3–6× vs cosine sampling, and
the guided/unguided mean stays unbiased (e26: ratio 0.9996 CPU,
0.9738 GPU). On the three real regression scenes, however, extensive
A/B measurement (8+ independent runs, e27) shows **pure direction-only
guiding saturates near neutral**:

| Scene | Variance ratio (guided/unguided) | Notes |
|---|---|---|
| pg-indirect (open window) | ~0.93–0.99× | direct-dominated; NEE already covers the aperture |
| pg-indirect-slit (0.24m slit) | ~1.00–1.05× | small gain; slit is the real aperture but leaf smear + NEE binary visibility dominate |
| pg-glossy-indirect | ~0.98–0.99× | rough-glossy floor excluded; wall guiding neutral |

Diagnosed mechanisms (each bisected with env switches):

- **The variance isn't in the bounce-direction decision.** Even with a
  converged, sharp field (κ~15 lobes on the correct axis) and aggressive
  activation (`W0=1`, `NOPEAK`), forced rendering is neutral-to-worse.
  The residual variance lives in NEE's binary visibility through the
  aperture and in multi-bounce accumulation — outside what a directional
  proposal can touch.
- **Field-only guiding on glossy receivers is harmful** (measured
  ~0.93×): the proposal ignores f·|cos|, so it chases the incident
  field through directions the BSDF weights to zero. This is the known
  motivation for product guiding, not a defect of our fit.
- **Variance-aware target must be unconditional**: `sqrt(E[x²])` over
  *all* visits. The earlier conditional per-bin RMS (dividing by bin hit
  count) erased hit-frequency information and collapsed the fit to a
  flat κ~1–2 blob. Fixed; the EM now recovers κ up to the cap correctly.
- **Persistent leaf statistics are required for convergence.** With
  per-round resets, depth-12 leaves never accumulate enough signal
  (median nz≈10 → overfit). Inheriting stats across rounds (children
  seeded from parent, capped directional mass so fresh data dominates)
  raised fitted-leaf coverage from ~14/209 to 708/708 at nz median ~300.
- **Spatial resolution matters more than mixture quality** for aperture
  fields: a ~1m leaf covering a 0.24m slit smears the direction to
  κ~2–3. `TREE_MAX_DEPTH` 8→12 fixed this; leaves near the slit now fit
  κ~15–30 on the right axis.
- **Informativeness gating must sit well above cosine parity**
  (4π·E[p²]−1 = 5/3 ≈ 1.67): firing below parity only adds proposal
  noise. `PeakGate` ramps 3.5→9, so diffuse/multimodal fields
  automatically read as "not worth guiding" and the mixture collapses to
  pure BSDF — this is what keeps open-window scenes regression-safe.
- **Warmup must be on visitation count, not signal records** (`count`,
  not `nz`): on slit-like scenes most surface leaves are visited often
  but record sparse positive flux; gating on `nz` leaves the field cold
  forever, gating on `count` fits a meaningful (if wide) lobe that the
  peak gate then evaluates honestly.

Net: keep the estimator and structures as the correct, conservative
foundation (unbiased, regression-safe, GPU-parity semantics encoded in
snapshot col 32 as `nz·PeakGate(peak)`), and direct the next milestone at
**BSDF-aware product guiding** — the field is only half the proposal;
the missing half is f·|cos|·L̂ product sampling on guidable receivers.

## Measured findings (M4b, RIS product guiding)

Implementation (`pathtracer.cpp`, env `LUX_PG_RISK=K`, default off):

- At a guidable vertex (depth ≥ `LUX_PG_MINDEPTH`, non-delta, `CanGuide`)
  draw K candidates from the usual (1−w)·BSDF + w·guide mixture, resample
  one ∝ w_i = t(ω_i)/p_mix(ω_i) with product target t = f·|cos|·L̂
  (Talbot 2005 / GRIS). Continuation weight = f|cos|·Ẑ_sel/t(ω*).
- Both MIS partners see the same effective bounce density p̂(ω) =
  t(ω)/Ẑ_mis where Ẑ_mis comes from an **independent second candidate
  pool** — conditioning on "ω* won the resample" tilts the selection
  pool's own W low for exactly the directions that reach a light, so a
  path-local Ẑ in the MIS pair inflates p̂ on emitter-hit paths.
- Candidates are drawn BEFORE `DirectLightSampling` because the DL MIS
  weight needs the path's Ẑ_mis; the winner's BSDF event uses the same
  shadow-draw trick as the field-only mixture path.
- Diagnostic envs: `LUX_PG_FLATT` (flatten t to f|cos| — bisects the
  L̂ factor), `LUX_PG_RISMIXPDF` (use winner's mixture pdf as the MIS
  density), `LUX_PG_RISDL` (DL side keeps plain bsdfPdfW),
  `LUX_PG_RISDUMP`/`RISDUMP2`/`RISWT`/`HITDUMP`/`CONTRIB` (per-pixel
  candidate dumps and contribution decomposition).

**Key measurement discovery — read the linear channel, not the pipeline.**
Early RIS runs showed a repeatable "+10–14%" mean shift on pg-indirect-slit
that survived every estimator bisect (flattened target unbiased, MIS
variants unbiased, frozen field unbiased, K=1 degenerate still shifted).
The cause was not in the estimator: e27 read `RGB_IMAGEPIPELINE`, which
applies a nonlinear transform — and a variance-reduced estimator reads
systematically brighter through a concave curve (Jensen effect). On the
raw linear `RGB` channel the same renders are unbiased to <0.1%
(guided/unguided = 1.0004), matching direct contribution counters. Rule
for all future estimator work: **unbiasedness and variance are only ever
compared on raw linear film output**; the image pipeline is for display
PNG export only. (e27 now reads `FilmOutputType.RGB`.)

With the artifact removed, honest linear-domain results on the current
scenes (e27, 6 runs, SOBOL, 64spp, aggressive gates `RISK=4 NOPEAK
MINDEPTH=1 DIFFUSE`):

| Scene | Mean ratio | Variance ratio |
|---|---|---|
| pg-indirect-slit | 1.0101 | 0.75× (worse) |
| pg-glossy-indirect | 1.0018 | 0.81× (worse) |
| pg-indirect-slit, frozen converged field | 1.0172 | 0.62× (worse) |

The estimator is exact — E[Ẑ·L(ω*)/L̂(ω*)] = ∫f|cos|·L — but the
continuation weight Ẑ/L̂(ω*) turns L̂ misfit directly into throughput
noise (measured tail: ~6% of RIS vertices carry wt>4, max ~107). On
scenes whose residual variance is dominated by NEE binary visibility
through a small aperture, a better bounce proposal only adds weight
noise. This closes the direction-proposal line of investigation on the
current scene set: **the remaining variance lever is on the light-
selection/NEE side** (portal-aware light sampling, light-tree, ReSTIR-
style reservoir light selection), not the bounce-direction side.

## M5: portal-guided bounce sampling (adaptive aperture routing)

Follow-up to the M4b finding: on slit/aperture scenes the residual
variance is not in which direction the BSDF picks, but in whether the
path *routes through the opening at all*. Directional fields cannot
resolve features narrower than a vMF lobe, so the fix is an explicit
aperture proposal - the same role portal lights play in production
renderers (Pantaleoni & Heitz-style; Corona/V-Ray/RenderMan portal
workflow), but here folded into the bounce mixture as an analytic
directional technique and allocated adaptively by the learned field.

### Mechanism

- `path.portal.<i>` = 12 floats (4 CCW corners) define a planar rect
  marking a light-carrying aperture; `path.portal.weight` (default .3,
  env `LUX_PG_PORTALW`) caps its one-sample MIS share. Off by default
  (`path.portal.count = 0`).
- With probability wP the continuation direction aims at a uniform
  point on a rect; the marginal density is the analytic solid-angle
  pdf `t^2/(A*|d.n|)` averaged over rects - no numerical inversion.
- The mixture `p = wP*p_portal + (1-wP)*p_rest` is evaluated identically
  on all three estimator sides: the bounce branch (`bsdfPdfW`),
  direct-light MIS (`bouncePdfW` gains the portal term under the same
  gates), and emitter-hit bookkeeping (via `lastBSDFPdfW`).
- Mutually exclusive with RIS product guiding (pHat already replaces
  the rest-mixture) and with the ReSTIR-GI-eligible first vertex;
  delta BSDFs and volume vertices are excluded. `PortalUsableAt`
  (on-plane degeneracy), `PortalSideOK` (`LUX_PG_PORTALSIDE` half-space
  gate) and `PortalFacingOK` (aperture must sit in the shading
  hemisphere) gate the proposal and are mirrored exactly on the DL side.

### Adaptive share (the part that makes it work)

A fixed share applied everywhere measured *worse* than baseline
(0.44x ungated, 0.83x side-gated, 0.95x +facing): portal draws at
interreflection-dominated vertices steal productive bounce samples.
`PortalShareAt(p)` therefore earns the technique the fraction of the
leaf's incident field arriving through the aperture,

    wP(p) = clamp( Sum_i Omega_i * Lhat(p, d_i) / leafTotal, 0, portalShare )

(a slit-dominated leaf gets the full cap; a leaf lit mostly by local
interreflection keeps its bounce budget). It falls back to the fixed
cap while the field is untrained, preserving exploration. The same
value is reused for the DL-side competing density. Env
`LUX_PG_PORTALADAPT=0` disables adaptation for A/B work. Note the
dependency: adaptation needs `path.guiding.enable=1` - the portal
rect supplies the aperture direction, the field supplies the
allocation.

### Linear-domain measurements (e27, RGB channel, 1280x720, 64spp x4)

pg-indirect-slit (0.24x0.3m slit), portal rect on the slit,
`PORTALSIDE=1`:

| Config | Mean ratio | Variance ratio |
|---|---|---|
| portal fixed .5 + facing/side gates, no guiding | 1.0005 | 0.95x (worse) |
| portal fixed .5 + guiding | 1.0007 | 0.96x (worse) |
| portal adapt .5 + guiding | 1.0003 | 1.026x |
| portal adapt .8 + guiding | 1.0003 | 1.038x |
| portal adapt .8 + guiding, frozen converged field | 1.0003 | 1.023x |

Unbiased in every configuration; the adaptive share is what flips the
sign of the result. The residual variance in this scene is deeper-path
routing (the portal only fixes the first transit), so the honest gain
is a few percent - the mechanism is the deliverable, bigger wins are
expected on wider apertures and portal-lit interiors where a larger
fraction of leaves are aperture-dominated.

Diagnostic envs: `LUX_PG_CONTRIB` adds `portalN`/`portalHitN` (draw
count / emission hits reached via the proposal) to the contribution
decomposition.
