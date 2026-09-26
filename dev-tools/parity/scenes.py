# SPDX-License-Identifier: Apache-2.0
#
# Parity scene registry — one entry per feature-covered scene.
# Scene property strings are imported from the standalone regression
# tests so a scene edit there automatically flows into the matrix.
#
# Fields:
#   props / props_file  scene definition (inline text or repo-relative .scn)
#   engine / sampler    GPU engine + sampler under test
#   cfg_extra           feature flags appended to the base film config
#   spp                 samples per pixel (halt condition)
#   timeout             render timeout seconds (default 300)
#   expect.ratio_min    mean-luminance ratio GPU/CPU floor
#   expect.rmse_max     luminance RMSE ceiling
#   expect.black_max    dead-pixel share ceiling (structural check)

from e31_bevel_texture_parity import SCENE_PROPS as BEVEL_PROPS
from e34_volume_guiding_parity import SCENE_PROPS as VOLGUIDE_PROPS
from e35_tilepath_lighttracing import SCENE_PROPS as TILELT_PROPS
from e36_metropolis_lighttracing import SCENE_PROPS as CAUSTIC_PROPS
from builders import build_vertex_motion, build_strand_motion

LT_EXTRA = ("path.hybridbackforward.enable = 1\n"
        "path.lighttracing.enable = 1\n"
        "path.lighttracing.taskfraction = 0.5\n")

SCENES = {
    # Phase 1.2: tile-aware light splats + screen-channel clearing
    "tilepath_lt": {
        "props": TILELT_PROPS,
        "engine": "TILEPATHOCL",
        "sampler": "TILEPATHSAMPLER",
        "cfg_extra": LT_EXTRA + "tile.size.x = 160\ntile.size.y = 90\n",
        "spp": 96,
        # rmse_max relaxed vs the generic 0.05: the scene is a caustic
        # dominated by clipped highlights, so 96spp tile sampling leaves
        # ~0.07 luminance noise while energy (ratio) and structure
        # (black_frac) stay tight. Structural regressions still trip
        # well below this (the screen-channel leak scored rmse~0.39).
        "expect": {"ratio_min": 0.98, "rmse_max": 0.10, "black_max": 0.01},
    },
    # Phase 1.1/1.6: emitter coverage + METROPOLIS light tasks
    "caustic_lt_sobol": {
        "props": CAUSTIC_PROPS,
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "cfg_extra": LT_EXTRA,
        "spp": 96,
        "expect": {"ratio_min": 0.98, "rmse_max": 0.05, "black_max": 0.01},
    },
    "caustic_lt_metropolis": {
        "props": CAUSTIC_PROPS,
        "engine": "PATHOCL",
        "sampler": "METROPOLIS",
        "cfg_extra": LT_EXTRA,
        "spp": 128,
        # Metropolis chains differ across implementations: looser RMSE
        "expect": {"ratio_min": 0.97, "rmse_max": 0.08, "black_max": 0.01},
    },
    # Phase 1.3/1.5: volume guiding gate + record fidelity
    "volume_guiding": {
        "props": VOLGUIDE_PROPS,
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "cfg_extra": "path.guiding.enable = 1\n",
        "spp": 96,
        "timeout": 420,
        "expect": {"ratio_min": 0.97, "rmse_max": 0.05, "black_max": 0.01},
    },
    # Phase 0.3: BEVEL_TEX parser + GPU eval
    "bevel_tex": {
        "props": BEVEL_PROPS,
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "spp": 64,
        "expect": {"ratio_min": 0.98, "rmse_max": 0.05, "black_max": 0.01},
    },
    # Phase 0.2: Disney spectral path
    "disney_spectral": {
        "props_file": "scenes/cornell/cornell-disney.scn",
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "cfg_extra": "path.spectral.enable = 1\n",
        "spp": 64,
        "expect": {"ratio_min": 0.97, "rmse_max": 0.06, "black_max": 0.01},
    },
    # Multi-bounce GI: ReSTIR GI first-bounce reservoir (depth-0 gate is
    # identical on CPU/GPU - see plan 1.4)
    "restir_gi": {
        "props_file": "scenes/cornell/cornell.scn",
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "cfg_extra": "path.restir.gi.enable = 1\n",
        "spp": 96,
        "expect": {"ratio_min": 0.97, "rmse_max": 0.05,
                "black_max": 0.01},
    },
    # Constrained glossy indirect: guiding field parity (coarse table vs
    # SD-tree - different fields, same expected image)
    "glossy_guiding": {
        "props_file": "scenes/cornell/pg-indirect.scn",
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "cfg_extra": "path.guiding.enable = 1\n",
        "spp": 96,
        # rmse_max relaxed: guided glossy convergence noise is uniform
        # across the frame (no structural diff pattern), ~0.06 at 96spp.
        "expect": {"ratio_min": 0.97, "rmse_max": 0.08,
                "black_max": 0.01},
    },
    # Vertex motion + shared-mesh instancing across accel paths
    "vertex_motion": {
        "builder": build_vertex_motion,
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "spp": 64,
        # Motion-blur coverage is position-dependent: wider bands
        "expect": {"ratio_min": 0.95, "ratio_max": 1.10,
                "rmse_max": 0.08, "black_max": 0.02},
    },
    # Hair/curve: strands with deformation motion ('solid' tessellation
    # = same primitive on CPU/OpenCL). The strands are emissive, so the
    # scene marks the mesh as a light source and Metal disables its
    # native curve primitives (SetCurvePrimitivesEnabled): light
    # sampling draws points on the tessellation and the round tube
    # would self-occlude shadow rays (~20% energy loss, fixed).
    # Non-emissive strands keep the curve fast path.
    "strand_motion": {
        "builder": build_strand_motion,
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "spp": 64,
        "expect": {"ratio_min": 0.95, "ratio_max": 1.10,
                "rmse_max": 0.08, "black_max": 0.02},
    },
    # Same scene with curve primitives forced off by env: covers the
    # LUXRAYS_METAL_CURVES=0 override path itself.
    "strand_motion_tris": {
        "builder": build_strand_motion,
        "engine": "PATHOCL",
        "sampler": "SOBOL",
        "env": {"LUXRAYS_METAL_CURVES": "0"},
        "spp": 64,
        "expect": {"ratio_min": 0.95, "ratio_max": 1.10,
                "rmse_max": 0.08, "black_max": 0.02},
    },
}
