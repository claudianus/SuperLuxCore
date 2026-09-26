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
#include "slg/materials/microfacet.h"
#include "slg/materials/roughglass.h"
#include "slg/materials/thinfilmcoating.h"
#include "slg/usings.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// RoughGlass material
//
// LuxRender RoughGlass material porting.
//------------------------------------------------------------------------------

RoughGlassMaterial::RoughGlassMaterial(TextureConstPtr frontTransp, TextureConstPtr backTransp,
		TextureConstPtr emitted, TextureConstPtr bump,
		TextureConstPtr refl, TextureConstPtr trans,
		TextureConstPtr exteriorIorFact, TextureConstPtr interiorIorFact,
		TextureConstPtr u, TextureConstPtr v,
		TextureConstPtr cauchyBFact, TextureConstPtr filmThickness, TextureConstPtr filmIor,
		const bool useGgx) :
			Material(frontTransp, backTransp, emitted, bump), Kr(refl), Kt(trans),
			exteriorIor(exteriorIorFact), interiorIor(interiorIorFact), nu(u), nv(v),
			cauchyB(cauchyBFact), filmThickness(filmThickness), filmIor(filmIor),
			useGgx(useGgx) {
	glossiness = ComputeGlossiness(nu, nv);
}

Spectrum RoughGlassMaterial::Evaluate(const HitPoint &hitPoint,
	const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
	float *directPdfW, float *reversePdfW) const {
	const Spectrum kt = Kt->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	const Spectrum kr = Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);

	const bool isKtBlack = kt.Black();
	const bool isKrBlack = kr.Black();
	if (isKtBlack && isKrBlack)
		return Spectrum();

	const float nc = ExtractExteriorIors(hitPoint, exteriorIor);
	const float nt = ExtractInteriorIors(hitPoint, interiorIor);
	const float cauchyBValue = cauchyB ? cauchyB->GetFloatValue(hitPoint) : 0.f;
	// Dispersion (S2): the direction-defining wavelength is the hero bin
	const float ntEff = DispersiveIOR(nt, cauchyBValue);
	const float ntc = ntEff / nc;

	const float u = Clamp(nu->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float v = Clamp(nv->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float u2 = u * u;
	const float v2 = v * v;
	const float anisotropy = (u2 < v2) ? (1.f - u2 / v2) : u2 > 0.f ? (v2 / u2 - 1.f) : 0.f;
	const float roughness = u * v;
	// GGX path: perceptual roughnesses map to squared GGX alphas
	const float alphaT = Max(u2, 1e-4f);
	const float alphaB = Max(v2, 1e-4f);

	const float threshold = isKrBlack ? 1.f : (isKtBlack ? 0.f : .5f);
	if (localLightDir.z * localEyeDir.z < 0.f) {
		// Transmit

		const bool entering = (CosTheta(localLightDir) > 0.f);
		const float eta = entering ? (nc / ntEff) : ntc;

		Vector wh = eta * localLightDir + localEyeDir;
		if (wh.z < 0.f)
			wh = -wh;

		const float lengthSquared = wh.LengthSquared();
		if (!(lengthSquared > 0.f))
			return Spectrum();
		wh /= sqrtf(lengthSquared);
		const float cosThetaI = fabsf(CosTheta(localEyeDir));
		const float cosThetaIH = AbsDot(localEyeDir, wh);
		const float cosThetaOH = Dot(localLightDir, wh);

		const float D = useGgx ? GgxD(wh, alphaT, alphaB) :
				SchlickDistribution_D(roughness, wh, anisotropy);
		const float G = useGgx ? GgxG2(localLightDir, localEyeDir, alphaT, alphaB) :
				SchlickDistribution_G(roughness, localLightDir, localEyeDir);
		// Half-vector pdf evaluated at the direction each pdf samples from
		const float specPdfD = useGgx ? GgxVNDFHalfPdf(localEyeDir, wh, alphaT, alphaB) :
				SchlickDistribution_Pdf(roughness, wh, anisotropy);
		const float specPdfR = useGgx ? GgxVNDFHalfPdf(localLightDir, wh, alphaT, alphaB) :
				specPdfD;
		const Spectrum F = DispersiveFresnelR(nt, nc, cauchyBValue, cosThetaOH);

		if (directPdfW)
			*directPdfW = threshold * specPdfD * (hitPoint.fromLight ? fabsf(cosThetaIH) : (fabsf(cosThetaOH) * eta * eta)) / lengthSquared;

		if (reversePdfW)
			*reversePdfW = threshold * specPdfR * (hitPoint.fromLight ? (fabsf(cosThetaOH) * eta * eta) : fabsf(cosThetaIH)) / lengthSquared;

		const Spectrum result = (fabsf(cosThetaOH) * cosThetaIH * D *
			G / (cosThetaI * lengthSquared)) *
			kt * (Spectrum(1.f) - F);

		*event = GLOSSY | TRANSMIT;

		return result;
	} else {
		// Reflect
		const float cosThetaO = fabsf(CosTheta(localLightDir));
		const float cosThetaI = fabsf(CosTheta(localEyeDir));
		if (cosThetaO == 0.f || cosThetaI == 0.f)
			return Spectrum();
		Vector wh = localLightDir + localEyeDir;
		if (wh == Vector(0.f))
			return Spectrum();
		wh = Normalize(wh);
		if (wh.z < 0.f)
			wh = -wh;

		float cosThetaH = Dot(localEyeDir, wh);
		const float D = useGgx ? GgxD(wh, alphaT, alphaB) :
				SchlickDistribution_D(roughness, wh, anisotropy);
		const float G = useGgx ? GgxG2(localLightDir, localEyeDir, alphaT, alphaB) :
				SchlickDistribution_G(roughness, localLightDir, localEyeDir);
		const float specPdfD = useGgx ? GgxVNDFReflectionPdf(localEyeDir, wh, alphaT, alphaB) :
				SchlickDistribution_Pdf(roughness, wh, anisotropy) / (4.f * AbsDot(localLightDir, wh));
		const float specPdfR = useGgx ? GgxVNDFReflectionPdf(localLightDir, wh, alphaT, alphaB) :
				specPdfD;
		const Spectrum F = DispersiveFresnelR(nt, nc, cauchyBValue, cosThetaH);

		if (directPdfW)
			*directPdfW = (1.f - threshold) * specPdfD;

		if (reversePdfW)
			*reversePdfW = (1.f - threshold) * specPdfR;

		const Spectrum result = (D * G / (4.f * cosThetaI)) * kr * F;

		*event = GLOSSY | REFLECT;
		
		const float localFilmThickness = filmThickness ? filmThickness->GetFloatValue(hitPoint) : 0.f;
		if (localFilmThickness > 0.f) {
			const float localFilmIor = filmIor ? filmIor->GetFloatValue(hitPoint) : 1.f;
			return result * CalcFilmColor(localEyeDir, localFilmThickness, localFilmIor);
		}
		return result;
	}
}

Spectrum RoughGlassMaterial::Sample(const HitPoint &hitPoint,
		const Vector &localFixedDir, Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const {
	if (fabsf(localFixedDir.z) < DEFAULT_COS_EPSILON_STATIC)
		return Spectrum();

	const Spectrum kt = Kt->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	const Spectrum kr = Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);

	const bool isKtBlack = kt.Black();
	const bool isKrBlack = kr.Black();
	if (isKtBlack && isKrBlack)
		return Spectrum();

	const float u = Clamp(nu->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float v = Clamp(nv->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float u2 = u * u;
	const float v2 = v * v;
	const float anisotropy = (u2 < v2) ? (1.f - u2 / v2) : u2 > 0.f ? (v2 / u2 - 1.f) : 0.f;
	const float roughness = u * v;
	const float alphaT = Max(u2, 1e-4f);
	const float alphaB = Max(v2, 1e-4f);

	Vector wh;
	float d = 0.f, specPdf = 0.f;
	if (useGgx) {
		wh = GgxSampleVNDF(localFixedDir, alphaT, alphaB, u0, u1);
		specPdf = GgxVNDFHalfPdf(localFixedDir, wh, alphaT, alphaB);
	} else {
		SchlickDistribution_SampleH(roughness, anisotropy, u0, u1, &wh, &d, &specPdf);
	}
	if (wh.z < 0.f)
		wh = -wh;
	const float cosThetaOH = Dot(localFixedDir, wh);

	const float nc = ExtractExteriorIors(hitPoint, exteriorIor);
	const float nt = ExtractInteriorIors(hitPoint, interiorIor);
	const float cauchyBValue = cauchyB ? cauchyB->GetFloatValue(hitPoint) : 0.f;
	// Dispersion (S2): the direction-defining wavelength is the hero bin
	const float ntEff = DispersiveIOR(nt, cauchyBValue);
	const float ntc = ntEff / nc;

	const float coso = fabsf(localFixedDir.z);

	// Decide to transmit or reflect
	float threshold;
	if (!isKrBlack) {
		if (!isKtBlack)
			threshold = .5f;
		else
			threshold = 0.f;
	} else {
		if (!isKtBlack)
			threshold = 1.f;
		else
			return Spectrum();
	}

	Spectrum result;
	if (passThroughEvent < threshold) {
		// Transmit

		const bool entering = (CosTheta(localFixedDir) > 0.f);
		const float eta = entering ? (nc / ntEff) : ntc;
		const float eta2 = eta * eta;
		const float sinThetaIH2 = eta2 * Max(0.f, 1.f - cosThetaOH * cosThetaOH);
		if (sinThetaIH2 >= 1.f)
			return Spectrum();
		float cosThetaIH = sqrtf(1.f - sinThetaIH2);
		if (entering)
			cosThetaIH = -cosThetaIH;
		const float length = eta * cosThetaOH + cosThetaIH;
		*localSampledDir = length * wh - eta * localFixedDir;

		const float lengthSquared = length * length;
		*pdfW = specPdf * fabsf(cosThetaIH) / lengthSquared;
		if (*pdfW <= 0.f)
			return Spectrum();

		const float cosi = fabsf(localSampledDir->z);

		if (useGgx) {
			// (f*cos)/pdf = kt*(1-F)*G2/G1(wo) with VNDF sampling
			const float g1 = GgxG1(localFixedDir, alphaT, alphaB);
			if (g1 <= 0.f)
				return Spectrum();
			const float g2 = GgxG2(*localSampledDir, localFixedDir, alphaT, alphaB);
			const Spectrum F = DispersiveFresnelR(nt, nc, cauchyBValue,
					hitPoint.fromLight ? cosThetaOH : cosThetaIH);
			result = kt * (Spectrum(1.f) - F) * (g2 / (g1 * threshold));
		} else {
			const float G = SchlickDistribution_G(roughness, localFixedDir, *localSampledDir);
			float factor = (d / specPdf) * G * fabsf(cosThetaOH) / threshold;

			if (!hitPoint.fromLight) {
				const Spectrum F = DispersiveFresnelR(nt, nc, cauchyBValue, cosThetaIH);
				result = (factor / coso) * kt * (Spectrum(1.f) - F);
			} else {
				const Spectrum F = DispersiveFresnelR(nt, nc, cauchyBValue, cosThetaOH);
				result = (factor / cosi) * kt * (Spectrum(1.f) - F);
			}
		}

		*pdfW *= threshold;
		*event = GLOSSY | TRANSMIT;
		// Dispersive refraction terminates the secondary wavelengths
		if (cauchyBValue > 0.f)
			result *= Spectral::CollapseToHero();
	} else {
		// Reflect
		*pdfW = specPdf / (4.f * fabsf(cosThetaOH));
		if (*pdfW <= 0.f)
			return Spectrum();

		*localSampledDir = 2.f * cosThetaOH * wh - localFixedDir;

		const float cosi = fabsf(localSampledDir->z);
		if ((cosi < DEFAULT_COS_EPSILON_STATIC) || (localFixedDir.z * localSampledDir->z < 0.f))
			return Spectrum();

		const Spectrum F = DispersiveFresnelR(nt, nc, cauchyBValue, cosThetaOH);
		if (useGgx) {
			// (f*cos)/pdf = kr*F*G2/G1(wo) with VNDF sampling
			const float g1 = GgxG1(localFixedDir, alphaT, alphaB);
			if (g1 <= 0.f)
				return Spectrum();
			result = kr * F * (GgxG2(*localSampledDir, localFixedDir, alphaT, alphaB) /
					(g1 * (1.f - threshold)));
		} else {
			const float G = SchlickDistribution_G(roughness, localFixedDir, *localSampledDir);
			float factor = (d / specPdf) * G * fabsf(cosThetaOH) / (1.f - threshold);
			factor /= (!hitPoint.fromLight) ? coso : cosi;
			result = factor * F * kr;
		}
		
		const float localFilmThickness = filmThickness ? filmThickness->GetFloatValue(hitPoint) : 0.f;
		if (localFilmThickness > 0.f) {
			const float localFilmIor = filmIor ? filmIor->GetFloatValue(hitPoint) : 1.f;
			result *= CalcFilmColor(localFixedDir, localFilmThickness, localFilmIor);
		}

		*pdfW *= (1.f - threshold);
		*event = GLOSSY | REFLECT;
	}

	return result;
}

void RoughGlassMaterial::Pdf(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
	if (directPdfW)
		*directPdfW = 0.f;
	if (reversePdfW)
		*reversePdfW = 0.f;

	const Spectrum kt = Kt->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);
	const Spectrum kr = Kr->GetSpectrumValue(hitPoint).Clamp(0.f, 1.f);

	const bool isKtBlack = kt.Black();
	const bool isKrBlack = kr.Black();
	if (isKtBlack && isKrBlack)
		return;

	const float nc = ExtractExteriorIors(hitPoint, exteriorIor);
	const float nt = ExtractInteriorIors(hitPoint, interiorIor);
	const float cauchyBValue = cauchyB ? cauchyB->GetFloatValue(hitPoint) : 0.f;
	// Dispersion (S2): the direction-defining wavelength is the hero bin
	const float ntEff = DispersiveIOR(nt, cauchyBValue);
	const float ntc = ntEff / nc;

	const float u = Clamp(nu->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float v = Clamp(nv->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float u2 = u * u;
	const float v2 = v * v;
	const float anisotropy = (u2 < v2) ? (1.f - u2 / v2) : u2 > 0.f ? (v2 / u2 - 1.f) : 0.f;
	const float roughness = u * v;
	const float alphaT = Max(u2, 1e-4f);
	const float alphaB = Max(v2, 1e-4f);

	const float threshold = isKrBlack ? 1.f : (isKtBlack ? 0.f : .5f);
	if (localLightDir.z * localEyeDir.z < 0.f) {
		// Transmit

		const bool entering = (CosTheta(localLightDir) > 0.f);
		const float eta = entering ? (nc / ntEff) : ntc;

		Vector wh = eta * localLightDir + localEyeDir;
		if (wh.z < 0.f)
			wh = -wh;

		const float lengthSquared = wh.LengthSquared();
		if (!(lengthSquared > 0.f))
			return;

		wh /= sqrtf(lengthSquared);
		const float cosThetaIH = AbsDot(localEyeDir, wh);
		const float cosThetaOH = AbsDot(localLightDir, wh);

		const float specPdfD = useGgx ? GgxVNDFHalfPdf(localEyeDir, wh, alphaT, alphaB) :
				SchlickDistribution_Pdf(roughness, wh, anisotropy);
		const float specPdfR = useGgx ? GgxVNDFHalfPdf(localLightDir, wh, alphaT, alphaB) :
				specPdfD;

		if (directPdfW)
			*directPdfW = threshold * specPdfD * cosThetaIH / lengthSquared;

		if (reversePdfW)
			*reversePdfW = threshold * specPdfR * cosThetaOH * eta * eta / lengthSquared;
	} else {
		// Reflect
		const float cosThetaO = fabsf(CosTheta(localLightDir));
		const float cosThetaI = fabsf(CosTheta(localEyeDir));
		if (cosThetaO == 0.f || cosThetaI == 0.f)
			return;

		Vector wh = localLightDir + localEyeDir;
		if (wh == Vector(0.f))
			return;
		wh = Normalize(wh);
		if (wh.z < 0.f)
			wh = -wh;

		const float specPdfD = useGgx ? GgxVNDFReflectionPdf(localEyeDir, wh, alphaT, alphaB) :
				SchlickDistribution_Pdf(roughness, wh, anisotropy) / (4.f * AbsDot(localLightDir, wh));
		const float specPdfR = useGgx ? GgxVNDFReflectionPdf(localLightDir, wh, alphaT, alphaB) :
				specPdfD;

		if (directPdfW)
			*directPdfW = (1.f - threshold) * specPdfD;

		if (reversePdfW)
			*reversePdfW = (1.f - threshold) * specPdfR;
	}
}

void RoughGlassMaterial::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Material::AddReferencedTextures(referencedTexs);

	Kr->AddReferencedTextures(referencedTexs);
	Kt->AddReferencedTextures(referencedTexs);
	if (exteriorIor)
		exteriorIor->AddReferencedTextures(referencedTexs);
	if (interiorIor)
		interiorIor->AddReferencedTextures(referencedTexs);
	nu->AddReferencedTextures(referencedTexs);
	nv->AddReferencedTextures(referencedTexs);
	if (cauchyB)
		cauchyB->AddReferencedTextures(referencedTexs);
	if (filmThickness)
		filmThickness->AddReferencedTextures(referencedTexs);
	if (filmIor)
		filmIor->AddReferencedTextures(referencedTexs);
}

void RoughGlassMaterial::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	Material::UpdateTextureReferences(oldTex, newTex);

	bool updateGlossiness = false;
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
	if (nu == &oldTex) {
		nu = &newTex;
		updateGlossiness = true;
	}
	if (nv == &oldTex) {
		nv = &newTex;
		updateGlossiness = true;
	}
	if (filmThickness == &oldTex)
		filmThickness = &newTex;
	if (filmIor == &oldTex)
		filmIor = &newTex;

	if (updateGlossiness)
		glossiness = ComputeGlossiness(nu, nv);
}

PropertiesUPtr RoughGlassMaterial::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const  {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.materials." + name + ".type")("roughglass"));
	props->Set(Property("scene.materials." + name + ".kr")(Kr->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".kt")(Kt->GetSDLValue()));
	if (exteriorIor)
		props->Set(Property("scene.materials." + name + ".exteriorior")(exteriorIor->GetSDLValue()));
	if (interiorIor)
		props->Set(Property("scene.materials." + name + ".interiorior")(interiorIor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".uroughness")(nu->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".vroughness")(nv->GetSDLValue()));
	if (cauchyB)
		props->Set(Property("scene.materials." + name + ".cauchyb")(cauchyB->GetSDLValue()));
	if (filmThickness)
		props->Set(Property("scene.materials." + name + ".filmthickness")(filmThickness->GetSDLValue()));
	if (filmIor)
		props->Set(Property("scene.materials." + name + ".filmior")(filmIor->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".distribution")(useGgx ? "ggx" : "schlick"));
	props->Set(Material::ToProperties(imgMapCache, useRealFileName));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
