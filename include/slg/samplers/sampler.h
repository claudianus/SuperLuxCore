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

#ifndef _SLG_SAMPLER_H
#define	_SLG_SAMPLER_H

#include <atomic>
#include <numeric>
#include <string>
#include <vector>

#include "luxrays/core/randomgen.h"
#include "luxrays/usings.h"
#include "luxrays/utils/utils.h"
#include "slg/slg.h"
#include "slg/film/film.h"
#include "slg/film/filmsamplesplatter.h"
#include "slg/film/sampleresult.h"

namespace slg {

//------------------------------------------------------------------------------
// OpenCL data types
//------------------------------------------------------------------------------

namespace ocl {
#include "slg/samplers/sampler_types.cl"
}

//------------------------------------------------------------------------------
// SamplerSharedData
//
// Used to share sampler specific data across multiple threads
//------------------------------------------------------------------------------

class SamplerSharedDataRegistry;

class SamplerSharedData {
public:
	SamplerSharedData() { }
	virtual ~SamplerSharedData() { }

	virtual void Reset() = 0;

	static std::unique_ptr<SamplerSharedData> FromProperties(
		const luxrays::Properties &cfg,
		const luxrays::RandomGeneratorUPtr & rndGen,
		FilmPtr film
	);

protected:
	// Golden-ratio stride permutation of a sequential bucket index:
	// consecutive buckets land ~n*0.618 apart, so the first pass covers
	// the frame as scattered tile chunks instead of a bottom-to-top
	// row sweep (visible on CPU engines where a handful of threads
	// consume consecutive buckets). Bijective for any n (k coprime to
	// n): every bucket is still served exactly once per cycle, so pixel
	// coverage and per-pixel pass accounting are unchanged (unbiased).
	static u_int ScatterBucketIndex(const u_int i, const u_int n) {
		if (n <= 2)
			return i;

		u_int k = luxrays::Max(1u, (u_int)(n * .61803398875));
		while (std::gcd(k, n) != 1)
			--k;

		return (u_int)(((u_longlong)i * k) % n);
	}
};

//------------------------------------------------------------------------------
// Sampler
//------------------------------------------------------------------------------

typedef enum {
	RANDOM, METROPOLIS, SOBOL, RTPATHCPUSAMPLER, TILEPATHSAMPLER, PMJ02SAMPLER,
	SAMPLER_TYPE_COUNT
} SamplerType;

typedef enum {
	PIXEL_NORMALIZED_ONLY, SCREEN_NORMALIZED_ONLY,
	PIXEL_NORMALIZED_AND_SCREEN_NORMALIZED, ONLY_AOV_SAMPLE
} SampleType;

class Sampler : public luxrays::NamedObject {
public:
	Sampler(
		const luxrays::RandomGeneratorUPtr & rnd,
		FilmPtr flm,
		const FilmSampleSplatterUPtr& flmSplatter,
		const bool imgSamplesEnable
	) :
		NamedObject("sampler"),
		threadIndex(0),
		rndGen(rnd),
		film(flm),
		filmSplatter(flmSplatter),
		imageSamplesEnable(imgSamplesEnable)
	{}

	virtual ~Sampler() { }

	virtual void SetThreadIndex(const u_int index) { threadIndex = index; }

	virtual SamplerType GetType() const = 0;
	virtual std::string GetTag() const = 0;
	virtual void RequestSamples(const SampleType sampleType, const u_int size);

	// index 0 and 1 are always image X and image Y
	virtual float GetSample(const u_int index) = 0;
	virtual void NextSample(const std::vector<SampleResult> &sampleResults) = 0;

	// Per-sample unique counter for path guiding (P1-3 M1): the guide
	// bin pick must not reuse a sampler dimension that shares a
	// Cranley-Patterson shift with the jitter dims (correlated triple =
	// biased pdf). Samplers with a semantic pass override this; the
	// default is a unique-per-call counter (unique values, racy
	// assignment across threads, still uniform and shift-independent).
	virtual u_int GetPass() const {
		return passCounter.fetch_add(1u, std::memory_order_relaxed);
	}

	// Transform the current object in Properties
	virtual luxrays::PropertiesUPtr ToProperties() const;

	//--------------------------------------------------------------------------
	// Static methods used by SamplerRegistry
	//--------------------------------------------------------------------------

	// Transform the current configuration Properties in a complete list of
	// object Properties (including all defaults values)
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	// Allocate a Object based on the cfg definition
	static SamplerUPtr FromProperties(
		const luxrays::Properties &cfg,
		const luxrays::RandomGeneratorUPtr & rndGen,
		FilmPtr film,
		const FilmSampleSplatterUPtr& flmSplatter,
		SamplerSharedDataSPtr sharedData
	);
	static slg::ocl::Sampler *FromPropertiesOCL(const luxrays::Properties &cfg);

	static void AddRequiredChannels(
		Film::FilmChannels &channels, const luxrays::Properties &cfg
	);

	static SamplerType String2SamplerType(const std::string &type);
	static std::string SamplerType2String(const SamplerType type);

	FilmRef GetFilm() { return *film; }
	FilmConstRef GetFilm() const { return *film; }

protected:
	static luxrays::PropertiesUPtr GetDefaultProps();


	void AtomicAddSampleToFilm(
		const SampleResult &sampleResult, const float weight = 1.f
	) const {
		if (sampleResult.useFilmSplat && filmSplatter)
			filmSplatter->AtomicSplatSample(GetFilm(), sampleResult, weight);
		else
			GetFilm().AtomicAddSample(
				sampleResult.pixelX, sampleResult.pixelY, sampleResult, weight
			);
	}

	void AtomicAddSamplesToFilm(
		const std::vector<SampleResult> &sampleResults, const float weight = 1.f
	) const {
		for (auto const &sr : sampleResults)
			AtomicAddSampleToFilm(sr, weight);
	}

	u_int threadIndex;
	const luxrays::RandomGeneratorUPtr & rndGen;
	FilmPtr film;
	const FilmSampleSplatterUPtr& filmSplatter;

	SampleType sampleType;
	u_int requestedSamples;
	// If samples 0 and 1 should be expressed in pixels
	bool imageSamplesEnable;

	// Unique-per-call fallback counter for GetPass() (see above)
	mutable std::atomic<u_int> passCounter{0};
};

}

#endif	/* _SLG_SAMPLER_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
