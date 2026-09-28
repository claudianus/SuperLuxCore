# ReSTIR PT — design notes (PT-0)

Status: **design locked, implementation staged**. See the ReSTIR GI
counterpart in `src/slg/engines/restirgi.cpp` and
`dev-tools/restir-gi-design.md` — this document only covers what PT
changes relative to that machine.

References: Lin & Kettunen & Wyman, *ReSTIR PT Enhanced* (PACMCGIT
9(1), 2026); Lin et al., *Generalized Resampled Importance Sampling
(GRIS)* (SIGGRAPH 2022); Ouyang et al., *ReSTIR GI* (2021).

## Idea

ReSTIR GI resamples *which* x2 the first bounce continues through and
evaluates a proxy target `pi_hat = f_r(x1) * G(x1,x2) * L_hat(x2)`
where L_hat is a one-sample direct-light estimate at x2. The winner is
then **retraced normally** — GI changes the proposal distribution, not
the payoff.

ReSTIR PT lifts the payload: the reservoir stores x2 plus the **fully
measured suffix radiance** `L_suf(x2)` = all radiance the winning path
delivered from x2 onward (emission + every later bounce, including
caustic and volume segments the proxy could never see). A pixel whose
RIS picks a *stored* suffix then contributes `f_reconnect * G * Vis *
L_suf * W` directly — **one shadow ray instead of a full suffix
retrace**. Fresh winners still retrace (and re-record) as usual.

So the reuse spectrum is:

| winner source | cost | payoff quality |
|---|---|---|
| fresh candidate | full path retrace | exact (this pass) |
| stored reservoir (temporal/spatial) | 1 shadow ray | exact measurement, older pass |

The equal-time win is the second row: at moderate convergence most
pixels replay a neighbor's measured tail for ~1% of its original cost.

## Estimator

Identical GRIS structure to GI (same `wSum`, `M`, `eps` floor,
2*K M-cap, seqlock publish, representative-winner gate,
`RESTIR_MERGE_MAX_TARGET_RATIO` clamp):

- Fresh candidates: K BSDF-sampled x2's. Target `pi_hat =
  fcos(dir).Y() * (proxy(x2).Y() + eps)` where proxy = GI's
  `GI_ProxyHitRadiance` (emission + 1 NEE shadow ray) — **or** the
  path-guiding field's `IncidentEstimate(x2, -dir)` when guiding is
  enabled (it sees indirect, which pure NEE cannot). Guiding-field
  proxy is preferred: PT reuses whole suffixes, so a proxy blind to
  indirect systematically under-weights exactly the suffixes PT exists
  to capture.
- Stored candidate: reconnect stored `x2` to current `x1`; same
  Jacobian `J = (cos_cur/d_cur^2)/(cos_src/d_src^2)` and binary-V on
  the segment as GI. `pi_new = f_eval(x1->x2).Y() * (L_suf.Y() + eps)`
  — the *measured* radiance, not the proxy. That asymmetry (fresh
  candidates rated by proxy, stored by measurement) is deliberate:
  the measurement is the better target estimate.
- Payoff on stored-winner hits: `throughput_scale = f_reconnect *
  cos * Vis * W`, contribution `+= throughput_scale * L_suf`.
  `outPdfW` for MIS bookkeeping = `pi_new*M/wSum` as in GI.

Bias position (honest): reusing a *noisy* measured L_suf as payoff is
the documented "empirical reuse" regime of ReSTIR PT — correlation
trades for variance, bounded by the M-cap, the 0.05 representativeness
gate, and the target-ratio clamp (all inherited from GI/DI). It is
**not** an unbiased estimator in the strict sense; e19-style
convergence-reference tolerance (3%) is the acceptance gate, same as
GI.

## Payload

```cpp
struct RestirPTRecord {          // ~64B, POD, GPU-flat mirrorable
    float x1[3];                 // prefix vertex (shift source)
    float x1n[3];                // its geometric normal
    float x2[3];                 // reconnection vertex
    float x2n[3];                // x2 geometric normal (Jacobian)
    float dir[3];                // x1 -> x2 (env-miss direction on miss)
    float lsuf[3];               // measured suffix radiance at x2
    float wSum, target;
    u_int m, isMiss, pass;       // pass = seqlock stamp (GI parity)
};
```

One reservoir per film pixel (screen-space — GI precedent; the DI
world-grid was rejected there for the same reason: shift bookkeeping
needs the pixel context).

`L_suf` extraction inside `RenderEyePath`: after the x1->x2 bounce
lands, record `thrX2 = throughput` (which contains the x1->x2 edge
factor `E = f*cos/pdfW`). Every later film contribution `c_d` added
as `throughput_d * local_d` contributes `c_d / E` to the suffix — so
maintain `suffixAcc += local_d` for contributions at depth >= 2 and
store `L_suf = suffixAcc` (the edge factor stays out of the stored
value; it is re-evaluated per merge at the *new* prefix). Channel
attribution (direct/indirect AOVs, LPE) is taken from the winning
candidate's recorded event class, kept conservative: reused suffixes
always book under `INDIRECT_*` buckets.

## CPU integration (PATHCPU first)

Hook = same site as `ResampleFirstBounce` (pathtracer.cpp ~1443), at
depth-0 non-delta vertices, mutually exclusive with GI and the portal
proposal (the existing `!(restirGIEnable && firstPathVertex)` gate
already encodes the priority order — PT slots in the same way:
`restirPTEnable` wins over GI when both are set, log once).

- `RestirPT::ResampleSuffix(...)` mirrors `ResampleFirstBounce` with
  the payload/target changes above. On a stored-suffix win it returns
  `consumed = true` and the caller **skips the bounce loop** after
  splatting the reused contribution — the suffix is complete already.
- Recording: at the end of a normal path walk, if the path had a
  depth-0 non-delta x1 and a landed x2, `RecordSuffix(px, py, rec)`.
  Stored pass-stamped, same seqlock.
- Mutual exclusion with GI for v1 (PT is a strict generalization;
  running both doubles the depth-0 proposal cost for no additional
  coverage).

## GPU port plan (PT-2)

Existing patterns to reuse (ReSTIR agent report):
- New `MK_PT_*` states beside `MK_RT_GI_BOUNCE`/`MK_RT_GI_RESOLVE`;
  candidate bounce + reconnect-visibility rays ride the `rays[]` tail
  like `giCandRayBase`.
- Reservoir storage extends the `restirReservoirs` slot arena — do
  NOT add a new kernel arg: the Apple cl2msl path has a real arg-count
  limit (`pathoclbaseoclthreadkernels.cpp:883` note).
- Candidate RNG via XOR-hashed seeds (`giSeed` precedent) — never
  consume the stratified Sobol stream.
- TILEPATHOCL gets the same tail layout as PATHOCL.

## Staging

| stage | content | gate |
|---|---|---|
| PT-0 | this doc + `RestirPTRecord` + reservoir arena | review |
| PT-1 | PATHCPU reconnection-shift reuse, `path.restir.pt.enable` off-by-default | e-test: convergence parity vs reference (3%), RMSE-vs-equal-time |
| PT-2 | GPU MK_PT_* + tail rays | CPU/GPU parity, e14/e16/e19-class gates |
| PT-3 | hybrid shift (multi-RC candidates, footprint pick), failed-shift mass, MIS transfer, volume segments | paper parity |
| PT-4 | Enhanced: reciprocal neighbor eval, duplication map, unified DI/GI/PT reservoir layout | benchmarks |

## Properties (planned)

```
path.restir.pt.enable            bool   default false (PT-1 scope)
path.restir.pt.candidates        int    default 4
path.restir.pt.temporal.enable   bool   default true
path.restir.pt.spatial.enable    bool   default true
```

Blender exposure: `denoiser`-adjacent ReSTIR section — mirror the
existing `restir_*` property/export/UI pattern
(`properties/config.py`, `export/config.py`, `ui/render/sampling.py`).
