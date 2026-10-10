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

#ifndef _SLG_METROPOLIS_SAMPLER_H
#define	_SLG_METROPOLIS_SAMPLER_H

#include <string>
#include <vector>
#include <atomic>

#include "luxrays/core/randomgen.h"
#include "slg/slg.h"
#include "slg/film/film.h"
#include "slg/samplers/sampler.h"

namespace slg {

//------------------------------------------------------------------------------
// MetropolisSamplerSharedData
//
// Used to share sampler specific data across multiple threads
//------------------------------------------------------------------------------

class MetropolisSamplerSharedData : public SamplerSharedData {
public:
	MetropolisSamplerSharedData();
	virtual ~MetropolisSamplerSharedData() { }

	virtual void Reset();

	static std::unique_ptr<SamplerSharedData> FromProperties(
		const luxrays::Properties &cfg,
		const luxrays::RandomGeneratorUPtr & rndGen,
		FilmPtr film
	);
	static std::unique_ptr<SamplerSharedData> FromProperties(
		const luxrays::Properties &cfg,
		const luxrays::RandomGeneratorUPtr & rndGen,
		FilmRef film
	);

	// I'm storing totalLuminance, sampleCount and noBlackSampleCount on shared variables
	// in order to have far more accurate estimation in the image mean intensity
	// computation
	
	// Updated by all threads
	std::atomic<double> totalLuminance;
	std::atomic<u_longlong> sampleCount, noBlackSampleCount;

	// Updated only by thread 0
	float lastLuminance;
	u_int lastSampleCount, lastNoBlackSampleCount;

	// Published by thread 0 and read by all chains. Plain fields here race
	// with workers during bootstrap and when the normalizer is frozen.
	std::atomic<float> invLuminance;
	std::atomic<bool> cooldown;
};

//------------------------------------------------------------------------------
// Metropolis sampler
//------------------------------------------------------------------------------

typedef enum {
	METRO_ACCEPTED, METRO_REJECTED
} MetropolisSampleType;

class MetropolisSampler : public Sampler {
public:
	MetropolisSampler(const luxrays::RandomGeneratorUPtr & rnd, FilmPtr film,
			const FilmSampleSplatterUPtr& flmSplatter, const bool imgSamplesEnable,
			const u_int maxRej, const float pLarge, const float imgRange,
			const bool addOnlyCstcs,
			SamplerSharedDataSPtr samplerSharedData
		);
	virtual ~MetropolisSampler();

	virtual SamplerType GetType() const { return GetObjectType(); }
	virtual std::string GetTag() const { return GetObjectTag(); }
	virtual void RequestSamples(const SampleType sampleType, const u_int size);

	virtual float GetSample(const u_int index);
	virtual void NextSampleImpl(const std::vector<SampleResult> &sampleResults, const u_int used);

	// Used, most of the times, when not having a film
	MetropolisSampleType GetLastSampleAcceptance(float &weight) const;

	virtual luxrays::PropertiesUPtr ToProperties() const;

	u_int GetLargeMutationCount() const { return largeMutationCount; }

	//--------------------------------------------------------------------------
	// Static methods used by SamplerRegistry
	//--------------------------------------------------------------------------

	static SamplerType GetObjectType() { return METROPOLIS; }
	static std::string GetObjectTag() { return "METROPOLIS"; }
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static SamplerUPtr FromProperties(
		const luxrays::Properties &cfg, const luxrays::RandomGeneratorUPtr & rndGen,
		FilmPtr film, const FilmSampleSplatterUPtr& flmSplatter,
		SamplerSharedDataSPtr sharedData);
	static slg::ocl::Sampler *FromPropertiesOCL(const luxrays::Properties &cfg);
	static void AddRequiredChannels(Film::FilmChannels &channels, const luxrays::Properties &cfg);

private:
	static luxrays::PropertiesUPtr GetDefaultProps();

	std::shared_ptr<MetropolisSamplerSharedData> sharedData;;

	// Legacy scene property; retained for round trips, never forces acceptance.
	u_int maxRejects;
	float largeMutationProbability, imageMutationRange;
	bool addOnlyCuastics;

	std::vector<float> samples;
	std::vector<u_int> sampleStamps;

	float weight;
	u_int consecRejects;
	u_int stamp;

	// Data saved for the current sample
	u_int currentStamp;
	double currentLuminance;
	std::vector<float> currentSamples;
	std::vector<u_int> currentSampleStamps;
	std::vector<SampleResult> currentSampleResults;
	// Live slots in currentSampleResults (PATHCPU light-path vector stays
	// at capacity; used marks the live prefix)
	u_int currentSampleResultsUsed = 0;

	// Used, most of the times, when not having a film
	MetropolisSampleType lastSampleAcceptance;
	float lastSampleWeight;

	u_int largeMutationCount;
	
	bool isLargeMutation;
};

}

#endif	/* _SLG_METROPOLIS_SAMPLER_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
