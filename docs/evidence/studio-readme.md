# Photographic showcase

All thirteen previously authored showcase scenes and their renders have been
replaced. The new scenes use **32 credited CC0 Poly Haven model, PBR texture and
HDRI families**, including photographed food, detailed products, furniture,
plants, ceramics, wood, plaster and linen. Source creators, licence, download
URLs and exact file fingerprints are in `studio-props.json`.
[The research brief](photoreal-research.md) documents the primary sources and
how their guidance was applied. These are actual native renderer images.

## Image construction

Blender 5.2.1 stages and exports geometry in metres; it does not render the beauty
images. SuperLuxCore 2.11.27 performs the native CPU and Apple Metal renders;
each manifest record identifies its actual backend. Original UVs and
separate colour, linear roughness/metal, normal and opacity maps are retained.
Opaque assets use OpenPBR, except the three upstream comparison scenes, which
use shared Disney materials. Glass uses actual transmission and, where present,
an absorbing interior. Small food/porcelain scattering weights are art directed;
this exporter is a scoped showcase translation, not universal Cycles coverage.

Photographic HDRIs are combined with placed windows and reflection strips.
Cameras, edge bevels, grounded props and focused compositions are reviewed from
native outputs. The wool is a real 50,000-strand curled groom on a lounge chair,
aligned to the seat surface. Ceiling-fan blur comes from time-varying native
object transforms. Pool water uses bulk refraction. Soap-film bubbles use the native archglass thin-shell approximation with interference. Wine packaging retains the scanned geometry and printed labels with an intentionally opaque dark lacquer finish. This image does not demonstrate bulk bottle transmission or wine absorption.

Beauty and raw beauty use the installed Blender OCIO **ACES 2.0** display view,
with a fixed recorded exposure for each scene and no per-image normalisation.
Native OIDN uses actual albedo and average shading normal auxiliaries. The raw
beauty view has no denoising. Albedo, normal and Cryptomatte views are actual
technical outputs, shown without the beauty display transform.

The flint prism uses Metal RGB wavelength-sampled dispersion with the N-SF6
Sellmeier model. This is the renderer's RGB dispersion illustration, not a
validated spectral-transport beauty. A candidate spectral output with a visible
green colour cast was rejected during review. The CPU pool scene uses hybrid
eye/light transport. The wine editorial uses Metal. No variance clamp,
painted caustic, artificial bloom or beauty retouching is applied. Periodic noise
estimation is disabled; final denoising runs after a completed sample target.
Exact dimensions, targets, observations, native fingerprints and display settings
are recorded in `provenance.json`. Render times are provenance, not benchmarks.

## Reproduce

Use Python 3.13, NumPy, Pillow and OpenColorIO 2.6. Extract the official native
wheel into a library directory. Library, wheel and OCIO fingerprints are in the
published manifest. Start with an empty work directory and download the assets:

```sh
python3.13 studio-assets.py --work /tmp/slc-showcase
/Applications/Blender.app/Contents/MacOS/Blender --background --factory-startup \
  --python-exit-code 1 --python studio-scenes.py -- --work /tmp/slc-showcase/scenes
python3.13 studio-render.py music --work /tmp/slc-showcase/scenes \
  --lib /path/to/extracted/fork-wheel --out /tmp/slc-showcase/music.jpg \
  --width 1920 --height 1200 --spp 512 --exposure .7 --threads 10
python3.13 studio-render.py music --work /tmp/slc-showcase/scenes \
  --lib /path/to/extracted/upstream-wheel --engine upstream --variant baseline \
  --out /tmp/slc-showcase/music-baseline.jpg \
  --width 1920 --height 1200 --spp 512 --exposure .7 --threads 10
```

Use each manifest record's backend, dimensions, sample count and exposure for the other
images. Asset and generated geometry directories are working data, not website
payload. Downloaded assets are fingerprinted before use. Published close-ups
apply identical crops within a pair, with no additional grading.

## Upstream comparisons

The reference is the official LuxCoreRender **2.11.2 fork baseline**, not a claim
about every later upstream development.

- **CD:** upstream silver Metal2 approximation versus the added diffraction
  material. This baseline has no equivalent diffraction grating shader. Spectral
  transport is disabled in both images; this demonstrates the added material.
- **Brass:** the same textured Disney vessels at roughness 0.98. Multiple-scattering
  compensation is enabled in the fork and disabled in the baseline.
- **Wool:** identical groom geometry with the added Huang fibre model versus a
  labelled upstream Glossy2 surface approximation. This is a material alternative;
  it is not a claim about the best artistic setup possible in the upstream renderer.

Camera, geometry, lights, sampler, seed, target SPP, exposure and display view
match within each CPU pair. Only the documented material overrides differ. The
upstream macOS library has optional OpenCL discovery disabled in memory to avoid
an unsupported discovery path; its native bytes are unchanged.

These are artist-directed CPU and Metal examples. They do not establish performance,
blanket GPU parity, universal Cycles compatibility or isolated validation of
every feature. The website labels image context and actual technical passes.
