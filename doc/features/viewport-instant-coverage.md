# Viewport instant coverage — lattice sampling + VIEWPORT_INFILL

## What

Two changes that together make the viewport render read as a coherent
frame from the first pass, on every render path:

1. **Rank-1 lattice coverage order** (`TILEPATHSAMPLER`, `RTPATHOCL`
   only): each pass walks a golden-ratio lattice over the whole film
   region instead of the old stride-R grid + Morton order inside R×R
   cells, which left R×R stale rectangles on screen for ~R² passes. Each
   pass lands `pixelCount / R²` maximally scattered real samples.

2. **`VIEWPORT_INFILL` imagepipeline plugin**: pixels with no accumulated
   sample weight yet are display-filled from the nearest covered pixels
   via a pull-push pyramid (classic hole-filling, GPU Gems II / gross2003).
   Filled pixels are replaced the moment a real sample lands — the film's
   own accumulation channels are never touched, so the displayed preview
   is honest reconstruction, not baked bias.

3. **RTPATHCPU first-frame**: the coarse first pass now writes a single
   pixel per sampled point instead of splatting a `zoomFactor×zoomFactor`
   block at fake weight — the infill produces a smoother reconstruction
   than the block splat did. `rtpathcpu.zoomphase.weight` semantics are
   preserved for the written pixel.

## Why

An artist editing a scene (camera orbit, material tweak, light move)
needs the *whole screen* to become visually correct immediately — not a
blocky checkerboard that takes dozens of passes to stop showing stale
rectangles. Sparse-but-scattered coverage + display-side reconstruction
is exactly what interactive path tracers (Stable Ray Tracing '17,
foveated PT, Blender Cycles adaptive preview) do: cover everything
coarsely first, then refine.

## How it works

### Lattice (kernel, `sampler_tilepath_funcs.cl`)

```
seqIdx = Σ activeCount(p<pass) + gid          // cumulative, ulong-safe
i      = seqIdx % pixelCount
epoch  = seqIdx / pixelCount
pix    = (i * 0x9E3779B1 + epoch) % pixelCount
```

`i*A mod N` is a rank-1 lattice: consecutive `i` land ~62% across the
frame, so every pass touches every screen region. When `gcd(A,N) > 1`
(e.g. 3 at 1080p) the lattice covers one residue coset per epoch; the
`epoch` offset rotates the coset so all pixels are reached within `gcd`
full cycles. The preview phase (`previewResolutionReduction`) and steady
phase (`resolutionReduction`) share one continuous `seqIdx` — coverage
continues seamlessly across the phase boundary. The old preview-phase
R×R block splat (weight 0.001) is removed: infill replaces it with real
reconstruction.

### Infill (`viewportinfill.cpp`)

- Coverage = any `RADIANCE_PER_PIXEL_NORMALIZED` weight > 0, or a
  non-zero `RADIANCE_PER_SCREEN_NORMALIZED` splat (light tracing /
  photonGI — works for BIDIR and LT paths too).
- Pull: premultiplied {r,g,b,w} pyramid, halving until 4 px.
- Push: each hole pixel walks up its ancestor column to the first
  covered texel and inherits its mean colour.
- Runs on the linear `IMAGEPIPELINE` buffer **before** tonemapping;
  CPU-only (pipelines that mix it with HW plugins ping-pong once —
  negligible at viewport sizes).
- All-valid films and fully-empty films are no-ops.

## Adapter

`VIEWPORT_INFILL` is injected as imagepipeline plugin 0 for the main
viewport pipeline only (`export/imagepipeline.py`), gated by
`scene.superluxcore.viewport.use_infill` ("Instant Coverage", default
on). AOV/output-switcher pipelines are not affected.

## Validation

`dev-tools/e51_viewport_infill.py` renders `scenes/cornell/cornell.scn`
with a tonemap-only pipeline and an infill pipeline side by side, reads
both at the first completed pass, and asserts:

- RTPATHOCL (Metal, 1280×720): raw coverage ≈ 11–16% → infill ≈ 88%,
  all 16/16 screen cells touched, finite, luminance-sane.
- PATHOCL + GPU light tracing, RTPATHCPU, BIDIRCPU: infill ≥ raw
  coverage (never loses data), finite, sane luminance ratio.

## Properties

| Property | Meaning |
|---|---|
| `film.imagepipelines.NNN.M.type = VIEWPORT_INFILL` | enable the plugin (no params) |
| `rtpath.resolutionreduction.preview` | preview-phase lattice stride (viewport default 8) |
| `rtpath.resolutionreduction` | steady-phase stride (viewport default 4) |
| `rtpathcpu.zoomphase.size` | CPU first-frame block size (unchanged semantics) |

## Limits / next steps

- Fill is nearest-ancestor only — no edge-awareness yet (depth/normal
  guided weights are a planned refinement).
- LT-only pixels count as covered and act as pull sources: a hot splat
  softens into a local blob ("light fog") rather than a single speckle.
  A dedicated screen-normalized display filter is planned (V-B).
- No priority/adaptive field yet (V-C): coverage order is uniform.
