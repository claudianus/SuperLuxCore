/* SPDX-License-Identifier: Apache-2.0 */
#ifndef _SLG_DENOISING_FEATURES_H
#define _SLG_DENOISING_FEATURES_H

#include "luxrays/core/color/color.h"
#include "luxrays/core/geometry/vector.h"

namespace slg {

// Auxiliary, directional white-environment estimates, never transport weights.
// Kept outside SampleResult/device task storage: requesting no guides costs no
// per-path allocation or extra GPU task memory.
struct DenoisingFeatures {
	luxrays::Spectrum diffuse, specular;
	luxrays::Vector normal;
	float roughness = 0.f, weight = 0.f, nonspecularWeight = 0.f;
	bool transmission = false;

	void Add(const luxrays::Spectrum &albedo, const luxrays::Vector &n,
			const float perceptualRoughness, const bool diffuseLobe) {
		const float w = albedo.Filter();
		if (!(w > 0.f)) return;
		if (diffuseLobe) diffuse += albedo;
		else specular += albedo;
		normal += n * w;
		roughness += perceptualRoughness * w;
		weight += w;
		// Cycles' feature deferral uses GGX alpha, not perceptual roughness.
		const float t = luxrays::Clamp(perceptualRoughness * perceptualRoughness / .15f, 0.f, 1.f);
		nonspecularWeight += w * t * t * (3.f - 2.f * t);
	}

	float FeatureWeight() const {
		if (!(weight > 0.f)) return 0.f;
		const float t = luxrays::Clamp(2.f * nonspecularWeight / weight, 0.f, 1.f);
		return t * t * (3.f - 2.f * t);
	}
};

}
#endif
