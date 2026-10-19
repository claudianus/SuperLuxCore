// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_SSS_BOUNDARY_GUARD_H
#define _SLG_SSS_BOUNDARY_GUARD_H
#include "luxrays/utils/utils.h"
namespace slg {
// Keep both comparisons in the same metric. An unclamped grazing cosine in
// the test followed by a clamped cosine in the displacement can move a valid
// interior scatter FORWARD toward a distant surface instead of away from it.
inline float SSSGuardedScatter(const float scatter, const float boundary,
        const float cosine, const float epsilon, const float mint) {
    const float guard = 8.f * epsilon;
    const float cosHit = luxrays::Max(cosine, 1e-3f);
    return (boundary - scatter) * cosHit < guard ?
        luxrays::Max(mint, boundary - guard / cosHit) : scatter;
}
}
#endif
