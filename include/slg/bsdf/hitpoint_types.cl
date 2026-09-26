#line 2 "hitpoint_types.cl"

/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
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

typedef struct {
	// The incoming direction. It is the eyeDir when fromLight = false and
	// lightDir when fromLight = true
	Vector fixedDir;
	Point p;
	Normal geometryN;
	Normal interpolatedN;
	Normal shadeN;

	UV defaultUV;

	// Note: dpdu and dpdv are orthogonal to shading normal (i.e not geometry normal)
	Vector dpdu, dpdv;
	Normal dndu, dndv;

	// Mesh information
	unsigned int meshIndex;
	unsigned int triangleIndex;
	float triangleBariCoord1, triangleBariCoord2;

	// passThroughEvent can be stored here in a path state even before of
	// BSDF initialization (while tracing the next path vertex ray)
	float passThroughEvent;

	// Transformation from local object to world reference frame
	Transform localToWorld;

	// Interior and exterior volume (this includes volume priority system
	// computation and scene default world volume)
	unsigned int interiorVolumeIndex, exteriorVolumeIndex;
	// Material code (i.e. glass, etc.) doesn't have access to materials list
	// so I use HitPoint to carry texture index information
	unsigned int interiorIorTexIndex, exteriorIorTexIndex;

	unsigned int objectID;

	int intoObject, throughShadowTransparency;

	// Hero-wavelength spectral state (SLG_SPECTRAL builds), copied from the
	// owning SampleResult by Scene_Intersect: texture leaf upsampling and
	// dispersive material events read it here.
	float spectralW[3];
	unsigned int spectralHeroAlive;
	// Transient flag: non-zero while an emission-context texture graph is
	// being evaluated (Material_GetEmittedRadiance sets it around the
	// emittedTex eval) so leaf RGB producers pick the illuminant basis.
	unsigned int spectralEmissionEval;

	// The context of the ray that generated this hit point. It is read by
	// the "rayinfo" texture to implement ray-dependent shading (i.e. the
	// Cycles LightPath node). Scene_Intersect() fills it for every BSDF
	// it creates; HitPoints initialized outside of a ray-traced path
	// (light source sampling, volume internals, utilities, ...) keep the
	// zero defaults, which decode as a ray with no event, depth 0 and
	// length 0.
	int rayEvent;
	// The SceneRayType bits of the incoming ray (i.e. CAMERA_RAY,
	// SHADOW_RAY, ...). 0 when the context is unknown.
	unsigned int rayFlags;
	// The number of bounces before the ray and the per-event counters
	unsigned int rayDepth;
	unsigned int rayDiffuseDepth;
	unsigned int rayGlossyDepth;
	unsigned int raySpecularDepth;
	// Number of transmission events and of transparent surfaces crossed
	// along the path before this hit (Cycles LightPath "Transmission
	// Depth" and "Transparent Depth")
	unsigned int rayTransmissionDepth;
	unsigned int rayTransparentDepth;
	// The length of the incoming ray segment
	float rayLength;
} HitPoint;
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
