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

#include <memory>

#include "luxrays/usings.h"
#include "slg/bsdf/bsdf.h"
#include "slg/scene/scene.h"
#include "slg/usings.h"
#include "slg/lights/infinitelight.h"

using namespace std;
using namespace luxrays;
using namespace slg;
		
//------------------------------------------------------------------------------
// InfiniteLight
//------------------------------------------------------------------------------

InfiniteLight::InfiniteLight() :
	imageMap(nullptr), cdfMaxDim(4096), imageMapDistribution(nullptr), visibilityMapCache(nullptr) {
}

InfiniteLight::~InfiniteLight() {
}

void InfiniteLight::Preprocess() {
	EnvLightSource::Preprocess();

	auto& imageMapStorage = imageMap->GetStorage();

	const u_int imgW = imageMap->GetWidth();
	const u_int imgH = imageMap->GetHeight();

	// Cap the importance-sampling resolution: a 16k HDRI would need a
	// ~1GB CDF (nv row distributions of 2*nu floats). Block-summing to
	// cdfMaxDim preserves the total mass per cell, so sampling stays
	// unbiased (the pdf is consistent with the sampled distribution);
	// only the variance of tiny hot spots grows slightly.
	const u_int maxDim = Max(imgW, imgH);
	const u_int decim = cdfMaxDim ? (maxDim + cdfMaxDim - 1) / cdfMaxDim : 1;
	const u_int distW = (imgW + decim - 1) / decim;
	const u_int distH = (imgH + decim - 1) / decim;
	if (decim > 1)
		SLG_LOG("InfiniteLight: downsampling importance CDF " <<
			imgW << "x" << imgH << " -> " << distW << "x" << distH);

	std::vector<float> data(distW * distH, 0.f);
	for (u_int y = 0; y < imgH; ++y) {
		const u_int dy = Min(y / decim, distH - 1);
		const bool upper = sampleUpperHemisphereOnly && (y > imgH / 2);
		for (u_int x = 0; x < imgW; ++x) {
			const float v = upper ? 0.f : imageMapStorage.GetFloat(x + y * imgW);
			if (!IsValid(v))
				throw runtime_error("Pixel (" + ToString(x) + ", " + ToString(y) + ") in infinite light has an invalid value: " + ToString(v));
			data[Min(x / decim, distW - 1) + dy * distW] += v;
		}
	}

	imageMapDistribution =
		std::make_unique<Distribution2D>(
			data,
			distW,
			distH
		);
}


std::tuple<Distribution2DRef, EnvLightVisibilityCacheRPtr>
InfiniteLight::GetPreprocessedData() const {
	return std::make_tuple(std::ref(*imageMapDistribution), std::cref(visibilityMapCache));
}

float InfiniteLight::GetPower(SceneConstRef scene) const {
	const float envRadius = GetEnvRadius(scene);

	// TODO: I should consider sampleUpperHemisphereOnly here
	return temperatureScale.Y() * gain.Y() * imageMap->GetSpectrumMeanY() *
			(4.f * M_PI * M_PI * envRadius * envRadius);
}

UV InfiniteLight::GetEnvUV(const luxrays::Vector &dir) const {
	UV uv;
	const Vector localDir = Normalize(Inverse(lightToWorld) * -dir);
	ToLatLongMapping(localDir, &uv.u, &uv.v);
	
	return uv;
}

Spectrum InfiniteLight::GetRadiance(SceneConstRef scene,
		const BSDF *bsdf, const Vector &dir,
		float *directPdfA, float *emissionPdfW) const {
	const Vector localDir = Normalize(Inverse(lightToWorld) * -dir);

	float u, v, latLongMappingPdf;
	ToLatLongMapping(localDir, &u, &v, &latLongMappingPdf);
	if (latLongMappingPdf == 0.f)
		return Spectrum();

	const float distPdf = imageMapDistribution->Pdf(u, v);
	if (directPdfA) {
		if (!bsdf)
			*directPdfA = 0.f;
		else if (visibilityMapCache && visibilityMapCache->IsCacheEnabled(*bsdf)) {
			*directPdfA = visibilityMapCache->Pdf(*bsdf, u, v) * latLongMappingPdf;
		} else
			*directPdfA = distPdf * latLongMappingPdf;
	}

	if (emissionPdfW) {
		const float envRadius = GetEnvRadius(scene);
		*emissionPdfW = distPdf * latLongMappingPdf / (M_PI * envRadius * envRadius);
	}

	return Spectral::Emission(temperatureScale * gain * imageMap->GetSpectrum(UV(u, v)));
}

Spectrum InfiniteLight::Emit(SceneConstRef scene,
		const float time, const float u0, const float u1,
		const float u2, const float u3, const float passThroughEvent,
		Ray &ray, float &emissionPdfW,
		float *directPdfA, float *cosThetaAtLight) const {
	float uv[2];
	float distPdf;
	imageMapDistribution->SampleContinuous(u0, u1, uv, &distPdf);
	if (distPdf == 0.f)
		return Spectrum();
	
	Vector localDir;
	float latLongMappingPdf;
	FromLatLongMapping(uv[0], uv[1], &localDir, &latLongMappingPdf);
	if (latLongMappingPdf == 0.f)
		return Spectrum();

	// Compute the ray direction
	const Vector rayDir = -Normalize(lightToWorld * localDir);

	// Compute the ray origin
	Vector x, y;
    CoordinateSystem(-rayDir, &x, &y);
    float d1, d2;
    ConcentricSampleDisk(u2, u3, &d1, &d2);

	const Point worldCenter = scene.GetDataSet().GetBSphere().center;
	const float envRadius = GetEnvRadius(scene);
	const Point pDisk = worldCenter + envRadius * (d1 * x + d2 * y);
	const Point rayOrig = pDisk - envRadius * rayDir;

	// Compute InfiniteLight ray weight
	emissionPdfW = distPdf * latLongMappingPdf / (M_PI * envRadius * envRadius);

	if (directPdfA)
		*directPdfA = distPdf * latLongMappingPdf;

	if (cosThetaAtLight)
		*cosThetaAtLight = Dot(Normalize(worldCenter - rayOrig), rayDir);

	const Spectrum result = temperatureScale * gain * imageMap->GetSpectrum(uv);
	assert (!result.IsNaN() && !result.IsInf() && !result.IsNeg());

	ray.Update(rayOrig, rayDir, time);

	return result;
}

Spectrum InfiniteLight::Illuminate(SceneConstRef scene, const BSDF &bsdf,
		const float time, const float u0, const float u1, const float passThroughEvent,
        Ray &shadowRay, float &directPdfW,
		float *emissionPdfW, float *cosThetaAtLight) const {
	float uv[2];
	float distPdf;	
	if (visibilityMapCache && visibilityMapCache->IsCacheEnabled(bsdf))
		visibilityMapCache->Sample(bsdf, u0, u1, uv, &distPdf);
	else
		imageMapDistribution->SampleContinuous(u0, u1, uv, &distPdf);
	if (distPdf == 0.f)
		return Spectrum();

	Vector localDir;
	float latLongMappingPdf;
	FromLatLongMapping(uv[0], uv[1], &localDir, &latLongMappingPdf);
	if (latLongMappingPdf == 0.f)
		return Spectrum();

	const Vector shadowRayDir = Normalize(lightToWorld * localDir);
	
	const Point worldCenter = scene.GetDataSet().GetBSphere().center;
	const float envRadius = GetEnvRadius(scene);

	const Point shadowRayOrig = bsdf.GetRayOrigin(shadowRayDir);
	const Vector toCenter(worldCenter - shadowRayOrig);
	const float centerDistanceSquared = Dot(toCenter, toCenter);
	const float approach = Dot(toCenter, shadowRayDir);
	const float shadowRayDistance = approach + sqrtf(Max(0.f, envRadius * envRadius -
		centerDistanceSquared + approach * approach));

	const Point emisPoint(shadowRayOrig + shadowRayDistance * shadowRayDir);
	const Normal emisNormal(Normalize(worldCenter - emisPoint));

	const float cosAtLight = Dot(emisNormal, -shadowRayDir);
	if (cosAtLight < DEFAULT_COS_EPSILON_STATIC)
		return Spectrum();
	if (cosThetaAtLight)
		*cosThetaAtLight = cosAtLight;

	directPdfW = distPdf * latLongMappingPdf;
	assert (!isnan(directPdfW) && !isinf(directPdfW) && (directPdfW > 0.f));

	if (emissionPdfW)
		*emissionPdfW = distPdf * latLongMappingPdf / (M_PI * envRadius * envRadius);

	const Spectrum result = temperatureScale * gain * imageMap->GetSpectrum(UV(uv[0], uv[1]));
	assert (!result.IsNaN() && !result.IsInf() && !result.IsNeg());

	shadowRay = Ray(shadowRayOrig, shadowRayDir, 0.f, shadowRayDistance, time);

	return result;
}

void InfiniteLight::UpdateVisibilityMap(SceneConstRef scene, const bool useRTMode) {
	visibilityMapCache.reset();

	if (useRTMode)
		return;

	if (useVisibilityMapCache) {
		// Scale the infinitelight image map to the requested size
		ImageMapUPtr luminanceMapImage(imageMap->Copy());
		// Select the image luminance
		luminanceMapImage->SelectChannel(ImageMapStorage::WEIGHTED_MEAN);
		luminanceMapImage->Preprocess();

		visibilityMapCache = std::make_unique<EnvLightVisibilityCache>(
			scene, this, std::move(luminanceMapImage), visibilityMapCacheParams
		);
		visibilityMapCache->Build();
	}
}

PropertiesUPtr InfiniteLight::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	const string prefix = "scene.lights." + GetName();
	PropertiesUPtr props = EnvLightSource::ToProperties(imgMapCache, useRealFileName);

	props->Set(Property(prefix + ".type")("infinite"));
	const string fileName = useRealFileName ?
		imageMap->GetName() : imgMapCache.GetSequenceFileName(*imageMap);
	props->Set(Property(prefix + ".file")(fileName));
	props->Set(imageMap->ToProperties(prefix, false));
	props->Set(Property(prefix + ".gamma")(1.f));
	props->Set(Property(prefix + ".sampleupperhemisphereonly")(sampleUpperHemisphereOnly));
	props->Set(Property(prefix + ".cdfdim")(cdfMaxDim));

	props->Set(Property(prefix + ".visibilitymapcache.enable")(useVisibilityMapCache));
	if (useVisibilityMapCache)
		*props << EnvLightVisibilityCache::Params2Props(prefix, visibilityMapCacheParams);

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
