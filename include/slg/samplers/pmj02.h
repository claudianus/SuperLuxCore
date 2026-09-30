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

#ifndef _SLG_PMJ02_SAMPLER_H
#define	_SLG_PMJ02_SAMPLER_H
#include <memory>
#include <vector>

#include "luxrays/core/randomgen.h"
#include "luxrays/usings.h"
#include "luxrays/utils/atomic.h"

#include "slg/slg.h"
#include "slg/usings.h"
#include "slg/film/film.h"
#include "slg/samplers/sampler.h"
#include "slg/samplers/sobol.h"

namespace slg {

//------------------------------------------------------------------------------
// PMJ02SamplerSharedData
//
// Same layout and behavior as SobolSamplerSharedData (bucket cursor +
// per-pixel passes); a distinct subclass only so the shared-data registry
// gets its own entry for the PMJ02SAMPLER tag.
//------------------------------------------------------------------------------

class PMJ02SamplerSharedData : public SobolSamplerSharedData {
public:
	PMJ02SamplerSharedData(const luxrays::RandomGeneratorUPtr & rndGen, FilmPtr engineFlm) :
		SobolSamplerSharedData(rndGen, engineFlm) { }
	PMJ02SamplerSharedData(const u_int seed, FilmPtr engineFlm) :
		SobolSamplerSharedData(seed, engineFlm) { }
	virtual ~PMJ02SamplerSharedData() { }

	// Passes start at 0 (Sobol starts at SOBOL_STARTOFFSET to skip
	// degenerate early points; PMJ02 prefixes are already stratified,
	// and a mid-sequence window would break the stratification).
	virtual void Reset();

	static std::unique_ptr<SamplerSharedData> FromProperties(
		const luxrays::Properties &cfg,
		const luxrays::RandomGeneratorUPtr & rndGen,
		FilmPtr film
	) {
		return std::make_unique<PMJ02SamplerSharedData>(rndGen, film);
	}
};

//------------------------------------------------------------------------------
// PMJ02 sampler
//
// Progressive multi-jittered (0,2) sequences (Christensen, Kensler and
// Kilpatrick 2018): every power-of-two prefix is stratified on every
// elementary 2D interval. 2D dimension pairs consume consecutive points of
// per-pair PMJ02 sets (generated at construction with pmj-cpp); each point
// gets a per-pixel Cranley-Patterson rotation so pixels stay decorrelated.
// Beyond the generated set size the sequence wraps with a per-cycle rotation
// (still unbiased).
//
// The pixel/bucket/pass bookkeeping is the Sobol one; the shared data object
// is a SobolSamplerSharedData subclass (same layout, own registry entry).
//------------------------------------------------------------------------------

class PMJ02Sampler : public Sampler {
public:

	PMJ02Sampler(
		const luxrays::RandomGeneratorUPtr & rnd,
		FilmPtr flm,
		const FilmSampleSplatterUPtr& flmSplatter,
		const bool imgSamplesEnable,
		const float adaptiveStr,
		const float adaptiveUserImpWeight,
		const u_int bucketSz,
		const u_int tileSz,
		const u_int superSmpl,
		const u_int overlap,
		SamplerSharedDataSPtr samplerSharedData,
		const u_int tableSamples
	);
	PMJ02Sampler(
		const luxrays::RandomGeneratorUPtr & rnd,
		FilmRef flm,
		const FilmSampleSplatterUPtr& flmSplatter,
		const bool imgSamplesEnable,
		const float adaptiveStr,
		const float adaptiveUserImpWeight,
		const u_int bucketSz,
		const u_int tileSz,
		const u_int superSmpl,
		const u_int overlap,
		SamplerSharedDataSPtr samplerSharedData,
		const u_int tableSamples
	);
	virtual ~PMJ02Sampler();

	virtual SamplerType GetType() const { return GetObjectType(); }
	virtual std::string GetTag() const { return GetObjectTag(); }
	virtual void RequestSamples(const SampleType sampleType, const u_int size);

	virtual float GetSample(const u_int index);
	virtual void NextSampleImpl(const std::vector<SampleResult> &sampleResults, const u_int used);
	virtual u_int GetPass() const { return pass; }

	virtual luxrays::PropertiesUPtr ToProperties() const;

	u_int GetPassCount() const;

	//--------------------------------------------------------------------------
	// Static methods used by SamplerRegistry
	//--------------------------------------------------------------------------

	static SamplerType GetObjectType() { return PMJ02SAMPLER; }
	static std::string GetObjectTag() { return "PMJ02SAMPLER"; }
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static SamplerUPtr FromProperties(
		const luxrays::Properties &cfg,
		const luxrays::RandomGeneratorUPtr & rndGen,
		FilmPtr film, const FilmSampleSplatterUPtr& flmSplatter,
		SamplerSharedDataSPtr sharedData
	);
	static slg::ocl::Sampler *FromPropertiesOCL(const luxrays::Properties &cfg);
	static void AddRequiredChannels(Film::FilmChannels &channels, const luxrays::Properties &cfg);

	// PMJ02 table parameters shared with the OpenCL port. 64 pairs cover
	// 128 sampling dimensions (path depth ~13); deeper tails clamp to the
	// last pair (documented limit, harmless in practice).
	static const u_int PMJ02_TABLE_PAIRS = 64;

	// Fill the host-side device table staging area (pairs x samples x xy
	// floats) with deterministic PMJ02 sets (seeded by seedBase, one seed
	// per pair). Used by the GPU backend init.
	static void FillDeviceTables(u_int pairs, u_int samples, u_int seedBase,
			float *tables);

private:
	void InitNewSample();
	void EnsurePairs(const u_int count);

	static luxrays::PropertiesUPtr GetDefaultProps();

	// One PMJ02 2D set per dimension pair (grown on demand)
	struct PMJ02Set {
		std::vector<float> x, y;
	};
	std::vector<PMJ02Set> pmjSets;
	u_int tableSamples;
	u_int baseSeed;

	std::shared_ptr<SobolSamplerSharedData> sharedData;
	float adaptiveStrength, adaptiveUserImportanceWeight;
	u_int bucketSize, tileSize, superSampling, overlapping;

	std::shared_ptr<u_int> bucketIndex;
	u_int pixelOffset, passOffset, pass;
	// Pixel-pass run batching, same contract as SobolSampler:
	// GetNewPixelPassBatch claims PASS_BATCH consecutive passes per
	// pixel; the adaptive gate evaluates once per run.
	static const u_int PASS_BATCH = 4;
	u_int pixelPassRunLeft, pixelPassRunIdx;
	u_int pixelX, pixelY;
	luxrays::TauswortheRandomGenerator rngGenerator;

	float sample0, sample1;
};

}

#endif	/* _SLG_PMJ02_SAMPLER_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
