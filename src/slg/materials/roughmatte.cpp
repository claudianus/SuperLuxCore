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

#include "slg/materials/roughmatte.h"
#include "slg/materials/microfacet.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Rough matte material
//
// Diffuse lobe: EON (Portsmouth, Kutz, Hill 2025, "EON: A practical
// energy-conserving Oren-Nayar"), replacing the qualitative Oren-Nayar '94
// model (which loses energy and exhibits a dark grazing ring). The sigma
// texture maps directly to the EON roughness r in [0,1].
//------------------------------------------------------------------------------

RoughMatteMaterial::RoughMatteMaterial(TextureConstPtr frontTransp, TextureConstPtr backTransp,
		TextureConstPtr emitted, TextureConstPtr bump,
		TextureConstPtr col, TextureConstPtr s) :
			Material(frontTransp, backTransp, emitted, bump), Kd(col), sigma(s) {
}

Spectrum RoughMatteMaterial::Albedo(const HitPoint &hitPoint) const {
	const float r = Clamp(sigma->GetFloatValue(hitPoint), 0.f, 1.f);
	// Hemisphere-average directional albedo proxy: use mu = 1 for the
	// classification test (exact value not needed for an albedo estimate)
	return eon::DirAlbedo(Kd->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f), r, 1.f);
}

Spectrum RoughMatteMaterial::Evaluate(const HitPoint &hitPoint,
	const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
	float *directPdfW, float *reversePdfW) const {
	const float r = Clamp(sigma->GetFloatValue(hitPoint), 0.f, 1.f);
	const Vector wi = (hitPoint.fromLight ? localLightDir : localEyeDir);
	const Vector wo = (hitPoint.fromLight ? localEyeDir : localLightDir);

	if (directPdfW)
		*directPdfW = eon::Pdf(wo, wi, r);

	if (reversePdfW)
		*reversePdfW = eon::Pdf(wi, wo, r);

	*event = DIFFUSE | REFLECT;
	const Spectrum rho = Kd->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	return eon::Eval(rho, r, wi, wo) * fabsf(localLightDir.z);
}

Spectrum RoughMatteMaterial::Sample(const HitPoint &hitPoint,
	const Vector &localFixedDir, Vector *localSampledDir,
	const float u0, const float u1, const float passThroughEvent,
	float *pdfW, BSDFEvent *event) const {
	if (fabsf(localFixedDir.z) < DEFAULT_COS_EPSILON_STATIC)
		return Spectrum();

	const float r = Clamp(sigma->GetFloatValue(hitPoint), 0.f, 1.f);
	// EON CLTC + uniform mixture sampling (pdf includes both lobes)
	const Vector wo = Sgn(localFixedDir.z) * localFixedDir;
	*localSampledDir = Sgn(localFixedDir.z) * eon::Sample(wo, r, u0, u1, *pdfW);
	if (fabsf(CosTheta(*localSampledDir)) < DEFAULT_COS_EPSILON_STATIC)
		return Spectrum();

	*event = DIFFUSE | REFLECT;
	const Spectrum rho = Kd->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	const Vector wi = Sgn(localSampledDir->z) * *localSampledDir;
	// Sample() returns f * |cos(theta)| / pdf
	return eon::Eval(rho, r, wi, wo) * (fabsf(localSampledDir->z) / *pdfW);
}

void RoughMatteMaterial::Pdf(const HitPoint &hitPoint,
	const Vector &localLightDir, const Vector &localEyeDir,
	float *directPdfW, float *reversePdfW) const {
	const float r = Clamp(sigma->GetFloatValue(hitPoint), 0.f, 1.f);
	const Vector wi = (hitPoint.fromLight ? localLightDir : localEyeDir);
	const Vector wo = (hitPoint.fromLight ? localEyeDir : localLightDir);

	if (directPdfW)
		*directPdfW = eon::Pdf(wo, wi, r);

	if (reversePdfW)
		*reversePdfW = eon::Pdf(wi, wo, r);
}

void RoughMatteMaterial::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Material::AddReferencedTextures(referencedTexs);

	Kd->AddReferencedTextures(referencedTexs);
	sigma->AddReferencedTextures(referencedTexs);
}

void RoughMatteMaterial::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	Material::UpdateTextureReferences(oldTex, newTex);

	if (Kd == &oldTex)
		Kd = &newTex;
	if (sigma == &oldTex)
		sigma = &newTex;
}

PropertiesUPtr RoughMatteMaterial::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const  {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.materials." + name + ".type")("roughmatte"));
	props->Set(Property("scene.materials." + name + ".kd")(Kd->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".sigma")(sigma->GetSDLValue()));
	props->Set(Material::ToProperties(imgMapCache, useRealFileName));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
