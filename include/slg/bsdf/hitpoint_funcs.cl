#line 2 "hitpoint_funcs.cl"

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

// Used when hitting a surface
OPENCL_FORCE_INLINE void HitPoint_Init(__global HitPoint *hitPoint, const bool throughShadowTransp,
		const uint meshIndex, const uint triIndex,
		const float3 pnt, const float3 fixedDir,
		const float b1, const float b2,
		const float passThroughEvnt
		MATERIALS_PARAM_DECL) {
	hitPoint->throughShadowTransparency = throughShadowTransp;
	hitPoint->passThroughEvent = passThroughEvnt;

	VSTORE3F(pnt, &hitPoint->p.x);
	VSTORE3F(fixedDir, &hitPoint->fixedDir.x);

	hitPoint->objectID = sceneObjs[meshIndex].objectID;

	const bool isCurveHit = (triIndex & RAYHIT_CURVE_FLAG) != 0u;

	// Interpolate face normal (curve hits: round-tube normal from the hit
	// point and the Catmull-Rom centerline)
	float3 geometryN, interpolatedN;
	if (isCurveHit) {
		geometryN = Curve_GetNormal(&hitPoint->localToWorld, meshIndex, triIndex, pnt, b1 EXTMESH_PARAM);
		interpolatedN = geometryN;
	} else {
		geometryN = ExtMesh_GetGeometryNormal(&hitPoint->localToWorld, meshIndex, triIndex EXTMESH_PARAM);
		interpolatedN = ExtMesh_GetInterpolateNormal(&hitPoint->localToWorld, meshIndex, triIndex, b1, b2 EXTMESH_PARAM);
	}
	VSTORE3F(geometryN,  &hitPoint->geometryN.x);
	VSTORE3F(interpolatedN,  &hitPoint->interpolatedN.x);
	const float3 shadeN = interpolatedN;
	VSTORE3F(shadeN,  &hitPoint->shadeN.x);

	hitPoint->intoObject = (dot(-fixedDir, geometryN) < 0.f);

	// Interpolate UV coordinates (curve branch handled inside)
	const float2 defaultUV = ExtMesh_GetInterpolateUV(meshIndex, triIndex, b1, b2, 0 EXTMESH_PARAM);
	VSTORE2F(defaultUV, &hitPoint->defaultUV.u);

	hitPoint->meshIndex = meshIndex;
	hitPoint->triangleIndex = triIndex;
	hitPoint->triangleBariCoord1 = b1;
	hitPoint->triangleBariCoord2 = b2;

	// Compute geometry differentials
	float3 dndu, dndv, dpdu, dpdv;
	if (isCurveHit) {
		// For a round tube: dpdu ~ world-space strand tangent, dpdv ~
		// shading normal, normal curvature ignored (sufficient for hair).
		const float3 tObj = Curve_GetTangentObj(meshIndex, triIndex, b1 EXTMESH_PARAM);
		const float3 tWorld = (meshDescs[meshIndex].type == TYPE_EXT_TRIANGLE) ?
				tObj : Transform_ApplyVector(&hitPoint->localToWorld, tObj);
		dpdu = tWorld;
		dpdv = cross(shadeN, dpdu);
		dndu = ZERO;
		dndv = ZERO;
	} else {
		ExtMesh_GetDifferentials(
				&hitPoint->localToWorld,
				meshIndex,
				triIndex,
				shadeN, 0,
				&dpdu, &dpdv,
				&dndu, &dndv
				EXTMESH_PARAM);
	}
	VSTORE3F(dpdu, &hitPoint->dpdu.x);
	VSTORE3F(dpdv, &hitPoint->dpdv.x);
	VSTORE3F(dndu, &hitPoint->dndu.x);
	VSTORE3F(dndv, &hitPoint->dndv.x);
}

// Neutral spectral state: default bin wavelengths, all bins alive, hero 0.
// Scene_Intersect() overwrites these from the owning SampleResult on SLG_SPECTRAL builds.
OPENCL_FORCE_INLINE void HitPoint_InitSpectral(__global HitPoint *hitPoint) {
	hitPoint->spectralW[0] = SLG_SPECTRAL_START;
	hitPoint->spectralW[1] = SLG_SPECTRAL_START + SLG_SPECTRAL_BIN_WIDTH;
	hitPoint->spectralW[2] = SLG_SPECTRAL_START + 2.f * SLG_SPECTRAL_BIN_WIDTH;
	hitPoint->spectralHeroAlive = SLG_SW_DEFAULT;
	hitPoint->spectralEmissionEval = 0u;
}

// Initialize all fields
OPENCL_FORCE_INLINE void HitPoint_InitDefault(__global HitPoint *hitPoint) {
	hitPoint->meshIndex = NULL_INDEX;

	hitPoint->throughShadowTransparency = false;
	hitPoint->passThroughEvent = 0.f;

	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f), &hitPoint->p.x);
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f), &hitPoint->fixedDir.x);

	hitPoint->objectID = 0;
	
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f),  &hitPoint->geometryN.x);
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f),  &hitPoint->interpolatedN.x);
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f),  &hitPoint->shadeN.x);

	hitPoint->intoObject = true;

	VSTORE2F(MAKE_FLOAT2(0.f, 0.f), &hitPoint->defaultUV.u);
	
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f), &hitPoint->dpdu.x);
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f), &hitPoint->dpdv.x);
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f), &hitPoint->dndu.x);
	VSTORE3F(MAKE_FLOAT3(0.f, 0.f, 0.f), &hitPoint->dndv.x);

	Transform_Init(&hitPoint->localToWorld);

	hitPoint->interiorVolumeIndex = NULL_INDEX;
	hitPoint->exteriorVolumeIndex = NULL_INDEX;
	hitPoint->interiorIorTexIndex = NULL_INDEX;
	hitPoint->exteriorIorTexIndex = NULL_INDEX;

	HitPoint_InitSpectral(hitPoint);
}

OPENCL_FORCE_INLINE void HitPoint_GetFrame(__global const HitPoint *hitPoint, Frame *frame) {
	Frame_Set_Private(frame, VLOAD3F(&hitPoint->dpdu.x), VLOAD3F(&hitPoint->dpdv.x), VLOAD3F(&hitPoint->shadeN.x));
}

OPENCL_FORCE_INLINE float3 HitPoint_GetGeometryN(__global const HitPoint *hitPoint) {
	return (hitPoint->intoObject ? 1.f : -1.f) * VLOAD3F(&hitPoint->geometryN.x);
}

OPENCL_FORCE_INLINE float3 HitPoint_GetInterpolatedN(__global const HitPoint *hitPoint) {
	return (hitPoint->intoObject ? 1.f : -1.f) * VLOAD3F(&hitPoint->interpolatedN.x);
}

OPENCL_FORCE_INLINE float3 HitPoint_GetShadeN(__global const HitPoint *hitPoint) {
	return (hitPoint->intoObject ? 1.f : -1.f) * VLOAD3F(&hitPoint->shadeN.x);
}

OPENCL_FORCE_INLINE float2 HitPoint_GetUV(__global const HitPoint *hitPoint, const uint dataIndex EXTMESH_PARAM_DECL) {
	const uint meshIndex = hitPoint->meshIndex;

	if (meshIndex != NULL_INDEX) {
		return (dataIndex == 0) ?
			VLOAD2F(&hitPoint->defaultUV.u) :
			ExtMesh_GetInterpolateUV(meshIndex, hitPoint->triangleIndex, hitPoint->triangleBariCoord1, hitPoint->triangleBariCoord2, dataIndex EXTMESH_PARAM);
	} else
		return MAKE_FLOAT2(0.f, 0.f);
}

OPENCL_FORCE_INLINE float3 HitPoint_GetColor(__global const HitPoint *hitPoint, const uint dataIndex EXTMESH_PARAM_DECL) {
	const uint meshIndex = hitPoint->meshIndex;

	if (meshIndex != NULL_INDEX)
		return ExtMesh_GetInterpolateColor(meshIndex, hitPoint->triangleIndex, hitPoint->triangleBariCoord1, hitPoint->triangleBariCoord2, dataIndex EXTMESH_PARAM);
	else
		return WHITE;
}	

OPENCL_FORCE_INLINE float HitPoint_GetAlpha(__global const HitPoint *hitPoint, const uint dataIndex EXTMESH_PARAM_DECL) {
	const uint meshIndex = hitPoint->meshIndex;

	if (meshIndex != NULL_INDEX)
		return ExtMesh_GetInterpolateAlpha(meshIndex, hitPoint->triangleIndex, hitPoint->triangleBariCoord1, hitPoint->triangleBariCoord2, dataIndex EXTMESH_PARAM);
	else
		return 1.f;
}

OPENCL_FORCE_INLINE float HitPoint_GetVertexAOV(__global const HitPoint *hitPoint, const uint dataIndex EXTMESH_PARAM_DECL) {
	const uint meshIndex = hitPoint->meshIndex;

	if (meshIndex != NULL_INDEX)
		return ExtMesh_GetInterpolateVertexAOV(meshIndex, hitPoint->triangleIndex, hitPoint->triangleBariCoord1, hitPoint->triangleBariCoord2, dataIndex EXTMESH_PARAM);
	else
		return 0.f;
}

OPENCL_FORCE_INLINE float HitPoint_GetTriAOV(__global const HitPoint *hitPoint, const uint dataIndex EXTMESH_PARAM_DECL) {
	const uint meshIndex = hitPoint->meshIndex;

	if (meshIndex != NULL_INDEX)
		return ExtMesh_GetTriAOV(meshIndex, hitPoint->triangleIndex, dataIndex EXTMESH_PARAM);
	else
		return 0.f;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
