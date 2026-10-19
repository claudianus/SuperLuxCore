/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _SLG_DENOISING_GUIDE_H
#define _SLG_DENOISING_GUIDE_H

#include <cstddef>

namespace slg {

// Version 1 renderer-owned per-eye-task record. Scalar packing is shared with
// pathoclbase_funcs.cl; no host float3 padding or SampleResult/task ABI change.
// [0:4] diffuse RGB, roughness; [4:8] specular RGB, status;
// [8:12] camera normal XYZ, sample weight; [12:16] primary world XYZ, camera Z;
// [16:20] tile-local pixel XY, ray time, reserved;
// Independent RTPATH frame guide_version=2 additionally owns:
// [19] = 2; [20:24] deferred feature RGB/active; [24:28] motion point/kind
// (0 none, 1 finite reflected virtual point, 2 primary environment direction,
// 3 reflected environment direction); [28:31] first shading normal;
// [31] first motion-relevant sampled event (0 none, 1 reflection with
// nonzero specular feature, 2 transmission).
// Diffuse/normal and depth follow the actual path; specular/roughness remain
// primary-boundary features. Physical path termination completes its sampled
// feature estimator; actually encountered unsupported vertices stay rejected.
// Independent-frame status5 is an exact primary holdout: zero current-frame
// radiance/alpha with its real first-hit position and normal. Status2 remains
// unsupported; an old consumer rejects the additive status5 safely.
constexpr size_t DENOISING_GUIDE_STRIDE = 32;

}
#endif
