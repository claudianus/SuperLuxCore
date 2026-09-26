# MNEE — Manifold Next Event Estimation (specular-chain solver)

Status: implemented. Newton-iteration specular-chain solver for hard caustics,
single + multi-specular chains, on CPU and GPU (OpenCL/Metal) kernels.

## What and why

Specular-only light paths (caustics through glass, mirror chains) are nearly
impossible to sample by ordinary path tracing because the connection direction
must satisfy refraction/reflection constraints at every vertex. MNEE treats
finding a valid specular chain as a **root-finding problem on a manifold**: it
places a specular chain between a shading point and a light, then iterates a
half-vector / Newton step until the half-vector constraint holds at every
vertex. This converts "invisible" caustics into an estimable connection.

## References

- Hanika, Droske, Fascione. **Manifold Next-Event Estimation.** Computer
  Graphics Forum (EGSR) 34(4), 2015. (The half-vector / Newton manifold walk.)
- Zeltner, Georgiev, Jakob. **Specular Manifold Sampling for Rendering
  High-Frequency Caustics and Glints.** SIGGRAPH 2020. (Production-grade
  variant; our chain solver is a MNEE-family implementation.)
- Jakob, Marschner. **Manifold Exploration: A Markov Chain Monte Carlo
  Technique for Rendering Scenes with Difficult Specular Transport.** SIGGRAPH
  2012. (Underlying manifold framework.)

## Implementation

- `slg/bsdf/manifold*` — Newton solver: builds a specular chain between the
  shading point and a sampled light point, iterates until half-vector
  constraints hold (within a tolerance) or rejects.
- Single-specular (`direct light through delta specular`) and multi-specular
  chains (`multi-specular chain solver`) supported; closed glass-slab and
  glass-ball caustics verified at CPU parity.
- GPU kernels mirror the CPU solver (Metal + OpenCL); `P1-2: MNEE specular
  chain solver in the GPU kernels (Metal)`.
- Correctness guards: reject the unphysical opposite-side mirror case, apply
  the light-weight `r12^2` measure factor only to the plain half-vector term,
  guard the optional Jacobian output.
- Instrumentation (default-off): `LUX_MNEE_ITER`, `LUX_MNEE_REJX`,
  `LUX_MNEE_DISC`, `LUX_MNEE_REJ` dump chain iterations / discovery /
  rejection reasons for debugging.

### Directional endpoints (distant / sharpdistant lights)

The single-vertex and multi-specular chain solvers share the `MneeEndpoint`
abstraction (`isDir` + `dir` | `pos`). A directional light has no finite
position, so everywhere the solver would use `Normalize(lightPos - p)` it
instead uses the constant light direction:

- `MneeChainResidual` treats the last vertex's `wo` as the fixed direction
  (`dirNext`), keeping the half-vector constraint in direction space.
- `MneeChainDiscover` walks the chain starting from `ep.dir` instead of a
  straight line to a point.
- `MneeChainLightJacobian` perturbs the direction itself on the tangent
  frame rather than moving a point (no `1/r` position->direction
  conversion, matching the single-vertex solver's `j2` block).
- The final shadow ray is rebuilt toward `ep.dir` and extended to the
  scene bounding sphere (a miss = the light is reached).
- The geometric term is already the direction-space Jacobian, so the only
  emitter factor left is the direction pdf (divides out for
  `sharpdistant`, whose pdf is 1).

GPU kernels mirror this via `MneeState::lightIsDir` +
`MneeChain_ResidualAt(woIsDir)` / `MneeChain_LightJac(lightIsDir)` and the
same bounding-sphere rebuild in `MneeChain_Seg2Setup`. LMNEE keeps a
finite endpoint (the lens point), so `lightIsDir` stays 0 there.

Gotcha that motivated the validation scene: the chain discovery walk keys
entering/exiting refraction off the **geometric** normal. A mesh with
inward-wound faces (shading normals out, winding in) flips the IOR ratio
and manufactures TIR at moderate incidence - the walk reflects out and
discovery fails ~70% of the time. `scenes/mnee_dir/sphere.ply` shipped
like that once; keep asset windings outward.

Validated on `scenes/mnee_dir/tinycaster.scn` (glass sphere + sharpdistant
sun, two-vertex entry+exit chain): CPU/GPU caustic profiles match to MC
noise (`e24_mnee_chain_distant_test.py`, interior 0.0899 vs 0.0000 with
MNEE off, CPU/GPU ratio 1.00).

### Estimator disjointness (eye-MNEE vs light tracing)

Every completed MNEE path is caustic-class (light -> specular chain ->
diffuse receiver -> eye). When `path.lighttracing.enable` (or
`hybridbackforward`) is on, the light pass owns that path class and the
eye side must NOT also estimate it. The existing `IsCausticPath` gate
only inspects specular events in the EYE prefix, while MNEE's specular
events sit in the connection leg - so without an explicit gate both
estimators fire and the caustic energy doubles. Measured on tinycaster
(umbra region): mnee-only 0.0154, lt-only 0.0149, both 0.0299 = exact
sum. Eye-side MNEE (single + chain) is now skipped when
`hybridBackForward.enabled`; LMNEE (the light-side connect solver) is
unaffected - it completes light-path deposits, not a competing
estimator. With the gate: both = 0.0149 = lt-only.

### Specular Polynomials evaluation (2024, Fan et al.)

Evaluated porting Spoly (SIGGRAPH 2024, github.com/mollnn/spoly) as a
Newton replacement: it rewrites the specular constraints as a bivariate
polynomial system per triangle (R degree 2 + T degree 4-6 with
interpolated normals), eliminates one variable via a hidden-variable
resultant, and enumerates ALL admissible roots deterministically.

Measured on our scenes (dense-seed Newton enumeration over barycentric
coordinates of each candidate triangle):

- flatsheet / wavysheet / sphere + sharpdistant light: every triangle
  had AT MOST ONE admissible root; line-seeded Newton found it in 100%
  of cases where a root existed (0 misses, 0 seed failures).
- The sphere admits a whole RING of solutions across ~190 triangles -
  but our solver treats the mesh as a continuous surface (Newton
  reprojects across triangle edges), so per-triangle enumeration adds
  nothing there.
- For k>=2 chains Spoly needs the triangle TUPLE and a bisection solve
  on a matrix-polynomial determinant - heavier than our Newton chain
  and still needs tuple candidate pruning to stay tractable.

Conclusion: Spoly's completeness wins on glints/high-frequency normals
(which we don't render anyway), not on smooth caustic casters. The
valuable transferable idea is CANDIDATE PRUNING (Bernstein-bound style:
which triangles can host a root for a given receiver-light pair) - a
multi-start Newton over pruned candidates gets deterministic coverage
without the resultant machinery (the reference port is ~80k lines of
generated coefficient code). Revisit if/when glint support lands.

### Manifold seed cache (E4)

Converged single-vertex solutions are stored in a fixed-size hashed grid
(`mneeSeeds`, 16384 x 32B on GPU; same scheme on CPU) keyed by (light,
occluder mesh, quantized shadow-ray occluder hit position, incident side).
Opt-out with `path.mnee.seedcache = 0` (default on).

**Cold-first seed policy** (found by fixing a real energy regression):
the cache is a basin-selection *rescue*, never the authority on which
root is found.

- Glass (eta != 1): the cold line seed is free (the shadow-ray hit
  itself) and defines the reference basin selection. The cache is
  consulted ONLY when the cold solve fails - a rescue, not a default.
- Mirror (eta == 1): the cold seed costs an extra mirrored-endpoint
  trace, so the cache gets first refusal and skips it on a hit.
- Entries are written only after the FULL connect validates (second-
  segment visibility + receiver BSDF), not at solve time: a vertex that
  solves but fails downstream lands its reuse in the same dead basin.

Why not cache-first for glass: on multi-root casters (the bumpy-sphere
seedcache scene) a cached vertex is a basin winner for a NEARBY query;
seeding from it directly pinned every nearby attempt to the first-cached
basin and measured ~5% caustic energy loss vs cache-off - the cached
vertex usually even wins a residual comparison, so comparing initial
residuals does not fix it (measured: 0.942 -> 0.946 only). Under
cold-first + failure-rescue the cache instead *adds* contributions where
the cold start diverges: +14% global / +6% caustic energy on the
seedcache scene at CPU/GPU parity, identical convergence otherwise.

Scope / honest limitations:
- **Single-vertex solves only.** Multi-specular chains
  (`path.mnee.maxspecular` >= 2) do not consult the cache - that is
  where the real solve cost lives, so extending the cache there is the
  natural follow-up.
- **Mirror blocking is almost always opposite-side** (eta = -1 -> chain
  solver), so the cache-first mirror path rarely triggers; in practice
  the cache is a glass-solve failure rescue.
- On multi-root surfaces the cold-seed policy still returns ONE root per
  (x0, light) pair - underestimating the full multi-root sum. The rescue
  adds coverage where cold diverged but is not a complete root
  enumerator (see the Specular Polynomials evaluation below).
- MNEE itself is opt-in (`path.mnee.enable`, default off), so this is a
  niche accelerator for caustic-heavy scenes, not a general speedup.

Corrected validation note: the original e17 scenes set
`transparency.shadow = 1`, which lets shadow rays pass through the
occluder so MNEE never fired - the earlier "8/8 unbiased" result was
vacuous (on/off trivially identical). The corrected harness uses a
point light + specular occluder with no shadow transparency and checks
that a real caustic is produced (proving solves actually ran).

### Properties

- `path.mnee.enable` (default off) — specular-chain direct light sampling.
- `path.mnee.maxiterations` (default 12) — Newton/line-search bound.
- `path.mnee.maxspecular` (default 1) — 2..4 enable multi-specular chains.
- `path.mnee.seedcache` (default on, CPU + GPU) — manifold seed cache
  (cold-first + failure-rescue policy; see above).
- Exposed to Blender via `MNEE specular caustics` option (`P1-2: expose MNEE
  specular caustics option`).

## Test scenes / validation

- `scenes/causticcube/`, glass slab / glass ball caustics — rendered at CPU
  parity on Metal GPU.
- `scenes/mnee/seedcache.scn` — point light inside a closed bumpy-glass
  sphere over a diffuse floor; a real single-interface MNEE-active scene
  used by `e17_mnee_seedcache_gpu_test.py` (replaces the old juice
  `test.scn`/`test-mirror.scn`, whose `transparency.shadow = 1` let
  shadow rays through so MNEE never actually ran).
- `dev-tools/mnee_design.md` — internal design notes.

## Platforms

CPU, OpenCL GPU, Metal GPU.
