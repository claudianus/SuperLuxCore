/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of SuperLuxCore.                                    *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <algorithm>
#include <limits>

#include <boost/serialization/shared_ptr.hpp>

#include "slg/film/film.h"
#include "slg/film/adaptiveerror/filmadaptiveerror.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// FilmAdaptiveError
//------------------------------------------------------------------------------

BOOST_CLASS_EXPORT_IMPLEMENT(slg::FilmAdaptiveError)

FilmAdaptiveError::FilmAdaptiveError(
	FilmConstRef flm, const float errorTargetVal,
	const u_int warmupVal, const u_int testStepVal,
	const u_int minSamplesVal, const bool haltEnabledVal
) :
	errorTarget(errorTargetVal),
	warmup(warmupVal),
	testStep(testStepVal),
	minSamples(minSamplesVal),
	haltEnabled(haltEnabledVal),
	film(&flm)
{
	Reset();
}

FilmAdaptiveError::FilmAdaptiveError() :
	film(nullptr) {
}

FilmAdaptiveError::~FilmAdaptiveError() {
}

void FilmAdaptiveError::Reset() {
	noiseLevel = numeric_limits<float>::infinity();
	convergedRatio = 0.f;
	todoPixelsCount = GetFilm().GetWidth() * GetFilm().GetHeight();
	errorVector.assign(todoPixelsCount, numeric_limits<float>::infinity());
	lastSamplesCount = 0.0;
	firstTest = true;
}

bool FilmAdaptiveError::IsTestUpdateRequired() const {
	// The film back pointer is rebound by Film::load - an orphaned
	// object (unbound by a non-film deserialize path) is inert
	if (!film)
		return false;

	if (!GetFilm().HasChannel(Film::VARIANCE) ||
			!GetFilm().HasChannel(Film::SAMPLECOUNT) ||
			!GetFilm().HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED))
		return false;

	const u_int pixelsCount = GetFilm().GetWidth() * GetFilm().GetHeight();

	// Run the test only after an initial warmup (variance needs a few
	// samples per pixel to be meaningful at all)
	if (GetFilm().GetTotalSampleCount() / pixelsCount <= warmup)
		return false;

	// Do not run the test if we don't have at least testStep new samples per pixel
	if (GetFilm().GetTotalSampleCount() - lastSamplesCount <=
			pixelsCount * static_cast<double>(testStep))
		return false;

	return true;
}

// ITU-R Rec.709 luminance
static inline float Luma709(const float r, const float g, const float b) {
	return 0.212671f * r + 0.715160f * g + 0.072169f * b;
}

float FilmAdaptiveError::Test() {
	if (!IsTestUpdateRequired())
		return convergedRatio;

	lastSamplesCount = GetFilm().GetTotalSampleCount();
	firstTest = false;

	const u_int pixelsCount = GetFilm().GetWidth() * GetFilm().GetHeight();
	const int width = GetFilm().GetWidth();
	const int height = GetFilm().GetHeight();

	// Per-pixel relative standard error of the mean:
	//   relErr = sqrt(max(E[c^2] - E[c]^2, 0) / n) / (E[c] + eps)
	// with E[c] taken from the merged per-pixel-normalized radiance and
	// E[c^2] from the VARIANCE channel second-moment accumulation.
	vector<float> rawErr(pixelsCount, numeric_limits<float>::infinity());
	for (u_int i = 0; i < pixelsCount; ++i) {
		const u_int n = *(GetFilm().channel_SAMPLECOUNT->GetPixel(i));
		if (n < minSamples) {
			rawErr[i] = numeric_limits<float>::infinity();
			continue;
		}

		const float *varP = GetFilm().channel_VARIANCE->GetPixel(i);
		const float w = varP[3];
		if (w <= 0.f) {
			rawErr[i] = numeric_limits<float>::infinity();
			continue;
		}
		const float e2 = Luma709(varP[0], varP[1], varP[2]) / w;

		const float *radP =
			GetFilm().channel_RADIANCE_PER_PIXEL_NORMALIZEDs[0]->GetPixel(i);
		const float wr = radP[3];
		if (wr <= 0.f) {
			rawErr[i] = numeric_limits<float>::infinity();
			continue;
		}
		const float mu = Luma709(radP[0], radP[1], radP[2]) / wr;

		// NaN/Inf-contaminated moments must never read as converged:
		// Max(NaN, 0) and (inf - inf) both collapse to 0 which would
		// mark a corrupt pixel as perfectly clean. Mark them non-finite
		// instead: excluded from the percentile below but counted as
		// not converged, and the NOISE map keeps them fully sampled.
		if (!isfinite(e2) || !isfinite(mu)) {
			rawErr[i] = numeric_limits<float>::infinity();
			continue;
		}

		const float var = Max(e2 - mu * mu, 0.f);
		rawErr[i] = sqrtf(var / static_cast<float>(n)) / (fabsf(mu) + 1e-6f);
	}

	// 3x3 max dilation (Arnold recipe): a pixel only counts as converged
	// when its neighborhood is converged too - catches sub-pixel detail
	// that would otherwise read as a converged smooth average.
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			float m = 0.f;
			for (int dy = -1; dy <= 1; ++dy) {
				const int yy = Clamp(y + dy, 0, height - 1);
				for (int dx = -1; dx <= 1; ++dx) {
					const int xx = Clamp(x + dx, 0, width - 1);
					m = Max(m, rawErr[yy * width + xx]);
				}
			}
			errorVector[y * width + x] = m;
		}
	}

	// Global statistics: converged fraction + robust 95th-percentile
	// noise level over evaluated pixels (pixels still under minSamples
	// count as not converged but are excluded from the percentile so a
	// few unvisited pixels don't pin the metric at infinity).
	vector<float> finite;
	finite.reserve(pixelsCount);
	u_int converged = 0, evaluated = 0;
	for (u_int i = 0; i < pixelsCount; ++i) {
		const float e = errorVector[i];
		if (!isfinite(e))
			continue;
		++evaluated;
		if (e < errorTarget)
			++converged;
		finite.push_back(e);
	}
	todoPixelsCount = pixelsCount - converged;
	noiseLevel = finite.empty() ? numeric_limits<float>::infinity() :
		[](vector<float> &v) {
			const size_t k = (size_t)(0.95 * (v.size() - 1));
			nth_element(v.begin(), v.begin() + k, v.end());
			return v[k];
		}(finite);
	convergedRatio = (evaluated > 0) ?
		static_cast<float>(converged) / pixelsCount : 0.f;

	// Feed the statistically-grounded importance map into the NOISE
	// channel: 0 = converged (samplers may still hit it at the
	// 1-strength floor), 1 = at/above the error target.
	if (GetFilm().channel_NOISE) {
		for (u_int i = 0; i < pixelsCount; ++i) {
			const float e = errorVector[i];
			const float imp = isfinite(e) ?
				Clamp(e / errorTarget, 0.f, 1.f) : 1.f;
			*(GetFilm().channel_NOISE->GetPixel(i)) = imp;
		}
	}

	SLG_LOG("Adaptive error: noiseLevel=" << (noiseLevel * 100.f) <<
		"% converged=" << (convergedRatio * 100.f) << "%");

	return convergedRatio;
}

template<class Archive> void FilmAdaptiveError::serialize(Archive &ar, const u_int version) {
	ar & errorTarget;
	ar & warmup;
	ar & testStep;
	ar & minSamples;
	ar & haltEnabled;
	// Version 1 serialized the film pointer inline, writing a full
	// nested film copy into every archive and binding the loaded object
	// to that frozen duplicate (GetFilm() then queried stale data and
	// the test never re-triggered after a resume). Version >= 2 drops
	// the field; Film::load rebinds it via BindFilm(). The v1 payload
	// still has to be consumed for stream alignment.
	if (version < 2) {
		FilmConstPtr legacyFilm = nullptr;
		ar & legacyFilm;
		delete const_cast<Film *>(legacyFilm.get());
	}
	ar & errorVector;
	ar & noiseLevel;
	ar & convergedRatio;
	ar & todoPixelsCount;
	ar & lastSamplesCount;
	ar & firstTest;
}

namespace slg {
// Explicit instantiations for portable archives
template void FilmAdaptiveError::serialize(LuxOutputArchive &ar, const u_int version);
template void FilmAdaptiveError::serialize(LuxInputArchive &ar, const u_int version);
template void FilmAdaptiveError::serialize(LuxOutputArchiveText &ar, const u_int version);
template void FilmAdaptiveError::serialize(LuxInputArchiveText &ar, const u_int version);
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
