# Sparse Metropolis startup normalization

The Cycles BSSRDF hybrid work exposed a separate startup defect after forced
acceptance had already been removed. A small mutation could discover light
before any productive uniform proposal had estimated the normalization constant
`b`. CPU chains then used the initial `b=1` fallback, and a positive chain on a
nonzero worker could continue using it until worker 0 was scheduled. Those
early contributions persisted in the accumulated film even after the estimate
became accurate. Device chains had the same local-mutation-before-`b` hazard,
although their normalizers are per task rather than shared across CPU workers.

The sampler now uses uniform proposals until a productive uniform proposal
has supplied `b`. Black proposals still complete and count toward sample halts.
While CPU warmup is active, each chain reads the live uniform luminance/count
estimate before splatting. The published inverse luminance and warmup state
are atomic, removing the worker read/write data races. The default mutation
rate, acceptance ratio, warmup exit thresholds, path estimators and renderer
quality settings are unchanged. Device proposal selection applies the same
bootstrap rule without changing its task layout.

The normalization factor is part of converting a Metropolis distribution back
to an image integral; the independent integral and mutation-chain roles are
described in [PBRT's MLT chapter](https://www.pbr-book.org/3ed-2018/Light_Transport_III_Bidirectional_Methods/Metropolis_Light_Transport).
This repair prevents use of the initial fallback by a productive local chain.
It does not replace the existing adaptive normalizer with an independent
pilot, or prove unbiasedness for arbitrary finite-budget renders.

## Numerical evidence

`dev-tools/metropolis_sparse_contract_test.py --startup` builds the Release
`luxcore` shared target, compiles `metropolis_startup_contract.cpp`, links that
actual library, and removes the temporary executable on completion. Building
only `pysuperluxcore` refreshes the static core/module and can leave the separate
shared library stale; the driver now explicitly builds its own link target.

The sparse oracle enumerates all `2^24` primary float-grid inputs to a hash
integrand with about 1/2048 productive support. The startup contract supplies
neither `b` nor a warmup override. Its declared seeds are 131, 817 and 919.
Each runs 32 million proposals on workers 0 and 1 independently; three further
cases run eight real concurrent workers with 64 million total proposals. Six
dense-support cases exercise exit from warmup with default/large mutation
rates, and three entirely black inputs include a configured zero mutation rate.
All 18 cases passed the unchanged 3% numerical gate. The largest observed error
was 2.4963% in an eight-worker sparse case. The separate fixed-`b` acceptance
contract also passed: 0.0463893% error with both legacy rejection limits.

The predeclared source-override diagnostic compared baseline, uniform bootstrap
alone, and uniform bootstrap plus live CPU estimates. Baseline seed817 retained
23.8594% excess energy at 32 million proposals; seed919 retained12.6844%.
The selected candidate's three single-chain errors were1.8582%,0.6974%,0.4812%.
Early four-million-sample candidate windows still had up to9.2228% error.
Uniform-estimate variance and finite startup convergence therefore remain
relevant; this is a specific fallback/worker-publication repair, not general
production convergence acceptance. The baseline/failed/stale-library runs
are preserved and excluded from passing product evidence.

Local evidence is under
`test-scenes/validation-2026-10-11/metropolis-startup` in the parent workspace.
The exact packaged native/source hashes are in `local-identity.json`;
`startup-contract-metrics.json` identifies the separately rebuilt shared core.
The CPU eye/light/hybrid all-black render contract uses actual film sample
halts, while the actual Metal device contract adds all-black completion for
both scheduler modes. Broader transport, images, CI and installed deployment
are recorded as they complete; the full Cycles-scene goal remains active.

## Packaged local validation before CI

The complete isolated local wheel's loaded native SHA256 is
`2147c5aaf6ebef88acb4a3eabceaccaf55c7dfa3d3b627fc25496a16a69cc807`.
Its 266 addon Python files match current addon main; it does not update the
user's production adapter or native installation. CPU50/hybrid23 completed,
as did actual Metal57, ordinary PSR3 and guarded emission/MNEE15. The separate
CPU eye/light/hybrid black-film halt cases all completed. Additional actual
eight-worker dense tests completed and exited warmup for the three declared
seeds, with maximum0.6149% integral error (`concurrent-freeze.log`).

Twelve original1280x720 PNGs were reviewed directly across CPU hybrid, CPU eye
and actual Metal eye, with their Cycles references. Each authored graph stayed
unchanged. Body-region maximum relative channel-mean errors against the new
independent CPU eye were0.3295%/1.9689% for hybrid sharp/rough and
0.7203%/0.1481% for Metal eye sharp/rough. The private diagnostic retains its
explicit unsupported-transport limits; chromatic hybrid grain remains visible
and these images do not close production SSS compatibility or convergence.
The pure-LIGHTCPU rough/sharp regression is still running at publication of
this initial source commit. CI and installed deployment remain unverified for
this revision until their exact artifacts pass the relevant checks.
