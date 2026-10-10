# Experimental Cycles BSSRDF device adjoint transport

The unchanged-Cycles-scene goal remains active. This candidate adds a light-side
random walk and a rough inverse entry boundary to the existing private PATHOCL
diagnostic. Production SSS adapter exposure, sharp device camera connections,
mixed/textured reverse kernels and broad production acceptance remain open.
The stable installed adapter still exports positive standalone SSS through its
earlier route. Shipping quality defaults are not changed by these private gates.

## State and transport

GPU BSDF phase 3 represents the inverse entry boundary after a light-side walk
escapes; phase 4 represents a pending light-side free flight. Eye phases 1/2
retain their pending-walk/white-escape meanings. The walk record occupies the
existing exclusive `GPUTask::tmpHitPoint` scratch. It now saves entry material
identity as well as mesh/group identity, without increasing the ordinary task
buffer or adding a kernel argument. The host size assertion is checked during
the Release build.

Light entry transposes the eye walk's white diffuse escape: it draws an inward
geometry-cosine direction, retaining the CPU shade/geometry correction once.
The existing channel-balanced distance sampling, Van de Hulst mapping, HG
scattering and roulette are shared with the eye walk. Low-albedo compensation
is retained in the stored throughput. Calls that mutate the shared RNG state
are placed in separate expressions.

At escape the light scheduler restores the entry material and original surface
identity, sets the actual incoming walk direction and light ray context, and
evaluates/samples the shared inverse boundary densities. Its directional
transpose and geometry-cosine correction match the CPU adjoint convention.
The user's scattering-depth and caustic classifier see a collapsed diffuse
reflection; the internal inverse transmission does not become an external
specular event. Camera splats retain the visible material/object Cryptomatte
names. The eye and light schedulers own their respective film/path states.

An absorbed light walk can terminate with phase 4 still set. A new emission
must reset that phase before tracing its first surface. Without the reset,
the next emission can resolve an old SSS flight against its new ray/hit. The
initial candidate produced roughly 10% excess gray energy under both Metal
schedulers. Resetting phase in `MK_LIGHT_INIT` removed that excess in the fixed
independent-eye comparison. Those failed and interrupted diagnostic runs are
preserved and excluded from passing evidence.

Blender 5.2's `intern/cycles/kernel/integrator/subsurface_random_walk.h` was
checked directly for the low-albedo coefficient/throughput compensation. This
implementation follows that semantic input model and the existing SuperLuxCore
boundary operator; it does not seek bit-identical Cycles images.

## Gates and current validation

PATHOCL reverse transport requires all three explicit experimental, device and
adjoint flags. Uniform standalone rough closures can use light-only or hybrid
transport. Positive nonlocal sharp boundaries are rejected because their
camera manifold connections are not implemented on the device. All-local
radius remains ordinary diffuse. Mixed, partitioned, Normal/Bump, opacity,
Volume, textured reverse, BIDIR, vertex-connection and cache contexts retain
their preflight restrictions.

The complete isolated candidate wheel's loaded native SHA256 is
`1f71800d1a2e40eb20fd3fd9e9905f03974e149b2715d2562c15278dcad8d551`;
wheel SHA256 is
`e047a30312d00f42c5c9d4bb4865af5d5c284a5942590b2a98b2fb6348c52714`.
The initial source/profile identity covers the complete wheel RECORD, cached
wheel, 266 adapter Python files, 459 profile entries and 75 critical source
files. After the original workers completed, diagnostic revision 2 added the
three device drivers and Blender extension and repaired the PSR driver. Its
79-file identity retains the same compiled native code and complete wheel.
These hashes identify a local candidate, not a public or installed revision.

Evidence is under the parent workspace's
`test-scenes/validation-2026-10-11/cycles-bssrdf-gpu-adjoint`. Candidate 4's first
32px gray probe completed: CPU light 1.1688%, Metal light 0.2716%/0.2547%, and
Metal eye 0.1524% maximum relative channel-mean difference from CPU eye. The
declared 3% gate and 8192 eye / 65536 light budgets were retained.

The permanent device adjoint suite completed all 42 conditions after the
existing CPU50 regressions: unsupported contexts, independent seeds, IOR
extremes, HG signs, low albedo, color, partial/all-local radius, spectral
transport, absorption, Cryptomatte identity and continuation to an ordinary
floor. Maximum channel-mean error was 2.1389%. The separate smooth/curved
geometry suite completed eight conditions, including a concave torus, with
maximum 1.0119% error. The same wheel completed CPU50/hybrid23,
CPU50/rough-adjoint22/sharp18. Their maxima were 2.2537%, 2.5374% and 0.7191%.

The first device hybrid diagnostic used only 8192 total tasks. PATHOCL reserves
at least 8192 eye tasks, so that left no light tail. With native threads set
to zero the engine safely disabled hybrid suppression, and the test's required
light sample count stayed zero. That run was interrupted and excluded; its
source and log remain in the evidence. The repaired diagnostic uses 16384
total tasks, zero native threads, and asserts actual light progress. The
65536 eye / 81920 light minimum counts and 3% gate are unchanged. Its first
six rough/PSR/colored-spectral conditions all completed. Maximum channel-mean
error was 1.3984%, and alpha stayed opaque. The rough mirror pair had
0.3075%/0.0589% error; the PSR pair had 0.0993%/0.0777%; the colored spectral
pair had 1.3984%/0.4664%. A separately rendered eye-hole partition omitted
35.2321% of the fixture's energy, proving meaningful caustic support.

The older ordinary PSR3 logs show GPU eye paths with a native CPU light
fallback at 8192 total tasks. Those results remain combined-transport evidence;
they do not establish an all-device light pass. The corrected PSR harness
uses a real GPU light tail and zero native threads, with explicit light-progress
checks. It completed all three CPU/Metal conditions, with maximum 2.0013% error.
Existing Metal57 and RANDOM/METROPOLIS light sampler4 conditions completed;
the sampler maximum was 0.1945%. Guarded ordinary emission/MNEE15 and black
film3 regressions also completed, without a GPU skip.

Three unchanged-graph 1280x720 comparison sets completed: independent CPU eye,
Metal hybrid wavefront off and on, at 128 eye / 512 light samples for the
hybrid. All six original PNGs were directly inspected. Silhouette, orientation
and warm rough scattering meaning are preserved, but native spectral grain,
especially in the hybrid, remains conspicuous. Raw finite EXR means inside the
predeclared 240px-radius body region differed from independent CPU eye by
0.3209%/0.3401%. This is operator validation, not production noise convergence.
Different schedulers complete different excess sample counts; their wall times
are not a matched performance comparison.

`candidate4/local-completion-proof.json` verifies the metrics, original image
hashes, complete wheel, 79 source files and 459 profile entries. All owned
workers finished, and the unused isolated stage was removed (403443339 logical
bytes; physical space freed is not measured). The whole wheel, raw outputs,
failed diagnostics and source identities remain in durable evidence. Two
misleading preflight messages were then corrected without changing the kernel
or validation logic. The fresh Release build completed, and all 12 existing preflight rejection
contracts passed against its raw module. Exact-source signed platform CI and
installation are pending.

Harnesses:

- `dev-tools/cycles_bssrdf_device_adjoint_test.py`
- `dev-tools/cycles_bssrdf_device_adjoint_geometry_test.py`
- `dev-tools/cycles_bssrdf_device_hybrid_test.py`

These are small estimator contracts. They establish neither production noise
convergence nor a speedup. The full compatibility goal is not completed by a
rough uniform device diagnostic.
