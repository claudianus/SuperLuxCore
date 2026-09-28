# SSP: eye-side specular tail recorder (path.ssp.enable)

The camera-side specular chain of a path (C -> S1 -> S2 -> ... -> Sm -> D)
is exactly the topology an LMNEE light->camera connect needs when its
visibility ray is blocked by a delta surface. SSP records that run while
the eye path walks it - nearly for free - and hands it to the blocked
connect as a ready-made manifold chain: no discovery walk, and the
recorded hit positions are Newton seeds far better than the straight
x0->lens line.

## Mechanics

- `SspTail` (include/slg/utils/pathinfo.h) holds up to
  `SSP_TAIL_MAX_VERTICES=8` anchors (position, geometry normal,
  objectID) plus the first non-eligible vertex (terminator).
- `SspTailRecordVertex` runs once per eye-path vertex in
  `PathTracer::RenderEyePath`, right after `EyePathInfo::AddVertex`.
  Eligibility mirrors `MneeChainVertexInit`: delta + SPECULAR +
  MIRROR/GLASS. Volume scatter vertices and pass-through pseudo-bounces
  neither extend nor break the run.
- The record lives in `PathTracerThreadState::sspTail` (thread-local,
  reset per eye sample). `RenderSample` passes it to the paired light
  sample; `RenderEyeSample`/`RenderLightSample` grew a trailing
  `SspTail*` parameter defaulted to nullptr, so engines without light
  paths (BAKECPU) or without eye paths (LIGHTCPU) are unaffected.
- On a blocked connect (`ConnectToEye`), after the single-vertex LMNEE
  solve fails, the tail is consumed when the connect blocker is one of
  the recorded objects and `specN <= path.mnee.maxspecular`.
  `LMNEETailConnectToEye` reprojects each anchor
  (`MneeReproject` + `MneeChainVertexInit`, objectID match = stale
  rejection), reverses eye order into light->lens order, and feeds the
  shared `LMneeChainSolveAndEval` (extracted from
  `LMNEEMultiConnectToEye`).

## Correctness

The tail is a topology/seed hint only. Every rebuilt vertex is
re-projected and re-accepted; the Newton solve, side/eta checks,
geometric-term Jacobian and last-segment visibility all run unchanged -
a stale tail merely wastes iterations, it cannot inject a wrong path.
The estimator stays unbiased: contributions are evaluated at the solved
positions with the same Jacobian weighting as the discovery path.

`specN=1` tails are handled too: they cover the case where the
single-vertex solver failed yet `mneeMaxSpecular>1` multi-discovery
would also miss (chain<2).

## Properties

| key | default | notes |
| --- | --- | --- |
| `path.ssp.enable` | 1 | effective only with `path.mnee.enable` |

## Status / validation

CPU complete (Phase 1-2). e93_ssp_tail_test.py: lmnee-slab PATHCPU
hybrid - 401K tail splats vs 630K discovery splats (~39% of chain
connects served by records), zero tail activity with ssp disabled,
ssp on/off images correlate 0.989 at matched spp (pure MC noise).

GPU port (Phase 3, planned): `sspTailsBuff` auxiliary buffer gated on
`path.ssp.enable`, `SspTail_RecordVertex` in `MK_GENERATE_NEXT_VERTEX_RAY`,
`MNEE_PHASE_TAIL_LOAD` consumer in `MK_LIGHT_VERTEX` - see
doc/engineering/ssp-tail.md.
