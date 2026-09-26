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

#ifndef _SLG_HOMOGENOUSVOL_H
#define	_SLG_HOMOGENOUSVOL_H

#include "slg/volumes/volume.h"
#include <functional>

namespace slg {

//------------------------------------------------------------------------------
// HomogeneousVolume
//------------------------------------------------------------------------------

class HomogeneousVolume : public Volume {
public:
	HomogeneousVolume(
		TextureConstRef iorTex,
		TextureConstPtr emiTex,
		TextureConstRef a, TextureConstRef s,
		TextureConstRef g, const bool multiScattering,
		const bool useHG = false,
		const bool useEquiangular = true,
		TextureConstPtr sssAlbedo = nullptr,
		TextureConstPtr sssMfp = nullptr);

	virtual float Scatter(const luxrays::Ray &ray, const float u, const bool scatteredStart,
		luxrays::Spectrum *connectionThroughput, luxrays::Spectrum *connectionEmission) const;
	// Distance sampling MIS between the transmittance-proportional and the
	// equiangular (Kulla & Fajardo, EGSR 2012) distributions around one of
	// the eqLightPoints, picked proportionally to its contribution estimate
	// lum_i * (thetaB_i - thetaA_i) / D_i on this segment. Unbiased: for a
	// homogeneous medium the free-flight pdf sigma_s * exp(-sigma_s * t)
	// is analytic, so the mixture pdf can be evaluated exactly, and the
	// conditioned MIS cancels the light selection probability.
	float ScatterEquiangular(const luxrays::Ray &ray, const float u, const bool scatteredStart,
		const std::vector<luxrays::Point> &eqLightPoints,
		const std::vector<float> &eqLightLuminances,
		luxrays::Spectrum *connectionThroughput, luxrays::Spectrum *connectionEmission) const;
	virtual luxrays::Spectrum TransmittanceEstimate(const luxrays::Ray &ray,
		const float u) const;

	// Material interface

	virtual MaterialType GetType() const { return HOMOGENEOUS_VOL; }
	virtual BSDFEvent GetEventTypes() const { return DIFFUSE | REFLECT; };

	virtual luxrays::Spectrum Albedo(const HitPoint &hitPoint) const;

	virtual luxrays::Spectrum Evaluate(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir, BSDFEvent *event,
		float *directPdfW = NULL, float *reversePdfW = NULL) const;
	virtual luxrays::Spectrum Sample(const HitPoint &hitPoint,
		const luxrays::Vector &localFixedDir, luxrays::Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const;
	virtual void Pdf(const HitPoint &hitPoint,
		const luxrays::Vector &localLightDir, const luxrays::Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const;

	virtual void AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexsreferencedTexs) const;
	virtual void UpdateTextureReferences(TextureConstRef oldTex, TextureRef newTex);

	virtual luxrays::PropertiesUPtr ToProperties() const;

	TextureConstRef GetSigmaA() const { return sigmaA; }
	TextureConstRef GetSigmaS() const { return sigmaS; }
	TextureConstRef GetG() const { return schlickScatter.GetG(); }
	// SSS albedo parametrization (random-walk subsurface): when
	// sssAlbedoTex is set, SigmaA/SigmaS are derived from the diffuse
	// surface albedo + mean free path instead of the raw coefficients.
	bool IsSSSParametrized() const { return sssAlbedoTex != nullptr; }
	TextureConstPtr GetSSSAlbedoTexture() const { return sssAlbedoTex; }
	TextureConstPtr GetSSSMfpTexture() const { return sssMfpTex; }
	bool IsMultiScattering() const { return multiScattering; }
	bool IsHGPhase() const { return schlickScatter.IsHGPhase(); }
	bool IsEquiangularEnabled() const { return equiangular; }

	static float Scatter(const float u, const bool scatterAllowed, const float segmentLength,
			const luxrays::Spectrum &sigmaA, const luxrays::Spectrum &sigmaS,
			const luxrays::Spectrum &emission,
			luxrays::Spectrum &segmentTransmittance, luxrays::Spectrum &segmentEmission);

protected:
	virtual luxrays::Spectrum SigmaA(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum SigmaS(const HitPoint &hitPoint) const;

private:
	// Evaluates sigma_t and the physical single-scatter albedo under
	// the SSS albedo parametrization at hitPoint.
	luxrays::Spectrum SSSCoeffs(const HitPoint &hitPoint,
			luxrays::Spectrum &alpha) const;

	std::reference_wrapper<const Texture> sigmaA, sigmaS;
	SchlickScatter schlickScatter;
	const bool multiScattering;
	const bool equiangular;
	TextureConstPtr sssAlbedoTex, sssMfpTex;
};

}

#endif	/* _SLG_HOMOGENOUSVOL_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
