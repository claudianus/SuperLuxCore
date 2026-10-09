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

#include "slg/volumes/homogenous.h"
#include "slg/bsdf/bsdf.h"
#include "slg/scene/scene.h"
#include "slg/cameras/camera.h"
#include "slg/lights/lightsourcedefs.h"
#include "slg/scene/sceneobjectdefs.h"
#include "slg/materials/glass.h"
#include <memory>

using namespace luxrays;
using namespace slg;
using namespace std;

// Used when hitting a surface
void BSDF::Init(
		const bool fixedFromLight, const bool throughShadowTransparency,
		SceneConstRef scene, const Ray &ray, const RayHit &rayHit,
		const float passThroughEvent, const PathVolumeInfo *volInfo) {
	nullSelectionConditioned = false;
	// Get the scene object
	sceneObject = &scene.GetObjects().GetSceneObject(rayHit.meshIndex);

	// Get the mesh
	auto& mesh = sceneObject->GetExtMesh();
	mesh.GetLocal2World(ray.time, hitPoint.localToWorld);

	hitPoint.Init(fixedFromLight, throughShadowTransparency,
			scene, *sceneObject, rayHit.triangleIndex,
			ray(rayHit.t), -ray.d,
			rayHit.b1, rayHit.b2,
			passThroughEvent);

	// Get the material
	material = &sceneObject->GetMaterial();
	// Set interior and exterior volumes. The whole resolution is skipped
	// when no volume can result: the material carries none (stored or
	// per-hit override), the path has no current volume and the scene has
	// no default world volume. HitPoint::Init() already nulls both fields.
	// Materials that override the volume getters (MixMaterial,
	// GlossyCoatingMaterial, TwoSidedMaterial) need the hitPoint-aware
	// virtual call; the rest return the stored pointers directly so the
	// hot path skips two vtable dispatches.
	const bool needsVolumes = material->HasAnyVolume() ||
		volInfo->HasCurrentVolume() || scene.HasDefaultWorldVolume();
	if (needsVolumes) {
		if (material->HasVolumeOverrides()) {
			volInfo->SetHitPointVolumes(hitPoint,
				material->GetInteriorVolume(hitPoint, hitPoint.passThroughEvent),
				material->GetExteriorVolume(hitPoint, hitPoint.passThroughEvent),
				scene.HasDefaultWorldVolume() ?
				VolumeConstPtr(&scene.GetDefaultWorldVolume()) :
				VolumeConstPtr(nullptr)
			);
		} else {
			volInfo->SetHitPointVolumes(hitPoint,
				material->GetInteriorVolume(),
				material->GetExteriorVolume(),
				scene.HasDefaultWorldVolume() ?
				VolumeConstPtr(&scene.GetDefaultWorldVolume()) :
				VolumeConstPtr(nullptr)
			);
		}
	}

	// For a transparent shadow hit we need only `material` and the
	// interpolated UV (GetPassThroughShadowTransparency reads a
	// Spectrum field). The triangle-light lookup, bump evaluation and
	// frame construction are dead work - the path does not terminate
	// here.
	if (!throughShadowTransparency) {
		// Check if it is a light source
		if (material->IsLightSource())
			triangleLightSource =
				&scene.GetLightSources().GetLightSourceByMeshAndTriIndex(
					rayHit.meshIndex, rayHit.triangleIndex
				);
		else
			triangleLightSource = nullptr;

		// Apply bump or normal mapping. Materials that carry no bump
		// texture return early - skip the virtual call entirely since
		// Material::Bump is a null-test on 99% of hits.
		if (material->GetBumpTexture()) {
			// Pixel footprint for the bump filter width (Cycles Bump
			// "Filter Width"): camera distance * pixel spread angle
			hitPoint.bumpFootprint = 0.f;
			if ((material->GetBumpFilterWidth() > 0.f) && scene.HasCamera()) {
				const Camera &cam = scene.GetCamera();
				const Point camPos = cam.GetCameraToWorld() * Point(0.f, 0.f, 0.f);
				hitPoint.bumpFootprint = Distance(hitPoint.p, camPos) * cam.GetPixelSpreadAngle();
			}
			material->Bump(&hitPoint);
		}

		// Build the local reference system
		frame = hitPoint.GetFrame();
	} else
		triangleLightSource = nullptr;
}


// Used when have a point of a surface
void BSDF::Init(
		SceneConstRef scene,
		const u_int meshIndex,
		const u_int triangleIndex,
		const Point &surfacePoint,
		const float surfacePointBary1,
		const float surfacePointBary2,
		const float time,
		const float passThroughEvent,
		const PathVolumeInfo *volInfo
) {
	nullSelectionConditioned = false;
	// Get the scene object
	sceneObject = &scene.GetObjects().GetSceneObject(meshIndex);

	auto& mesh = sceneObject->GetExtMesh();
	mesh.GetLocal2World(time, hitPoint.localToWorld);

	const Normal geoN = mesh.GetGeometryNormal(hitPoint.localToWorld, triangleIndex);
	hitPoint.Init(false, false,
			scene, *sceneObject, triangleIndex,
			surfacePoint, Vector(geoN), geoN,
			surfacePointBary1, surfacePointBary2, passThroughEvent);

	// Get the material
	material = &sceneObject->GetMaterial();

	// Set interior and exterior volumes. Same fast-path as the ray-hit
	// Init(): skip when the material carries no volume (stored or
	// override), the path has no current volume and there is no default
	// world volume. Materials that override the volume getters
	// (MixMaterial, GlossyCoatingMaterial, TwoSidedMaterial) need the
	// hitPoint-aware virtual call; the rest return the stored pointers.
	const bool needsVolumes = material->HasAnyVolume() ||
		volInfo->HasCurrentVolume() || scene.HasDefaultWorldVolume();
	if (needsVolumes) {
		if (material->HasVolumeOverrides()) {
			volInfo->SetHitPointVolumes(hitPoint,
					material->GetInteriorVolume(hitPoint, hitPoint.passThroughEvent),
					material->GetExteriorVolume(hitPoint, hitPoint.passThroughEvent),
					scene.HasDefaultWorldVolume() ?
						VolumeConstPtr(&scene.GetDefaultWorldVolume()) :
						VolumeConstPtr()
			);
		} else {
			volInfo->SetHitPointVolumes(hitPoint,
					material->GetInteriorVolume(),
					material->GetExteriorVolume(),
					scene.HasDefaultWorldVolume() ?
						VolumeConstPtr(&scene.GetDefaultWorldVolume()) :
						VolumeConstPtr()
			);
		}
	}

	// Check if it is a light source
	if (material->IsLightSource())
		triangleLightSource = &scene.GetLightSources().GetLightSourceByMeshAndTriIndex(meshIndex, triangleIndex);
	else
		triangleLightSource = nullptr;

	// Apply bump or normal mapping
	hitPoint.bumpFootprint = 0.f;
	material->Bump(&hitPoint);

	// Build the local reference system
	frame = hitPoint.GetFrame();
}

// Used when hitting a volume scatter point
void BSDF::Init(
	const bool fixedFromLight,
	const bool throughShadowTransparency,
	SceneConstRef scene,
	const luxrays::Ray &ray,
	VolumeConstRef volume,
	const float t,
	const float passThroughEvent
) {
	nullSelectionConditioned = false;
	hitPoint.fromLight = fixedFromLight;
	hitPoint.throughShadowTransparency = throughShadowTransparency;
	hitPoint.passThroughEvent = passThroughEvent;
	// The ray context is set later by Scene::Intersect()
	hitPoint.SetRayContext(0, NONE, nullptr, 0.f);

	// Normalize the incoming direction: a non-unit ray.d makes the frame
	// non-orthonormal (SetFromZ does not normalize) and every following
	// sampled direction inherits a quadratic magnitude error - long
	// random-walk volume paths then collapse to a zero vector or blow up
	// to Inf/NaN.
	const Vector rayDir = Normalize(ray.d);

	hitPoint.p = ray(t);
	hitPoint.fixedDir = -rayDir;

	sceneObject = nullptr;
	material = &volume;

	hitPoint.geometryN = Normal(-rayDir);
	hitPoint.interpolatedN = hitPoint.geometryN;
	hitPoint.shadeN = hitPoint.geometryN;

	hitPoint.intoObject = true;
	hitPoint.interiorVolume = &volume;
	hitPoint.exteriorVolume = &volume;

	triangleLightSource = nullptr;

	hitPoint.defaultUV = UV(0.f, 0.f);

	CoordinateSystem(Vector(hitPoint.shadeN), &hitPoint.dpdu, &hitPoint.dpdv);
	hitPoint.dndu = Normal();
	hitPoint.dndv = Normal();

	hitPoint.mesh = nullptr;
	hitPoint.triangleIndex = NULL_INDEX;
	hitPoint.triangleBariCoord1 = 0.f;
	hitPoint.triangleBariCoord2 = 0.f;

	hitPoint.objectID = NULL_INDEX;

	// Build the local reference system
	frame.SetFromZ(hitPoint.shadeN);
}

void BSDF::MoveHitPoint(const Point &p, const Normal &n) {
	hitPoint.p = p;
	hitPoint.geometryN = n;
	hitPoint.interpolatedN = n;
	hitPoint.shadeN = n;

	Vector x, y;
	CoordinateSystem(Vector(n), &x, &y);
	frame = Frame(x, y, n);
}

bool BSDF::IsAlbedoEndPoint(const AlbedoSpecularSetting albedoSpecularSetting,
		const float albedoSpecularGlossinessThreshold) const {
	const BSDFEvent eventTypes = GetEventTypes();
	if (!IsDelta() && !((eventTypes & GLOSSY) && (GetGlossiness() < albedoSpecularGlossinessThreshold)))
		return true;

	switch (albedoSpecularSetting) {
		case NO_REFLECT_TRANSMIT:
			return true;
		case ONLY_REFLECT:
			return !((eventTypes & REFLECT) && !(eventTypes & TRANSMIT));
		case ONLY_TRANSMIT:
			return !(!(eventTypes & REFLECT) && (eventTypes & TRANSMIT));
		case REFLECT_TRANSMIT:
			return !((eventTypes & REFLECT) || (eventTypes & TRANSMIT));
		default:
			throw runtime_error("Unknown AlbedoSpecularSetting in BSDF::IsAlbedoEndPoint(): " + ToString(albedoSpecularSetting));
	}
}

bool BSDF::IsCameraInvisible() const {
	return (sceneObject) ? sceneObject->IsCameraInvisible() : false;
}

u_int BSDF::GetObjectID() const {
	return (sceneObject) ? sceneObject->GetID() : std::numeric_limits<u_int>::max();
}

static string MaterialNULLptrName = "NULL pointer";

const string &BSDF::GetMaterialName() const {
	if (material)
		return material->GetName();
	else
		return MaterialNULLptrName;
}

Spectrum BSDF::Albedo() const {
	return material->Albedo(hitPoint);
}

Spectrum BSDF::EvaluateTotal() const {
	return material->EvaluateTotal(hitPoint);
}

bool BSDF::HasBakeMap(const BakeMapType type) const {
	return sceneObject && sceneObject->HasBakeMap(type);
}

Spectrum BSDF::GetBakeMapValue() const {
	return sceneObject->GetBakeMapValue(hitPoint.GetUV(sceneObject->GetBakeMapUVIndex()));
}

//------------------------------------------------------------------------------
// "A Microfacet-Based Shadowing Function to Solve the Bump Terminator Problem"
// by Alejandro Conty Estevez, Pascal Lecocq, and Clifford Stein
// http://www.aconty.com/pdf/bump-terminator-nvidia2019.pdf
//------------------------------------------------------------------------------

static u_int g_shadowTerminatorMode = 0;

void BSDF::SetShadowTerminatorMode(const u_int mode) { g_shadowTerminatorMode = mode; }
u_int BSDF::GetShadowTerminatorMode() { return g_shadowTerminatorMode; }

// Cycles bump_shadowing_term(): a GGX-like masking with the roughness
// derived from the shading/interpolated normal divergence
static float ContyBumpShadowingTerm(const Normal &Ni, const Normal &Ns,
		const Vector &lightDir) {
	const float cos_i = Dot(Ni, lightDir);
	if (cos_i < 0.f)
		return 0.f;
	const float cos_d = Min(fabsf(Dot(Ni, Ns)), 1.f);
	if (cos_d <= 0.f)
		return 0.f;
	const float tan2_d = (1.f - cos_d * cos_d) / (cos_d * cos_d);
	const float alpha2 = Clamp(.125f * tan2_d, 0.f, 1.f);
	const float cos2_i = Max(cos_i * cos_i, 1e-12f);
	const float tan2_i = (1.f - cos2_i) / cos2_i;
	return 2.f / (1.f + sqrtf(1.f + alpha2 * tan2_i));
}

//------------------------------------------------------------------------------
// "Taming the Shadow Terminator"
// by Matt Jen-Yuan Chiang, Yining Karl Li and Brent Burley
// https://www.yiningkarlli.com/projects/shadowterminator.html
//------------------------------------------------------------------------------

static float ShadowTerminatorAvoidanceFactor(const Normal &Ni, const Normal &Ns,
		const Vector &lightDir) {
	const float dotNsLightDir = Dot(Ns, lightDir);
	if (dotNsLightDir <= 0.f)
		return 0.f;

	const float dotNiNs = Dot(Ni, Ns);
	if (dotNiNs <= 0.f)
		return 0.f;

	const float G = Min(1.f, Dot(Ni, lightDir) / (dotNsLightDir * dotNiNs));
	if (G <= 0.f)
		return 0.f;
	
	const float G2 = G * G;
	const float G3 = G2 * G;

	return -G3 + G2 + G;
}

Spectrum BSDF::Evaluate(const Vector &generatedDir,
		BSDFEvent *event, float *directPdfW, float *reversePdfW) const {
	const Vector &eyeDir = hitPoint.fromLight ? generatedDir : hitPoint.fixedDir;
	const Vector &lightDir = hitPoint.fromLight ? hitPoint.fixedDir : generatedDir;

	const float dotLightDirNG = Dot(lightDir, hitPoint.geometryN);
	const float absDotLightDirNG = fabsf(dotLightDirNG);
	const float dotEyeDirNG = Dot(eyeDir, hitPoint.geometryN);
	const float absDotEyeDirNG = fabsf(dotEyeDirNG);

	if (!IsVolume()) {
		// These kind of tests make sense only for materials

		// Avoid glancing angles
		if ((absDotLightDirNG < DEFAULT_COS_EPSILON_STATIC) ||
				(absDotEyeDirNG < DEFAULT_COS_EPSILON_STATIC))
			return Spectrum();

		// Check geometry normal and light direction side
		const float sideTestNG = dotEyeDirNG * dotLightDirNG;
		const BSDFEvent matEvents = material->GetEventTypes();
		if (((sideTestNG > 0.f) && !(matEvents & REFLECT)) ||
				((sideTestNG < 0.f) && !(matEvents & TRANSMIT)))
			return Spectrum();

		// Check shading normal and light direction side
		const float sideTestIS = Dot(eyeDir, hitPoint.interpolatedN) * Dot(lightDir, hitPoint.interpolatedN);
		if (((sideTestIS > 0.f) && !(matEvents & REFLECT)) ||
				((sideTestIS < 0.f) && !(matEvents & TRANSMIT)))
			return Spectrum();
	}

	const Vector localLightDir = frame.ToLocal(lightDir);
	const Vector localEyeDir = frame.ToLocal(eyeDir);
	Spectrum result = material->Evaluate(hitPoint, localLightDir, localEyeDir,
			event, directPdfW, reversePdfW);
	verify (!result.IsNaN() && !result.IsInf());
	if (nullSelectionConditioned) {
		const float probability = material->GetNonNullSelectionProbability(hitPoint);
		if (!(probability > 0.f))
			return Spectrum();
		result /= probability;
		if (directPdfW) *directPdfW /= probability;
		if (reversePdfW) *reversePdfW /= probability;
	}
	if (result.Black())
		return result;

	if (!IsVolume()) {
		// Shadow terminator artefact avoidance
		if ((*event & REFLECT) && (hitPoint.shadeN != hitPoint.interpolatedN)) {
			if (g_shadowTerminatorMode == 0) {
				if (*event & (DIFFUSE | GLOSSY))
					result *= ShadowTerminatorAvoidanceFactor(hitPoint.GetLandingInterpolatedN(),
							hitPoint.GetLandingShadeN(), lightDir);
			} else if ((g_shadowTerminatorMode == 1) && (*event & DIFFUSE))
				result *= ContyBumpShadowingTerm(hitPoint.GetLandingInterpolatedN(),
						hitPoint.GetLandingShadeN(), lightDir);
		}

		// Adjoint BSDF (not for volumes)
		if (hitPoint.fromLight)
			result *= (absDotEyeDirNG / absDotLightDirNG);
	}

	return result;
}

Spectrum BSDF::ShadowCatcherSample(Vector *sampledDir,
		float *pdfW, float *absCosSampledDir, BSDFEvent *event) const {
	// Just continue to trace the ray
	*sampledDir = -hitPoint.fixedDir;
	*absCosSampledDir = AbsDot(*sampledDir, hitPoint.geometryN);

	*pdfW = 1.f;
	*event = SPECULAR | TRANSMIT;
	const Spectrum result(1.f);

	// Adjoint BSDF
	if (hitPoint.fromLight) {
		const float absDotFixedDirNG = AbsDot(hitPoint.fixedDir, hitPoint.geometryN);
		const float absDotSampledDirNG = AbsDot(*sampledDir, hitPoint.geometryN);
		return result * (absDotSampledDirNG / absDotFixedDirNG);
	} else
		return result;
}

Spectrum BSDF::Sample(Vector *sampledDir,
		const float u0, const float u1,
		float *pdfW, float *absCosSampledDir,
		BSDFEvent *event) const {
	Vector localFixedDir = frame.ToLocal(hitPoint.fixedDir);
	Vector localSampledDir;

	Spectrum result = material->Sample(hitPoint,
		localFixedDir, &localSampledDir, u0, u1, hitPoint.passThroughEvent,
		pdfW, event);
	if (result.Black())
		return result;

	// The f/pdf throughput is unchanged when both f and pdf are conditioned.
	if (nullSelectionConditioned) {
		const float probability = material->GetNonNullSelectionProbability(hitPoint);
		if (!(probability > 0.f))
			return Spectrum();
		*pdfW /= probability;
	}
	*absCosSampledDir = fabsf(CosTheta(localSampledDir));
	*sampledDir = frame.ToWorld(localSampledDir);

	// Shadow terminator artefact avoidance
	if ((*event & REFLECT) && (hitPoint.shadeN != hitPoint.interpolatedN)) {
		const Vector &lightDir = hitPoint.fromLight ? hitPoint.fixedDir : (*sampledDir);

		if (g_shadowTerminatorMode == 0) {
			if (*event & (DIFFUSE | GLOSSY))
				result *= ShadowTerminatorAvoidanceFactor(hitPoint.GetLandingInterpolatedN(),
						hitPoint.GetLandingShadeN(), lightDir);
		} else if ((g_shadowTerminatorMode == 1) && (*event & DIFFUSE))
			result *= ContyBumpShadowingTerm(hitPoint.GetLandingInterpolatedN(),
					hitPoint.GetLandingShadeN(), lightDir);
	}

	// Adjoint BSDF
	if (hitPoint.fromLight) {
		const float absDotFixedDirNG = AbsDot(hitPoint.fixedDir, hitPoint.geometryN);
		const float absDotSampledDirNG = AbsDot(*sampledDir, hitPoint.geometryN);
		result *= (absDotSampledDirNG / absDotFixedDirNG);
	}

	return result;
}

bool BSDF::IsChainTransparentMedium() const {
	if (!IsVolume())
		return false;
	if (material->GetType() == HOMOGENEOUS_VOL)
		return !static_cast<const HomogeneousVolume *>(material.get())->IsSSSParametrized();
	return true;
}

void BSDF::Pdf(const Vector &sampledDir, float *directPdfW, float *reversePdfW) const {
	const Vector &eyeDir = hitPoint.fromLight ? sampledDir : hitPoint.fixedDir;
	const Vector &lightDir = hitPoint.fromLight ? hitPoint.fixedDir : sampledDir;
	Vector localLightDir = frame.ToLocal(lightDir);
	Vector localEyeDir = frame.ToLocal(eyeDir);

	material->Pdf(hitPoint, localLightDir, localEyeDir, directPdfW, reversePdfW);
	if (nullSelectionConditioned) {
		const float probability = material->GetNonNullSelectionProbability(hitPoint);
		if (directPdfW) *directPdfW = probability > 0.f ? *directPdfW / probability : 0.f;
		if (reversePdfW) *reversePdfW = probability > 0.f ? *reversePdfW / probability : 0.f;
	}
}

Spectrum BSDF::GetPassThroughTransparency(const bool backTracing) const {
	const Vector localFixedDir = frame.ToLocal(hitPoint.fixedDir);

	return material->GetPassThroughTransparency(hitPoint, localFixedDir,
			hitPoint.passThroughEvent, backTracing);
}

Spectrum BSDF::GetEmittedRadiance(float *directPdfA, float *emissionPdfW) const {
	Spectrum radiance = triangleLightSource ?
		triangleLightSource->GetRadiance(hitPoint, directPdfA, emissionPdfW) :
		Spectrum();
	if (nullSelectionConditioned) {
		const float probability = material->GetNonNullSelectionProbability(hitPoint);
		return probability > 0.f ? radiance / probability : Spectrum();
	}
	return radiance;
}

AlbedoSpecularSetting slg::String2AlbedoSpecularSetting(const string &type) {
	if (type == "NO_REFLECT_TRANSMIT")
		return NO_REFLECT_TRANSMIT;
	else if (type == "ONLY_REFLECT")
		return ONLY_REFLECT;
	else if (type == "ONLY_TRANSMIT")
		return ONLY_TRANSMIT;
	else if (type == "REFLECT_TRANSMIT")
		return REFLECT_TRANSMIT;
	else
		throw runtime_error("Unknown albedo specular setting in String2AlbedoSpecularSetting(): " + type);
}

const string slg::AlbedoSpecularSetting2String(const AlbedoSpecularSetting type) {
	switch (type) {
		case NO_REFLECT_TRANSMIT:
			return "NO_REFLECT_TRANSMIT";
		case ONLY_REFLECT:
			return "ONLY_REFLECT";
		case ONLY_TRANSMIT:
			return "ONLY_TRANSMIT";
		case REFLECT_TRANSMIT:
			return "REFLECT_TRANSMIT";
		default:
			throw runtime_error("Unknown albedo specular setting in AlbedoSpecularSetting2String(): " + ToString(type));
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
