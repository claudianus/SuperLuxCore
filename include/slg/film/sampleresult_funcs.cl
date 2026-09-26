#line 2 "sampleresult_funcs.cl"

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

OPENCL_FORCE_INLINE void SampleResult_ClearRadiance(__global SampleResult *sampleResult) {
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[0].c);
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[1].c);
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[2].c);
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[3].c);
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[4].c);
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[5].c);
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[6].c);
	VSTORE3F(BLACK, sampleResult->radiancePerPixelNormalized[7].c);
}

OPENCL_FORCE_INLINE void SampleResult_Init(__constant const Film* restrict film,
		__global SampleResult *sampleResult) {
	// Initialize only Spectrum fields

	SampleResult_ClearRadiance(sampleResult);

	VSTORE3F(BLACK, sampleResult->directDiffuse.c);
	VSTORE3F(BLACK, sampleResult->directDiffuseReflect.c);
	VSTORE3F(BLACK, sampleResult->directDiffuseTransmit.c);
	VSTORE3F(BLACK, sampleResult->directGlossy.c);
	VSTORE3F(BLACK, sampleResult->directGlossyReflect.c);
	VSTORE3F(BLACK, sampleResult->directGlossyTransmit.c);
	VSTORE3F(BLACK, sampleResult->emission.c);
	VSTORE3F(BLACK, sampleResult->indirectDiffuse.c);
	VSTORE3F(BLACK, sampleResult->indirectDiffuseReflect.c);
	VSTORE3F(BLACK, sampleResult->indirectDiffuseTransmit.c);
	VSTORE3F(BLACK, sampleResult->indirectGlossy.c);
	VSTORE3F(BLACK, sampleResult->indirectGlossyReflect.c);
	VSTORE3F(BLACK, sampleResult->indirectGlossyTransmit.c);
	VSTORE3F(BLACK, sampleResult->indirectSpecular.c);
	VSTORE3F(BLACK, sampleResult->indirectSpecularReflect.c);
	VSTORE3F(BLACK, sampleResult->indirectSpecularTransmit.c);
	sampleResult->rayCount = 0.f;
	VSTORE3F(BLACK, sampleResult->irradiance.c);
	VSTORE3F(BLACK, sampleResult->albedo.c);
	sampleResult->motionVector[0] = 0.f;
	sampleResult->motionVector[1] = 0.f;
	sampleResult->motionVector[2] = 0.f;
	sampleResult->motionVector[3] = 0.f;
	sampleResult->cryptoObjectID = 0.f;
	sampleResult->cryptoMaterialID = 0.f;

	sampleResult->firstPathVertexEvent = NONE;
	sampleResult->firstPathVertex = true;
	// sampleResult->lastPathVertex can not be really initialized here without knowing
	// the max. path depth.
	sampleResult->lastPathVertex = true;

	// Neutral spectral state; GenerateEyePath() overwrites it with the
	// drawn wavelengths on SLG_SPECTRAL builds.
	sampleResult->spectralW[0] = SLG_SPECTRAL_START;
	sampleResult->spectralW[1] = SLG_SPECTRAL_START + SLG_SPECTRAL_BIN_WIDTH;
	sampleResult->spectralW[2] = SLG_SPECTRAL_START + 2.f * SLG_SPECTRAL_BIN_WIDTH;
	sampleResult->spectralHeroAlive = SLG_SW_DEFAULT;
}

OPENCL_FORCE_INLINE void SampleResult_AddEmission(__constant const Film* restrict film,
		__global SampleResult *sampleResult, const uint lightID,
		const float3 pathThroughput, const float3 incomingRadiance) {
	const float3 radiance = pathThroughput * incomingRadiance;

	// Avoid out of bound access if the light group doesn't exist. This can happen
	// with RT modes.
	const uint id = min(lightID, film->radianceGroupCount - 1u);
	VADD3F(sampleResult->radiancePerPixelNormalized[id].c, radiance);

	if (sampleResult->firstPathVertex) {
		VADD3F(sampleResult->emission.c, radiance);
	} else {
		sampleResult->indirectShadowMask = 0.f;

		const BSDFEvent firstPathVertexEvent = sampleResult->firstPathVertexEvent;
		if ((firstPathVertexEvent & (DIFFUSE | REFLECT)) == (DIFFUSE | REFLECT)) {
			VADD3F(sampleResult->indirectDiffuseReflect.c, radiance);
		} else if ((firstPathVertexEvent & (DIFFUSE | TRANSMIT)) == (DIFFUSE | TRANSMIT)) {
			VADD3F(sampleResult->indirectDiffuseTransmit.c, radiance);
		} else if ((firstPathVertexEvent & (GLOSSY | REFLECT)) == (GLOSSY | REFLECT)) {
			VADD3F(sampleResult->indirectGlossyReflect.c, radiance);
		} else if ((firstPathVertexEvent & (GLOSSY | TRANSMIT)) == (GLOSSY | TRANSMIT)) {
			VADD3F(sampleResult->indirectGlossyTransmit.c, radiance);
		} else if ((firstPathVertexEvent & (SPECULAR | REFLECT)) == (SPECULAR | REFLECT)) {
			VADD3F(sampleResult->indirectSpecularReflect.c, radiance);
		} else if ((firstPathVertexEvent & (SPECULAR | TRANSMIT)) == (SPECULAR | TRANSMIT)) {
			VADD3F(sampleResult->indirectSpecularTransmit.c, radiance);
		}
	}
}

OPENCL_FORCE_INLINE void SampleResult_AddDirectLight(__constant const Film* restrict film,
		__global SampleResult *sampleResult, const uint lightID,
		const BSDFEvent bsdfEvent, const float3 pathThroughput, const float3 incomingRadiance,
		const float lightScale) {
	const float3 radiance = pathThroughput * incomingRadiance;

	// Avoid out of bound access if the light group doesn't exist. This can happen
	// with RT modes.
	const uint id = min(lightID, film->radianceGroupCount - 1u);
	VADD3F(sampleResult->radiancePerPixelNormalized[id].c, radiance);

	if (sampleResult->firstPathVertex) {
		sampleResult->directShadowMask = fmax(0.f, sampleResult->directShadowMask - lightScale);

		if ((bsdfEvent & (DIFFUSE | REFLECT)) == (DIFFUSE | REFLECT)) {
			VADD3F(sampleResult->directDiffuseReflect.c, radiance);
		} else if ((bsdfEvent & (DIFFUSE | TRANSMIT)) == (DIFFUSE | TRANSMIT)) {
			VADD3F(sampleResult->directDiffuseTransmit.c, radiance);
		} else if ((bsdfEvent & (GLOSSY | REFLECT)) == (GLOSSY | REFLECT)) {
			VADD3F(sampleResult->directGlossyReflect.c, radiance);
		} else if ((bsdfEvent & (GLOSSY | TRANSMIT)) == (GLOSSY | TRANSMIT)) {
			VADD3F(sampleResult->directGlossyTransmit.c, radiance);
		}
	} else {
		sampleResult->indirectShadowMask = fmax(0.f, sampleResult->indirectShadowMask - lightScale);

		const BSDFEvent firstPathVertexEvent = sampleResult->firstPathVertexEvent;
		if ((firstPathVertexEvent & (DIFFUSE | REFLECT)) == (DIFFUSE | REFLECT)) {
			VADD3F(sampleResult->indirectDiffuseReflect.c, radiance);
		} else if ((firstPathVertexEvent & (DIFFUSE | TRANSMIT)) == (DIFFUSE | TRANSMIT)) {
			VADD3F(sampleResult->indirectDiffuseTransmit.c, radiance);
		} else if ((firstPathVertexEvent & (GLOSSY | REFLECT)) == (GLOSSY | REFLECT)) {
			VADD3F(sampleResult->indirectGlossyReflect.c, radiance);
		} else if ((firstPathVertexEvent & (GLOSSY | TRANSMIT)) == (GLOSSY | TRANSMIT)) {
			VADD3F(sampleResult->indirectGlossyTransmit.c, radiance);
		} else if ((firstPathVertexEvent & (SPECULAR | REFLECT)) == (SPECULAR | REFLECT)) {
			VADD3F(sampleResult->indirectSpecularReflect.c, radiance);
		} else if ((firstPathVertexEvent & (SPECULAR | TRANSMIT)) == (SPECULAR | TRANSMIT)) {
			VADD3F(sampleResult->indirectSpecularTransmit.c, radiance);
		}

		VADD3F(sampleResult->irradiance.c, VLOAD3F(sampleResult->irradiancePathThroughput.c) * incomingRadiance);
	}
}

OPENCL_FORCE_INLINE float3 SampleResult_GetSpectrum(__constant const Film* restrict film,
		__global SampleResult *sampleResult,
		float3 filmRadianceGroupScale[FILM_MAX_RADIANCE_GROUP_COUNT]) {
	float3 c = BLACK;
	for (uint i = 0; i < film->radianceGroupCount; ++i)
		c += VLOAD3F(sampleResult->radiancePerPixelNormalized[i].c) * filmRadianceGroupScale[i];

	return c;
}

#if defined(SLG_SPECTRAL)
// Project every spectral color field of a SampleResult (wavelength bins)
// back to film RGB under the CIE matching functions -- the device mirror of
// CPU ProjectSampleResultToRGB(). Called once at film splat so all channels
// (radiance groups, light groups, denoiser inputs) receive RGB.
OPENCL_FORCE_INLINE void SampleResult_ProjectSpectralToRGB(
		__global SampleResult *sampleResult) {
	for (uint i = 0; i < FILM_MAX_RADIANCE_GROUP_COUNT; ++i)
		VSTORE3F(Spectral_ProjectToRGB(VLOAD3F(sampleResult->radiancePerPixelNormalized[i].c),
				sampleResult->spectralW, sampleResult->spectralHeroAlive),
				sampleResult->radiancePerPixelNormalized[i].c);
#define SLG_PROJECT_FIELD(f) \
		VSTORE3F(Spectral_ProjectToRGB(VLOAD3F(sampleResult->f.c), \
				sampleResult->spectralW, sampleResult->spectralHeroAlive), \
				sampleResult->f.c)
	SLG_PROJECT_FIELD(directDiffuse);
	SLG_PROJECT_FIELD(directDiffuseReflect);
	SLG_PROJECT_FIELD(directDiffuseTransmit);
	SLG_PROJECT_FIELD(directGlossy);
	SLG_PROJECT_FIELD(directGlossyReflect);
	SLG_PROJECT_FIELD(directGlossyTransmit);
	SLG_PROJECT_FIELD(emission);
	SLG_PROJECT_FIELD(indirectDiffuse);
	SLG_PROJECT_FIELD(indirectDiffuseReflect);
	SLG_PROJECT_FIELD(indirectDiffuseTransmit);
	SLG_PROJECT_FIELD(indirectGlossy);
	SLG_PROJECT_FIELD(indirectGlossyReflect);
	SLG_PROJECT_FIELD(indirectGlossyTransmit);
	SLG_PROJECT_FIELD(indirectSpecular);
	SLG_PROJECT_FIELD(indirectSpecularReflect);
	SLG_PROJECT_FIELD(indirectSpecularTransmit);
	SLG_PROJECT_FIELD(irradiance);
	SLG_PROJECT_FIELD(irradiancePathThroughput);
	SLG_PROJECT_FIELD(albedo);
#undef SLG_PROJECT_FIELD
}
#endif

OPENCL_FORCE_INLINE float SampleResult_GetRadianceY(__constant const Film* restrict film,
		__global SampleResult *sampleResult) {
	float y = 0.f;
	
	for (uint i = 0; i < film->radianceGroupCount; ++i)
		y += Spectrum_Y(VLOAD3F(sampleResult->radiancePerPixelNormalized[i].c));

	return y;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
