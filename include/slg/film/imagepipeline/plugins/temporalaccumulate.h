/***************************************************************************
 * Copyright 1998-2025 by authors (see AUTHORS.txt)                        *
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

#ifndef _SLG_TEMPORAL_ACCUMULATE_H
#define	_SLG_TEMPORAL_ACCUMULATE_H

#include <string>

#include <boost/serialization/export.hpp>

#include "luxrays/luxrays.h"
#include "luxrays/core/color/color.h"
#include "slg/film/film.h"
#include "slg/film/imagepipeline/imagepipeline.h"

namespace slg {

//------------------------------------------------------------------------------
// Temporal accumulation
//
// Reprojects the previous frame's accumulated (linear, pre-tonemap)
// radiance into the current frame through the MOTION_VECTOR channel and
// blends it with an exponential moving average. Every radiance component
// channel present in the film (DIRECT_DIFFUSE, INDIRECT_GLOSSY, EMISSION,
// ...) is accumulated together with the image pipeline beauty, so a
// downstream denoiser reading the raw film channels (e.g. OIDN in
// "components" mode) receives temporally filtered inputs. History pixels
// are validated with DEPTH / AVG_SHADING_NORMAL / OBJECT_ID (disocclusion
// detection) and clipped to the current frame's 5x5 neighbourhood
// statistics widened by the VARIANCE channel estimate (TAA-style variance
// clipping). Flicker is suppressed before spatial denoising.
//
// History state persists across render sessions through a state EXR in
// stateDir, so the plugin works for animation rendering where each frame
// is an independent session.
//------------------------------------------------------------------------------

class TemporalAccumulate : public ImagePipelinePlugin {
public:
	TemporalAccumulate(const u_int frameIndex, const std::string &stateDir,
			const float historyCap, const float clipSigma,
			const float depthRelThreshold, const float normalCosThreshold);

	virtual ImagePipelinePlugin *Copy() const;

	virtual void Apply(Film &film, const u_int index);

	friend class boost::serialization::access;

private:
	// Used by serialization
	TemporalAccumulate();

	template<class Archive> void serialize(Archive &ar, const u_int version) {
		ar & BOOST_SERIALIZATION_BASE_OBJECT_NVP(ImagePipelinePlugin);
		ar & frameIndex;
		ar & stateDir;
		ar & historyCap;
		ar & clipSigma;
		ar & depthRelThreshold;
		ar & normalCosThreshold;
	}

	// Current frame index (0 resets the history)
	u_int frameIndex;
	// Directory holding the per-pipeline state EXRs
	std::string stateDir;
	// Cap of the EMA window, in frames (bounds ghosting/adaptation lag)
	float historyCap;
	// Neighbourhood clamp box half-width, in units of combined
	// spatial + sample standard deviation (0 disables clipping)
	float clipSigma;
	// Disocclusion test: relative depth difference threshold
	float depthRelThreshold;
	// Disocclusion test: minimum dot(prevN, curN)
	float normalCosThreshold;
};

}

BOOST_CLASS_VERSION(slg::TemporalAccumulate, 0)

BOOST_CLASS_EXPORT_KEY(slg::TemporalAccumulate)

#endif	/* _SLG_TEMPORAL_ACCUMULATE_H */
