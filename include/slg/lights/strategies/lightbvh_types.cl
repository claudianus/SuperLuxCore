#line 2 "lightbvh_types.cl"

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

// Light BVH node (Estevez & Kulla 2018 style): implicit-layout binary
// tree over the light list. Internal nodes keep an oriented bounding
// cone (axis + half-angle thetaO) over the emission directions of their
// lights plus a tight bbox for the geometric attenuation bound; leaves
// keep one light index each.
//
// Emission directionalities bound by the cone (per class):
//   spot/projection/laser: cone axis + cone theta
//   triangle (single-sided): triangle normal, theta = PI/2
//   point/map-point, sun/distant, infinite/env: omni -> theta = PI
//   (delta and infinite lights are stored as flat leaves: their
//   clustering bounds degenerate, see LightStrategyLightBVH::Preprocess)
//
// Layout: node 0 = root; internal node i has the left subtree in
// [i+1, i+1+2*lcL) and the right subtree starting at rightChildIndex.
// nodeCount = 2*lightCount-1, leaf count = lightCount.

#define LIGHTBVH_NULL_INDEX 0xffffffffu

typedef struct {
	// World-space bbox of the clustered lights (flat leaves: the
	// light's own extent; infinite/delta lights: their anchor point)
	Point bboxMin, bboxMax;
	// Bounding-cone axis (emission orientation bound)
	Vector axis;
	float thetaO;       // bounding-cone half-angle (flat leaves: 0)
	float energyFlat;   // total power of flat leaves below
	float energyLocal;  // total power of local (clusterable) lights below
	union {
		unsigned int rightChildIndex; // internal: right subtree start
		unsigned int lightIndex;      // leaf: lightDefs index
	} u;
	// bit0 = leaf, bits[8:31] = leafCount-1 (internal only, 0 => 1 light
	// per subtree side impossible by construction: leafCount >= 2 for
	// internals)
	unsigned int flags;
} LightBVHNode;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
