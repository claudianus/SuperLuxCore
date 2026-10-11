# Photographic showcase: research and applied art direction

Research date: 2026-10-11. This is a production brief, not a claim that a renderer or a larger sample count automatically guarantees photorealism. Final images must be reviewed at their actual rendered size.

## Sources and decisions

- **Material values and texture semantics.** [Adobe's PBR Guide, Part 2](https://www.adobe.com/learn/substance-3d-designer/web/the-pbr-guide-part-2) explains the distinction between colour, metal reflectance, roughness and linear data maps. Apply the original colour/roughness/metal/normal maps independently. Do not multiply baked ambient occlusion into the albedo or increase all reflections to make an image look expensive.
- **Layered material construction.** [Adobe's OpenPBR artist guide](https://experienceleague.adobe.com/en/docs/substance-3d/general-knowledge/openpbr/openpbr-overview) provides an artist-facing description of the material layers. Use the existing SuperLuxCore OpenPBR material for opaque PBR assets, including the actual scattering interior when the source material calls for it. This workflow is a scoped material translation, not proof of universal Cycles compatibility.
- **Surface detail and roughness.** [Physically Based Rendering, fourth edition: Microfacet Theory](https://pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory) connects microgeometry to reflection. Keep scanned roughness and tangent-space normal maps. Avoid uniformly smooth plastic surfaces, oversized bump and fake metallic dirt.
- **Photographic capture quality.** [Poly Haven's texture standards](https://docs.polyhaven.com/en/technical-standards/textures) describe photograph-derived texture acquisition. [Its model standards](https://docs.polyhaven.com/en/technical-standards/models) describe production asset requirements. Select finished assets with credible silhouettes, joins, labels and surface wear rather than substituting primitive geometry for a recognisable product.
- **Metric scale.** [Poly Haven's Blender workflow](https://docs.polyhaven.com/en/guides/blender-addon) and [its dimensions implementation](https://blog.polyhaven.com/dev-log-09/) explain measured texture dimensions. Preserve model proportions and record every scale change. Place objects in metres. Check contact after rendering; a bounding box alone does not guarantee believable contact.
- **Lighting and reflections.** [Lightmap's HDRI lighting guide](https://www.lightmap.co.uk/blog/howdoeshdrilightingwork/) explains captured illumination and the limits of environment-only lighting. Combine photographic HDRIs with placed softboxes or windows. Design the reflection seen by the camera, use narrow strips to define glass and metal edges, and retain shadow contrast with negative fill.
- **Translucent surfaces.** [Pixar RenderMan's photorealistic head tutorial](https://prod.renderman.pixar.com/photorealistic-head) shows why colour, scattering scale, specular response and small-scale detail must be considered together. Apply the principle to scanned food and porcelain without claiming that a skin recipe transfers numerically to these materials.
- **Camera.** [Blender's camera documentation](https://docs.blender.org/manual/en/4.4/render/cameras.html) explains perspective and focus controls. Use a deliberate camera height and distance, moderate depth of field, and a clear focal subject. Depth of field cannot repair poor geometry or material mapping.
- **Reuse rights.** [Poly Haven's licence](https://polyhaven.com/license) grants CC0 use for the downloaded assets. Preserve each creator, source URL and exact file fingerprint in `studio-props.json`; do not imply that the asset modelling was authored by SuperLuxCore.

## Scene brief

Every earlier showcase scene and its render has been discarded. The replacement uses a coherent photographic setting for each subject:

| Scene ID | New subject | Surface and lighting focus |
|---|---|---|
| beauty | Breakfast still life | Scanned pastry, porcelain, woven linen, coffee, a large side window |
| perfume | Wine editorial | Scanned wine packaging, opaque polished lacquer, fruit and controlled reflections |
| audio | Brass still life | Engraved vessels, textured wood and rough-conductor compensation |
| music | Analogue listening desk | Scanned electronics and a physically proportioned 120 mm diffraction CD |
| interior | Afternoon living room | Upholstery, scanned plants, timber floor, wall and skirting details |
| textile | Sheepskin and leather | Real lounge chair with a short curled native wool groom |
| jewelry | The navigator's desk | Engraved compass, spectacles, linen and worn paper |
| pearl | Porcelain and soap film | Glazed ceramic forms and actual thin-film dielectric bubbles |
| architecture | Shaded terrace | Metric architecture, photographed materials, furnished seating and real water |
| atmosphere | A quiet reading room | Furnished interior, window structure and participating dust medium |
| heritage | Analogue camera editorial | Detailed heritage camera, paper, spectacles and cassette |
| fan | Ceiling-fan motion study | Real wooden ceiling fan with native motion transforms |
| prism | Optical desk | Precision flint glass, a measured Sellmeier model and narrow reflection sources |

## Quality gates

1. Review an actual native 1280-pixel or larger representative image before full-resolution batching.
2. Reject floating/intersecting props, implausible scale, missing maps, excessive bump, undirected highlights, black glass and unrecognisable subject silhouettes.
3. Inspect both raw and native-denoised output. A smooth denoised image alone is insufficient.
4. Display beauty with OCIO ACES 2.0 from the installed Blender configuration; record the exposure and configuration fingerprint. Do not normalise each image independently.
5. Render comparisons from the same scene, framing, exposure, sample target and light setup. State when upstream has no equivalent material. Do not use spectral on/off as a proxy for the new CD shader.
6. Publish only completed, inspected renders. Keep the actual renderer, settings, dimensions and asset credits in a reproducible manifest.

The examples are artist-directed CPU/Metal artwork, not performance benchmarks, blanket backend parity evidence, or a claim that every feature is demonstrated by a separate isolated experiment.
