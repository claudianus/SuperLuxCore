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

#include <cstddef>

#include "slg/volumes/homogenous.h"
#include "luxrays/usings.h"
#include "slg/bsdf/bsdf.h"
#include "slg/textures/texture.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// HomogeneousVolume
//------------------------------------------------------------------------------

HomogeneousVolume::HomogeneousVolume(
	TextureConstRef iorTex,
	TextureConstPtr emiTex,
	TextureConstRef a, TextureConstRef s, TextureConstRef g,
	const bool multiScat, const bool useHG, const bool equiang
) :
	Volume(iorTex, emiTex),
	schlickScatter(*this, g, useHG),
	multiScattering(multiScat),
	equiangular(equiang),
	sigmaA(a),
	sigmaS(s)
{}

float HomogeneousVolume::Scatter(const float u,
		const bool scatterAllowed, const float segmentLength,
		const Spectrum &sigmaA, const Spectrum &sigmaS, const Spectrum &emission,
		Spectrum &segmentTransmittance, Spectrum &segmentEmission) {
	// This code must work also with segmentLength = INFINITY

	bool scatter = false;
	segmentTransmittance = Spectrum(1.f);
	segmentEmission = Spectrum(0.f);

	//--------------------------------------------------------------------------
	// Check if there is a scattering event
	//--------------------------------------------------------------------------

	float scatterDistance = segmentLength;
	const float sigmaSValue = sigmaS.Filter();
	if (scatterAllowed && (sigmaSValue > 0.f)) {
		// Determine scattering distance
		const float proposedScatterDistance = -logf(1.f - u) / sigmaSValue;

		scatter = (proposedScatterDistance < segmentLength);
		scatterDistance = scatter ? proposedScatterDistance : segmentLength;

		// Note: scatterDistance can not be infinity because otherwise there would
		// have been a scatter event before.
		const float tau = scatterDistance * sigmaSValue;
		const float pdf = expf(-tau) * (scatter ? sigmaSValue : 1.f);
		segmentTransmittance /= pdf;
	}

	//--------------------------------------------------------------------------
	// Volume transmittance
	//--------------------------------------------------------------------------
	
	const Spectrum sigmaT = sigmaA + sigmaS;
	if (!sigmaT.Black()) {
		if (isinf(scatterDistance)) {
			// This avoid NaN in case scatterDistance is inf
			segmentTransmittance = Spectrum(0.f);
		} else {
			const Spectrum tau = scatterDistance * sigmaT;
			segmentTransmittance *= Exp(-tau) * (scatter ? sigmaT : Spectrum(1.f));
		}
	}

	//--------------------------------------------------------------------------
	// Volume emission
	//--------------------------------------------------------------------------

	segmentEmission += segmentTransmittance * scatterDistance * emission;

	return scatter ? scatterDistance : -1.f;
}

Spectrum HomogeneousVolume::SigmaA(const HitPoint &hitPoint) const {
	return GetSigmaA().GetSpectrumValue(hitPoint).Clamp();
}

Spectrum HomogeneousVolume::SigmaS(const HitPoint &hitPoint) const {
	return GetSigmaS().GetSpectrumValue(hitPoint).Clamp();
}

float HomogeneousVolume::Scatter(const Ray &ray, const float u,
		const bool scatteredStart, Spectrum *connectionThroughput,
		Spectrum *connectionEmission) const {
	const float segmentLength = ray.maxt - ray.mint;

	// Check if I have to support multi-scattering
	const bool scatterAllowed = (!scatteredStart || multiScattering);

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	const Spectrum sigmaA = SigmaA(hitPoint);
	const Spectrum sigmaS = SigmaS(hitPoint);
	const Spectrum emission = Emission(hitPoint);

	Spectrum segmentTransmittance, segmentEmission;
	const float scatterDistance = HomogeneousVolume::Scatter(u, scatterAllowed,
			segmentLength, sigmaA, sigmaS, emission,
			segmentTransmittance, segmentEmission);

	// I need to update first connectionEmission and than connectionThroughput
	*connectionEmission += *connectionThroughput * emission;
	*connectionThroughput *= segmentTransmittance;

	return (scatterDistance == -1.f) ? -1.f : (ray.mint + scatterDistance);
}

float HomogeneousVolume::ScatterEquiangular(const Ray &ray, const float u,
		const bool scatteredStart, const std::vector<Point> &eqLightPoints,
		const std::vector<float> &eqLightLuminances,
		Spectrum *connectionThroughput,
		Spectrum *connectionEmission) const {
	const float segmentLength = ray.maxt - ray.mint;
	const bool scatterAllowed = (!scatteredStart || multiScattering);

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	const Spectrum sigmaA = SigmaA(hitPoint);
	const Spectrum sigmaS = SigmaS(hitPoint);
	const Spectrum emission = Emission(hitPoint);
	const float sigmaSValue = sigmaS.Filter();
	const Spectrum sigmaT = sigmaA + sigmaS;

	// Equiangular geometry requires a finite segment and at least one
	// eligible light
	const u_int lightCount = (u_int)eqLightPoints.size();
	if (!scatterAllowed || (sigmaSValue <= 0.f) || !isfinite(segmentLength) ||
			(segmentLength <= 0.f) || (lightCount == 0u))
		return Scatter(ray, u, scatteredStart, connectionThroughput, connectionEmission);

	TauswortheRandomGenerator rng(u);

	// Contribution-aware light selection: the glow mass of light i along
	// the segment is ~ lum_i * int 1/(D_i^2 + x^2) dx =
	// lum_i * (thetaB_i - thetaA_i) / D_i. Sampling proportionally to it
	// concentrates equiangular vertices where the illumination gradient is
	// steepest; the selection probability cancels in the conditioned MIS,
	// so any weight keeps the estimator unbiased.
	const float u1 = rng.floatValue();
	const auto weightOf = [&](const u_int i) -> float {
		const Vector toL(eqLightPoints[i] - ray.o);
		const float dlt = Dot(toL, ray.d);
		const float D = sqrtf(Max(1e-10f, toL.LengthSquared() - dlt * dlt));
		const float thA = atan2f(ray.mint - dlt, D);
		const float thB = atan2f(ray.maxt - dlt, D);
		return eqLightLuminances[i] * (thB - thA) / D;
	};
	float weightsSum = 0.f;
	for (u_int i = 0; i < lightCount; ++i)
		weightsSum += weightOf(i);
	u_int pick = lightCount - 1u;
	if (weightsSum > 0.f) {
		float acc = 0.f;
		for (u_int i = 0; i < lightCount; ++i) {
			acc += weightOf(i);
			if (acc >= u1 * weightsSum) {
				pick = i;
				break;
			}
		}
	} else
		pick = Min((u_int)(u1 * lightCount), lightCount - 1u);
	const Point &eqLightPos = eqLightPoints[pick];

	// delta: projection of the light on the ray (absolute t domain);
	// D: perpendicular distance of the light from the ray
	const Vector toLight(eqLightPos - ray.o);
	const float delta = Dot(toLight, ray.d);
	const float D = sqrtf(Max(1e-10f, toLight.LengthSquared() - delta * delta));
	const float thetaA = atan2f(ray.mint - delta, D);
	const float thetaB = atan2f(ray.maxt - delta, D);
	const float invThetaRange = 1.f / (thetaB - thetaA);

	// One-sample MIS between the equiangular (E) and transmittance (T)
	// strategies, conditioned on the already-picked light: the light
	// selection probability is shared by both strategies and cancels in
	// the balance heuristic, so it must not appear in the pdfs.
	float dist, pdf;
	bool collision;
	if (rng.floatValue() < .5f) {
		// Equiangular strategy: always produces a vertex in [mint, maxt]
		const float tAbs = delta + D * tanf(thetaA +
				(thetaB - thetaA) * rng.floatValue());
		// Guard against fp error pushing the vertex outside the segment
		dist = Clamp(tAbs - ray.mint, 0.f, segmentLength);
		const float pdfT = sigmaSValue * expf(-sigmaSValue * dist);
		const float pdfE = D * invThetaRange /
				(D * D + (tAbs - delta) * (tAbs - delta));
		pdf = .5f * (pdfT + pdfE);
		collision = true;
	} else {
		// Transmittance strategy (same sampler as Scatter())
		dist = -logf(1.f - rng.floatValue()) / sigmaSValue;
		collision = (dist < segmentLength);
		if (collision) {
			const float pdfT = sigmaSValue * expf(-sigmaSValue * dist);
			const float tAbs = ray.mint + dist;
			const float pdfE = D * invThetaRange /
					(D * D + (tAbs - delta) * (tAbs - delta));
			pdf = .5f * (pdfT + pdfE);
		} else {
			// The pass-through event belongs to the T strategy only:
			// the mixture probability is .5 * exp(-sigmaS * L)
			dist = segmentLength;
			pdf = .5f * expf(-sigmaSValue * segmentLength);
		}
	}

	// Same weighting convention as Scatter(): sigma_t * T(t) / pdf at a
	// collision, T(L) / pdf on pass-through
	Spectrum segmentTransmittance = Spectrum(1.f) / pdf;
	if (collision) {
		const Spectrum tau = dist * sigmaT;
		segmentTransmittance *= Exp(-tau) * sigmaT;
	} else {
		const Spectrum tau = segmentLength * sigmaT;
		segmentTransmittance *= Exp(-tau);
	}

	// I need to update first connectionEmission and than connectionThroughput
	*connectionEmission += *connectionThroughput * emission;
	*connectionThroughput *= segmentTransmittance;

	return collision ? (ray.mint + dist) : -1.f;
}

Spectrum HomogeneousVolume::TransmittanceEstimate(const Ray &ray, const float u) const {
	const float segmentLength = ray.maxt - ray.mint;

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	const Spectrum sigmaT = SigmaT(hitPoint);
	if (sigmaT.Black() || (segmentLength <= 0.f))
		return Spectrum(1.f);

	const Spectrum tau = (segmentLength * sigmaT).Clamp();
	return Exp(-tau);
}

Spectrum HomogeneousVolume::Albedo(const HitPoint &hitPoint) const {
	return schlickScatter.Albedo(hitPoint);
}

Spectrum HomogeneousVolume::Evaluate(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
		float *directPdfW, float *reversePdfW) const {
	return schlickScatter.Evaluate(hitPoint, localLightDir, localEyeDir, event, directPdfW, reversePdfW);
}

Spectrum HomogeneousVolume::Sample(const HitPoint &hitPoint,
		const Vector &localFixedDir, Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const {
	return schlickScatter.Sample(hitPoint, localFixedDir, localSampledDir,
			u0, u1, passThroughEvent, pdfW, event);
}

void HomogeneousVolume::Pdf(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
	schlickScatter.Pdf(hitPoint, localLightDir, localEyeDir, directPdfW, reversePdfW);
}

void HomogeneousVolume::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Volume::AddReferencedTextures(referencedTexs);

	GetSigmaA().AddReferencedTextures(referencedTexs);
	GetSigmaS().AddReferencedTextures(referencedTexs);
	schlickScatter.GetG().AddReferencedTextures(referencedTexs);
}

void HomogeneousVolume::UpdateTextureReferences(
	TextureConstRef oldTex, TextureRef newTex
) {
	Volume::UpdateTextureReferences(oldTex, newTex);

	updtex(sigmaA, oldTex, newTex);
	updtex(sigmaS, oldTex, newTex);
	if (&schlickScatter.GetG() == &oldTex)
		schlickScatter.SetG(newTex);
}

PropertiesUPtr HomogeneousVolume::ToProperties() const {
	PropertiesUPtr props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.volumes." + name + ".type")("homogeneous"));
	props->Set(Property("scene.volumes." + name + ".absorption")(GetSigmaA().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".scattering")(GetSigmaS().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".asymmetry")(schlickScatter.GetG().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".multiscattering")(multiScattering));
	props->Set(Property("scene.volumes." + name + ".phase")(schlickScatter.IsHGPhase() ? "hg" : "schlick"));
	props->Set(Property("scene.volumes." + name + ".distancesampling")(equiangular ? "equiangular" : "transmittance"));
	props->Set(Volume::ToProperties());

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
