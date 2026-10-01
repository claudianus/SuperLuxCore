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

#include "luxrays/utils/atomic.h"

#include "slg/film/filters/gaussian.h"
#include "slg/film/filmsamplesplatter.h"
#include "slg/film/sampleresult.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// FilmSampleSplatter
//------------------------------------------------------------------------------


const FilmSampleSplatterUPtr FilmSampleSplatter::Null{};  // Static

FilmSampleSplatter::FilmSampleSplatter(const FilterUPtr& flt) : filter(flt) {
	if (filter) {
		const u_int size = Max<u_int>(4, Max(filter->xWidth, filter->yWidth) + 1);
		filterLUTs = new FilterLUTs(*filter, size);
	} else
		filterLUTs = NULL;
}

FilmSampleSplatter::~FilmSampleSplatter() {
	delete filterLUTs;
}

void FilmSampleSplatter::AtomicSplatSample(FilmConstRef film, const SampleResult &sampleResult, const float weight) const {
	const u_int *subRegion = film.GetSubRegion();

	// Accumulate the sample luminance first and second moments for the
	// samplers' second-moment adaptive convergence estimate. The sample
	// is attributed once to its generating pixel, before any pixel
	// filter redistribution, so the moments stay consistent with the
	// per-pixel pass counters used to derive the sample count.
	if (!film.pixelLumaMoments.empty() && sampleResult.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED)) {
		const int x = Floor2Int(sampleResult.filmX);
		const int y = Floor2Int(sampleResult.filmY);

		if ((x >= (int)subRegion[0]) && (x <= (int)subRegion[1]) && (y >= (int)subRegion[2]) && (y <= (int)subRegion[3])) {
			const float l = sampleResult.radiance[0].Y();
			if (!isnan(l) && !isinf(l)) {
				const u_int i = (x + y * film.GetWidth()) * 2;
				AtomicAdd(&film.pixelLumaMoments[i], l);
				AtomicAdd(&film.pixelLumaMoments[i + 1], l * l);
			}
		}
	}

	if (!filter || (filter->GetType() == FILTER_NONE)) {
		const int x = Floor2Int(sampleResult.filmX);
		const int y = Floor2Int(sampleResult.filmY);

		if ((x >= (int)subRegion[0]) && (x <= (int)subRegion[1]) && (y >= (int)subRegion[2]) && (y <= (int)subRegion[3])) {
			film.AtomicAddSample(x, y, sampleResult, weight);
		}
	} else {
		//----------------------------------------------------------------------
		// Add all data related information (not filtered)
		//----------------------------------------------------------------------

		if (film.HasDataChannel()) {
			const int x = Floor2Int(sampleResult.filmX);
			const int y = Floor2Int(sampleResult.filmY);

			if ((x >= (int)subRegion[0]) && (x <= (int)subRegion[1]) && (y >= (int)subRegion[2]) && (y <= (int)subRegion[3]))
				film.AtomicAddSampleResultData(x, y, sampleResult);
		}

		//----------------------------------------------------------------------
		// Add all color related information (filtered)
		//----------------------------------------------------------------------

		// Compute sample's raster extent
		const float dImageX = sampleResult.filmX - .5f;
		const float dImageY = sampleResult.filmY - .5f;
		const FilterLUT *filterLUT = filterLUTs->GetLUT(dImageX - floorf(sampleResult.filmX), dImageY - floorf(sampleResult.filmY));
		auto lut = filterLUT->GetLUT().begin();

		const int x0 = Floor2Int(dImageX - filter->xWidth * .5f + .5f);
		const int x1 = x0 + filterLUT->GetWidth();
		const int y0 = Floor2Int(dImageY - filter->yWidth * .5f + .5f);
		const int y1 = y0 + filterLUT->GetHeight();

		// Fast-out: the whole filter extent sits outside the active subRegion
		// (sub-tile renders, lens borders). One comparison replaces the inner
		// loop's per-pixel bounds test on every rejected splat.
		const int xMin = Max(x0, (int)subRegion[0]);
		const int xMax = Min(x1 - 1, (int)subRegion[1]);
		const int yMin = Max(y0, (int)subRegion[2]);
		const int yMax = Min(y1 - 1, (int)subRegion[3]);
		if (xMin > xMax || yMin > yMax)
			return;

		for (int iy = yMin; iy <= yMax; ++iy) {
			// Rewind lut to the first in-bounds row
			auto rowLut = lut + (iy - y0) * filterLUT->GetWidth();

			for (int ix = xMin; ix <= xMax; ++ix) {
				const float filterWeight = rowLut[ix - x0];

				const float filteredWeight = weight * filterWeight;
				// A zero-weight splat carries no contribution; skip the
				// ~29 atomic channel updates it would still pay
				if (filteredWeight == 0.f)
					continue;
				film.AtomicAddSampleResultColor(ix, iy, sampleResult, filteredWeight);
			}
		}
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
