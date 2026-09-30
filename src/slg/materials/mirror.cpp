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

#include "slg/materials/mirror.h"
#include "slg/materials/glassmicrofacet.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Mirror material
//------------------------------------------------------------------------------

MirrorMaterial::MirrorMaterial(TextureConstPtr frontTransp, TextureConstPtr backTransp,
		TextureConstPtr emitted, TextureConstPtr bump,
		TextureConstPtr refl) : Material(frontTransp, backTransp, emitted, bump), Kr(refl) {
}

Spectrum MirrorMaterial::Evaluate(const HitPoint &hitPoint,
	const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
	float *directPdfW, float *reversePdfW) const {
	if (hitPoint.regularization > 0.f) {
		// PSR delta->lobe: GGX conductor lobe, alpha = sigma. No
		// dispersion textures and no transmission on a mirror.
		return GlassMicrofacet_Evaluate(hitPoint, localLightDir, localEyeDir,
				event, directPdfW, reversePdfW,
				Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f),
				Spectrum(0.f),
				1.f, 1.f, Dispersion(), false);
	}
	return Spectrum();
}

Spectrum MirrorMaterial::Sample(const HitPoint &hitPoint,
	const Vector &localFixedDir, Vector *localSampledDir,
	const float u0, const float u1, const float passThroughEvent,
	float *pdfW, BSDFEvent *event) const {
	if (hitPoint.regularization > 0.f) {
		return GlassMicrofacet_Sample(hitPoint, localFixedDir,
				localSampledDir, u0, u1, passThroughEvent,
				pdfW, event,
				Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f),
				Spectrum(0.f),
				1.f, 1.f, Dispersion(), false);
	}

	*event = SPECULAR | REFLECT;

	*localSampledDir = Vector(-localFixedDir.x, -localFixedDir.y, localFixedDir.z);
	*pdfW = 1.f;

	return Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
}

void MirrorMaterial::Pdf(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
	if (hitPoint.regularization > 0.f) {
		GlassMicrofacet_Pdf(hitPoint, localLightDir, localEyeDir,
				directPdfW, reversePdfW,
				Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f),
				Spectrum(0.f),
				1.f, 1.f, Dispersion(), false);
		return;
	}
	if (directPdfW)
		*directPdfW = 0.f;
	if (reversePdfW)
		*reversePdfW = 0.f;
}

void MirrorMaterial::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Material::AddReferencedTextures(referencedTexs);

	Kr->AddReferencedTextures(referencedTexs);
}

void MirrorMaterial::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	Material::UpdateTextureReferences(oldTex, newTex);

	if (Kr == &oldTex)
		Kr = &newTex;
}

PropertiesUPtr MirrorMaterial::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const  {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.materials." + name + ".type")("mirror"));
	props->Set(Property("scene.materials." + name + ".kr")(Kr->GetSDLValue()));
	props->Set(Material::ToProperties(imgMapCache, useRealFileName));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
