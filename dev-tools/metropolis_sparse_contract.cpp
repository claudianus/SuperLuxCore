// SPDX-License-Identifier: Apache-2.0
// Exercise the actual CPU sampler on a sparse, discontinuous integrand.
// A seeded SSS walk has this kind of support in primary sample space.
#include <cmath>
#include <cstdint>
#include <iostream>
#include "slg/samplers/metropolis.h"

static uint32_t Hash(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	return x ^ (x >> 16);
}

int main() {
	using namespace luxrays;
	using namespace slg;
	constexpr uint32_t grid = 1u << 24;
	uint32_t nonzero = 0;
	for (uint32_t i = 0; i < grid; ++i)
		nonzero += (Hash(i) & 2047u) == 0;
	const double expected = double(nonzero) / grid;
	bool passed = true;
	for (const unsigned legacyLimit : {1u, 512u}) {
		auto rng = std::make_unique<RandomGenerator>(817u);
		FilmSampleSplatterUPtr splatter;
		auto shared = std::make_shared<MetropolisSamplerSharedData>();
		MetropolisSampler sampler(rng, FilmPtr(nullptr), splatter, false,
				legacyLimit, 1.f, .1f, false, shared);
		// Isolate acceptance from the separately estimated warmup normalizer:
		// enumerate the complete float grid for an exact, independent b.
		shared->cooldown = false;
		shared->invLuminance = float(1. / expected);
		sampler.SetThreadIndex(1);
		sampler.RequestSamples(SCREEN_NORMALIZED_ONLY, 1);
		Film::FilmChannels channels{Film::RADIANCE_PER_SCREEN_NORMALIZED};
		std::vector<SampleResult> results(2);
		for (auto &result : results) result.Init(&channels, 1, 0);
		// The live prefix must exclude this stale reserved slot.
		results[1].radiance[0] = Spectrum(1e9f);
		double sum = 0., current = 0.;
		constexpr unsigned count = 4000000;
		for (unsigned i = 0; i < count; ++i) {
			const uint32_t cell = uint32_t(sampler.GetSample(0) * grid);
			const double proposed = (Hash(cell) & 2047u) == 0 ? 1. : 0.;
			results[0].radiance[0] = Spectrum(float(proposed));
			sampler.NextSample(results, 1);
			float weight;
			if (sampler.GetLastSampleAcceptance(weight) == METRO_ACCEPTED) {
				sum += weight * current;
				current = proposed;
			} else sum += weight * proposed;
		}
		const double mean = sum / count;
		const double error = std::abs(mean - expected) / expected;
		std::cout << "METROPOLIS_SPARSE limit=" << legacyLimit
				<< " expected=" << expected << " mean=" << mean
				<< " error=" << error << '\n';
		passed &= std::isfinite(mean) && error < .01;
	}
	return passed ? 0 : 1;
}
