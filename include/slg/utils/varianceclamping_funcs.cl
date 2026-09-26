#line 2 "varianceclamping_funcs.cl"

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
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

// Adaptive Robust Clamping (ARC) - GPU twin of
// src/slg/utils/varianceclamping.cpp. See the C++ file for the algorithm
// description: 3x3-neighborhood median/MAD margin (DeCoro'10-style robust
// outlier statistics) + path-class scope (all/indirect/direct).

OPENCL_FORCE_INLINE void VarianceClamping_ScaledClamp3(__global float *value, const float low, const float high) {
	const float maxValue = fmax(value[0], fmax(value[1], value[2]));

	if (maxValue > 0.f) {
		if (maxValue > high) {
			const float scale = high / maxValue;

			value[0] *= scale;
			value[1] *= scale;
			value[2] *= scale;
			return;
		}

		if (maxValue < low) {
			const float scale = low / maxValue;

			value[0] *= scale;
			value[1] *= scale;
			value[2] *= scale;
			return;
		}
	}
}

OPENCL_FORCE_INLINE void VarianceClamping_Clamp3Margin(
		__global const float *expectedValue, __global float *value,
		const float margin) {
	if (expectedValue[3] > 0.f) {
		// Use the current pixel value as expected value
		const float invWeight = 1.f / expectedValue[3];

		const float minExpectedValue = fmin(expectedValue[0] * invWeight,
				fmin(expectedValue[1] * invWeight, expectedValue[2] * invWeight));
		const float maxExpectedValue = fmax(expectedValue[0] * invWeight,
				fmax(expectedValue[1] * invWeight, expectedValue[2] * invWeight));

		VarianceClamping_ScaledClamp3(value,
				fmax(minExpectedValue - margin, 0.f),
				maxExpectedValue + margin);
	} else
		VarianceClamping_ScaledClamp3(value, 0.f, margin);
}

OPENCL_FORCE_INLINE void VarianceClamping_Clamp3(const float sqrtVarianceClampMaxValue,
		__global const float *expectedValue, __global float *value) {
	VarianceClamping_Clamp3Margin(expectedValue, value, sqrtVarianceClampMaxValue);
}

// Insertion sort for the 9-element neighborhood luminance list
OPENCL_FORCE_INLINE void VarianceClamping_Sort9(__private float *v, const uint n) {
	for (uint i = 1; i < n; ++i) {
		const float t = v[i];
		int j = (int)i - 1;
		while (j >= 0 && v[j] > t) {
			v[j + 1] = v[j];
			--j;
		}
		v[j + 1] = t;
	}
}

// Luminance of the per-pixel mean over the 3x3 neighborhood of a weighted
// (rgb, weight) film channel. Neighbor coordinates are clamped to the film
// sub-region. Returns false when fewer than 3 pixels have weight > 0.
OPENCL_FORCE_INLINE bool VarianceClamping_RobustLumStats3x3(
		__global const float *buf,
		const uint x, const uint y,
		const uint filmWidth,
		const uint sub0, const uint sub1, const uint sub2, const uint sub3,
		__private float *med, __private float *mad, __private float *ownLum) {
	float v[9];
	uint n = 0;
	*ownLum = 0.f;

	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			const uint px = clamp((int)x + dx, (int)sub0, (int)sub1);
			const uint py = clamp((int)y + dy, (int)sub2, (int)sub3);
			__global const float *p = &buf[(px + py * filmWidth) * 4];

			if (p[3] > 0.f) {
				const float l = Spectrum_Y(MAKE_FLOAT3(p[0], p[1], p[2])) / p[3];
				v[n++] = l;
				if ((dx == 0) && (dy == 0))
					*ownLum = l;
			}
		}
	}

	if (n < 3)
		return false;

	VarianceClamping_Sort9(v, n);
	*med = v[n / 2];
	for (uint i = 0; i < n; ++i)
		v[i] = fabs(v[i] - *med);
	VarianceClamping_Sort9(v, n);
	*mad = v[n / 2];

	return true;
}

OPENCL_FORCE_INLINE void VarianceClamping_Clamp(
		__global SampleResult *sampleResult,
		const float sqrtVarianceClampMaxValue,
		const uint adaptive, const uint scope, const float sigma
		FILM_PARAM_DECL) {
	// Recover the current pixel value
	const int x = sampleResult->pixelX;
	const int y = sampleResult->pixelY;

	const uint index1 = x + y * filmWidth;
	const uint index4 = index1 * 4;

	// Adaptive margin from the 3x3 neighborhood of the first radiance group;
	// the own-pixel mean is always part of the expected value, matching the
	// legacy semantic bound mean +- margin.
	float ownLum = 0.f;
	{
		__global const float *own = &filmRadianceGroup[0][index4];
		if (own[3] > 0.f)
			ownLum = Spectrum_Y(MAKE_FLOAT3(own[0], own[1], own[2])) / own[3];
	}
	float med = 0.f, mad = 0.f, nLum = 0.f;
	const bool robust = adaptive &&
			VarianceClamping_RobustLumStats3x3(
					filmRadianceGroup[0], x, y, filmWidth,
					filmSubRegion0, filmSubRegion1, filmSubRegion2, filmSubRegion3,
					&med, &mad, &nLum);
	const float expEff = fmax(ownLum, med);
	const float margin = robust ?
			fmax(sigma * mad, sqrtVarianceClampMaxValue * (0.1f + expEff)) :
			sqrtVarianceClampMaxValue;

	if (scope == 0) {
		// CLAMP_ALL: legacy per-channel clamping (adaptive margin when enabled)

		// Apply variance clamping to each radiance group. This help to avoid problems
		// with extreme clamping settings and multiple light groups
		for (uint radianceGroupIndex = 0; radianceGroupIndex < film->radianceGroupCount; ++radianceGroupIndex) {
			VarianceClamping_Clamp3Margin(
					&((filmRadianceGroup[radianceGroupIndex])[index4]),
					sampleResult->radiancePerPixelNormalized[radianceGroupIndex].c,
					margin);
		}

		// Clamp the AOVs too

		// DIRECT_DIFFUSE

		if (film->hasChannelDirectDiffuseReflect)
			VarianceClamping_Clamp3Margin(&filmDirectDiffuseReflect[index4], sampleResult->directDiffuseReflect.c, margin);
		else if (film->hasChannelDirectDiffuse)
			VarianceClamping_Clamp3Margin(&filmDirectDiffuse[index4], sampleResult->directDiffuseReflect.c, margin);

		if (film->hasChannelDirectDiffuseTransmit)
			VarianceClamping_Clamp3Margin(&filmDirectDiffuseTransmit[index4], sampleResult->directDiffuseTransmit.c, margin);
		else if (film->hasChannelDirectDiffuse)
			VarianceClamping_Clamp3Margin(&filmDirectDiffuse[index4], sampleResult->directDiffuseTransmit.c, margin);

		// DIRECT_GLOSSY

		if (film->hasChannelDirectGlossyReflect)
			VarianceClamping_Clamp3Margin(&filmDirectGlossyReflect[index4], sampleResult->directGlossyReflect.c, margin);
		else if (film->hasChannelDirectGlossy)
			VarianceClamping_Clamp3Margin(&filmDirectGlossy[index4], sampleResult->directGlossyReflect.c, margin);

		if (film->hasChannelDirectGlossyTransmit)
			VarianceClamping_Clamp3Margin(&filmDirectGlossyTransmit[index4], sampleResult->directGlossyTransmit.c, margin);
		else if (film->hasChannelDirectGlossy)
			VarianceClamping_Clamp3Margin(&filmDirectGlossy[index4], sampleResult->directGlossyTransmit.c, margin);

		// EMISSION

		if (film->hasChannelEmission)
			VarianceClamping_Clamp3Margin(&filmEmission[index4], sampleResult->emission.c, margin);

		// INDIRECT_DIFFUSE

		if (film->hasChannelIndirectDiffuseReflect)
			VarianceClamping_Clamp3Margin(&filmIndirectDiffuseReflect[index4], sampleResult->indirectDiffuseReflect.c, margin);
		else if (film->hasChannelIndirectDiffuse)
			VarianceClamping_Clamp3Margin(&filmIndirectDiffuse[index4], sampleResult->indirectDiffuseReflect.c, margin);

		if (film->hasChannelIndirectDiffuseTransmit)
			VarianceClamping_Clamp3Margin(&filmIndirectDiffuseTransmit[index4], sampleResult->indirectDiffuseTransmit.c, margin);
		else if (film->hasChannelIndirectDiffuse)
			VarianceClamping_Clamp3Margin(&filmIndirectDiffuse[index4], sampleResult->indirectDiffuseTransmit.c, margin);

		// INDIRECT_GLOSSY

		if (film->hasChannelIndirectGlossyReflect)
			VarianceClamping_Clamp3Margin(&filmIndirectGlossyReflect[index4], sampleResult->indirectGlossyReflect.c, margin);
		else if (film->hasChannelIndirectGlossy)
			VarianceClamping_Clamp3Margin(&filmIndirectGlossy[index4], sampleResult->indirectGlossyReflect.c, margin);

		if (film->hasChannelIndirectGlossyTransmit)
			VarianceClamping_Clamp3Margin(&filmIndirectGlossyTransmit[index4], sampleResult->indirectGlossyTransmit.c, margin);
		else if (film->hasChannelIndirectGlossy)
			VarianceClamping_Clamp3Margin(&filmIndirectGlossy[index4], sampleResult->indirectGlossyTransmit.c, margin);

		// INDIRECT_SPECULAR

		if (film->hasChannelIndirectSpecularReflect)
			VarianceClamping_Clamp3Margin(&filmIndirectSpecularReflect[index4], sampleResult->indirectSpecularReflect.c, margin);
		else if (film->hasChannelIndirectSpecular)
			VarianceClamping_Clamp3Margin(&filmIndirectSpecular[index4], sampleResult->indirectSpecularReflect.c, margin);

		if (film->hasChannelIndirectSpecularTransmit)
			VarianceClamping_Clamp3Margin(&filmIndirectSpecularTransmit[index4], sampleResult->indirectSpecularTransmit.c, margin);
		else if (film->hasChannelIndirectSpecular)
			VarianceClamping_Clamp3Margin(&filmIndirectSpecular[index4], sampleResult->indirectSpecularTransmit.c, margin);
	} else {
		// Partial scope (CLAMP_INDIRECT / CLAMP_DIRECT): decompose the sample
		// into direct-ish (emission + first-vertex direct light) and indirect
		// energy, clamp only the selected class. The beauty loses exactly the
		// removed share so beauty and AOVs stay consistent.
		const float3 directish =
				VLOAD3F(sampleResult->emission.c) +
				VLOAD3F(sampleResult->directDiffuseReflect.c) +
				VLOAD3F(sampleResult->directDiffuseTransmit.c) +
				VLOAD3F(sampleResult->directGlossyReflect.c) +
				VLOAD3F(sampleResult->directGlossyTransmit.c);
		float3 total = 0.f;
		for (uint i = 0; i < film->radianceGroupCount; ++i)
			total += VLOAD3F(sampleResult->radiancePerPixelNormalized[i].c);

		const float totalY = Spectrum_Y(total);
		const float directY = Spectrum_Y(directish);
		const float indirectY = fmax(totalY - directY, 0.f);
		const float T = expEff + margin;

		if (scope == 1) {
			// CLAMP_INDIRECT
			const float sInd = (indirectY > 0.f) ? fmin(1.f, T / indirectY) : 1.f;

			if (sInd < 1.f) {
				VSTORE3F(VLOAD3F(sampleResult->indirectDiffuseReflect.c) * sInd, sampleResult->indirectDiffuseReflect.c);
				VSTORE3F(VLOAD3F(sampleResult->indirectDiffuseTransmit.c) * sInd, sampleResult->indirectDiffuseTransmit.c);
				VSTORE3F(VLOAD3F(sampleResult->indirectGlossyReflect.c) * sInd, sampleResult->indirectGlossyReflect.c);
				VSTORE3F(VLOAD3F(sampleResult->indirectGlossyTransmit.c) * sInd, sampleResult->indirectGlossyTransmit.c);
				VSTORE3F(VLOAD3F(sampleResult->indirectSpecularReflect.c) * sInd, sampleResult->indirectSpecularReflect.c);
				VSTORE3F(VLOAD3F(sampleResult->indirectSpecularTransmit.c) * sInd, sampleResult->indirectSpecularTransmit.c);
				VSTORE3F(VLOAD3F(sampleResult->irradiance.c) * sInd, sampleResult->irradiance.c);

				// Group 0: exact decomposition (directish untouched);
				// other groups: proportional scale.
				const float excess = indirectY * (1.f - sInd);
				float3 indShare = VLOAD3F(sampleResult->radiancePerPixelNormalized[0].c) - directish;
				indShare = fmax(indShare, 0.f);
				VSTORE3F(directish + indShare * sInd, sampleResult->radiancePerPixelNormalized[0].c);

				if (film->radianceGroupCount > 1) {
					const float sB = (totalY > 0.f) ? (totalY - excess) / totalY : 1.f;
					for (uint i = 1; i < film->radianceGroupCount; ++i)
						VSTORE3F(VLOAD3F(sampleResult->radiancePerPixelNormalized[i].c) * sB,
								sampleResult->radiancePerPixelNormalized[i].c);
				}
			}
		} else {
			// CLAMP_DIRECT
			const float sDir = (directY > 0.f) ? fmin(1.f, T / directY) : 1.f;

			if (sDir < 1.f) {
				VSTORE3F(VLOAD3F(sampleResult->emission.c) * sDir, sampleResult->emission.c);
				VSTORE3F(VLOAD3F(sampleResult->directDiffuseReflect.c) * sDir, sampleResult->directDiffuseReflect.c);
				VSTORE3F(VLOAD3F(sampleResult->directDiffuseTransmit.c) * sDir, sampleResult->directDiffuseTransmit.c);
				VSTORE3F(VLOAD3F(sampleResult->directGlossyReflect.c) * sDir, sampleResult->directGlossyReflect.c);
				VSTORE3F(VLOAD3F(sampleResult->directGlossyTransmit.c) * sDir, sampleResult->directGlossyTransmit.c);

				const float excess = directY * (1.f - sDir);
				VSTORE3F(directish * sDir +
						(VLOAD3F(sampleResult->radiancePerPixelNormalized[0].c) - directish),
						sampleResult->radiancePerPixelNormalized[0].c);

				if (film->radianceGroupCount > 1) {
					const float sB = (totalY > 0.f) ? (totalY - excess) / totalY : 1.f;
					for (uint i = 1; i < film->radianceGroupCount; ++i)
						VSTORE3F(VLOAD3F(sampleResult->radiancePerPixelNormalized[i].c) * sB,
								sampleResult->radiancePerPixelNormalized[i].c);
				}
			}
		}
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
