// SPDX-License-Identifier: Apache-2.0
//
// E15: statistical validation of the vendored PMJ(0,2) tables
// (src/slg/samplers/pmj02/) used by PMJ02Sampler on CPU and GPU.
//
// Defining property of a (0,2)-sequence in base 2 (Christensen et al.
// 2018): the first 4^m points form a (0,m,2)-net — every elementary
// interval [a/2^i,(a+1)/2^i) x [b/2^j,(b+1)/2^j) with i+j = 2m contains
// exactly one point. This is deterministic, not a statistical test.
//
// Additionally: chi-squared uniformity over a fine grid, and a range/
// NaN check. Two seeds are exercised (the sampler generates one table
// set per dimension pair with different seeds).
//
// Build & run:
//   clang++ -std=c++17 -O2 -I src/slg/samplers/pmj02 \
//       dev-tools/e15_pmj02_tables_test.cpp \
//       src/slg/samplers/pmj02/pmj02.cc src/slg/samplers/pmj02/pmj02_util.cc \
//       src/slg/samplers/pmj02/pmj_util.cc src/slg/samplers/pmj02/select_subquad.cc \
//       -o /tmp/e15_pmj02 && /tmp/e15_pmj02

#include <cstdio>
#include <cmath>
#include <memory>
#include <vector>

#include "pmj02.h"
#include "pmj_util.h"

static int failures = 0;

static void check(const char *name, bool ok, const char *detail = "") {
	printf("[%s] %s %s\n", ok ? "PASS" : "FAIL", name, detail);
	if (!ok)
		++failures;
}

// Verify the (0,m,2)-net property for the first 4^m points: every
// elementary interval with i+j = 2m holds exactly one point.
static bool check_net(const pmj::Point *pts, const int m) {
	const int n = 1 << (2 * m);
	// Enumerate (i,j) with i+j = 2m
	for (int i = 0; i <= 2 * m; ++i) {
		const int j = 2 * m - i;
		const int nx = 1 << i, ny = 1 << j;
		std::vector<int> count(nx * ny, 0);
		for (int k = 0; k < n; ++k) {
			const int cx = std::min(nx - 1, (int)(pts[k].x * nx));
			const int cy = std::min(ny - 1, (int)(pts[k].y * ny));
			count[cx + cy * nx]++;
		}
		for (int c = 0; c < nx * ny; ++c)
			if (count[c] != 1)
				return false;
	}
	return true;
}

int main() {
	const int N = 4096; // 4^6

	for (const unsigned int seed : {17u, 0x9e3779b9u}) {
		pmj::SetSeed(seed);
		std::unique_ptr<pmj::Point[]> pts = pmj::GetPMJ02Samples(N);

		// Range / sanity
		bool range_ok = true;
		for (int i = 0; i < N; ++i) {
			if (!(pts[i].x >= 0.0 && pts[i].x < 1.0 &&
					pts[i].y >= 0.0 && pts[i].y < 1.0)) {
				range_ok = false;
				break;
			}
		}
		check("range [0,1)^2", range_ok);

		// (0,m,2)-net for every prefix m = 1..6
		for (int m = 1; m <= 6; ++m) {
			char name[64];
			snprintf(name, sizeof(name), "(0,%d,2)-net property (seed %u)", m, seed);
			check(name, check_net(pts.get(), m));
		}

		// Chi-squared uniformity over a 64x64 grid (N=4096 -> 1 expected/cell)
		{
			const int G = 64;
			std::vector<int> count(G * G, 0);
			for (int i = 0; i < N; ++i) {
				const int cx = std::min(G - 1, (int)(pts[i].x * G));
				const int cy = std::min(G - 1, (int)(pts[i].y * G));
				count[cx + cy * G]++;
			}
			// Expected 1 per cell; chi2 = sum((obs-1)^2 / 1)
			double chi2 = 0.0;
			int occupied = 0;
			for (int c = 0; c < G * G; ++c) {
				const double d = count[c] - 1.0;
				chi2 += d * d;
				occupied += (count[c] > 0);
			}
			char detail[128];
			snprintf(detail, sizeof(detail),
					"chi2=%.1f (df=%d), occupied=%d/%d (seed %u)",
					chi2, G * G - 1, occupied, G * G, seed);
			// A (0,6,2)-net puts exactly one point per 64x64 cell
			check("chi-squared 64x64 uniformity", occupied == G * G, detail);
		}
	}

	printf("\n%s (%d failures)\n", failures ? "FAILURES" : "ALL PASS", failures);
	return failures ? 1 : 0;
}
