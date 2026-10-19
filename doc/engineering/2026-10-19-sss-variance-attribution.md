# SSS path variance attribution and rejected object-group BVHs

This work does not establish a renderer speedup or improved convergence.
It rejects another overhead hypothesis and measures which physical paths
carry the raw image variance before selecting another estimator change.

## Object-group traversal screen

The local Cycles source uses object-local traversal. A CPU prototype shared
committed Embree geometry with separate scenes, an API supported by the
[Embree reference](https://raw.githubusercontent.com/embree/embree/master/readme.pdf).
It preserved global mesh/triangle IDs, ray-count accounting, and a global
fallback on misses. Only closed opaque homogeneous SSS groups whose padded
bounds did not overlap any outside geometry were eligible. Bevel/motion
uncertainty fell back. Storage was bounded to 8 objects/200000 triangles;
views were released before scene edits replaced geometry. A view retained
its Embree device explicitly; omitting that reference failed the parent-
release test during development and was corrected before the group screen.

The first single-object rule activated zero accelerators because the artist
fixture shares one volume across overlapping round/thin objects. Those
times are not evidence about local traversal. Grouping every object with
the same interior volume activated 3 domains/13344 triangles, adding about
.011 seconds of accelerator construction in the first active run.

131074 direct queries matched the full accelerator bit for bit (distance,
mesh/triangle IDs, barycentrics). 129 views were built/released, with instance
update and parent-release lifetime checks. That probe preceded the final
dynamic-flag trial. Artist RGB and sample-count
buffers were exact; global fallbacks increased the actual query counts
slightly. The active static-scene screen took 26.2281s versus 22.4997s off.
A dynamic-scene variant matching the global build flag took 23.2696s,
including a short profiler capture. Process CPU times were 26.7030,
22.8097 and 23.4461 seconds respectively. These sequential shared-host
screens are not uncontended ratios, and the profiled run is not a timing
gate. No variant provided adoption evidence. All production edits were
removed, retaining source snapshots and receipts in
`../test-scenes/validation-2026-10-19/sss-object-bvh/`.

After restoration, all six edited source files matched their snapshots;
the rebuilt payload SHA was again
`98fca869ee1b29512af7331f44dfc2a7b7a8f7231a2b3361058192bb70af66a6`,
identical to the prior 112-buffer CPU regression payload. Consumed module
overlays were removed. No experimental wheel was installed.

## Diagnostic contract

Evidence root: `../test-scenes/validation-2026-10-19/sss-aov-variance/`.
320x180, CPU1, RGB, 144 exact SPP, independent seeds 17/53/97, authored SHA
`d913ff75bc07a85ff0d4d70f196668558909667d40d188d5291ea628cb16816c`.
The checked unregularized-v2 controls disable PSR, glossy filtering,
radiance/variance clamps, denoising and adaptive sampling. Diagnostic film
channels add accumulation/readback work; these times are not benchmarks.

`sss_export_benchmark.py --aovs` exports eleven mutually composing standard
reflection/transmission AOVs. Their sum reconstructs raw RGB within ordinary
float accumulation error. Across all three seeds, RGB/count/ray buffers
were bit exact to the earlier non-AOV baseline. Standard AOVs classify the
first camera lobe; they cannot count internal collisions or distinguish
the terminal NEE and BSDF-hit estimators. The transmission first-lobe share
of SSS-mask variance was .973871 (.996692 in the thin mask).

`--path-audit` instead uses existing LPE automata to accept complete paths
with at least 1, 2 or 4 volume vertices. The terminal union includes area
lights and environments. Subtracting the nested cumulative channels gives
0, 1, 2-3 and 4+ volume-vertex bins. The all-path channel reconstructs raw
RGB exactly. Internal reflections/surface vertices do not increment V;
these are actual volume scattering events anywhere on the complete path.
This is still not a partition by terminal sampling technique.

For each component X and total T, report Cov(X,T)/Var(T), averaging the
independent-seed image covariance over the same mask and RGB channels.
Shares sum to one and may be negative for anticorrelated components; do
not clamp them. A known anticorrelation oracle yields shares -1.5 and 2.5.
This is a finite three-seed diagnosis, not a variance-reduction claim or
an asymptotic variance estimate. The analyzers check metadata, exact sample
counts, finite buffers, channel reconstruction and covariance sums.

| Volume vertices | SSS variance share | SSS mean RGB share |
|---|---:|---:|
| 0 | .008900 | .155179 |
| 1 | .054404 | .115877 |
| 2-3 | .125959 | .153360 |
| 4+ | .810737 | .575584 |

The 4+ bin shares are .803148 for the bulk mask and .830771 for the thin
mask. These measurements justify targeting multiply scattered transmission
paths. They do not by themselves separate direction/flight variance from
the final refractive boundary/light connection.

## LPE-only CPU film bug repaired

The initial audit failed closed: all LPEs were black and could not
reconstruct RGB. Both normal and atomic CPU film splats accumulated LPEs
inside `hasComposingChannel`, which only ordinary AOVs enabled. LPE
expressions are configured independently. Moving their accumulation
outside that conditional repairs both initial and live LPE-only films;
transport, RGB accumulation and existing composing AOVs are unchanged.

`lpe_composing_test.py --before <old filmaddsample.cpp> --out <receipt>`
compiles the real film methods. 24 normal/atomic, composing/noncomposing,
initial/live checks pass on the candidate. A separately linked old object
reproduces the expected black LPE-only results. Temporary binaries and
objects are removed automatically. Final payload SHA:
`de35280d6030c8bc9b9cc91989261e7842b1e0ae5c1c507d36db58ddebe2059e`.
All three artist RGB/count/ray outputs remain exact after enabling LPEs.
The 14-case CPU regression and 112-buffer comparison are retained in the
evidence root. These checks do not certify Metal or spectral LPE projection;
the diagnostic writer explicitly restricts these new outputs to RGB.

Next estimator work should target the 4+ volume-vertex contribution with
full physical kernels, positive proposal support and correct joint PDFs.
Any exit-cone proposal is a sampling hypothesis only: keep refraction,
internal reflection and all channels, test integral/support before artist
convergence, and reject it unless raw error and total cost both justify it.
