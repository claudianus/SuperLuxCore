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

#include <atomic>

#include <boost/lexical_cast.hpp>

#include "luxrays/core/color/color.h"
#include "luxrays/utils/atomic.h"
#include "slg/samplers/sampler.h"
#include "slg/samplers/metropolis.h"
#include "luxcore/luxcore.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// MetropolisSamplerSharedData
//------------------------------------------------------------------------------

MetropolisSamplerSharedData::MetropolisSamplerSharedData() : SamplerSharedData() {
	Reset();
}

std::unique_ptr<SamplerSharedData> MetropolisSamplerSharedData::FromProperties(
	const Properties &cfg,
	const RandomGeneratorUPtr & rndGen,
	FilmPtr film
) {
	return std::make_unique<MetropolisSamplerSharedData>();
}

std::unique_ptr<SamplerSharedData> MetropolisSamplerSharedData::FromProperties(
	const Properties &cfg,
	const RandomGeneratorUPtr & rndGen,
	FilmRef film
) {
	return std::make_unique<MetropolisSamplerSharedData>();
}

void MetropolisSamplerSharedData::Reset() {
	totalLuminance = 0.f;
	sampleCount = 0;
	noBlackSampleCount = 0;

	lastLuminance = 0.f;
	lastSampleCount = 0;
	lastNoBlackSampleCount = 0;

	invLuminance = 1.f;
	cooldown = true;
}

//------------------------------------------------------------------------------
// Metropolis sampler
//------------------------------------------------------------------------------

MetropolisSampler::MetropolisSampler(
		const RandomGeneratorUPtr & rnd,
		FilmPtr flm,
		const FilmSampleSplatterUPtr& flmSplatter, const bool imgSamplesEnable,
		const u_int maxRej, const float pLarge, const float imgRange, const bool addOnlyCstcs,
		SamplerSharedDataSPtr samplerSharedData) : Sampler(rnd, flm, flmSplatter, imgSamplesEnable),
		sharedData(dynamic_pointer_cast<MetropolisSamplerSharedData>(samplerSharedData)),
		maxRejects(maxRej),	largeMutationProbability(pLarge), imageMutationRange(imgRange),
		addOnlyCuastics(addOnlyCstcs),
		largeMutationCount(0) {
}

MetropolisSampler::~MetropolisSampler() {
}

// Mutate a value in the range [0-1]
//
// The original version used in old LuxRender
static float Mutate(const float x, const float randomValue) {
	constexpr float s1 = 1.f / 512.f;
	constexpr float s2 = 1.f / 16.f;
	// s1/s2 is a power-of-two quotient (32.f) and the second term is a
	// constant: hoist both out of the hot per-dimension mutation path
	// (bit-identical - same operands, same rounding, computed once;
	// constexpr also drops the static-init guard the local statics paid
	// on every Mutate call)
	constexpr float s1OverS2 = s1 / s2;
	constexpr float s1Term = s1 / (s1 / s2 + 1.f);

	const float dx = s1 / (s1OverS2 + fabsf(2.f * randomValue - 1.f)) -
			s1Term;

	float mutatedX = x;
	if (randomValue < .5f) {
		mutatedX += dx;
		mutatedX = (mutatedX < 1.f) ? mutatedX : (mutatedX - 1.f);
	} else {
		mutatedX -= dx;
		mutatedX = (mutatedX < 0.f) ? (mutatedX + 1.f) : mutatedX;
	}

	// mutatedX can still be 1.f due to numerical precision problems
	if (mutatedX == 1.f)
		mutatedX = 0.f;

	return mutatedX;
}

// Mutate a value in the range [0-1]
//
// Original version from paper "A Simple and Robust Mutation Strategy for the
// Metropolis Light Transport Algorithm"
/*static float Mutate(const float x, const float randomValue) {
	static const float s1 = 1.f / 1024.f;
	static const float s2 = 1.f / 64.f;

	const float dx = s2 * expf(-logf(s2 / s1) * randomValue);

	float mutatedX = x;
	if (randomValue < .5f) {
		mutatedX += dx;
		mutatedX = (mutatedX < 1.f) ? mutatedX : (mutatedX - 1.f);
	} else {
		mutatedX -= dx;
		mutatedX = (mutatedX < 0.f) ? (mutatedX + 1.f) : mutatedX;
	}

	// mutatedX can still be 1.f due to numerical precision problems
	if (mutatedX == 1.f)
		mutatedX = 0.f;

	return mutatedX;
}*/

// Mutate a value max. by a range value
float MutateScaled(const float x, const float range, const float randomValue) {
	constexpr float s1 = 32.f;
	// The kernel's two denominator constants are compile-time constants
	// and s1 is a power of two (range/s1 == range*(1/s1), exact): hoist
	// them so the hot path keeps a single division (bit-identical)
	constexpr float aTerm = s1 / (1.f + s1);
	constexpr float bTerm = (s1 * s1) / (1.f + s1);
	constexpr float invS1 = 1.f / s1;

	const float dx = range / (aTerm + bTerm *
		fabsf(2.f * randomValue - 1.f)) - range * invS1;

	float mutatedX = x;
	if (randomValue < .5f) {
		mutatedX += dx;
		mutatedX = (mutatedX < 1.f) ? mutatedX : (mutatedX - 1.f);
	} else {
		mutatedX -= dx;
		mutatedX = (mutatedX < 0.f) ? (mutatedX + 1.f) : mutatedX;
	}

	// mutatedX can still be 1.f due to numerical precision problems
	if (mutatedX == 1.f)
		mutatedX = 0.f;

	return mutatedX;
}

void MetropolisSampler::RequestSamples(const SampleType smplType, const u_int size) {
	Sampler::RequestSamples(smplType, size);

	samples.clear();
	samples.resize(requestedSamples);

	sampleStamps.clear();
	sampleStamps.resize(requestedSamples);

	currentSamples.clear();
	currentSamples.resize(requestedSamples);

	currentSampleStamps.clear();
	currentSampleStamps.resize(requestedSamples, 0);

	isLargeMutation = true;
	weight = 0.f;
	consecRejects = 0;
	currentLuminance = 0.f;
	stamp = 1;
	currentStamp = 1;
	currentSampleResults.clear();
}

float MetropolisSampler::GetSample(const u_int index) {
	assert (index < requestedSamples);

	u_int sampleStamp = sampleStamps[index];

	float s;
	if (sampleStamp == 0) {
		s = rndGen->floatValue();
		sampleStamp = 1;
		
		assert (s != 1.f);
	} else
		s = samples[index];

	// Mutate the sample up to the currentStamp
	if (imageSamplesEnable && film && ((index == 0) || (index == 1))) {
		// 0 and 1 are used for image X/Y
		for (u_int i = sampleStamp; i < stamp; ++i)
			s = MutateScaled(s, imageMutationRange, rndGen->floatValue());
		
		samples[index] = s;
		sampleStamps[index] = stamp;

		const u_int *subRegion = film->GetSubRegion();
		const u_int subRegionIndex = (index == 0) ? 0 : 2;
		s = subRegion[subRegionIndex] + s * (subRegion[subRegionIndex + 1] - subRegion[subRegionIndex] + 1);

		return s;
	} else {
		for (u_int i = sampleStamp; i < stamp; ++i)
			s = Mutate(s, rndGen->floatValue());

		samples[index] = s;
		sampleStamps[index] = stamp;

		return s;
	}
}

void MetropolisSampler::NextSampleImpl(const vector<SampleResult> &sampleResults, const u_int used) {
	//--------------------------------------------------------------------------
	// Some hard coded parameter:
	//--------------------------------------------------------------------------
	const u_longlong warmupMinSampleCount = 250000;
	const u_longlong warmupMinNoBlackSampleCount = warmupMinSampleCount / 100 * 5; // 5%

	const u_longlong stepMinSampleCount = 250000;
	const u_longlong stepMinNoBlackSampleCount = warmupMinSampleCount / 100 * 5; // 5%

	const double luminanceThreshold = .01; // 1%	// Thread 0 check if the cooldown is over
	//--------------------------------------------------------------------------

	if (film) {
		double pixelNormalizedCount, screenNormalizedCount;
		switch (sampleType) {
			case PIXEL_NORMALIZED_ONLY:
				pixelNormalizedCount = 1.0;
				screenNormalizedCount = 0.0;
				break;
			case SCREEN_NORMALIZED_ONLY:
				pixelNormalizedCount = 0.0;
				screenNormalizedCount = 1.0;
				break;
			case PIXEL_NORMALIZED_AND_SCREEN_NORMALIZED:
				pixelNormalizedCount = 1.0;
				screenNormalizedCount = 1.0;
				break;
			default:
				throw runtime_error("Unknown sample type in MetropolisSampler::NextSample(): " + ToString(sampleType));
		}
		film->AddSampleCount(threadIndex, pixelNormalizedCount, screenNormalizedCount);
	}

	// Calculate the sample result luminance
	float newLuminance = 0.f;
	for (u_int sri = 0; sri < used; ++sri) {
		const SampleResult &sr = sampleResults[sri];
		if (sr.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED)) {
			for (u_int i = 0; i < sr.radiance.Size(); ++i) {
				const float luminance = sr.radiance[i].Y();
				verify (!isnan(luminance) && !isinf(luminance) && (luminance >= 0.f));

				if ((luminance > 0.f) && !isnan(luminance) && !isinf(luminance))
					newLuminance += luminance;
			}
		}

		if (sr.HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED)) {
			for (u_int i = 0; i < sr.radiance.Size(); ++i) {
				const float luminance = sr.radiance[i].Y();
				verify (!isnan(luminance) && !isinf(luminance) && (luminance >= 0.f));

				if ((luminance > 0.f) && !isnan(luminance) && !isinf(luminance))
					newLuminance += luminance;
			}
		}
	}
	
	if (sharedData->cooldown && isLargeMutation) {
		//AtomicAdd(&sharedData->totalLuminance, (double)newLuminance);
		sharedData->totalLuminance.fetch_add(static_cast<double>(newLuminance));
		sharedData->sampleCount++;
		if (newLuminance > 0.f)
			sharedData->noBlackSampleCount++;
	}

	const float invMeanIntensity = sharedData->invLuminance;

	// Define the probability of large mutations.
	const float currentLargeMutationProbability = (sharedData->cooldown) ? .5 : largeMutationProbability;

	// Calculate accept probability from old and new image sample
	float accProb;
	if ((currentLuminance > 0.f) && (consecRejects < maxRejects))
		accProb = Min<float>(1.f, newLuminance / currentLuminance);
	else
		accProb = 1.f;
	const float newWeight = accProb + (isLargeMutation ? 1.f : 0.f);
	weight += 1.f - accProb;

	const float rndVal = rndGen->floatValue();

	/*if (!cooldown && (currentSampleResult.size() > 0) && (sampleResults.size() > 0))
		printf("[%d] Current: (%f, %f, %f) [%f] Proposed: (%f, %f, %f) [%f] accProb: %f <%f>\n",
				consecRejects,
				currentSampleResult[0].radiance[0].c[0], currentSampleResult[0].radiance[0].c[1], currentSampleResult[0].radiance[0].c[2], weight,
				sampleResults[0].radiance[0].c[0], sampleResults[0].radiance[0].c[1], sampleResults[0].radiance[0].c[2], newWeight,
				accProb, rndVal);*/
	
	// Try or force accepting of the new sample
	if ((accProb == 1.f) || (rndVal < accProb)) {
		/*if (!cooldown)
			printf("\t\tACCEPTED !\n");*/

		// Add accumulated SampleResult of previous reference sample
		const float norm = weight / (currentLuminance * invMeanIntensity + currentLargeMutationProbability);
		if (norm > 0.f) {
			/*if (!cooldown)
				printf("\t\tContrib: (%f, %f, %f) [%f] consecutiveRejects: %d\n",
						currentSampleResult[0].radiance[0].c[0],
						currentSampleResult[0].radiance[0].c[1],
						currentSampleResult[0].radiance[0].c[2],
						norm, consecRejects);*/

			if (film) {
				for (u_int i = 0; i < currentSampleResultsUsed; ++i) {
					const SampleResult &sr = currentSampleResults[i];
					if (!addOnlyCuastics || (sr.HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED) && sr.isCaustic))
						AtomicAddSampleToFilm(sr, norm);
				}
			}
		}

		lastSampleAcceptance = METRO_ACCEPTED;
		lastSampleWeight = norm;

		// Save new contributions for reference
		weight = newWeight;
		currentStamp = stamp;
		currentLuminance = newLuminance;
		std::copy_n(samples.begin(), requestedSamples, currentSamples.begin());
		std::copy_n(sampleStamps.begin(), requestedSamples, currentSampleStamps.begin());
		currentSampleResultsUsed = used;
		// Only the first `used` slots are live - element-assign avoids the
		// vector-assign of stale tail slots (each SampleResult copy hits
		// its SpectrumGroup inner vector).
		if (currentSampleResults.size() < used)
			currentSampleResults.resize(used);
		for (u_int i = 0; i < used; ++i)
			currentSampleResults[i] = sampleResults[i];

		consecRejects = 0;
	} else {
		/*if (!cooldown)
			printf("\t\tREJECTED !\n");*/
		
		// Add contribution of new sample before rejecting it
		const float norm = newWeight / (newLuminance * invMeanIntensity + currentLargeMutationProbability);
		if (norm > 0.f) {
			/*if (!cooldown)
				printf("\t\tContrib: (%f, %f, %f) [%f] consecutiveRejects: %d\n",
						sampleResults[0].radiance[0].c[0],
						sampleResults[0].radiance[0].c[1],
						sampleResults[0].radiance[0].c[2],
						norm, consecRejects);*/

			if (film) {
				for (u_int i = 0; i < used; ++i) {
					const SampleResult &sr = sampleResults[i];
					if (!addOnlyCuastics || (sr.HasChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED) && sr.isCaustic))
						AtomicAddSampleToFilm(sr, norm);
				}
			}
		}

		lastSampleAcceptance = METRO_REJECTED;
		lastSampleWeight = norm;

		// Restart from previous reference
		stamp = currentStamp;
		std::copy_n(currentSamples.begin(), requestedSamples, samples.begin());
		std::copy_n(currentSampleStamps.begin(), requestedSamples, sampleStamps.begin());

		++consecRejects;
	}

	// Cooldown is used in order to not have problems in the estimation of meanIntensity
	// when large mutation probability is very small.
	if (threadIndex == 0) {
		// Update shared inv. luminance
		const double luminance = (sharedData->totalLuminance > 0.) ?
			(sharedData->totalLuminance / sharedData->sampleCount) : 1.;
		sharedData->invLuminance = (float)(1. / luminance);
	
		/*SLG_LOG("Step: " << sharedData->sampleCount <<  "/" << sharedData->noBlackSampleCount <<
				" Luminance: " << luminance);*/

		const u_longlong sampleCount = sharedData->sampleCount;
		const u_longlong noBlackSampleCount = sharedData->noBlackSampleCount;
		const u_longlong lastSampleCount = sharedData->lastSampleCount;
		const u_longlong lastNoBlackSampleCount = sharedData->lastNoBlackSampleCount;
		const double lastLuminance = sharedData->lastLuminance;

		if (
			// Warmup period
			((sampleCount > warmupMinSampleCount) &&
				(noBlackSampleCount > warmupMinNoBlackSampleCount)) &&
			// Step period
			((sampleCount - lastSampleCount > stepMinSampleCount) &&
				(noBlackSampleCount - lastNoBlackSampleCount > stepMinNoBlackSampleCount))
			) {
			// Time to check if I can end the cooldown
			const double deltaLuminance = fabs(luminance  - lastLuminance) / luminance;

			SLG_LOG("Metropolis sampler image luminance estimation: Step[" << sampleCount <<  "/" << noBlackSampleCount <<
					"] Luminance[" << luminance << "] Delta[" << (deltaLuminance * 100.) << "%]");

			if (
				// To avoid any kind of overflow
				(sampleCount > 0xefffffffu) ||
				// Check if the delta estimated luminance is small enough
				((luminance > 0.) && (deltaLuminance < luminanceThreshold))
				) {
				// I can end the cooldown phase
				SLG_LOG("Metropolis sampler estimated image luminance: " << luminance << " (" << (deltaLuminance * 100.) << "%)");

				sharedData->cooldown = false;
			}

			sharedData->lastSampleCount = sampleCount;
			sharedData->lastNoBlackSampleCount = noBlackSampleCount;
			sharedData->lastLuminance = luminance;
		}
	}

	isLargeMutation = (rndGen->floatValue() < currentLargeMutationProbability);
	if (isLargeMutation) {
		stamp = 1;
		std::fill_n(sampleStamps.begin(), requestedSamples, 0);
		
		++largeMutationCount;
	} else
		++stamp;
}

// Used, most of the times, when not having a film
MetropolisSampleType MetropolisSampler::GetLastSampleAcceptance(float &weight) const {
	weight = lastSampleWeight;
	
	return lastSampleAcceptance;
}

PropertiesUPtr MetropolisSampler::ToProperties() const {
	auto props = std::make_unique<Properties>();
	*props << Sampler::ToProperties() <<
			Property("sampler.metropolis.largesteprate")(largeMutationProbability) <<
			Property("sampler.metropolis.maxconsecutivereject")(maxRejects) <<
			Property("sampler.metropolis.imagemutationrate")(imageMutationRange) <<
			Property("sampler.metropolis.addonlycaustics")(addOnlyCuastics);
	return props;
}

//------------------------------------------------------------------------------
// Static methods used by SamplerRegistry
//------------------------------------------------------------------------------

PropertiesUPtr MetropolisSampler::ToProperties(const Properties &cfg) {
	PropertiesUPtr props = std::make_unique<Properties>();
	*props <<
				cfg.Get(GetDefaultProps()->Get("sampler.type")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.imagesamples.enable")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.metropolis.largesteprate")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.metropolis.maxconsecutivereject")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.metropolis.imagemutationrate")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.metropolis.addonlycaustics"));
	return props;
}

SamplerUPtr MetropolisSampler::FromProperties(const Properties &cfg, const RandomGeneratorUPtr & rndGen,
		FilmPtr film, FilmSampleSplatterRPtr flmSplatter, SamplerSharedDataSPtr sharedData) {
	const bool imageSamplesEnable = cfg.Get(GetDefaultProps()->Get("sampler.imagesamples.enable")).Get<bool>();

	const float rate = Clamp(cfg.Get(GetDefaultProps()->Get("sampler.metropolis.largesteprate")).Get<double>(), 0.0, 1.0);
	const u_int reject = cfg.Get(GetDefaultProps()->Get("sampler.metropolis.maxconsecutivereject")).Get<u_int>();
	const float mutationRate = Clamp(cfg.Get(GetDefaultProps()->Get("sampler.metropolis.imagemutationrate")).Get<double>(), 0.0, 1.0);
	const bool addOnlyCaustics = cfg.Get(GetDefaultProps()->Get("sampler.metropolis.addonlycaustics")).Get<bool>();

	return std::make_unique<MetropolisSampler>(rndGen, film, flmSplatter, imageSamplesEnable,
			reject, rate, mutationRate, addOnlyCaustics,
			dynamic_pointer_cast<MetropolisSamplerSharedData>(sharedData)
	);
}

slg::ocl::Sampler *MetropolisSampler::FromPropertiesOCL(const Properties &cfg) {
	slg::ocl::Sampler *oclSampler = new slg::ocl::Sampler();

	oclSampler->type = slg::ocl::METROPOLIS;
	oclSampler->metropolis.largeMutationProbability = cfg.Get(GetDefaultProps()->Get("sampler.metropolis.largesteprate")).Get<double>();
	oclSampler->metropolis.imageMutationRange = cfg.Get(GetDefaultProps()->Get("sampler.metropolis.imagemutationrate")).Get<double>();
	oclSampler->metropolis.maxRejects = cfg.Get(GetDefaultProps()->Get("sampler.metropolis.maxconsecutivereject")).Get<u_int>();

	return oclSampler;
}

void MetropolisSampler::AddRequiredChannels(Film::FilmChannels &channels, const luxrays::Properties &cfg) {
	// No additional channels required
}

PropertiesUPtr MetropolisSampler::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			Sampler::GetDefaultProps() <<
			Property("sampler.type")(GetObjectTag()) <<
			Property("sampler.metropolis.largesteprate")(.4f) <<
			Property("sampler.metropolis.maxconsecutivereject")(512) <<
			Property("sampler.metropolis.imagemutationrate")(.1f) <<
			Property("sampler.metropolis.addonlycaustics")(false);

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
