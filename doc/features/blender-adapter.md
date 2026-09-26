# SuperBlendLuxCore — Blender adapter improvements

Status: implemented, actively developed. Lives in the SuperBlendLuxCore
repository; documented here because the upstream criteria apply to the
adapter too. Target: **Blender 5.2.x LTS** (installed: 5.2.1, Python 3.13).

## What and why

The goal is near-Cycles coverage so existing Blender scenes render correctly:
more supported shader nodes, more object/volume types, and a simpler UX.

## Feature groups

| Area | Commits / notes | What |
|---|---|---|
| Node coverage | Cycles reader auto-routes native Blender node trees; **42 mapped + 32 approx of 102 registered `ShaderNode*` types** (8 warn + neutral fallback rather than silent black; world/light trees handled on separate paths) — see `SuperBlendLuxCore/doc/cycles_node_coverage.md` | ~73% functional coverage incl. approximations; mathfunc/gabor/squeeze/VectorMath/LightPath(rayinfo)/IES(iesblob)/GN named-attr included |
| Cycles/Eevee compat | `401ae38` compat layer | Existing Cycles node trees re-interpreted without rebuild |
| Materials | Principled v2 + Disney transmission + thin-film, **OpenPBR material node** (S1), **Hair node with Chiang/Huang select** (`5517f8bf`) | First-class mappings, no mix hacks |
| Objects | VOLUME(OpenVDB)/POINTCLOUD/displacement/dupli/instances, **`.lxm` mesh proxy + auto-proxy** (`superluxcore.bake_lxm_proxy`, `config.proxy_auto*`, cluster stride UI `e35043ad`) | Evaluated-geometry bake, demand-paged .lxm load |
| Volumes | fire emission via flame/temperature grid, blackbody | OpenVDB heterogeneous volumes; fire emission is a true Planckian |
| Render features UI | GPU light tracing + LT-only, caustic focus/MGE, adaptive caustics, ReSTIR GI/visibility toggles, **vertex connection controls** (`fa10122e`/`d764044e`), **P5 path-guiding controls** (`d0711457`), **adaptive robust clamping** (`644e34f0`), **Vulkan backend + device select** (`2919ba87`) | Artist-first panels; engine features exposed promptly |
| View layer / AOV | Cryptomatte toggles (`082a2854`), **LPE outputs** (`8c8b0548`), light-linking UI + instanced coverage (`a2ddf8cd`/`dfe8be83`/`c256ac89`), view-layer flags + opt-in halt overrides (`d0155949`), temporal accumulation panel | LPE: `film.lpe.N.*`; Cryptomatte: object/material |
| UX | Quick Setup 2.0 + scene-aware auto config (`884b95fa`/`6216356a`), auto light strategy/clamp/device, low-VRAM profile, quality presets, convergence stat row | Post-restore property trap documented in `SuperBlendLuxCore/doc/engineering/render-session.md` |
| Viewport | Async session worker, edit pacing, hold-last-frame, interactive OIDN, **runtime resolution reduction (~26 ms apply→sample)**, external-process render | depth-less reprojection reverted |
| Branding | `387883c1` extension/engine rebrand to SuperLuxCore, `d28ec167` smoke test | `extensions/user_default/superluxcore`, `pysuperluxcore` wheel |

## Validation

- Node exports produce LuxCore SDL that parses + renders.
- `dev-tools/cycles_node_coverage_test.py` (non-rendering audit) and
  `dev-tools/e23_cycles_compat_e2e_test.py` (headless E2E render).
- `dev-tools/lxmv4_test.py`, `cluster_residency_test.py`,
  `imagemap_stream_test.py`, `puregpu_spill_test.py` for the
  proxy/out-of-core paths.
- Blender 5.2.1 LTS target; node coverage: 42 mapped + 32 approx +
  8 warn-tier of 102 registered node types (see
  `doc/cycles_node_coverage.md` for the denominator definition).

## Platforms

All platforms LuxCore runs on; the Metal backend option is Apple-only;
the Vulkan backend renders PATHOCL end-to-end via MoltenVK (experimental
— native-driver validation pending, see `vulkan-backend-assessment.md`).
