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

#include "slg/materials/metal2.h"
#include "slg/materials/microfacet.h"
#include "slg/textures/fresnel/fresnelcolor.h"
#include "slg/textures/fresnel/fresnelconst.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Metal2 material
//
// LuxRender Metal2 material porting.
//------------------------------------------------------------------------------

Metal2Material::Metal2Material(
	TextureConstPtr frontTransp,
	TextureConstPtr backTransp,
	TextureConstPtr emitted,
	TextureConstPtr bump,
	TextureConstPtr nn,
	TextureConstPtr kk,
	TextureConstPtr u,
	TextureConstPtr v,
	const bool mbounce,
	const bool useGgx
) :
	Material(frontTransp, backTransp, emitted, bump),
	fresnelTex(nullptr),
	n(nn),
	k(kk),
	nu(u),
	nv(v),
	multibounce(mbounce),
	useGgx(useGgx)
{
	glossiness = ComputeGlossiness(nu, nv);
}

Metal2Material::Metal2Material(
	TextureConstPtr frontTransp,
	TextureConstPtr backTransp,
	TextureConstPtr emitted,
	TextureConstPtr bump,
	FresnelTextureConstPtr ft,
	TextureConstPtr u,
	TextureConstPtr v,
	const bool mbounce,
	const bool useGgx)
	:
	Material(frontTransp, backTransp, emitted, bump),
	fresnelTex(ft),
	n(nullptr),
	k(nullptr),
	nu(u),
	nv(v),
	multibounce(mbounce),
	useGgx(useGgx)
{
	glossiness = ComputeGlossiness(nu, nv);
}

// Resolves the conductor complex IOR (n, k) used by the multi-bounce walk.
// Mirrors Metal2Material_GetNK on the GPU side for parity.
void Metal2Material::GetNK(const HitPoint &hitPoint, Spectrum &nVal, Spectrum &kVal) const {
	if (fresnelTex) {
		if (fresnelTex->GetType() == FRESNELCONST_TEX) {
			const FresnelConstTexture *fct = static_cast<const FresnelConstTexture *>(
					std::addressof(*fresnelTex));
			nVal = fct->GetN();
			kVal = fct->GetK();
		} else if (fresnelTex->GetType() == FRESNELCOLOR_TEX) {
			const Spectrum f = static_cast<const FresnelColorTexture *>(
					std::addressof(*fresnelTex))->GetKr().GetSpectrumValue(hitPoint);
			nVal = FresnelTexture::ApproxN(f);
			kVal = FresnelTexture::ApproxK(f);
		} else {
			// Fallback for other Fresnel textures: approximate n,k from F(0)
			const Spectrum f = fresnelTex->Evaluate(hitPoint, 1.f);
			nVal = FresnelTexture::ApproxN(f);
			kVal = FresnelTexture::ApproxK(f);
		}
	} else {
		nVal = n->GetSpectrumValue(hitPoint).Clamp(.001f);
		kVal = k->GetSpectrumValue(hitPoint).Clamp(.001f);
	}
}

Spectrum Metal2Material::Albedo(const HitPoint &hitPoint) const {
	Spectrum F;
	if (fresnelTex)
		F = fresnelTex->Evaluate(hitPoint, 1.f);
	else {
		// For compatibility with the past
		const Spectrum etaVal = n->GetSpectrumValue(hitPoint).Clamp(.001f);
		const Spectrum kVal = k->GetSpectrumValue(hitPoint).Clamp(.001f);
		F = FresnelTexture::GeneralEvaluate(etaVal, kVal, 1.f);
	}
	F.Clamp(0.f, 1.f);
	
	return F;
}
	
Spectrum Metal2Material::Evaluate(const HitPoint &hitPoint,
	const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
	float *directPdfW, float *reversePdfW) const {
	const float u = Clamp(nu->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float v = Clamp(nv->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float u2 = u * u;
	const float v2 = v * v;
	const float anisotropy = (u2 < v2) ? (1.f - u2 / v2) : u2 > 0.f ? (v2 / u2 - 1.f) : 0.f;
	const float roughness = u * v;
	// GGX path: perceptual roughnesses map to squared GGX alphas
	const float alphaT = Max(u2, 1e-4f);
	const float alphaB = Max(v2, 1e-4f);

	const Vector wh(Normalize(localLightDir + localEyeDir));
	const float cosWH = Dot(localLightDir, wh);

	if (useGgx) {
		if (directPdfW)
			*directPdfW = GgxVNDFReflectionPdf(localEyeDir, wh, alphaT, alphaB);
		if (reversePdfW)
			*reversePdfW = GgxVNDFReflectionPdf(localLightDir, wh, alphaT, alphaB);
	} else {
		const float schlickPdf = SchlickDistribution_Pdf(roughness, wh, anisotropy) / (4.f * cosWH);
		if (directPdfW)
			*directPdfW = schlickPdf;
		if (reversePdfW)
			*reversePdfW = schlickPdf;
	}

	Spectrum F;
	if (fresnelTex)
		F = fresnelTex->Evaluate(hitPoint, cosWH);
	else {
		// For compatibility with the past
		const Spectrum etaVal = n->GetSpectrumValue(hitPoint).Clamp(.001f);
		const Spectrum kVal = k->GetSpectrumValue(hitPoint).Clamp(.001f);
		F = FresnelTexture::GeneralEvaluate(etaVal, kVal, cosWH);
	}
	F.Clamp(0.f, 1.f);

	*event = GLOSSY | REFLECT;
	if (useGgx) {
		if (multibounce) {
			// Heitz'16 height-tracking multi-bounce evaluation: the walk
			// estimator already contains the single-scatter term.
			Spectrum nVal, kVal;
			GetNK(hitPoint, nVal, kVal);
			return GgxMSConductorEval(localEyeDir, localLightDir,
					hitPoint.p, alphaT, alphaB, nVal, kVal);
		}
		// f*|cos(lightDir)| = D * G2 * F / (4 * |eyeDir.z|)
		return (GgxD(wh, alphaT, alphaB) *
				GgxG2(localLightDir, localEyeDir, alphaT, alphaB) /
				(4.f * fabsf(localEyeDir.z))) * F;
	}
	const float G = SchlickDistribution_G(roughness, localLightDir, localEyeDir);
	return (SchlickDistribution_D(roughness, wh, anisotropy) * G / (4.f * fabsf(localEyeDir.z))) * F;
}

Spectrum Metal2Material::Sample(const HitPoint &hitPoint,
	const Vector &localFixedDir, Vector *localSampledDir,
	const float u0, const float u1, const float passThroughEvent,
	float *pdfW, BSDFEvent *event) const {
	if (fabsf(localFixedDir.z) < DEFAULT_COS_EPSILON_STATIC)
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
	float d, specPdf;
	if (useGgx)
		wh = GgxSampleVNDF(localFixedDir, alphaT, alphaB, u0, u1);
	else
		SchlickDistribution_SampleH(roughness, anisotropy, u0, u1, &wh, &d, &specPdf);
	const float cosWH = Dot(localFixedDir, wh);
	*localSampledDir = 2.f * cosWH * wh - localFixedDir;

	const float coso = fabsf(localFixedDir.z);
	const float cosi = fabsf(localSampledDir->z);
	if ((cosi < DEFAULT_COS_EPSILON_STATIC) || (localFixedDir.z * localSampledDir->z < 0.f))
		return Spectrum();

	if (useGgx)
		*pdfW = GgxVNDFReflectionPdf(localFixedDir, wh, alphaT, alphaB);
	else
		*pdfW = specPdf / (4.f * fabsf(cosWH));
	if (*pdfW <= 0.f)
		return Spectrum();

	Spectrum F;
	if (fresnelTex)
		F = fresnelTex->Evaluate(hitPoint, cosWH);
	else {
		// For compatibility with the past
		const Spectrum etaVal = n->GetSpectrumValue(hitPoint).Clamp(.001f);
		const Spectrum kVal = k->GetSpectrumValue(hitPoint).Clamp(.001f);
		F = FresnelTexture::GeneralEvaluate(etaVal, kVal, cosWH);
	}
	F.Clamp(0.f, 1.f);

	*event = GLOSSY | REFLECT;

	if (useGgx) {
		if (multibounce) {
			// VNDF single-scatter sampling covers the full multi-bounce
			// support; weight = (f_ss+ms)*cos / pdf_ss stays unbiased.
			Spectrum nVal, kVal;
			GetNK(hitPoint, nVal, kVal);
			const Spectrum ms = GgxMSConductorEval(localFixedDir, *localSampledDir,
					hitPoint.p, alphaT, alphaB, nVal, kVal);
			return ms / *pdfW;
		}
		// (f*cos_i)/pdf = F * G2/G1(wo) for VNDF sampling
		const float g1 = GgxG1(localFixedDir, alphaT, alphaB);
		if (g1 <= 0.f)
			return Spectrum();
		return F * (GgxG2(*localSampledDir, localFixedDir, alphaT, alphaB) / g1);
	}

	const float G = SchlickDistribution_G(roughness, localFixedDir, *localSampledDir);
	float factor = (d / specPdf) * G * fabsf(cosWH);
	if (!hitPoint.fromLight)
		factor /= coso;
	else
		factor /= cosi;

	return factor * F;
}

void Metal2Material::Pdf(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
	const float u = Clamp(nu->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float v = Clamp(nv->GetFloatValue(hitPoint), 1e-9f, 1.f);
	const float u2 = u * u;
	const float v2 = v * v;
	const float anisotropy = (u2 < v2) ? (1.f - u2 / v2) : u2 > 0.f ? (v2 / u2 - 1.f) : 0.f;
	const float roughness = u * v;
	const float alphaT = Max(u2, 1e-4f);
	const float alphaB = Max(v2, 1e-4f);

	const Vector wh(Normalize(localLightDir + localEyeDir));

	if (useGgx) {
		if (directPdfW)
			*directPdfW = GgxVNDFReflectionPdf(localEyeDir, wh, alphaT, alphaB);
		if (reversePdfW)
			*reversePdfW = GgxVNDFReflectionPdf(localLightDir, wh, alphaT, alphaB);
	} else {
		const float schlickPdf = SchlickDistribution_Pdf(roughness, wh, anisotropy) / (4.f * AbsDot(localLightDir, wh));
		if (directPdfW)
			*directPdfW = schlickPdf;
		if (reversePdfW)
			*reversePdfW = schlickPdf;
	}
}

void Metal2Material::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Material::AddReferencedTextures(referencedTexs);

	if (fresnelTex)
		fresnelTex->AddReferencedTextures(referencedTexs);
	if (n)
		n->AddReferencedTextures(referencedTexs);
	if (k)
		k->AddReferencedTextures(referencedTexs);

	nu->AddReferencedTextures(referencedTexs);
	nv->AddReferencedTextures(referencedTexs);
}

void Metal2Material::UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex) {
	Material::UpdateTextureReferences(oldTex, newTex);

	bool updateGlossiness = false;
	if (fresnelTex == static_cast<const FresnelTexture *>(std::addressof(oldTex)))
		fresnelTex = static_cast<const FresnelTexture *>(&newTex);
	if (n == &oldTex)
		n = &newTex;
	if (k == &oldTex)
		k = &newTex;
	if (nu == &oldTex) {
		nu = &newTex;
		updateGlossiness = true;
	}
	if (nv == &oldTex) {
		nv = &newTex;
		updateGlossiness = true;
	}
	
	if (updateGlossiness)
		glossiness = ComputeGlossiness(nu, nv);
}

PropertiesUPtr Metal2Material::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const  {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.materials." + name + ".type")("metal2"));
	if (fresnelTex)
		props->Set(Property("scene.materials." + name + ".fresnel")(fresnelTex->GetSDLValue()));
	if (n)
		props->Set(Property("scene.materials." + name + ".n")(n->GetSDLValue()));
	if (k)
		props->Set(Property("scene.materials." + name + ".k")(k->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".uroughness")(nu->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".vroughness")(nv->GetSDLValue()));
	props->Set(Property("scene.materials." + name + ".multibounce")(multibounce));
	props->Set(Property("scene.materials." + name + ".distribution")(useGgx ? "ggx" : "schlick"));
	props->Set(Material::ToProperties(imgMapCache, useRealFileName));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
