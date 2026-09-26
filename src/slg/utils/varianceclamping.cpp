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

#include "slg/utils/varianceclamping.h"
#include "slg/film/film.h"
#include "slg/film/sampleresult.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// VarianceClamping
//------------------------------------------------------------------------------

VarianceClamping::VarianceClamping() :
		sqrtVarianceClampMaxValue(0.f), varianceClampAdaptive(1),
		varianceClampScope(CLAMP_INDIRECT), varianceClampSigma(6.f) {
}

VarianceClamping::VarianceClamping(const float sqrtMaxValue,
		const bool adaptive, const int scope, const float sigma) :
		sqrtVarianceClampMaxValue(sqrtMaxValue), varianceClampAdaptive(adaptive ? 1 : 0),
		varianceClampScope(scope), varianceClampSigma(sigma) {
}

//------------------------------------------------------------------------------
// Robust spatial statistics
//
// Luminance of the per-pixel mean over the 3x3 neighborhood, read straight
// from the film channel buffer (no extra accumulation buffers needed).
// Neighbor coordinates are clamped to the film sub-region, so exactly 9
// reads are performed and the same code works for CPU and GPU kernels.
//------------------------------------------------------------------------------

static inline float LumOf(const float r, const float g, const float b) {
	return 0.212671f * r + 0.715160f * g + 0.072169f * b;
}

static inline void Sort9(float *v, const u_int n) {
	for (u_int i = 1; i < n; ++i) {
		const float t = v[i];
		int j = (int)i - 1;
		while (j >= 0 && v[j] > t) {
			v[j + 1] = v[j];
			--j;
		}
		v[j + 1] = t;
	}
}

// Weighted (rgb, weight) channel: mean = sum / weight
static bool RobustLumStats3x3(const GenericFrameBuffer<4, 1, float> *buf,
		const u_int x, const u_int y, const u_int *subRegion,
		float &med, float &mad, float &ownLum) {
	float v[9];
	u_int n = 0;
	ownLum = 0.f;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			const u_int px = luxrays::Clamp((int)x + dx, (int)subRegion[0], (int)subRegion[1]);
			const u_int py = luxrays::Clamp((int)y + dy, (int)subRegion[2], (int)subRegion[3]);
			const float *p = buf->GetPixel(px, py);

			if (p[3] > 0.f) {
				const float l = LumOf(p[0], p[1], p[2]) / p[3];
				v[n++] = l;
				if ((dx == 0) && (dy == 0))
					ownLum = l;
			}
		}
	}

	if (n < 3)
		return false;

	Sort9(v, n);
	med = v[n / 2];
	for (u_int i = 0; i < n; ++i)
		v[i] = fabs(v[i] - med);
	Sort9(v, n);
	mad = v[n / 2];

	return true;
}

// Unweighted (rgb) channel (PER_SCREEN splats): mean approximated by
// sum * scale (scale = 1 / lightSampleCount). A pixel is valid when its
// scaled luminance is > 0.
static bool RobustLumStats3x3Unweighted(const GenericFrameBuffer<3, 0, float> *buf,
		const u_int x, const u_int y, const u_int *subRegion, const float scale,
		float &med, float &mad, float &ownLum) {
	float v[9];
	u_int n = 0;
	ownLum = 0.f;
	for (int dy = -1; dy <= 1; ++dy) {
		for (int dx = -1; dx <= 1; ++dx) {
			const u_int px = luxrays::Clamp((int)x + dx, (int)subRegion[0], (int)subRegion[1]);
			const u_int py = luxrays::Clamp((int)y + dy, (int)subRegion[2], (int)subRegion[3]);
			const float *p = buf->GetPixel(px, py);

			const float l = LumOf(p[0], p[1], p[2]) * scale;
			if (l > 0.f) {
				v[n++] = l;
				if ((dx == 0) && (dy == 0))
					ownLum = l;
			}
		}
	}

	if (n < 3)
		return false;

	Sort9(v, n);
	med = v[n / 2];
	for (u_int i = 0; i < n; ++i)
		v[i] = fabs(v[i] - med);
	Sort9(v, n);
	mad = v[n / 2];

	return true;
}

//------------------------------------------------------------------------------

static void ScaledClamp3(float value[3], const float low, const float high) {
	const float maxValue = Max(value[0], Max(value[1], value[2]));

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

// Clamp3 with an explicit margin (adaptive path); Clamp3 keeps the
// legacy sqrtVarianceClampMaxValue margin for ClampFilm and
// non-adaptive configurations.
void VarianceClamping::Clamp3Margin(const float expectedValue[4], float value[3],
		const float margin) const {
	if (expectedValue[3] > 0.f) {
		const float invWeight = 1.f / expectedValue[3];

		const float minExpectedValue = Min(expectedValue[0] * invWeight,
				Min(expectedValue[1] * invWeight, expectedValue[2] * invWeight));
		const float maxExpectedValue = Max(expectedValue[0] * invWeight,
				Max(expectedValue[1] * invWeight, expectedValue[2] * invWeight));

		ScaledClamp3(value,
				Max(minExpectedValue - margin, 0.f),
				maxExpectedValue + margin);
	} else
		ScaledClamp3(value, 0.f, margin);
}

void VarianceClamping::Clamp3(const float expectedValue[4], float value[3]) const {
	Clamp3Margin(expectedValue, value, sqrtVarianceClampMaxValue);
}

//------------------------------------------------------------------------------

static void ScaledClamp4(float value[4], const float low, const float high) {
	// I have already checked inside Clamp() that value[3] > 0.f
	const float invWeight = 1.f / value[3];

	const float maxValue = Max(value[0] * invWeight, Max(value[1] * invWeight, value[2] * invWeight));

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

void VarianceClamping::Clamp4(const float expectedValue[4], float value[4]) const {
	if (value[3] <= 0.f)
		return;

	if (expectedValue[3] > 0.f) {
		// Use the current pixel value as expected value
		const float invWeight = 1.f / expectedValue[3];

		const float minExpectedValue = Min(expectedValue[0] * invWeight,
				Min(expectedValue[1] * invWeight, expectedValue[2] * invWeight));
		const float maxExpectedValue = Max(expectedValue[0] * invWeight,
				Max(expectedValue[1] * invWeight, expectedValue[2] * invWeight));

		ScaledClamp4(value,
				Max(minExpectedValue - sqrtVarianceClampMaxValue, 0.f),
				maxExpectedValue + sqrtVarianceClampMaxValue);
	} else
		ScaledClamp4(value, 0.f, sqrtVarianceClampMaxValue);
}

//------------------------------------------------------------------------------

void VarianceClamping::ClampFilm(Film &dstFilm , const Film &srcFilm,
		const u_int srcOffsetX, const u_int srcOffsetY,
		const u_int srcWidth, const u_int srcHeight,
		const u_int dstOffsetX, const u_int dstOffsetY) const {
	if (dstFilm.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED) && srcFilm.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED)) {
		for (u_int i = 0; i < Min(dstFilm.GetRadianceGroupCount(), srcFilm.GetRadianceGroupCount()); ++i) {
			for (u_int y = 0; y < srcHeight; ++y) {
				for (u_int x = 0; x < srcWidth; ++x) {
					float *srcPixel = srcFilm.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[i]->GetPixel(srcOffsetX + x, srcOffsetY + y);
					const float *dstPixel = dstFilm.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[i]->GetPixel(dstOffsetX + x, dstOffsetY + y);

					Clamp4(dstPixel, srcPixel);
				}
			}
		}
	}
}

void VarianceClamping::ClampFilm(Film &dstFilm , const Film &srcFilm) const {
	ClampFilm(dstFilm, srcFilm, 0, 0, dstFilm.GetWidth(), dstFilm.GetHeight(), 0, 0);
}

//------------------------------------------------------------------------------

void VarianceClamping::Clamp(const Film &film, SampleResult &sampleResult) const {
	// Recover the current pixel value
	u_int x, y;
	if (sampleResult.useFilmSplat) {
		x = Floor2UInt(sampleResult.filmX);
		y = Floor2UInt(sampleResult.filmY);
	} else {
		x = sampleResult.pixelX;
		y = sampleResult.pixelY;
	}

	// A safety net to avoid out of bound accesses to Film channels
	const u_int *subRegion = film.GetSubRegion();
	x = luxrays::Clamp(x, subRegion[0], subRegion[1]);
	y = luxrays::Clamp(y, subRegion[2], subRegion[3]);

	if (sampleResult.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED)) {
		// Adaptive margin from the 3x3 neighborhood of the first radiance
		// group channel: robust statistics (median + MAD) estimate the local
		// expected value E and the local scale. The margin
		//     max(sigma * mad, sqrtMax * (0.1 + E))
		// is tight in flat/dark regions (kills fireflies) and relaxes in
		// spatially coherent bright regions (protects highlights/caustics).
		// The own-pixel mean is always part of the expected value, matching
		// the legacy semantic bound mean +- margin.
		float ownLum = 0.f;
		{
			const float *own = film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[0]->GetPixel(x, y);
			if (own[3] > 0.f)
				ownLum = LumOf(own[0], own[1], own[2]) / own[3];
		}
		float med = 0.f, mad = 0.f, nLum = 0.f;
		const bool robust = varianceClampAdaptive &&
				RobustLumStats3x3(film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[0].get(),
						x, y, subRegion, med, mad, nLum);
		const float expEff = Max(ownLum, med);
		const float margin = robust ?
				Max(varianceClampSigma * mad,
						sqrtVarianceClampMaxValue * (0.1f + expEff)) :
				sqrtVarianceClampMaxValue;

		if (varianceClampScope == CLAMP_ALL) {
			// Legacy per-channel clamping (with the adaptive margin when enabled)
			for (u_int radianceGroupIndex = 0; radianceGroupIndex < sampleResult.radiance.Size(); ++radianceGroupIndex) {
				Clamp3Margin(film.channel_RADIANCE_PER_PIXEL_NORMALIZEDs[radianceGroupIndex]->GetPixel(x, y),
						sampleResult.radiance[radianceGroupIndex].c, margin);
			}

			// Clamp the AOVs too

			// DIRECT_DIFFUSE

			if (film.HasChannel(Film::DIRECT_DIFFUSE_REFLECT))
				Clamp3Margin(film.channel_DIRECT_DIFFUSE_REFLECT->GetPixel(x, y), sampleResult.directDiffuseReflect.c, margin);
			else if (film.HasChannel(Film::DIRECT_DIFFUSE))
				Clamp3Margin(film.channel_DIRECT_DIFFUSE->GetPixel(x, y), sampleResult.directDiffuseReflect.c, margin);

			if (film.HasChannel(Film::DIRECT_DIFFUSE_TRANSMIT))
				Clamp3Margin(film.channel_DIRECT_DIFFUSE_TRANSMIT->GetPixel(x, y), sampleResult.directDiffuseTransmit.c, margin);
			else if (film.HasChannel(Film::DIRECT_DIFFUSE))
				Clamp3Margin(film.channel_DIRECT_DIFFUSE->GetPixel(x, y), sampleResult.directDiffuseTransmit.c, margin);

			// DIRECT_GLOSSY

			if (film.HasChannel(Film::DIRECT_GLOSSY_REFLECT))
				Clamp3Margin(film.channel_DIRECT_GLOSSY_REFLECT->GetPixel(x, y), sampleResult.directGlossyReflect.c, margin);
			else if (film.HasChannel(Film::DIRECT_GLOSSY))
				Clamp3Margin(film.channel_DIRECT_GLOSSY->GetPixel(x, y), sampleResult.directGlossyReflect.c, margin);

			if (film.HasChannel(Film::DIRECT_GLOSSY_TRANSMIT))
				Clamp3Margin(film.channel_DIRECT_GLOSSY_TRANSMIT->GetPixel(x, y), sampleResult.directGlossyTransmit.c, margin);
			else if (film.HasChannel(Film::DIRECT_GLOSSY))
				Clamp3Margin(film.channel_DIRECT_GLOSSY->GetPixel(x, y), sampleResult.directGlossyTransmit.c, margin);

			// EMISSION

			if (film.HasChannel(Film::EMISSION))
				Clamp3Margin(film.channel_EMISSION->GetPixel(x, y), sampleResult.emission.c, margin);

			// INDIRECT_DIFFUSE

			if (film.HasChannel(Film::INDIRECT_DIFFUSE_REFLECT))
				Clamp3Margin(film.channel_INDIRECT_DIFFUSE_REFLECT->GetPixel(x, y), sampleResult.indirectDiffuseReflect.c, margin);
			else if (film.HasChannel(Film::INDIRECT_DIFFUSE))
				Clamp3Margin(film.channel_INDIRECT_DIFFUSE->GetPixel(x, y), sampleResult.indirectDiffuseReflect.c, margin);

			if (film.HasChannel(Film::INDIRECT_DIFFUSE_TRANSMIT))
				Clamp3Margin(film.channel_INDIRECT_DIFFUSE_TRANSMIT->GetPixel(x, y), sampleResult.indirectDiffuseTransmit.c, margin);
			else if (film.HasChannel(Film::INDIRECT_DIFFUSE))
				Clamp3Margin(film.channel_INDIRECT_DIFFUSE->GetPixel(x, y), sampleResult.indirectDiffuseTransmit.c, margin);

			// INDIRECT_GLOSSY

			if (film.HasChannel(Film::INDIRECT_GLOSSY_REFLECT))
				Clamp3Margin(film.channel_INDIRECT_GLOSSY_REFLECT->GetPixel(x, y), sampleResult.indirectGlossyReflect.c, margin);
			else if (film.HasChannel(Film::INDIRECT_GLOSSY))
				Clamp3Margin(film.channel_INDIRECT_GLOSSY->GetPixel(x, y), sampleResult.indirectGlossyReflect.c, margin);

			if (film.HasChannel(Film::INDIRECT_GLOSSY_TRANSMIT))
				Clamp3Margin(film.channel_INDIRECT_GLOSSY_TRANSMIT->GetPixel(x, y), sampleResult.indirectGlossyTransmit.c, margin);
			else if (film.HasChannel(Film::INDIRECT_GLOSSY))
				Clamp3Margin(film.channel_INDIRECT_GLOSSY->GetPixel(x, y), sampleResult.indirectGlossyTransmit.c, margin);

			// INDIRECT_SPECULAR

			if (film.HasChannel(Film::INDIRECT_SPECULAR_REFLECT))
				Clamp3Margin(film.channel_INDIRECT_SPECULAR_REFLECT->GetPixel(x, y), sampleResult.indirectSpecularReflect.c, margin);
			else if (film.HasChannel(Film::INDIRECT_SPECULAR))
				Clamp3Margin(film.channel_INDIRECT_SPECULAR->GetPixel(x, y), sampleResult.indirectSpecularReflect.c, margin);

			if (film.HasChannel(Film::INDIRECT_SPECULAR_TRANSMIT))
				Clamp3Margin(film.channel_INDIRECT_SPECULAR_TRANSMIT->GetPixel(x, y), sampleResult.indirectSpecularTransmit.c, margin);
			else if (film.HasChannel(Film::INDIRECT_SPECULAR))
				Clamp3Margin(film.channel_INDIRECT_SPECULAR->GetPixel(x, y), sampleResult.indirectSpecularTransmit.c, margin);
		} else {
			// Partial scope: decompose the sample into direct-ish
			// (emission + first-vertex direct light) and indirect energy
			// and clamp only the selected class. The beauty radiance loses
			// exactly the removed share so beauty and AOVs stay consistent.
			const Spectrum directish = sampleResult.emission +
					sampleResult.directDiffuseReflect +
					sampleResult.directDiffuseTransmit +
					sampleResult.directGlossyReflect +
					sampleResult.directGlossyTransmit;
			Spectrum total = 0.f;
			for (u_int i = 0; i < sampleResult.radiance.Size(); ++i)
				total += sampleResult.radiance[i];

			const float totalY = total.Y();
			const float directY = directish.Y();
			const float indirectY = Max(totalY - directY, 0.f);
			const float T = expEff + margin;

			if (varianceClampScope == CLAMP_INDIRECT) {
				const float sInd = (indirectY > 0.f) ? Min(1.f, T / indirectY) : 1.f;

				if (sInd < 1.f) {
					sampleResult.indirectDiffuseReflect *= sInd;
					sampleResult.indirectDiffuseTransmit *= sInd;
					sampleResult.indirectGlossyReflect *= sInd;
					sampleResult.indirectGlossyTransmit *= sInd;
					sampleResult.indirectSpecularReflect *= sInd;
					sampleResult.indirectSpecularTransmit *= sInd;
					sampleResult.irradiance *= sInd;

					// Group 0 keeps the exact decomposition (directish is
					// untouched); other groups scale proportionally.
					const float excess = indirectY * (1.f - sInd);
					Spectrum indShare = sampleResult.radiance[0] - directish;
					indShare.c[0] = Max(0.f, indShare.c[0]);
					indShare.c[1] = Max(0.f, indShare.c[1]);
					indShare.c[2] = Max(0.f, indShare.c[2]);
					sampleResult.radiance[0] = directish + indShare * sInd;

					if (sampleResult.radiance.Size() > 1) {
						const float sB = (totalY > 0.f) ? (totalY - excess) / totalY : 1.f;
						for (u_int i = 1; i < sampleResult.radiance.Size(); ++i)
							sampleResult.radiance[i] *= sB;
					}
				}
			} else { // CLAMP_DIRECT
				const float sDir = (directY > 0.f) ? Min(1.f, T / directY) : 1.f;

				if (sDir < 1.f) {
					sampleResult.emission *= sDir;
					sampleResult.directDiffuseReflect *= sDir;
					sampleResult.directDiffuseTransmit *= sDir;
					sampleResult.directGlossyReflect *= sDir;
					sampleResult.directGlossyTransmit *= sDir;

					const float excess = directY * (1.f - sDir);
					sampleResult.radiance[0] = directish * sDir +
							(sampleResult.radiance[0] - directish);

					if (sampleResult.radiance.Size() > 1) {
						const float sB = (totalY > 0.f) ? (totalY - excess) / totalY : 1.f;
						for (u_int i = 1; i < sampleResult.radiance.Size(); ++i)
							sampleResult.radiance[i] *= sB;
					}
				}
			}
		}
	} else if (sampleResult.HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED) &&
			(varianceClampScope != CLAMP_DIRECT)) {
		// Light-traced splats are indirect energy: they are clamped under
		// the indirect and all scopes and skipped under the direct scope.
		float expectedValue[3] = { 0.f, 0.f, 0.f };
		for (u_int i = 0; i < film.channel_RADIANCE_PER_SCREEN_NORMALIZEDs.size(); ++i)
			film.channel_RADIANCE_PER_SCREEN_NORMALIZEDs[i]->AccumulateWeightedPixel(
					x, y, &expectedValue[0]);

		const double lightSampleCount = film.GetTotalLightSampleCount();
		const float factor = (float)((lightSampleCount > 0.0) ?
			(1.0 / lightSampleCount) :
			1.0);

		expectedValue[0] *= factor;
		expectedValue[1] *= factor;
		expectedValue[2] *= factor;

		float med = 0.f, mad = 0.f, ownLum = 0.f;
		const bool robust = varianceClampAdaptive &&
				RobustLumStats3x3Unweighted(
						film.channel_RADIANCE_PER_SCREEN_NORMALIZEDs[0].get(),
						x, y, subRegion, factor, med, mad, ownLum);
		const float expEff = Max(ownLum, med);
		const float margin = robust ?
				Max(varianceClampSigma * mad,
						sqrtVarianceClampMaxValue * (0.1f + expEff)) :
				sqrtVarianceClampMaxValue;

		const float minExpectedValue = Min(expectedValue[0], Min(expectedValue[1], expectedValue[2]));
		const float maxExpectedValue = Max(expectedValue[0], Max(expectedValue[1], expectedValue[2]));
		const float minRadiance = Max(minExpectedValue - margin, 0.f);
		const float maxRadiance = maxExpectedValue + margin;

		for (u_int radianceGroupIndex = 0; radianceGroupIndex < sampleResult.radiance.Size(); ++radianceGroupIndex)
			sampleResult.radiance[radianceGroupIndex] = sampleResult.radiance[radianceGroupIndex].ScaledClamp(minRadiance, maxRadiance);
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
