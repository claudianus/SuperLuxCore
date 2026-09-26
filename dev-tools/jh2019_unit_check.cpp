/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 *   Licensed under the Apache License, Version 2.0 (the "License");       *
 *   you may not use this file except in compliance with the License.      *
 *   You may obtain a copy of the License at                               *
 *                                                                         *
 *   Unless required by applicable law or agreed to in writing, software   *
 *   distributed under the License is distributed on an "AS IS" BASIS,     *
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or       *
 *   implied.                                                              *
 *   See the License for the specific language governing permissions and   *
 *   limitations under the License.                                        *
 ***************************************************************************/

// E22 unit check: JH2019 (rgb2spec) spectral upsampling invariants on the
// CPU Spectral::Reflectance path. Compiled and run by
// dev-tools/e22_jh2019_upsampling_test.py -- not part of the build.
//
// Gates (exit 1 on failure):
//   - RGB(1,1,1) upsamples to ~1 in every spectral bin
//   - bins of a dense RGB grid stay finite and inside [0,1] (energy
//     conservation; the sigmoid bounds reflectance by construction)
//   - achromatic inputs give flat bins (metameric-exact)
//   - 3-bin CIE projection round-trip error is reported (and must not be
//     worse than the Smits baseline it replaces)
//   - Smits mode still produces its classic basis decomposition
//     (regression sentinel: red must differ from jh2019)

#include "luxrays/core/color/spectral.h"
#include <cstdio>
#include <cmath>

using namespace luxrays;

static int fails = 0;

static void check(const bool ok, const char *name, const char *detail) {
	printf("[%s] %s: %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (!ok)
		++fails;
}

int main() {
	Spectral::SetEnabled(true);

	PathWavelengths sw;

	// ---- JH2019: white -> flat 1 --------------------------------------
	Spectral::SetUpsamplingModel(Spectral::UPSAMPLING_JH2019);
	sw.Sample(0.31f);
	const Spectrum w = Spectral::Reflectance(Spectrum(1.f, 1.f, 1.f), sw);
	char buf[160];
	snprintf(buf, sizeof(buf), "bins=(%.6f %.6f %.6f)",
			w.c[0], w.c[1], w.c[2]);
	check(std::fabs(w.c[0] - 1.f) < 1e-4f &&
			std::fabs(w.c[1] - 1.f) < 1e-4f &&
			std::fabs(w.c[2] - 1.f) < 1e-4f, "jh2019.white", buf);

	const Spectrum g5 = Spectral::Reflectance(Spectrum(.5f, .5f, .5f), sw);
	snprintf(buf, sizeof(buf), "bins=(%.6f %.6f %.6f)",
			g5.c[0], g5.c[1], g5.c[2]);
	check(std::fabs(g5.c[0] - .5f) < 1e-4f &&
			std::fabs(g5.c[1] - .5f) < 1e-4f &&
			std::fabs(g5.c[2] - .5f) < 1e-4f, "jh2019.gray-flat", buf);

	// ---- JH2019: dense grid stays in [0,1] ----------------------------
	float minV = 1e9f, maxV = -1e9f;
	int nonfinite = 0;
	const int N = 16;
	for (int ri = 0; ri <= N; ++ri)
		for (int gi = 0; gi <= N; ++gi)
			for (int bi = 0; bi <= N; ++bi) {
				const float r = ri / float(N), g = gi / float(N),
					b = bi / float(N);
				for (int s = 0; s < 8; ++s) {
					sw.Sample(s / 8.f + 0.5f / 8.f);
					const Spectrum sp =
						Spectral::Reflectance(Spectrum(r, g, b), sw);
					for (int i = 0; i < SPECTRAL_BINS; ++i) {
						if (!std::isfinite(sp.c[i])) {
							++nonfinite;
							continue;
						}
						minV = std::min(minV, sp.c[i]);
						maxV = std::max(maxV, sp.c[i]);
					}
				}
			}
	snprintf(buf, sizeof(buf), "grid range [%.6f %.6f] nonfinite=%d",
			minV, maxV, nonfinite);
	check(nonfinite == 0 && minV >= 0.f && maxV <= 1.f,
			"jh2019.energy-conservation", buf);

	// ---- round-trip stats for both models (3-bin sampling error) ------
	float maxErr[2] = {0.f, 0.f}, sumErr[2] = {0.f, 0.f};
	int cnt = 0;
	for (int m = 0; m < 2; ++m) {
		Spectral::SetUpsamplingModel(m ? Spectral::UPSAMPLING_JH2019
				: Spectral::UPSAMPLING_SMITS);
		for (int ri = 0; ri <= N; ++ri)
			for (int gi = 0; gi <= N; ++gi)
				for (int bi = 0; bi <= N; ++bi) {
					const float r = ri / float(N), g = gi / float(N),
						b = bi / float(N);
					for (int s = 0; s < 8; ++s) {
						sw.Sample(s / 8.f + 0.5f / 8.f);
						const Spectrum sp =
							Spectral::Reflectance(Spectrum(r, g, b), sw);
						const Spectrum pr = Spectral::ProjectToRGB(sp, sw);
						for (int i = 0; i < 3; ++i) {
							const float in = (i == 0) ? r : ((i == 1) ? g : b);
							const float e = std::fabs(pr.c[i] - in);
							sumErr[m] += e;
							maxErr[m] = std::max(maxErr[m], e);
							++cnt;
						}
					}
				}
	}
	cnt /= 2;
	printf("[INFO] roundtrip smits max=%.4f mean=%.4f | jh2019 max=%.4f mean=%.4f\n",
			maxErr[0], sumErr[0] / cnt, maxErr[1], sumErr[1] / cnt);
	snprintf(buf, sizeof(buf), "jh2019 max=%.4f <= smits max=%.4f",
			maxErr[1], maxErr[0]);
	check(maxErr[1] <= maxErr[0], "jh2019.roundtrip-vs-smits", buf);

	// ---- Smits sentinel: classic decomposition still active by default -
	Spectral::SetUpsamplingModel(Spectral::UPSAMPLING_SMITS);
	const Spectrum smRed = Spectral::Reflectance(Spectrum(1.f, 0.f, 0.f), sw);
	Spectral::SetUpsamplingModel(Spectral::UPSAMPLING_JH2019);
	const Spectrum jhRed = Spectral::Reflectance(Spectrum(1.f, 0.f, 0.f), sw);
	const float redDiff = std::fabs(smRed.c[0] - jhRed.c[0]) +
		std::fabs(smRed.c[1] - jhRed.c[1]) + std::fabs(smRed.c[2] - jhRed.c[2]);
	snprintf(buf, sizeof(buf), "smits=(%.4f %.4f %.4f) jh2019=(%.4f %.4f %.4f)",
			smRed.c[0], smRed.c[1], smRed.c[2],
			jhRed.c[0], jhRed.c[1], jhRed.c[2]);
	check(redDiff > 1e-3f, "smits-vs-jh2019-differ", buf);

	// Out-of-gamut input (>1) scales bins by the max channel
	const Spectrum hot = Spectral::Reflectance(Spectrum(2.f, 2.f, 2.f), sw);
	snprintf(buf, sizeof(buf), "bins=(%.4f %.4f %.4f)",
			hot.c[0], hot.c[1], hot.c[2]);
	check(std::fabs(hot.c[0] - 2.f) < 1e-3f &&
			std::fabs(hot.c[1] - 2.f) < 1e-3f &&
			std::fabs(hot.c[2] - 2.f) < 1e-3f, "jh2019.hdr-scale", buf);

	printf("unit-check %s\n", fails ? "FAIL" : "PASS");
	return fails ? 1 : 0;
}
