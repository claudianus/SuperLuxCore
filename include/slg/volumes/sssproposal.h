// SPDX-License-Identifier: Apache-2.0
#ifndef _SLG_SSS_PROPOSAL_H
#define _SLG_SSS_PROPOSAL_H

#include "luxrays/core/color/color.h"
#include "luxrays/core/geometry/vector.h"
#include "luxrays/utils/mc.h"

namespace slg {

// Conditional distance distribution after a direction has been sampled.
// Fixed storage per path; no training data or approximate radiance cache.
struct SSSFlightProposal {
	bool active = false;
	float probabilities[9] = {};
	float rates[9] = {};
};

// Classic phase + two opposite Dwivedi proposals. The last entry normal is
// only a proposal axis: curved/thin/nested geometry is still traced globally.
// All physical phase functions, extinction and boundary BSDFs remain intact.
struct SSSDirectionProposal {
	luxrays::Vector axis;
	float channel[3], sigmaT[3], v[3], phaseLog[3];
	float classic = .5f;
	float forward = .9f;

	float GuidedPdf(const float cosine, const unsigned ch) const {
		return 1.f / (2.f * M_PI * (v[ch] - cosine) * phaseLog[ch]);
	}

	float Pdf(const luxrays::Vector &direction, const float classicPdf,
			SSSFlightProposal *flight = nullptr) const {
		const float cosine = luxrays::Clamp(luxrays::Dot(direction, axis), -1.f, 1.f);
		float sum = 0.f;
		for (unsigned i = 0; i < 3; ++i) {
			const float densities[3] = {classic * channel[i] * classicPdf,
					forward * (1.f - classic) * channel[i] * GuidedPdf(cosine, i),
					(1.f - forward) * (1.f - classic) * channel[i] * GuidedPdf(-cosine, i)};
			for (unsigned branch = 0; branch < 3; ++branch) {
				const unsigned index = 3 * branch + i;
				sum += densities[branch];
				if (flight) {
					flight->probabilities[index] = densities[branch];
					flight->rates[index] = sigmaT[i] * (branch == 0 ? 1.f :
							1.f + (branch == 1 ? -cosine : cosine) / v[i]);
				}
			}
		}
		if (flight) {
			flight->active = sum > 0.f;
			if (flight->active)
				for (unsigned i = 0; i < 9; ++i) flight->probabilities[i] /= sum;
		}
		return sum;
	}

	luxrays::Vector SampleGuide(float u0, const float u1) const {
		// Called on the guide side with u0 rescaled to [0,1).
		unsigned ch = 2;
		for (unsigned i = 0; i < 2; ++i) {
			if (u0 < channel[i]) { ch = i; break; }
			u0 -= channel[i];
		}
		u0 = luxrays::Clamp(u0 / channel[ch], 0.f, .99999994f);
		const bool reverse = u0 >= forward;
		u0 = reverse ? (u0 - forward) / (1.f - forward) : u0 / forward;
		const float cosine = luxrays::Clamp(v[ch] - (v[ch] + 1.f) * expf(-u0 * phaseLog[ch]), -1.f, 1.f);
		const float sine = sqrtf(luxrays::Max(0.f, 1.f - cosine * cosine));
		luxrays::Vector t, b;
		luxrays::CoordinateSystem(axis, &t, &b);
		return sine * (cosf(2.f * M_PI * u1) * t + sinf(2.f * M_PI * u1) * b) +
				(reverse ? -cosine : cosine) * axis;
	}
};
}
#endif
