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
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#ifndef _SLG_VARIANCECLAMPING_H
#define	_SLG_VARIANCECLAMPING_H

#include "luxrays/luxrays.h"
#include "luxrays/utils/serializationutils.h"

namespace slg {

class Film;
class SampleResult;

// Adaptive Robust Clamping (ARC)
//
// Firefly suppression driven by robust per-pixel spatial statistics instead
// of the legacy fixed "pixel mean +- sqrt(maxvalue)" bound. Two orthogonal
// mechanisms, selected by render properties:
//
//  - path.clamping.variance.adaptive (default 1): the clamp margin is
//    estimated from the 3x3 neighborhood of pixel means: med = median of
//    neighbor luminance, mad = median absolute deviation. The bound becomes
//        T = max(ownMean, med) + max(sigma * mad, sqrtMax * (0.1 + E))
//    with E = max(ownMean, med). Legitimately bright/variable content
//    (sun glints, caustic patches, strong highlights) is spatially coherent
//    so its neighborhood median/MAD is large and the bound relaxes;
//    isolated fireflies sit in dark neighborhoods with tiny MAD and are
//    clamped hard. Robust statistics (median/MAD break only at >50%
//    contamination) are the online counterpart of the density-based outlier
//    rejection of DeCoro et al., PG 2010. With < 3 valid neighbors the bound
//    falls back to the legacy [0, sqrtMax] virgin behavior.
//
//  - path.clamping.variance.scope ("all" | "indirect" | "direct",
//    default "indirect"): which path classes are clamped, Cycles-style.
//    "indirect" leaves emission + direct (first-vertex) contributions
//    untouched - direct sun disks, NEE highlights and emissive surfaces are
//    never dimmed - and removes only the indirect excess from the beauty.
//    "direct" is the symmetric counterpart. "all" reproduces the legacy
//    per-channel clamping (with the adaptive margin when enabled).
//
//  - path.clamping.variance.sigma (default 6): MAD multiplier. MAD ~ 0.67
//    sigma for Gaussian samples, so 6*MAD ~ 4 sigma: conservative.
//
// Every clamp scales the RGB triple proportionally (preserves chromaticity);
// under a partial scope the beauty radiance loses exactly the removed
// share of the clamped class, keeping beauty and AOVs consistent.
// Clamping is still biased by construction (see ScaledClamp3) - the goal of
// ARC is to confine the bias to statistically implausible samples only.
class VarianceClamping {
public:
	// Scope values must match the CL kernel: 0 = all, 1 = indirect, 2 = direct
	enum Scope {
		CLAMP_ALL = 0,
		CLAMP_INDIRECT = 1,
		CLAMP_DIRECT = 2
	};

	VarianceClamping();
	VarianceClamping(const float sqrtMaxValue, const bool adaptive = true,
			const int scope = CLAMP_INDIRECT, const float sigma = 6.f);

	bool hasClamping() const { return (sqrtVarianceClampMaxValue > 0.f); }

	void Clamp(const Film &film, SampleResult &sampleResult) const;
	void ClampFilm(Film &dstFilm , const Film &srcFilm,
			const u_int srcOffsetX, const u_int srcOffsetY,
			const u_int srcWidth, const u_int srcHeight,
			const u_int dstOffsetX, const u_int dstOffsetY) const;
	void ClampFilm(Film &dstFilm , const Film &srcFilm) const;

	float sqrtVarianceClampMaxValue;
	// 0/1 int (not bool) so the layout matches the CL kernel params
	int varianceClampAdaptive;
	int varianceClampScope;
	float varianceClampSigma;

	friend class boost::serialization::access;

private:
	void Clamp3(const float expectedValue[4], float value[3]) const;
	void Clamp3Margin(const float expectedValue[4], float value[3],
			const float margin) const;
	void Clamp4(const float expectedValue[4], float value[4]) const;

	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & sqrtVarianceClampMaxValue;
		if (version >= 2) {
			ar & varianceClampAdaptive;
			ar & varianceClampScope;
			ar & varianceClampSigma;
		}
	}
};

}

BOOST_CLASS_VERSION(slg::VarianceClamping, 2)

#endif	/* _SLG_VARIANCECLAMPING_H */

// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
