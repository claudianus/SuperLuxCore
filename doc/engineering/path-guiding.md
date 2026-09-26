# Path guiding — implementation notes

> Engineering note for SuperLuxCore — extracted from AGENTS.md.
> Feature/user-facing docs live in `doc/features/` (SuperLuxCore) or `doc/` (SuperBlendLuxCore).

## Path guiding (`path.guiding.*`)

Adaptive SD-tree + per-leaf vMF mixtures (see
`doc/features/path-guiding.md` P5 section for the full property table).
All artist knobs are formal `path.guiding.*` / `path.portal.*`
properties; `LUX_PG_*` envs survive only as debug fallbacks.

- `PathGuidingCache::Settings` + `SettingsFromProperties(cfg)` is the
  single resolution path shared by PATHCPU and PATHOCL — never re-read
  env vars in engine code.
- Bounce gates live in the OCL task config (`guidingRisK/MinDepth/
  Glossiness/Diffuse/Strength` in `pathtracer_types.cl`, filled in
  `compilepathtracer.cpp`) — the kernels must never use host constants.
- Cold-leaf fallback: `BuildReadTree` walks cold leaves to their nearest
  warm ancestor (aggregate `AggStats` count >= warmup) and installs the
  ancestor fit with a damped synthetic count — the GPU leaf layout is
  unchanged, only provenance differs.
- Adaptive K: per-leaf BIC over candidate lobe counts 0..components
  (default cap 4); the uniform lobe stays as a coverage floor.
- `path.guiding.savetable` dumps the trained read tree on StopLockLess
  on BOTH PATHCPU and PATHOCL (a `ForceSwap()` first, so the pending
  write-side records are included); `path.guiding.tablefile` warm-starts.
- Regression: `dev-tools/e43_pathguiding_test.py` (10/10),
  `dev-tools/e43_pathguiding_visual.py` (720p pg-gallery, warm-start
  RMSE ~1.1x at 48 spp — honest modest gain, the scene's residual
  variance is dominated by the bright window's NEE).
- **PATHCPU renders are nondeterministic run-to-run**: each thread
  walks its own Sobol stream over normalized pixel space, so
  pixel/sample assignment follows thread scheduling + external CPU
  load. Same-seed renders differ by max pixel ~4; 5-run ensemble
  variance ratios swing 0.5x-1.8x on unchanged code. Any quantitative
  variance/RMSE assertion must run on PATHOCL (deterministic
  task/seed batching, ~2% run-to-run) — or accept CPU only for
  mean/bias checks.

