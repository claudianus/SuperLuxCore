# Light Path Expressions (LPE)

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Light Path Expressions (`film.lpe.N.expression`)

PBRT-style LPE AOVs. Each `film.lpe.N.expression` (+ optional `.name`)
compiles to a bounded NFA (`slg::LPEAutomaton`, `include/slg/utils/lpe.h`
+ `lpe.cpp`; `SLG_LPE_MAX_STATES=32`, `SLG_LPE_MAX_EXPRESSIONS=8`).
`film.outputs.*.type = LPE` + `.index` selects the expression; EXR layer
name = `LPE.<name>` (HDR only). Grammar: `|`, `()`, `* + ?`, `.` (any
vertex), `<preds>` (e.g. `<RD>` = diffuse reflect), symbols C L E B +
classes D G S + directions R T + V.

- Path carries one u32 live-state mask per expression in `EyePathInfo`
  (`lpeStates`), seeded from `startAfterC` in `InitLPE`/`GenerateEyePath`
  and stepped per vertex in `AddVertex`/`EyePathInfo_AddVertex`.
- **NEE/VC/MNEE terminals are TWO symbols**: a light connection at vN is
  the path `C v1..vN L`, but `AddVertex(vN)` runs after the NEE eval in
  the loop — so the terminal eval must first step vN's own event, then
  L/E (`LPEAcceptMask(vSym, termSym)` / `LPE_AccumulateVertex`). A
  direct emitter hit is single-symbol (the emitter vertex IS L).
  Getting this wrong shifts every NEE contribution one vertex earlier:
  `C<RD>L` silently loses first-vertex direct light (measured 0.04 vs
  0.076 expected).
- GPU NEE vertex event: the direction bit comes from geometry, not the
  material's event types — `dot(geometryN, fixedDir) * dot(geometryN,
  shadowRay.d) < 0` = TRANSMIT (fixedDir = -ray.d, origin-side).
- Accumulation: `SampleResult::lpeRadiance[8]` (RGB weighted) splats to
  `channel_LPEs` (`GenericFrameBuffer<4,1,float>` — ch3 = weight);
  GPU uses ONE flat `filmLPE` buffer (expression-blocked: `e * 4 *
  pixelCount`) + one `lpeAutomata` table buffer = 3 extra kernel args.
- Scope: PATHCPU/PATHOCL (eye paths incl. MNEE + M6 vertex-connect).
  BIDIR/light-tracing do not carry LPE state. `B` (miss) is a dead
  symbol — a miss carries no radiance so nothing accumulates.
- `ResetEyeSampleResults` clears per-sample SampleResult fields between
  samples WITHOUT a full Init — any new accumulation field MUST be
  zeroed there or it accumulates the thread's whole render history
  (lpeRadiance hit exactly this: ~1e4-1e5x blowup, uniform across
  expressions so partition checks still passed).
- Regression: `dev-tools/e42_lpe_test.py` (partition vs RGB, CL ==
  EMISSION, C<RD>L == DIRECT_DIFFUSE, wildcard/alternation, CPU/GPU
  parity, malformed-expr rejection, EXR layer names);
  `dev-tools/e42_lpe_visual.py` (720p luxball-hdr: CE/C<RD>E/
  C<RD><RD>+E/C<RS>.*E decomposition + AgX Punchy beauty).

