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

#include "luxrays/core/color/spectral.h"
#include "slg/textures/fresnel/fresneltexture.h"
#include "slg/materials/glass.h"
#include "slg/materials/thinfilmcoating.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Glass material
//------------------------------------------------------------------------------

GlassMaterial::GlassMaterial(TextureConstPtr frontTransp, TextureConstPtr backTransp,
		TextureConstPtr emitted, TextureConstPtr bump,
		TextureConstPtr refl, TextureConstPtr trans,
		TextureConstPtr exteriorIorFact, TextureConstPtr interiorIorFact,
		TextureConstPtr B, TextureConstPtr filmThickness, TextureConstPtr filmIor) :
			Material(frontTransp, backTransp, emitted, bump),
			Kr(refl), Kt(trans), exteriorIor(exteriorIorFact), interiorIor(interiorIorFact),
			cauchyB(B), filmThickness(filmThickness), filmIor(filmIor) {
}

Spectrum GlassMaterial::Evaluate(const HitPoint &hitPoint,
	const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
	float *directPdfW, float *reversePdfW) const {
	return Spectrum();
}

static Spectrum WaveLength2RGB(const float waveLength) {
	float r, g, b;
	if ((waveLength >= 380.f) && (waveLength < 440.f)) {
		r = -(waveLength - 440.f) / (440 - 380.f);
		g = 0.f;
		b = 1.f;
	} else if ((waveLength >= 440.f) && (waveLength < 490.f)) {
		r = 0.f;
		g = (waveLength - 440.f) / (490.f - 440.f);
		b = 1.f;
	} else if ((waveLength >= 490.f) && (waveLength < 510.f)) {
		r = 0.f;
		g = 1.f;
		b = -(waveLength - 510.f) / (510.f - 490.f);
	} else if ((waveLength >= 510.f) && (waveLength < 580.f)) {
		r = (waveLength - 510.f) / (580.f - 510.f);
		g = 1.f;
		b = 0.f;
	} else if ((waveLength >= 580.f) && (waveLength < 645.f)) {
		r = 1.f;
		g = -(waveLength - 645.f) / (645 - 580.f);
		b = 0.f;
	} else if ((waveLength >= 645.f) && (waveLength < 780.f)) {
		r = 1.f;
		g = 0.f;
		b = 0.f;
	} else
		return Spectrum();

	// The intensity fall off near the upper and lower limits
	float factor;
	if ((waveLength >= 380.f) && (waveLength < 420.f))
		factor = .3f + .7f * (waveLength - 380.f) / (420.f - 380.f);
	else if ((waveLength >= 420) && (waveLength < 700))
		factor = 1.f;
	else
		factor = .3f + .7f * (780.f - waveLength) / (780.f - 700.f);

	const Spectrum result = Spectrum(r, g, b) * factor;

	/*
	Spectrum white;
	for (u_int i = 380; i < 780; ++i)
		white += WaveLength2RGB(i);
	white *= 1.f / 400.f;
	cout << std::setprecision(std::numeric_limits<float>::digits10 + 1) << white.c[0] << ", " << white.c[1] << ", " << white.c[2] << "\n";
	 
	 Result: 0.5652729, 0.36875, 0.265375
	 */

	// To normalize the output
	const Spectrum normFactor(1.f / .5652729f, 1.f / .36875f, 1.f / .265375f);
	
	return result * normFactor;
}

Spectrum GlassMaterial::EvalSpecularReflection(const HitPoint &hitPoint,
		const Vector &localFixedDir, const Spectrum &kr,
		const float nc, const float nt, const float cauchyB,
		Vector *localSampledDir,
		const float localFilmThickness, const float localFilmIor) {
	if (kr.Black())
		return Spectrum();

	const float cosTheta = CosTheta(localFixedDir);
	*localSampledDir = Vector(-localFixedDir.x, -localFixedDir.y, localFixedDir.z);

	// Per-bin Fresnel under dispersion: each sampled wavelength reflects at
	// its own IOR (grazing-angle color separation). Reduces to the scalar
	// CauchyEvaluate when spectral transport is off or cauchyB == 0.
	const Spectrum result = kr * DispersiveFresnelR(nt, nc, cauchyB, cosTheta);

	if (localFilmThickness > 0.f) {
		const Spectrum filmColor = CalcFilmColor(localFixedDir, localFilmThickness, localFilmIor);
		return result * filmColor;
	}
	return result;
}

Spectrum GlassMaterial::EvalSpecularTransmission(const HitPoint &hitPoint,
		const Vector &localFixedDir, const float u0,
		const Spectrum &kt, const float nc, const float nt, const float cauchyB,
		Vector *localSampledDir) {
	if (kt.Black())
		return Spectrum();

	// Compute transmitted ray direction
	Spectrum lkt;
	float lnt;
	if (cauchyB > 0.f) {
		const PathWavelengths *sw = Spectral::Current();
		if (sw) {
			// Hero-wavelength dispersion (S2): refract at the path's hero
			// wavelength; kt stays in spectral bins (no RGB tint). The
			// bin collapse happens in Sample() once the transmit branch
			// is actually picked.
			lnt = WaveLength2IOR(sw->w[sw->hero], nt, cauchyB);
			lkt = kt;
		} else {
			// Select the wavelength to sample
			const float waveLength = Lerp(u0, 380.f, 780.f);

			lnt = WaveLength2IOR(waveLength, nt, cauchyB);

			lkt = kt * WaveLength2RGB(waveLength);
		}
	} else {
		lnt = nt;
		lkt = kt;
	}

	const float ntc = lnt / nc;
	const float cosTheta = CosTheta(localFixedDir);
	const bool entering = (cosTheta > 0.f);
	const float eta = entering ? (nc / lnt) : ntc;
	const float eta2 = eta * eta;
	const float sini2 = SinTheta2(localFixedDir);
	const float sint2 = eta2 * sini2;

	// Handle total internal reflection for transmission
	if (sint2 >= 1.f)
		return Spectrum();

	const float cost = sqrtf(Max(0.f, 1.f - sint2)) * (entering ? -1.f : 1.f);
	*localSampledDir = Vector(-eta * localFixedDir.x, -eta * localFixedDir.y, cost);

	float ce;
	if (!hitPoint.fromLight)
		ce = (1.f - FresnelTexture::CauchyEvaluate(ntc, cost)) * eta2;
	else {
		const float absCosSampledDir = fabsf(CosTheta(*localSampledDir));
		ce = (1.f - FresnelTexture::CauchyEvaluate(ntc, cosTheta)) * fabsf(CosTheta(localFixedDir) / absCosSampledDir);
	}

	return lkt * ce;
}

Spectrum GlassMaterial::Sample(const HitPoint &hitPoint,
		const Vector &localFixedDir, Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const {
	const Spectrum kr = Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	const Spectrum kt = Kt->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);

	const float nc = ExtractExteriorIors(hitPoint, exteriorIor);
	const float nt = ExtractInteriorIors(hitPoint, interiorIor);

	const float cauchyBValue = cauchyB ? cauchyB->GetFloatValue(hitPoint) : 0.f;

	Vector transLocalSampledDir; 
	const Spectrum trans = EvalSpecularTransmission(hitPoint, localFixedDir, u0,
			kt, nc, nt, cauchyBValue, &transLocalSampledDir);
	
	const float localFilmThickness = filmThickness ? filmThickness->GetFloatValue(hitPoint) : 0.f;
	const float localFilmIor = (localFilmThickness > 0.f && filmIor) ? filmIor->GetFloatValue(hitPoint) : 1.f;
	Vector reflLocalSampledDir;
	const Spectrum refl = EvalSpecularReflection(hitPoint, localFixedDir,
			kr, nc, nt, cauchyBValue, &reflLocalSampledDir, localFilmThickness, localFilmIor);

	// Decide to transmit or reflect
	float threshold;
	if (!refl.Black()) {
		if (!trans.Black()) {
			// Importance sampling
			const float reflFilter = refl.Filter();
			const float transFilter = trans.Filter();
			threshold = transFilter / (reflFilter + transFilter);

			// A place an upper and lower limit to not under sample
			// reflection or transmission
			threshold = Clamp(threshold, .25f, .75f);
		} else
			threshold = 0.f;
	} else {
		if (!trans.Black())
			threshold = 1.f;
		else
			return Spectrum();
	}

	Spectrum result;
	if (passThroughEvent < threshold) {
		// Transmit

		*localSampledDir = transLocalSampledDir;

		*event = SPECULAR | TRANSMIT;
		*pdfW = threshold;

		result = trans;
		// Dispersive refraction terminates the secondary wavelengths:
		// the surviving hero bin carries the path (uniform-pick weight).
		if (cauchyBValue > 0.f)
			result *= Spectral::CollapseToHero();
	} else {
		// Reflect

		*localSampledDir = reflLocalSampledDir;

		*event = SPECULAR | REFLECT;
		*pdfW = 1.f - threshold;
		
		result = refl;
	}
	
	return result / *pdfW;
}

void GlassMaterial::Pdf(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir,
	float *directPdfW, float *reversePdfW) const {
	if (directPdfW)
		*directPdfW = 0.f;
	if (reversePdfW)
		*reversePdfW = 0.f;
}

void GlassMaterial::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Material::AddReferencedTextures(referencedTexs);

	Kr->AddReferencedTextures(referencedTexs);
	Kt->AddReferencedTextures(referencedTexs);
	if (exteriorIor)
		exteriorIor->AddReferencedTextures(referencedTexs);
	if (interiorIor)
		interiorIor->AddReferencedTextures(referencedTexs);
	if (cauchyB)
		cauchyB->AddReferencedTextures(referencedTexs);
	if (filmThickness)
		filmThickness->AddReferencedTextures(referencedTexs);
	if (filmIor)
		filmIor->AddReferencedTextures(referencedTexs);
}

void GlassMaterial::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	Material::UpdateTextureReferences(oldTex, newTex);

	if (Kr == &oldTex)
		Kr = &newTex;
	if (Kt == &oldTex)
		Kt = &newTex;
	if (exteriorIor == &oldTex)
		exteriorIor = &newTex;
	if (interiorIor == &oldTex)
		interiorIor = &newTex;
	if (cauchyB == &oldTex)
		cauchyB = &newTex;
	if (filmThickness == &oldTex)
		filmThickness = &newTex;
	if (filmIor == &oldTex)
		filmIor = &newTex;
}

PropertiesUPtr GlassMaterial::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const  {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.materials." + name + ".type")("glass"));
	props->Set(Property("scene.materials." + name + ".kr")(Kr->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".kt")(Kt->GetSDLValue()));
	if (exteriorIor)
		props->Set(Property("scene.materials." + name + ".exteriorior")(exteriorIor->GetSDLValue()));
	if (interiorIor)
		props->Set(Property("scene.materials." + name + ".interiorior")(interiorIor->GetSDLValue()));
	if (cauchyB)
		props->Set(Property("scene.materials." + name + ".cauchyb")(cauchyB->GetSDLValue()));
	if (filmThickness)
		props->Set(Property("scene.materials." + name + ".filmthickness")(filmThickness->GetSDLValue()));
	if (filmIor)
		props->Set(Property("scene.materials." + name + ".filmior")(filmIor->GetSDLValue()));
	props->Set(Material::ToProperties(imgMapCache, useRealFileName));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
