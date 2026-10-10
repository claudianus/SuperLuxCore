// SPDX-License-Identifier: Apache-2.0
#ifndef SLG_CYCLES_BSSRDF_BOUNDARY_H
#define SLG_CYCLES_BSSRDF_BOUNDARY_H
#include "luxrays/core/geometry/vector.h"

namespace slg { namespace cyclesbssrdfboundary {
// Compile the same directional sampler/PDF implementation as the GPU. Keep
// the former CPU sampler's double PI multiplication for its RNG mapping.
#define OPENCL_FORCE_INLINE inline
#define float3 luxrays::Vector
#define MAKE_FLOAT3(x, y, z) luxrays::Vector(x, y, z)
#define normalize luxrays::Normalize
#define dot luxrays::Dot
#define cross luxrays::Cross
#define sqrt sqrtf
#define cos cosf
#define sin sinf
#define fabs fabsf
#define fmax fmaxf
#define isfinite std::isfinite
#define CYCLES_BSSRDF_BOUNDARY_PI M_PI
#include "slg/materials/cyclesbssrdf_boundary.cl"
#undef CYCLES_BSSRDF_BOUNDARY_PI
#undef isfinite
#undef fmax
#undef fabs
#undef sin
#undef cos
#undef sqrt
#undef cross
#undef dot
#undef normalize
#undef MAKE_FLOAT3
#undef float3
#undef OPENCL_FORCE_INLINE
} }
#endif
