# Adaptive-error review fixes: NaN/Inf handling + serialization rebind

Review-fix round (2026-10-04): four defects found while auditing the
E5 statistical adaptive error / noise-level work.

## 1. Corrupt moments collapse to "converged"

`Max(e2 - mu*mu, 0.f)` maps `e2 = NaN` to `0` (`NaN > 0` is false), and
`mu = +inf` makes `inf - inf = NaN` collapse the same way. Both produce
`rawErr = 0` — a NaN/Inf-contaminated pixel read as perfectly clean and
counted toward the noise-level halt.

Fix (`filmadaptiveerror.cpp`): `!isfinite(e2) || !isfinite(mu)` marks the
pixel `+inf` — excluded from the percentile (so a few corrupt pixels
can't pin the metric forever) but counted as not converged and written
as `NOISE = 1` so adaptive samplers keep sampling it.

## 2. Same collapse in the Sobol moments estimators

CPU (`sobol.cpp`) and GPU (`sampler_sobol_funcs.cl`) luma-moment paths
had the identical collapse: `fmax(NaN, 0) = 0` → `relErr = 0` →
`noise = 0` → the pixel gets *skipped* at the `1 - adaptiveStrength`
rate — the corrupt pixel is both starved and hidden from the map.

Guard the **raw accumulators** (`isfinite(mom[0]) && isfinite(mom[1])`),
not `relErr`: `mean = +inf` yields `relErr = 0/inf = 0`, which is
finite and would slip a result-level check. On failure `noise` stays
`INFINITY` → the pixel is always sampled (CPU) or deferred to the film
NOISE map via `noiseValid = false`.

## 3. `FilmAdaptiveError::serialize` nested-film wart

Version 1 serialized the `film` back pointer inline (`ar & film`). Boost
pointer serialization writes the *entire film* a second time inside the
adaptive-error blob (channel buffers, `SetUpHW()` on load), and after
load `adaptiveError->film` bound to that frozen duplicate — the test
queried save-time totals and never re-triggered after a resume.

Version 2 drops the field; `Film::load` calls `adaptiveError->BindFilm(this)`.
v1 archives still consume the legacy pointer field for stream alignment —
but it must **not** be deleted: `FilmConstPtr` is an observer_ptr
serialized as a boost-tracked raw pointer, so on load it resolves either
to the parent film currently inside `Film::load` (pointer-rooted
archives) or to the nested copy that `convTest->film` /
`noiseEstimation->film` still legitimately reference. `delete`ing it
frees a live (or mid-deserialization) object → `~Film` re-enters and
deletes the very `FilmAdaptiveError` whose `serialize` is running →
UAF/double-free. The one-time copy stays alive exactly as it did under
v1 semantics. `IsTestUpdateRequired()` early-outs on an unbound `film`
so orphaned objects stay inert; all scalar members are initialized in
the default ctor for the same reason.

Note: `FilmConvTest`/`FilmNoiseEstimation` carry the identical upstream
`ar & film` pattern — deliberately left untouched (shipped v1 format
compatibility).

## 4. PhotonGI update u_int wrap

`filmSPP - lastUpdateSpp` wraps to a huge delta if the film resets
mid-session (`filmSPP < lastUpdateSpp`), relaunching an update worker
every poll. Guarded with `filmSPP > lastUpdateSpp`.

## Pre-existing finding — fixed in this round

Standalone `.flm` / `.rsm` / `.rst` / config round-trips were broken at
the archive root (upstream-inherited): by-value or smart-pointer save
roots vs `unique_ptr`/`shared_ptr` load roots never matched — boost root
records are per-type. Repaired by moving every archive root to a raw
`T*` record on both sides; see `serialization-roots.md` for the full
mechanism and the pyluxcore binding fixes. Verified by
`dev-tools/e94_serialize_roundtrip_test.py`.
