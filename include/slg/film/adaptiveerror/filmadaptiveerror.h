/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of SuperLuxCore.                                    *
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

#ifndef _SLG_FILM_ADAPTIVE_ERROR_H
#define	_SLG_FILM_ADAPTIVE_ERROR_H

#include <vector>

#include "luxrays/usings.h"
#include "luxrays/utils/properties.h"
#include "luxrays/utils/serializationutils.h"
#include "slg/usings.h"
#include "slg/film/framebuffer.h"

namespace slg {

//------------------------------------------------------------------------------
// FilmAdaptiveError - statistical per-pixel convergence test (E5).
//
// Production-style "noise level" metric following the Arnold recipe
// (Kulla et al. ToG'18) and Corona's global noise level: the standard
// error of the pixel mean derived from the VARIANCE channel
// (E[c^2] - E[c]^2) divided by the mean, evaluated per pixel, dilated
// by a 3x3 max window to catch sub-pixel detail, and aggregated into a
// robust global percentile. The per-pixel residual error is also written
// into the NOISE channel so the existing adaptive samplers keep working
// on the statistically-grounded map instead of the image-diff heuristic.
//------------------------------------------------------------------------------

class Film;

class FilmAdaptiveError {
public:
	FilmAdaptiveError(
		FilmConstRef film,
		const float errorTarget,
		const u_int warmup,
		const u_int testStep,
		const u_int minSamples,
		const bool haltEnabled
	);
	~FilmAdaptiveError();

	bool IsTestUpdateRequired() const;

	void Reset();
	// Returns the fraction of converged pixels; sets noiseLevel.
	float Test();

	float noiseLevel;       // 95th percentile of the dilated rel-error map
	float convergedRatio;   // fraction of pixels under target
	u_int todoPixelsCount;

	FilmConstRef GetFilm() const { return *film; }
	// The film back pointer is not serialized: Film::load rebinds it
	// after deserialization (version 1 archives carried a full nested
	// film copy which was consumed and dropped).
	void BindFilm(const Film *f) { film = f; }

	friend class boost::serialization::access;

private:
	// Used by serialization
	FilmAdaptiveError();

	template<class Archive> void serialize(Archive &ar, const u_int version);

	float errorTarget;
	u_int warmup;
	u_int testStep;
	u_int minSamples;
	bool haltEnabled;

	FilmConstPtr film;  // This could be a const ref, but due to
	                    // boost serialization, it isn't...

	std::vector<float> errorVector;   // dilated relative error per pixel

	double lastSamplesCount;
	bool firstTest;
};

}

BOOST_CLASS_VERSION(slg::FilmAdaptiveError, 2)

BOOST_CLASS_EXPORT_KEY(slg::FilmAdaptiveError)

#endif	/* _SLG_FILM_ADAPTIVE_ERROR_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
