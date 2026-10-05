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

#include "slg/bsdf/hitpoint.h"
#include "slg/scene/scene.h"
#include "slg/scene/sceneobjectdefs.h"
#include "slg/utils/pathdepthinfo.h"

using namespace luxrays;
using namespace slg;
using namespace std;

// Used when hitting a surface
//
// Note: very important, this method assume localToWorld file has been _already_
// initialized. This is done for performance reasons. 
//
// Note: This is also _not_ initializing volume related information.

void HitPoint::Init(const bool fixedFromLight, const bool throughShadowTransp,
		SceneConstRef scene, SceneObjectConstRef sceneObject,
		const u_int triIndex,
		const Point &pnt, const Vector &dir,
		const float b1, const float b2,
		const float passThroughEvnt) {
	fromLight = fixedFromLight;
	throughShadowTransparency = throughShadowTransp;
	passThroughEvent = passThroughEvnt;

	p = pnt;
	fixedDir = dir;

	// scene object is already fetched by BSDF::Init
	objectID = sceneObject.GetID();

	// Mesh information
	mesh = &sceneObject.GetExtMesh();
	triangleIndex = triIndex;
	triangleBariCoord1 = b1;
	triangleBariCoord2 = b2;

	// Interpolate face normal and (unless this is a throughShadow hit)
	// geometry differentials. Both reuse the same triangle vertex
	// normals; GetShadingInfo shares the fetch on meshes with the
	// per-triangle cache and applies the same degenerate fallback as
	// the separate InterpolateTriNormal (geometryN when the
	// interpolated normal is non-finite).
	geometryN = mesh->GetGeometryNormal(localToWorld, triangleIndex);
	// Non-finite geometry (NaN vertices, degenerate transforms) can also
	// produce a NaN geometric normal; last resort: face the ray
	const float gnl2 = Dot(geometryN, geometryN);
	if (!isfinite(gnl2) || (gnl2 < 1e-20f))
		geometryN = Normal(-fixedDir.x, -fixedDir.y, -fixedDir.z);
	if (throughShadowTransparency) {
		// No differentials needed: keep the cheap canonical frame and
		// skip the fused fetch - the UV interpolation still fetches the
		// layer-0 corners once.
		interpolatedN = mesh->InterpolateTriNormal(localToWorld, triangleIndex, b1, b2);
		const float inl2 = Dot(interpolatedN, interpolatedN);
		if (!isfinite(inl2) || (inl2 < 1e-20f))
			interpolatedN = geometryN;
		shadeN = interpolatedN;
		intoObject = (Dot(-fixedDir, geometryN) < 0.f);
		defaultUV = mesh->InterpolateTriUV(triangleIndex, b1, b2, 0);
		CoordinateSystem(Vector(shadeN), &dpdu, &dpdv);
		dndu = Normal();
		dndv = Normal();
	} else {
		mesh->GetShadingInfo(localToWorld, triangleIndex, geometryN, 0,
				&interpolatedN, &dpdu, &dpdv, &dndu, &dndv,
				b1, b2, &defaultUV);
		shadeN = interpolatedN;
		intoObject = (Dot(-fixedDir, geometryN) < 0.f);
	}

	// Note: I'm not initializing volume related information here
	interiorVolume = nullptr;
	exteriorVolume = nullptr;

	// The ray context is set later by Scene::Intersect()
	SetRayContext(0, NONE, nullptr, 0.f);
}

void HitPoint::Init(const bool fixedFromLight, const bool throughShadowTransp,
		SceneConstRef scene, const u_int meshIndex,
		const u_int triIndex,
		const Point &pnt, const Vector &dir,
		const float b1, const float b2,
		const float passThroughEvnt) {
	Init(fixedFromLight, throughShadowTransp, scene,
			scene.GetObjects().GetSceneObject(meshIndex),
			triIndex, pnt, dir, b1, b2, passThroughEvnt);
}

// Initialize all fields (i.e. the one missing a default constructor)
void HitPoint::Init() {
	mesh = nullptr;

	passThroughEvent = 0.f;
	interiorVolume = nullptr;
	exteriorVolume = nullptr;
	objectID = 0;
	fromLight = false;
	intoObject = true;
	throughShadowTransparency = false;

	SetRayContext(0, NONE, nullptr, 0.f);
}

void HitPoint::SetRayContext(const u_int rayType, const BSDFEvent event,
		const PathDepthInfo *depthInfo, const float length,
		const float viewDepth) {
	rayEvent = event;
	rayFlags = rayType;
	rayDepth = depthInfo ? depthInfo->depth : 0;
	rayDiffuseDepth = depthInfo ? depthInfo->diffuseDepth : 0;
	rayGlossyDepth = depthInfo ? depthInfo->glossyDepth : 0;
	raySpecularDepth = depthInfo ? depthInfo->specularDepth : 0;
	rayTransmissionDepth = depthInfo ? depthInfo->transmitDepth : 0;
	rayTransparentDepth = depthInfo ? depthInfo->transparentDepth : 0;
	rayLength = length;
	rayViewDepth = viewDepth;
	// PSR: the engine seeds PathDepthInfo::regularization at path init;
	// gate it per vertex so first-bounce shading stays exact
	// Only after a non-specular event (Kaplanyan & Dachsbacher): a pure
	// specular chain from the camera (looking through glass) stays sharp;
	// regularizing it blurred every view through a window or a wine glass
	regularization = (depthInfo && (depthInfo->depth >= depthInfo->regularizationMinDepth) &&
			(depthInfo->diffuseDepth + depthInfo->glossyDepth + depthInfo->volumeDepth > 0)) ?
			depthInfo->regularization : 0.f;
	// Cycles Filter Glossy (surface_shader_prepare_closures): after a
	// low-pdf bounce every microfacet lobe gets an alpha floor of
	// sqrt(1 - blur_pdf) / 2, blur_pdf = min path pdf / blur_glossy.
	// Stored negative: RegularizeAlpha takes max(alpha, -reg).
	if (depthInfo && (depthInfo->filterGlossy > 0.f)) {
		const float blurPdf = depthInfo->filterGlossy * depthInfo->minRayPdf;
		if (blurPdf < 1.f)
			regularization = -sqrtf(1.f - blurPdf) * .5f;
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4

void HitPoint::Init(const bool fixedFromLight, const bool throughShadowTransp,
	SceneConstRef scene, SceneObjectConstRef sceneObject,
	const u_int triIndex,
	const luxrays::Point &pnt, const luxrays::Vector &dir,
	const luxrays::Normal &geoN,
	const float b1, const float b2,
	const float passThroughEvnt) {
	fromLight = fixedFromLight;
	throughShadowTransparency = throughShadowTransp;
	passThroughEvent = passThroughEvnt;

	p = pnt;
	fixedDir = dir;

	objectID = sceneObject.GetID();
	mesh = &sceneObject.GetExtMesh();
	triangleIndex = triIndex;
	triangleBariCoord1 = b1;
	triangleBariCoord2 = b2;

	// Caller already fetched the geometric normal (for fixedDir) - reuse it.
	geometryN = geoN;
	const float gnl2 = Dot(geometryN, geometryN);
	if (!isfinite(gnl2) || (gnl2 < 1e-20f))
		geometryN = Normal(-fixedDir.x, -fixedDir.y, -fixedDir.z);
	if (throughShadowTransparency) {
		interpolatedN = mesh->InterpolateTriNormal(localToWorld, triangleIndex, b1, b2);
		const float inl2 = Dot(interpolatedN, interpolatedN);
		if (!isfinite(inl2) || (inl2 < 1e-20f))
			interpolatedN = geometryN;
		shadeN = interpolatedN;
		intoObject = (Dot(-fixedDir, geometryN) < 0.f);
		defaultUV = mesh->InterpolateTriUV(triangleIndex, b1, b2, 0);
		CoordinateSystem(Vector(shadeN), &dpdu, &dpdv);
		dndu = Normal();
		dndv = Normal();
	} else {
		mesh->GetShadingInfo(localToWorld, triangleIndex, geometryN, 0,
				&interpolatedN, &dpdu, &dpdv, &dndu, &dndv,
				b1, b2, &defaultUV);
		shadeN = interpolatedN;
		intoObject = (Dot(-fixedDir, geometryN) < 0.f);
	}

	interiorVolume = nullptr;
	exteriorVolume = nullptr;
	SetRayContext(0, NONE, nullptr, 0.f);
}
