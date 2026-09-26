# SuperBlendLuxCore — Blender adapter improvements

Status: implemented. Lives in the SuperBlendLuxCore repository; documented here
because the upstream criteria apply to the adapter too.

## What and why

The goal is near-Cycles coverage so existing Blender scenes render correctly:
more supported shader nodes, more object/volume types, and a simpler UX.

## Feature groups

| Area | Commits | What |
|---|---|---|
| Node coverage | `Cycles node reader: 36 -> 71`, `+TexBrick/+BsdfMetallic/+VectorCurve/+VolumeCoefficients`, `ParticleInfo`, `WhiteNoise`, `Hair Info`, `Blackbody linked temperature` | Auto-routes Blender node trees to the Cycles reader; ~70% of Blender shader nodes map to LuxCore equivalents. |
| Materials | `Principled v2 + Disney transmission + thin-film`, `Principled Hair BSDF` | First-class mappings, no mix hacks. |
| Objects | `VOLUME + POINTCLOUD export`, `Displacement output`, `dupli/instances`, Blender 5.2 export-bug fixes | More Blender object types export to LuxCore. |
| Volumes | `fire emission via flame/temperature grid`, `blackbody` | OpenVDB heterogeneous volumes; fire emission is now a true Planckian (blackbody), not a hand ramp. |
| UX | `Corona-style Quick Setup`, `auto-caustics`, `viewport black-flash/freeze fixes`, `error log`, `Metal GPU backend option`, `spectral render option` | Quality slider + denoise; fewer deadlocks/crashes; backend selection. |

## Validation

- Node exports produce LuxCore SDL that parses + renders.
- Blender 5.2 target; node coverage measured at 71/101 Cycles nodes.

## Platforms

All platforms LuxCore runs on; the Metal backend option is Apple-only.
