#line 2 "lightbvh_funcs.cl"

/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

// Light BVH traversal (Estevez & Kulla 2018, "Importance Sampling of
// Many Lights with Adaptive Tree Splitting"). CPU parity: the same
// math lives in LightStrategyLightBVH::NodeImportance /
// ::SampleLights / ::SampleLightPdf (lightbvh.cpp) - keep in sync.
//
// Importance bound at receiver (x, n) for node C:
//   I(C) = E_flat + E_local * (1/max(d^2(C,x),dmin^2))
//          * max(0, cos(max(0, <(n, toC) - thetaB)))
//          * max(0, cos(max(0, <(axis, -toC) - thetaO - thetaB)))
// where toC is the direction from x to the bbox center, thetaB the
// bbox bounding-cone half-angle seen from x (PI when x is inside the
// bbox), d^2 the squared distance from x to the bbox. The
// orientation and surface cos terms are the E&K'18 multiplicative
// bounds; volume receivers drop the surface term.

OPENCL_FORCE_INLINE float LightBVH_NodeImportance(
		__global const LightBVHNode* restrict node,
		const float3 x, const float3 n,
		const bool isVolume,
		const float minDist2) {
	float imp = node->energyFlat;
	if (node->energyLocal > 0.f) {
		const float3 bmin = VLOAD3F(&node->bboxMin.x);
		const float3 bmax = VLOAD3F(&node->bboxMax.x);
		const float3 c = 0.5f * (bmin + bmax);
		const float3 c2x = x - c;
		const float3 q = fmax(fmax(bmin - x, x - bmax), 0.f);
		const float d2 = dot(q, q);
		const float geo = 1.f / fmax(d2, minDist2);

		const float r2 = 0.25f * dot(bmax - bmin, bmax - bmin);
		const float dc2 = dot(c2x, c2x);
		float cosSurf = 1.f, cosOrient = 1.f;
		if ((d2 > 0.f) && (dc2 > r2)) {
			const float thetaB = asin(fmin(sqrt(r2 / dc2), 1.f));
			const float3 toC = -c2x / sqrt(dc2);
			if (!isVolume) {
				const float aN = acos(clamp(dot(n, toC), -1.f, 1.f));
				cosSurf = fmax(0.f, cos(fmax(0.f, aN - thetaB)));
			}
			const float3 axis = VLOAD3F(&node->axis.x);
			const float aO = acos(clamp(dot(axis, -toC), -1.f, 1.f));
			cosOrient = fmax(0.f, cos(fmax(0.f, aO - node->thetaO - thetaB)));
		}
		imp += node->energyLocal * geo * cosSurf * cosOrient;
	}
	return imp;
}

OPENCL_FORCE_INLINE uint LightBVH_SampleLights(
		__global const LightBVHNode* restrict nodes,
		const float3 x, const float3 n,
		const bool isVolume,
		const float minDist2,
		const float u0, float *pdf) {
	float u = u0;
	float pickPdf = 1.f;
	uint i = 0u;
	for (;;) {
		__global const LightBVHNode* restrict node = &nodes[i];
		if (node->flags & 1u) {
			*pdf = pickPdf;
			return node->u.lightIndex;
		}
		const float impL = LightBVH_NodeImportance(&nodes[i + 1u],
				x, n, isVolume, minDist2);
		const float impR = LightBVH_NodeImportance(&nodes[node->u.rightChildIndex],
				x, n, isVolume, minDist2);
		const float sum = impL + impR;
		// Both bounds can be 0 (e.g. fully backfacing clusters): the
		// branch choice still has to be sampled, keep it uniform so
		// the pdf stays the true selection probability
		const float pL = (sum > 0.f) ? (impL / sum) : 0.5f;
		if (u < pL) {
			pickPdf *= pL;
			u = (pL > 0.f) ? (u / pL) : 0.f;
			i = i + 1u;
		} else {
			const float pR = 1.f - pL;
			pickPdf *= pR;
			u = (pR > 0.f) ? ((u - pL) / pR) : 0.f;
			i = node->u.rightChildIndex;
		}
	}
}

OPENCL_FORCE_INLINE float LightBVH_SampleLightPdf(
		__global const LightBVHNode* restrict nodes,
		__global const uint* restrict lightToLeaf,
		const float3 x, const float3 n,
		const bool isVolume,
		const float minDist2,
		const uint lightIndex) {
	const uint leaf = lightToLeaf[lightIndex];
	if (leaf == LIGHTBVH_NULL_INDEX)
		return 0.f;
	float pickPdf = 1.f;
	uint i = 0u;
	for (;;) {
		__global const LightBVHNode* restrict node = &nodes[i];
		if (node->flags & 1u)
			return pickPdf;
		const float impL = LightBVH_NodeImportance(&nodes[i + 1u],
				x, n, isVolume, minDist2);
		const float impR = LightBVH_NodeImportance(&nodes[node->u.rightChildIndex],
				x, n, isVolume, minDist2);
		const float sum = impL + impR;
		const float pL = (sum > 0.f) ? (impL / sum) : 0.5f;
		if (leaf < node->u.rightChildIndex) {
			pickPdf *= pL;
			i = i + 1u;
		} else {
			pickPdf *= (1.f - pL);
			i = node->u.rightChildIndex;
		}
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
