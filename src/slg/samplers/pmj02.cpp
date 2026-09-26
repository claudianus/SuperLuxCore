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

#include <boost/lexical_cast.hpp>
#include <memory>

#include "luxrays/core/color/color.h"
#include "luxrays/usings.h"
#include "slg/usings.h"
#include "slg/samplers/sampler.h"
#include "slg/samplers/pmj02.h"
#include "slg/samplers/sobolsequence.h"
#include "slg/utils/mortoncurve.h"

#include "pmj02/pmj02.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PMJ02 sampler
//
// Progressive multi-jittered (0,2) sequences (Christensen, Kensler and
// Kilpatrick 2018). The 2D generator is vendored from pmj-cpp (MIT,
// Andrew Helmer, based on Christensen et al. + Pharr 2019); the sampler
// scaffolding (buckets, passes, adaptive) mirrors SobolSampler.
//------------------------------------------------------------------------------

PMJ02Sampler::PMJ02Sampler(
	const RandomGeneratorUPtr & rnd,
	FilmPtr flm,  // Film is optional!
	const FilmSampleSplatterUPtr& flmSplatter,
	const bool imgSamplesEnable,
	const float adaptiveStr,
	const float adaptiveUserImpWeight,
	const u_int bucketSz,
	const u_int tileSz,
	const u_int superSmpl,
	const u_int overlap,
	SamplerSharedDataSPtr samplerSharedData,
	const u_int tableSmpls
) :
		Sampler(rnd, flm, flmSplatter, imgSamplesEnable),
		sharedData(static_pointer_cast<SobolSamplerSharedData>(samplerSharedData)),
		tableSamples(RoundUpPow2(Max(tableSmpls, 16u))),
		adaptiveStrength(adaptiveStr),
		adaptiveUserImportanceWeight(adaptiveUserImpWeight),
		bucketSize(bucketSz),
		tileSize(tileSz),
		superSampling(superSmpl),
		overlapping(overlap),
		bucketIndex(std::make_shared<u_int>(0))
{
	// Seed the per-pair generation (distinct deterministic seed per pair so
	// pairs stay independent; renderengine.seed flows in through rnd).
	baseSeed = rnd->uintValue();
	// A few pairs up front; RequestSamples grows to the needed count.
	EnsurePairs(Min(PMJ02_TABLE_PAIRS, 8u));
}
PMJ02Sampler::PMJ02Sampler(
	const RandomGeneratorUPtr & rnd,
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
	const u_int tableSmpls
) :
		PMJ02Sampler(rnd, FilmPtr(std::addressof(flm)), flmSplatter, imgSamplesEnable,
				adaptiveStr, adaptiveUserImpWeight, bucketSz, tileSz,
				superSmpl, overlap, samplerSharedData, tableSmpls)
{
}

PMJ02Sampler::~PMJ02Sampler() {
}

void PMJ02Sampler::InitNewSample() {
	const bool doImageSamples = (imageSamplesEnable && film);

	const u_int *filmSubRegion;
	u_int subRegionWidth, subRegionHeight, tiletWidthCount, tileHeightCount, bucketCount;

	if (doImageSamples) {
		filmSubRegion = GetFilm().GetSubRegion();

		subRegionWidth = filmSubRegion[1] - filmSubRegion[0] + 1;
		subRegionHeight = filmSubRegion[3] - filmSubRegion[2] + 1;

		tiletWidthCount = (subRegionWidth + tileSize - 1) / tileSize;
		tileHeightCount = (subRegionHeight + tileSize - 1) / tileSize;

		bucketCount = overlapping * (tiletWidthCount * tileSize * tileHeightCount * tileSize + bucketSize - 1) / bucketSize;
	} else
		bucketCount = 0xffffffffu;

	// Update pixelIndexOffset

	// Bound for the adaptive re-pick loop below (see sobol.cpp)
	u_int skipAttempts = 0;

	for (;;) {
		passOffset++;
		if (passOffset >= superSampling) {
			pixelOffset++;
			passOffset = 0;

			if (pixelOffset >= bucketSize) {
				// Ask for a new bucket
				auto [newBucketIndex, newBucketSeed] = sharedData->GetNewBucket(bucketCount);
				*bucketIndex = newBucketIndex;

				pixelOffset = 0;
				passOffset = 0;

				// Initialize the rng0, rng1 and rngPass generator
				rngGenerator.init(newBucketSeed);
			}
		}

		// Initialize sample0 and sample 1

		u_int px, py;
		if (doImageSamples) {
			// Transform the bucket index in a pixel coordinate

			const u_int pixelBucketIndex = (*bucketIndex / overlapping) * bucketSize + pixelOffset;
			const u_int mortonCurveOffset = pixelBucketIndex % (tileSize * tileSize);
			const u_int pixelTileIndex = pixelBucketIndex / (tileSize * tileSize);

			const u_int subRegionPixelX = (pixelTileIndex % tiletWidthCount) * tileSize + DecodeMorton2X(mortonCurveOffset);
			const u_int subRegionPixelY = (pixelTileIndex / tiletWidthCount) * tileSize + DecodeMorton2Y(mortonCurveOffset);
			if ((subRegionPixelX >= subRegionWidth) || (subRegionPixelY >= subRegionHeight)) {
				// Skip the pixels out of the film sub region
				continue;
			}

			px = filmSubRegion[0] + subRegionPixelX;
			py = filmSubRegion[2] + subRegionPixelY;

			// Check if the current pixel is over or under the convergence threshold
			auto& film = sharedData->GetEngineFilm();
			if ((adaptiveStrength > 0.f) && GetFilm().HasChannel(Film::NOISE)) {
				// Pixels are sampled in accordance with how far from convergence they are
				const float noise = *(GetFilm().channel_NOISE->GetPixel(px, py));

				// Factor user driven importance sampling too
				float threshold;
				if (GetFilm().HasChannel(Film::USER_IMPORTANCE)) {
					const float userImportance = *(GetFilm().channel_USER_IMPORTANCE->GetPixel(px, py));

					// Noise is initialized to INFINITY at start
					if (isinf(noise))
						threshold = userImportance;
					else
						threshold = (userImportance > 0.f) ? Lerp(adaptiveUserImportanceWeight, noise, userImportance) : 0.f;
				} else
					threshold = noise;

				// The floor for the pixel importance is given by the adaptiveness strength
				threshold = Max(threshold, 1.f - adaptiveStrength);

				if (rndGen->floatValue() > threshold) {
					// Skip this pixel and try the next one; after a full
					// bucket sweep accept it anyway (bounded loop)
					if (++skipAttempts < bucketSize * superSampling) {
						// Workaround for preserving random number distribution behavior
						rngGenerator.floatValue();
						rngGenerator.floatValue();
						rngGenerator.uintValue();

						continue;
					}
				}
			}

			pass = sharedData->GetNewPixelPass(subRegionPixelX + subRegionPixelY * subRegionWidth);
		} else {
			px = 0;
			py = 0;

			pass = sharedData->GetNewPixelPass();
		}

		pixelX = px;
		pixelY = py;

		// Screen sample from PMJ02 pair 0 (matches Sobol sample0/sample1 role)
		const PMJ02Set &set0 = pmjSets[0];
		const u_int idx0 = pass % tableSamples;
		const u_int cycle0 = pass / tableSamples;
		const float sx = ((SobolSequence::BlueNoiseHash(
				px + py * 0x9e3779b9u + cycle0 * 0xc2b2ae35u) ^
				*sharedData->seedBase) & 0xffffffu) * (1.f / 16777216.f);
		const float sy = ((SobolSequence::BlueNoiseHash(
				py + px * 0x9e3779b9u + 0x85ebca6bu + cycle0 * 0xc2b2ae35u) ^
				*sharedData->seedBase) & 0xffffffu) * (1.f / 16777216.f);
		float fx0 = set0.x[idx0] + sx;
		float fy0 = set0.y[idx0] + sy;
		// Wrap (Cranley-Patterson), like the dimension pairs below: clamping
		// would pile half the screen samples at the pixel corner.
		sample0 = px + (fx0 >= 1.f ? fx0 - 1.f : fx0);
		sample1 = py + (fy0 >= 1.f ? fy0 - 1.f : fy0);
		break;
	}
}

void PMJ02Sampler::EnsurePairs(const u_int count) {
	const u_int target = Min(count, (u_int)PMJ02_TABLE_PAIRS);
	while (pmjSets.size() < target) {
		const u_int p = pmjSets.size();
		pmj::SetSeed(baseSeed + p * 0x9e3779b9u);
		std::unique_ptr<pmj::Point[]> pts =
				pmj::GetPMJ02Samples(tableSamples);
		PMJ02Set s;
		s.x.resize(tableSamples);
		s.y.resize(tableSamples);
		for (u_int i = 0; i < tableSamples; ++i) {
			s.x[i] = static_cast<float>(pts[i].x);
			s.y[i] = static_cast<float>(pts[i].y);
		}
		pmjSets.push_back(std::move(s));
	}
}

void PMJ02Sampler::RequestSamples(const SampleType smplType, const u_int size) {
	Sampler::RequestSamples(smplType, size);

	// One 2D set per dimension pair
	EnsurePairs((size + 1) / 2);

	pixelOffset = bucketSize * bucketSize;
	passOffset = superSampling;

	InitNewSample();
}

float PMJ02Sampler::GetSample(const u_int index) {
	assert (index < requestedSamples);

	if (index < 2)
		return (index == 0) ? sample0 : sample1;

	// Dimension pair (dims 2,3 -> pair 1; 4,5 -> pair 2; ...; odd leftover
	// dims reuse the pair's x). Per-pixel Cranley-Patterson rotation keeps
	// pixels decorrelated; the wrap cycle rotates too so any spp is unbiased.
	// Beyond PMJ02_TABLE_PAIRS the tables wrap around BUT the scramble is
	// keyed on the unclamped pair index so over-range dims still decorrelate
	// (clamping the index would make all high dims return identical values).
	const u_int pairIdx = index >> 1;
	// Lazily grow so pairIdx < pmjSets.size() whenever pairIdx <
	// PMJ02_TABLE_PAIRS; above that the modulus is the fixed table count,
	// matching the device-side wrap (sampler_pmj02_funcs.cl).
	EnsurePairs(pairIdx + 1);
	const PMJ02Set &set = pmjSets[pairIdx % pmjSets.size()];
	const u_int idx = pass % tableSamples;
	const u_int cycle = pass / tableSamples;
	const float sx = ((SobolSequence::BlueNoiseHash(
			pixelX + pixelY * 0x9e3779b9u + pairIdx * 0x85ebca6bu +
			cycle * 0xc2b2ae35u) ^
			*sharedData->seedBase) & 0xffffffu) * (1.f / 16777216.f);
	const float sy = ((SobolSequence::BlueNoiseHash(
			pixelY + pixelX * 0x9e3779b9u + pairIdx * 0x85ebca6bu + 0x27d4eb2du +
			cycle * 0xc2b2ae35u) ^
			*sharedData->seedBase) & 0xffffffu) * (1.f / 16777216.f);
	const float u = set.x[idx] + sx;
	const float v = set.y[idx] + sy;
	return ((index & 1) == 0) ?
			(u >= 1.f ? u - 1.f : u) :
			(v >= 1.f ? v - 1.f : v);
}

void PMJ02Sampler::NextSample(const vector<SampleResult> &sampleResults) {
	if (film) {
		switch (sampleType) {
			case PIXEL_NORMALIZED_ONLY:
				GetFilm().AddSampleCount(threadIndex, 1.0, 0.0);
				break;
			case SCREEN_NORMALIZED_ONLY:
				GetFilm().AddSampleCount(threadIndex, 0.0, 1.0);
				break;
			case PIXEL_NORMALIZED_AND_SCREEN_NORMALIZED:
				GetFilm().AddSampleCount(threadIndex, 1.0, 1.0);
				break;
			case ONLY_AOV_SAMPLE:
				break;
			default:
				throw runtime_error("Unknown sample type in PMJ02Sampler::NextSample(): " + ToString(sampleType));
		}

		AtomicAddSamplesToFilm(sampleResults);
	}

	InitNewSample();
}

u_int PMJ02Sampler::GetPassCount() const {
	const bool doImageSamples = (imageSamplesEnable && film);
	if (!doImageSamples)
		throw runtime_error("Called PMJ02Sampler::GetPassCount() without sampling an image");
	
	const u_int *filmSubRegion = GetFilm().GetSubRegion();

	const u_int subRegionWidth = filmSubRegion[1] - filmSubRegion[0] + 1;
	const u_int subRegionHeight = filmSubRegion[3] - filmSubRegion[2] + 1;

	const u_int tiletWidthCount = (subRegionWidth + tileSize - 1) / tileSize;
	const u_int tileHeightCount = (subRegionHeight + tileSize - 1) / tileSize;

	const u_int bucketCount = overlapping * (tiletWidthCount * tileSize * tileHeightCount * tileSize + bucketSize - 1) / bucketSize;

	return sharedData->GetPassCount(bucketCount);
}

void PMJ02Sampler::FillDeviceTables(u_int pairs, u_int samples, u_int seedBase,
		float *tables) {
	for (u_int p = 0; p < pairs; ++p) {
		pmj::SetSeed(seedBase + p * 0x9e3779b9u);
		std::unique_ptr<pmj::Point[]> pts = pmj::GetPMJ02Samples(samples);
		for (u_int i = 0; i < samples; ++i) {
			tables[(p * samples + i) * 2] = static_cast<float>(pts[i].x);
			tables[(p * samples + i) * 2 + 1] = static_cast<float>(pts[i].y);
		}
	}
}

void PMJ02SamplerSharedData::Reset() {
	if (HasEngineFilm()) {
		const u_int *subRegion = GetEngineFilm().GetSubRegion();
		const u_int filmRegionPixelCount = (subRegion[1] - subRegion[0] + 1) * (subRegion[3] - subRegion[2] + 1);

		passPerPixel.resize(filmRegionPixelCount, 0);
	} else
		passPerPixel.resize(1, 0);

	bucketIndex = std::make_shared<u_int>(0);
}

PropertiesUPtr PMJ02Sampler::ToProperties() const {
	auto props_ptr = std::make_unique<Properties>();
	auto& props = *props_ptr;
	props << Sampler::ToProperties() <<
			Property("sampler.pmj02.adaptive.strength")(adaptiveStrength) <<
			Property("sampler.pmj02.adaptive.userimportanceweight")(adaptiveUserImportanceWeight) <<
			Property("sampler.pmj02.bucketsize")(bucketSize) <<
			Property("sampler.pmj02.tilesize")(tileSize) <<
			Property("sampler.pmj02.supersampling")(superSampling) <<
			Property("sampler.pmj02.overlapping")(overlapping) <<
			Property("sampler.pmj02.samples")(tableSamples);
	return props_ptr;
}

//------------------------------------------------------------------------------
// Static methods used by SamplerRegistry
//------------------------------------------------------------------------------

PropertiesUPtr PMJ02Sampler::ToProperties(const Properties &cfg) {
	PropertiesUPtr props = std::make_unique<Properties>();
	*props <<
				cfg.Get(GetDefaultProps()->Get("sampler.type")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.imagesamples.enable")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.pmj02.adaptive.strength")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.pmj02.adaptive.userimportanceweight")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.pmj02.bucketsize")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.pmj02.tilesize")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.pmj02.supersampling")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.pmj02.overlapping")) <<
			cfg.Get(GetDefaultProps()->Get("sampler.pmj02.samples"));
	return props;
}

SamplerUPtr PMJ02Sampler::FromProperties(const Properties &cfg, const RandomGeneratorUPtr & rndGen,
		FilmPtr film, const FilmSampleSplatterUPtr& flmSplatter,
		SamplerSharedDataSPtr sharedData
) {
	const bool imageSamplesEnable = cfg.Get(GetDefaultProps()->Get("sampler.imagesamples.enable")).Get<bool>();

	const float adaptiveStrength = Clamp(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.adaptive.strength")).Get<double>(), 0.0, .95);
	const float adaptiveUserImportanceWeight = cfg.Get(GetDefaultProps()->Get("sampler.pmj02.adaptive.userimportanceweight")).Get<double>();
	const float bucketSize = RoundUpPow2(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.bucketsize")).Get<u_int>());
	const float tileSize = RoundUpPow2(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.tilesize")).Get<u_int>());
	const float superSampling = cfg.Get(GetDefaultProps()->Get("sampler.pmj02.supersampling")).Get<u_int>();
	const float overlapping = cfg.Get(GetDefaultProps()->Get("sampler.pmj02.overlapping")).Get<u_int>();
	const u_int tableSamples = RoundUpPow2(Max(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.samples")).Get<u_int>(), 16u));

	auto sampler = std::make_unique<PMJ02Sampler>(rndGen, film, flmSplatter, imageSamplesEnable,
			adaptiveStrength, adaptiveUserImportanceWeight,
			bucketSize, tileSize, superSampling, overlapping,
			dynamic_pointer_cast<SobolSamplerSharedData>(sharedData),
			tableSamples
	);

	return sampler;
}

slg::ocl::Sampler *PMJ02Sampler::FromPropertiesOCL(const Properties &cfg) {
	slg::ocl::Sampler *oclSampler = new slg::ocl::Sampler();

	oclSampler->type = slg::ocl::PMJ02SAMPLER;
	oclSampler->pmj02.adaptiveStrength = Clamp(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.adaptive.strength")).Get<double>(), 0.0, .95);
	oclSampler->pmj02.adaptiveUserImportanceWeight = cfg.Get(GetDefaultProps()->Get("sampler.pmj02.adaptive.userimportanceweight")).Get<double>();
	oclSampler->pmj02.bucketSize = RoundUpPow2(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.bucketsize")).Get<u_int>());
	oclSampler->pmj02.tileSize = RoundUpPow2(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.tilesize")).Get<u_int>());
	oclSampler->pmj02.superSampling = cfg.Get(GetDefaultProps()->Get("sampler.pmj02.supersampling")).Get<u_int>();
	oclSampler->pmj02.overlapping = cfg.Get(GetDefaultProps()->Get("sampler.pmj02.overlapping")).Get<u_int>();
	oclSampler->pmj02.tableSamples = RoundUpPow2(Max(cfg.Get(GetDefaultProps()->Get("sampler.pmj02.samples")).Get<u_int>(), 16u));
	oclSampler->pmj02.tablePairs = PMJ02_TABLE_PAIRS;

	return oclSampler;
}

void PMJ02Sampler::AddRequiredChannels(Film::FilmChannels &channels, const luxrays::Properties &cfg) {
	const bool imageSamplesEnable = cfg.Get(GetDefaultProps()->Get("sampler.imagesamples.enable")).Get<bool>();

	const float str = cfg.Get(GetDefaultProps()->Get("sampler.pmj02.adaptive.strength")).Get<double>();

	if (imageSamplesEnable && (str > 0.f))
		channels.insert(Film::NOISE);
}

PropertiesUPtr PMJ02Sampler::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			Sampler::GetDefaultProps() <<
			Property("sampler.type")(GetObjectTag()) <<
			Property("sampler.pmj02.adaptive.strength")(.95f) <<
			Property("sampler.pmj02.adaptive.userimportanceweight")(.75f) <<
			Property("sampler.pmj02.bucketsize")(16) <<
			Property("sampler.pmj02.tilesize")(16) <<
			Property("sampler.pmj02.supersampling")(1) <<
			Property("sampler.pmj02.overlapping")(1) <<
			Property("sampler.pmj02.samples")(4096);

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
