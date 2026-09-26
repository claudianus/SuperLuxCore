# ReSTIR GI / PT — E2 continuation design

Status: **design stage** — no code yet. Roadmap item E2 (the
"ReSTIR PT/GI/PG" remainder after E2a–E2d landed). This document fixes
the scope, estimator math, integration points and validation plan so
the implementation lands in reviewable chunks like the DI stages did.

## What / why

ReSTIR DI (shipped) resamples *which light* a direct-lighting estimate
uses. ReSTIR GI resamples *which second-bounce vertex* an indirect
estimate continues through: at a primary vertex x1 the indirect
contribution is `∫ f_r(x1,ω)·G(x1,x2)·L_o(x2→x1) dx2`, and correlated
reuse of good x2 proposals across pixels collapses variance in
indirect-heavy scenes exactly like DI collapsed it in many-light
scenes. This is the single largest remaining quality/perf lever in the
roadmap; the DI infrastructure (per-pixel reservoirs, same-surface
gates, reconnection shifts, tail-queue shadow tracing, ratio clamp)
carries over almost verbatim.

## References

- Ouyang et al. 2021, *ReSTIR GI: Path Resampling in Real-Time* —
  secondary-vertex reservoirs + Jacobian-corrected reconnection shift.
- Lin et al. 2022, *Generalized Resampled Importance Sampling:
  Foundations of ReSTIR* — GRIS framework, `b = wSum·(π̂_new/π̂_old)`
  merge weight, proxy-target correctness (target need not equal the
  integrand; W corrects the residual).
- Bitterli et al. 2020 (ReSTIR DI) — reservoir semantics the DI stage
  already implements (`doc/features/restir-di.md`).

## Scope decision

**G1 (this design): first-bounce resampling at depth-0 vertices.**

The path's continuation vertex x2 becomes an RIS-selected sample:
K candidate x2's are proposed (the path's own BSDF continuation is
candidate 0 — its ray is already traced), reservoirs merge temporal +
gated screen-space spatial reuse, and the winning x2's continuation
is scaled by the RIS weight. Properties:

- The target function π̂(x2) is a *proxy*: `f_r(x1)·G(x1,x2)·L̂(x2)`
  where L̂(x2) is x2's direct-lighting estimate (one light sample +
  one shadow ray at x2). The estimator stays unbiased because the
  payoff uses the real continuation `L_real(x2)`: estimate =
  `W·f_r·G·L_real(x2)`, `W = wSum/(M·π̂)`. Since `f_r·G` appears in
  both π̂ and the payoff, the per-path correction reduces to
  `wSum·L_real/(M·L̂)` — a throughput multiplier on the normal path
  continuation. Cheap-target / exact-payoff asymmetry is the GRIS
  property that makes this viable: proxy quality trades variance, not
  bias.

**Deferred (documented, not implemented in G1):**

- Recursive GI (L̂ including x2's own indirect reservoir) — feedback
  risk needs the pre-spatial-store + clamp machinery extended; do it
  after G1 is measured.
- ReSTIR PT (whole-path-suffix reservoirs, multi-vertex reconnection
  shifts) — path-payload storage and multi-vertex Jacobians are an
  order larger than G1.
- ReSTIR PG (volumes) — needs medium reservoirs and a different shift;
  separate design.
- Deeper-vertex GI (depth ≥ 1 x1) — same machinery, but per-pixel
  reservoir semantics assume primary hits for the pixel gate.

## Reservoir layout

`RestirGIReservoir` (per pixel, ~64B, same slab discipline as
`RestirReservoir`):

- `x2` hit point (3 floats), `x2` geometric normal (3 floats)
- `x1` hit point + geometric normal (same-surface merge gate, E2b rule)
- `L̂(x2)` proxy radiance (Spectrum, 3 floats)
- `wSum`, `M`, `target` (=π̂ at store time)
- incident direction ω (x1→x2, 3 floats) — needed to re-evaluate
  `f_r`/`G` at merge time without re-tracing
- replayable BSDF draws that produced x2 (`u4`-equivalent, for the
  E2c-style exact-sample replay on shifts)

## Estimator (G1, per depth-0 vertex)

1. Candidate 0: the path's BSDF continuation ray (already traced by
   the normal pipeline — zero marginal cost until x2 is shaded).
2. Candidates 1..K-1: extra BSDF samples → bounce rays → x2 hits.
   Each candidate's π̂ needs x2's direct light: one light pick + one
   shadow ray at x2 (rides the candidate tail queue, same pattern as
   E2a/E2d — the tail stride becomes `visCandCount + merges + giK`).
3. `wSum += π̂_i/q_i` per candidate, `M += 1`; merge temporal reservoir
   (same pixel → Jacobian 1 on static geometry), then ≤2 gated pixel
   neighbours with the **Jacobian-corrected reconnection shift**:
   `b_nbr = wSum_nbr·(π̂_new/π̂_old)·J` where
   `J = G(x1_cur, x2) / G(x1_nbr, x2)` — the solid-angle↔area measure
   correction the GI paper requires (the DI shift needs no J because
   the sample lives on a light, not on a scene surface).
   `RESTIR_MERGE_MAX_TARGET_RATIO` clamps the composite ratio.
4. Winner x2: the path physically continues through it (existing
   ray/next-vertex machinery) with `pathThroughput *= W`
   (`W = wSum/(M·π̂)`). Everything past x2 (RR, depth, MIS on later
   bounces) is untouched — the reservoir only decides *which* x2.

Delta BSDF at x1 skips GI (same rule as DI). The estimator never
creates NaN/Inf: empty reservoirs contribute nothing, and a culled
winner is excluded by `π̂ ≤ 0` before acceptance.

## Integration points

GPU (`pathoclbase`):
- New `restirGIReservoirs` buffer, `filmWidth·filmHeight` entries —
  same allocation site as `restirReservoirsBuff`.
- Candidate bounce rays + x2 shadow rays extend the `rays[]`/`rayHits[]`
  tail (E2a pattern); a resolve state analogous to `MK_RT_RESTIR`
  evaluates π̂ (material at x2 + one NEE shadow ray) after the trace.
- Hook: `MK_HIT_OBJECT` at `depth==0` enqueues GI candidates instead
  of blindly continuing; the resolved winner sets the next-vertex ray
  and the `W` throughput multiplier.
- cl2msl: no new machinery — same tail-queue and resolve-state pattern.

CPU (`pathtracer.cpp` / `LightStrategyRestirDI`-adjacent):
- `RenderEyeSample` knows `sampleResult.pixelX/Y` → per-pixel CPU
  reservoirs are addressable without pixel-context plumbing (the DI
  world-grid restriction does not apply here — GI reservoirs are
  inherently screen-space).
- Synchronous candidate shading: extra BSDF samples + `scene.Intersect`
  calls inline (the CPU visibility path already traces inline).

BlendLuxCore: `restir_gi_enable` checkbox beside the existing ReSTIR
visibility toggle once validated.

## Properties

- `lightstrategy.restir.gi.enable` (default 0)
- `lightstrategy.restir.gi.candidates` (K fresh candidates, default 4;
  budget rule mirrors DI — merge slots subtract from K)
- `lightstrategy.restir.gi.temporal.enable` / `.spatial.enable`
  (default 1/1)
- `lightstrategy.restir.gi.depth` — max x1 depth (G1: 0)

## Validation plan (mirrors e14/e16/e18 structure)

- `dev-tools/e19_restir_gi_test.py`: indirect-heavy scene (enclosed
  room, light through an opening — cornell with blocked direct light);
  gates: unbiased mean within 3% vs converged reference, bounded RMSE,
  finite output, temporal/spatial/vis combos, K=1 deterministic edge.
- CPU/GPU parity on the same scene (e9-style centre-value gate).
- Regression: e14/e16/e17/e18 must stay green (the tail stride grows —
  the E2a OOB lesson applies: allocate before use).
- Cost report: rays/sample before/after on cornell + interior.

## Risks

- **Correlation artifacts**: spatial reuse shares paths between
  pixels — splotchy low-frequency noise under too-aggressive reuse.
  Mitigations: same-surface gate, merge cap, ratio clamp, K floor.
- **Proxy quality**: `L̂` = direct-only under-proposes x2's that are
  strong *indirect* reflectors (e.g. bright walls). W corrects bias;
  variance residual is the measured item for the recursive-GI follow-up.
- **Temporal lag** on dynamic scenes: same bounded-bias story as DI.
- **Tail-buffer arithmetic**: GI adds slots to the candidate tail —
  the E2a OOB wedge came from exactly this class of bug; the stride
  change needs the same allocation-site audit plus a 512K-task smoke.
