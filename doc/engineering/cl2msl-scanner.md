# cl2msl.py scanner invariants

`src/slg/utils/cl2msl.py` translates the concatenated OpenCL program to
MSL with regex + hand-rolled span walkers. Three scanners walk parens /
braces over raw text:

- `_fn_spans` (helper/kernle definitions; feeds `propagate_gid`)
- `rule_scalar_ptr_params` (rewrites `float *x` -> `thread float *x` in
  function signatures; Metal requires explicit address spaces)
- `_rewrite_kernel_bodies_only` (builtin rewrites inside kernels)

## The trap

An **unbalanced paren inside a comment** breaks the walkers: the paren
depth counter includes comment text, so a `// ... (foo` that never
closes extends the current candidate span until a much later `)`.
Everything inside the swallowed span is emitted *raw* — signatures
skip their `thread` rewrite, and Metal rejects them hundreds of lines
downstream with the opaque

    program_source:NNNNN:CC: error: pointer type must have explicit
    address space qualifier

Cascade signature: the erroring functions are innocent; the poison
comment is usually in the most recently added block. The real span
that swallows them can be found by replaying
`rule_scalar_ptr_params`' candidate iteration and printing which span
covers the first erroring signature.

## Fix (e23d67847)

`_scan_safe(text)` returns a comment-blanked copy (identical length,
newlines preserved, string literals skipped). All three scanners match
and walk on the safe copy while slicing emitted text from the
original, so indices stay 1:1. Comment parens can no longer corrupt
span detection; unbalanced comment parens in existing code (there are
many) are now harmless.

## Debugging recipe

The translator writes its exact input to `/tmp/luxcore_metal_src.cl`
and the MSL output to `/tmp/luxcore_metal_src.msl`. To reproduce a
compile failure offline:

    python3 src/slg/utils/cl2msl.py /tmp/out.msl /tmp/out.json \
        "-D LUXRAYS_OPENCL_KERNEL" "-D SLG_OPENCL_KERNEL" \
        "-D RENDER_ENGINE_PATHOCL" "-D PARAM_RAY_EPSILON_MIN=1e-5f" \
        "-D PARAM_RAY_EPSILON_MAX=1e-2f" "-D LUXCORE_METAL" \
        < /tmp/luxcore_metal_src.cl

then inspect the MSL at the reported `program_source` line. Stock
`clang -x cl` accepts sources that still fail this pipeline — always
verify Metal translation, not just OpenCL syntax.

## Related gotcha (PT-2): tail-ray coverage

`EnqueueTraceRayBuffer(raysBuff, hitsBuff, rayCount)` traces exactly
`rayCount` slots. Every ReSTIR tail region (DI visibility rays, GI
bounce/NEE/merge rays, PT bounce/NEE/merge rays) must be added to the
call sites' `raySlotCount` AND to the buffer allocation — forgetting
the call-site count leaves tail rays untraced while hits read stale
data, which degrades gracefully to "feature silently no-ops" (all
misses -> fallback path), passing loose parity gates. PATHOCL call
site: `pathocl/pathoclopenclthread.cpp`; TILEPATHOCL:
`tilepathoclthread.cpp`; allocation: `pathoclbaseoclthreadinit.cpp`.
