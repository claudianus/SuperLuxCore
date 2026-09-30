/***************************************************************************
 * Copyright 1998-2018 by authors (see AUTHORS.txt)                        *
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

#include "slg/film/film.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// FilmSamplesCounts
//------------------------------------------------------------------------------

FilmSamplesCounts::FilmSamplesCounts() {
	Init(1);
}

FilmSamplesCounts::~FilmSamplesCounts() {
}

void FilmSamplesCounts::Init(const u_int count) {
	assert (count > 0);

	threadCount = count;
	perThread.reset(static_cast<PerThreadCounts *>(
			std::aligned_alloc(64, threadCount * sizeof(PerThreadCounts))));
	for (u_int i = 0; i < threadCount; ++i) {
		perThread[i].total = 0.0;
		perThread[i].pixelNorm = 0.0;
		perThread[i].screenNorm = 0.0;
		perThread[i].pendingTotal = 0.0;
	}
	total_SampleCountAtomic.store(0.0, std::memory_order_relaxed);
}

void FilmSamplesCounts::Clear() {
	for (u_int i = 0; i < threadCount; ++i) {
		perThread[i].total = 0.0;
		perThread[i].pixelNorm = 0.0;
		perThread[i].screenNorm = 0.0;
		perThread[i].pendingTotal = 0.0;
	}
	total_SampleCountAtomic.store(0.0, std::memory_order_relaxed);
}

void FilmSamplesCounts::SetSampleCount(const double sampleCount,
		const double RADIANCE_PER_PIXEL_NORMALIZED_count,
		const double RADIANCE_PER_SCREEN_NORMALIZED_count) {
	perThread[0].total = sampleCount;
	perThread[0].pixelNorm = RADIANCE_PER_PIXEL_NORMALIZED_count;
	perThread[0].screenNorm = RADIANCE_PER_SCREEN_NORMALIZED_count;
	perThread[0].pendingTotal = 0.0;
	total_SampleCountAtomic.store(sampleCount, std::memory_order_relaxed);

	for (u_int i = 1; i < threadCount; ++i) {
		perThread[i].total = 0.0;
		perThread[i].pixelNorm = 0.0;
		perThread[i].screenNorm = 0.0;
		perThread[i].pendingTotal = 0.0;
	}
}

void FilmSamplesCounts::AddSampleCount(const double sampleCount,
		const double RADIANCE_PER_PIXEL_NORMALIZED_count,
		const double RADIANCE_PER_SCREEN_NORMALIZED_count) {
	perThread[0].total += sampleCount;
	perThread[0].pixelNorm += RADIANCE_PER_PIXEL_NORMALIZED_count;
	perThread[0].screenNorm += RADIANCE_PER_SCREEN_NORMALIZED_count;
	total_SampleCountAtomic.fetch_add(sampleCount, std::memory_order_relaxed);
}

void FilmSamplesCounts::AddSampleCount(const u_int threadIndex,
		const double RADIANCE_PER_PIXEL_NORMALIZED_count,
		const double RADIANCE_PER_SCREEN_NORMALIZED_count) {
	assert (threadIndex < threadCount);

	PerThreadCounts &t = perThread[threadIndex];
	const double total = Max(RADIANCE_PER_PIXEL_NORMALIZED_count, RADIANCE_PER_SCREEN_NORMALIZED_count);
	t.total += total;
	t.pixelNorm += RADIANCE_PER_PIXEL_NORMALIZED_count;
	t.screenNorm += RADIANCE_PER_SCREEN_NORMALIZED_count;

	// pendingTotal accumulates the per-splat total locally and pushes one
	// batch to the shared atomic when it crosses SAMPLE_COUNT_BATCH -
	// the previous per-splat fetch_add made the line bounce across all
	// cores on every contribution.
	t.pendingTotal += total;
	if (t.pendingTotal >= SAMPLE_COUNT_BATCH) {
		total_SampleCountAtomic.fetch_add(t.pendingTotal, std::memory_order_relaxed);
		t.pendingTotal = 0.0;
	}
}

double FilmSamplesCounts::GetSampleCount() const {
	return total_SampleCountAtomic.load(std::memory_order_relaxed);
}

double FilmSamplesCounts::GetSampleCount_RADIANCE_PER_PIXEL_NORMALIZED() const {
	double result = 0.0;
	for (u_int i = 0; i < threadCount; ++i)
		result += perThread[i].pixelNorm;
	return result;
}

double FilmSamplesCounts::GetSampleCount_RADIANCE_PER_SCREEN_NORMALIZED() const {
	double result = 0.0;
	for (u_int i = 0; i < threadCount; ++i)
		result += perThread[i].screenNorm;
	return result;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
