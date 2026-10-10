// SPDX-License-Identifier: Apache-2.0
// Full startup, with an independent complete-float-grid integral.
// Unlike the acceptance contract, this never supplies b or disables warmup.
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <thread>
#include "slg/samplers/metropolis.h"

static uint32_t Hash(uint32_t x) {
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	return x ^ (x >> 16);
}

struct ChainResult {
	double sum = 0.;
	bool finite = true;
};

static ChainResult Chain(const unsigned seed, const float rate,
		const unsigned count, const unsigned mask, const bool black,
		const unsigned threadIndex,
		const std::shared_ptr<slg::MetropolisSamplerSharedData> &shared) {
	using namespace luxrays;
	using namespace slg;
	auto rng = std::make_unique<RandomGenerator>(seed);
	FilmSampleSplatterUPtr splatter;
	MetropolisSampler sampler(rng, FilmPtr(nullptr), splatter, false,
			512, rate, .1f, false, shared);
	sampler.SetThreadIndex(threadIndex);
	sampler.RequestSamples(SCREEN_NORMALIZED_ONLY, 1);
	Film::FilmChannels channels{Film::RADIANCE_PER_SCREEN_NORMALIZED};
	std::vector<SampleResult> results(2);
	for (auto &result : results) result.Init(&channels, 1, 0);
	results[1].radiance[0] = Spectrum(1e9f);
	ChainResult result;
	double current = 0.;
	for (unsigned i = 0; i < count; ++i) {
		const uint32_t cell = uint32_t(sampler.GetSample(0) * (1u << 24));
		const double proposed = !black && ((Hash(cell) & mask) == 0) ? 1. : 0.;
		results[0].radiance[0] = Spectrum(float(proposed));
		sampler.NextSample(results, 1);
		float weight;
		if (sampler.GetLastSampleAcceptance(weight) == METRO_ACCEPTED) {
			result.sum += weight * current;
			current = proposed;
		} else result.sum += weight * proposed;
		result.finite &= std::isfinite(weight);
	}
	return result;
}

static double Oracle(const unsigned mask) {
	uint32_t nonzero = 0;
	for (uint32_t i = 0; i < (1u << 24); ++i)
		nonzero += (Hash(i) & mask) == 0;
	return double(nonzero) / (1u << 24);
}

int main() {
	using slg::MetropolisSamplerSharedData;
	std::cout << std::setprecision(12);
	bool passed = true;
	const double sparse = Oracle(2047), dense = Oracle(7);
	for (const unsigned seed : {131u, 817u, 919u}) {
		// A nonzero-index worker must not depend on thread 0 being scheduled.
		for (const unsigned threadIndex : {0u, 1u}) {
			auto shared = std::make_shared<MetropolisSamplerSharedData>();
			const auto result = Chain(seed, .4f, 32000000, 2047, false, threadIndex, shared);
			const double mean = result.sum / 32000000;
			const double error = std::abs(mean - sparse) / sparse;
			const bool ok = result.finite && error < .03;
			std::cout << "METROPOLIS_STARTUP sparse seed=" << seed << " thread=" << threadIndex
					<< " samples=32000000 expected=" << sparse << " mean=" << mean
					<< " error=" << error << " passed=" << ok << std::endl;
			passed &= ok;
		}
		// Actual concurrent chains share the live normalizer and its freeze.
		auto shared = std::make_shared<MetropolisSamplerSharedData>();
		std::vector<ChainResult> results(8);
		std::vector<std::thread> workers;
		for (unsigned i = 0; i < 8; ++i)
			workers.emplace_back([&, i] { results[i] = Chain(seed + 1009 * i, .4f,
					8000000, 2047, false, i, shared); });
		for (auto &worker : workers) worker.join();
		double sum = 0.;
		bool finite = true;
		for (const auto &result : results) { sum += result.sum; finite &= result.finite; }
		const double mean = sum / 64000000;
		const double error = std::abs(mean - sparse) / sparse;
		const bool ok = finite && error < .03;
		std::cout << "METROPOLIS_STARTUP concurrent seed=" << seed
				<< " workers=8 samples=64000000 expected=" << sparse << " mean=" << mean
				<< " error=" << error << " passed=" << ok << std::endl;
		passed &= ok;
		for (const float rate : {.4f, 1.f}) {
			auto denseShared = std::make_shared<MetropolisSamplerSharedData>();
			const auto result = Chain(seed, rate, 4000000, 7, false, 0, denseShared);
			const double mean = result.sum / 4000000;
			const double error = std::abs(mean - dense) / dense;
			const bool ok = result.finite && error < .03 && !denseShared->cooldown;
			std::cout << "METROPOLIS_STARTUP dense seed=" << seed << " rate=" << rate
					<< " samples=4000000 expected=" << dense << " mean=" << mean
					<< " error=" << error << " cooldown=" << denseShared->cooldown
					<< " passed=" << ok << std::endl;
			passed &= ok;
		}
	}
	for (const float rate : {0.f, .4f, 1.f}) {
		auto shared = std::make_shared<MetropolisSamplerSharedData>();
		const auto result = Chain(817, rate, 20000, 2047, true, 0, shared);
		const bool ok = result.finite && result.sum == 0. &&
				shared->sampleCount == 20000 && shared->noBlackSampleCount == 0;
		std::cout << "METROPOLIS_STARTUP black rate=" << rate << " samples=20000"
				<< " uniform=" << shared->sampleCount << " sum=" << result.sum
				<< " passed=" << ok << std::endl;
		passed &= ok;
	}
	return passed ? 0 : 1;
}
