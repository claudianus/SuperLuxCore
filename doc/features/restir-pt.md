# ReSTIR PT (PT-1): path-suffix reservoir

**Status:** CPU (PATHCPU) landed; GPU is the PT-2 track. Opt-in.
**Properties:** `path.restir.pt.enable` (default `false`),
`path.restir.pt.candidates` (default 4),
`path.restir.pt.temporal.enable`, `path.restir.pt.spatial.enable`
(default `true`).
**Design:** `doc/engineering/restir-pt-design.md`.
**Regression:** `dev-tools/e95_restir_pt_test.py` (6/6 PASS).
**Mutual exclusion:** PT overrides `path.restir.gi.enable` when both
are set (PT is the strict generalization of GI's payload).

## What it does

ReSTIR GI stores "which x2 the first bounce continues through" plus a
cheap one-sample proxy of that vertex's direct light; the winner's
suffix is always retraced. ReSTIR PT stores instead the *measured*
suffix radiance `L_suf` — all radiance the path delivered from x2
onward. When a stored suffix wins the resampling tournament, the path
does not retrace it:

- the reconnection edge is visibility-tested (one shadow ray),
- the contribution `throughput * f*cos * W * connThr * L_suf` is added
  directly,
- the path terminates — a potentially long suffix costs one shadow
  ray.

Fresh candidates keep the GI semantics: the winning direction is
traced and `L_suf` is recovered at path end as
`Δradiance / throughput_at_x2` (the suffix-local estimate per unit
landing throughput — no per-bounce bookkeeping needed).

## Guards (GI-parity)

- Seqlock publication (`pass` stamp) — readers accept strictly-older
  passes; torn entries rejected by bracket comparison.
- Jacobian-corrected reconnection + binary-V for temporal merge.
- Same-surface gating (x1 distance + 25° normal cone) for the 2-sample
  spatial merge in a 5×5 window.
- M-cap at 2× candidate count; representative-winner gate
  (`target >= 5% * wSum/m`); `RESTIR_PT_MAX_TARGET_RATIO = 64` clamp.
- Reconnected-edge volume transmittance (`connThr`) is folded into the
  consumed contribution; fresh picks let the walk apply it.

## Bias position

Measured-suffix reuse is the bounded-bias *empirical reuse* regime —
reusing a noisy suffix estimate as payoff is not strictly unbiased.
The M-cap, representative gate, and ratio clamp bound the failure
modes; e95 gates mean parity at 5% (vs GI's 3%) plus RMSE and
merge-explosion tripwires.

## Measured (PATHCPU, 1280×720)

| Scene | spp | RMSE off | RMSE PT | ratio |
|---|---|---|---|---|
| pg-indirect | 32 | 0.0435 | 0.0369 | **0.85 (PT wins)** |
| cornell | 48 | 0.0223 | 0.0516 | 2.31 (diffuse-flat — no win) |

pg-indirect mean parity: off −6.6%, PT −20.6% at 32 spp — the
bounded-bias symptom: rare high-radiance suffixes contribute only
after a path first probes them, so consumed picks underfill early.
Converges toward parity as reservoirs populate; tighten spp or
candidate count to close the gap. PT is a variance-reallocation tool:
enable it on indirect-heavy/caustic scenes, not flat diffuse.

## Known limits (PT-1)

- Consumed contributions are booked to light group 0 and the
  indirect-reflect AOV bucket of the winning edge's event class.
- Miss (env) suffixes carry `connThr = 1` — no volume transmittance on
  infinite reconnection edges.
- Diffuse-only depth-0 vertices for reuse eligibility is deliberate:
  delta primary vertices skip resampling (same as GI).

## References

- Lin, Kettunen & Wyman, *ReSTIR PT Enhanced* (PACMCGIT 9(1), 2026)
- Lin et al. 2022 (GRIS)
- Ouyang et al. 2021 (ReSTIR GI)
