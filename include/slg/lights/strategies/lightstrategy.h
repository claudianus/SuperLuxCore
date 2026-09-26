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

#ifndef _SLG_LIGHTSTRATEGY_H
#define	_SLG_LIGHTSTRATEGY_H

#include "slg/usings.h"
#include "slg/lights/light.h"
#include "slg/scene/scene.h"

namespace slg { class BSDF; }

namespace slg {

//------------------------------------------------------------------------------
// LightStrategy
//------------------------------------------------------------------------------

typedef enum {
	TASK_EMIT, TASK_ILLUMINATE, TASK_INFINITE_ONLY,
	LIGHT_STRATEGY_TASK_COUNT
} LightStrategyTask;

typedef enum {
	TYPE_UNIFORM, TYPE_POWER, TYPE_LOG_POWER, TYPE_DLS_CACHE, TYPE_RESTIR_DI,
	TYPE_LIGHT_BVH,
	LIGHT_STRATEGY_TYPE_COUNT
} LightStrategyType;


class LightStrategy {
public:
	virtual ~LightStrategy() { }

	virtual LightStrategyType GetType() const = 0;
	virtual std::string GetTag() const = 0;

	virtual void Preprocess(SceneConstRef scn, const LightStrategyTask taskType,
			const bool useRTMode) = 0;

	// Used for direct light sampling
	virtual LightSourcePtr SampleLights(
			SceneConstRef scene,
			const float u,
			const luxrays::Point &p, const luxrays::Normal &n,
			const bool isVolume,
			float *pdf) const = 0;

	// BSDF-aware direct light sampling (ReSTIR-style target functions).
	// Default implementation: fall back to the plain SampleLights().
	// Strategies that can use BSDF context (e.g. RESTIR_DI) override this
	// to weight candidates by their estimated contribution.
	// *pdf must be the light-selection pdf used by the direct-hit MIS
	// (SampleLightPdf convention) so both MIS sides stay consistent;
	// strategies that resample candidates (RIS) return their output
	// weight through *risScale (defaults to 1) instead of folding it
	// into the pdf.
	// (BSDF is forward-declared to avoid a heavy include here.)
	// lightSurfaceUs (optional out, 3 floats): strategies that resample
	// candidates with visibility weighting (RESTIR_DI +
	// lightstrategy.restir.visibility.enable) return the WINNING
	// candidate's light-surface sample here. The caller must Illuminate()
	// the returned light with exactly these values so the binary
	// visibility term folded into the candidate's target and the final
	// payoff share the same surface point - re-sampling the winner at a
	// different point would decouple them and bias the estimator.
	// Strategies leave it untouched when unused (the caller's own
	// sample applies, e.g. for merged reservoir winners).
	virtual LightSourcePtr SampleLightsBSDF(
			SceneConstRef scene,
			const BSDF &bsdf,
			const float time,
			const float u,
			float *pdf,
			float *risScale = nullptr,
			float *lightSurfaceUs = nullptr) const;

	virtual float SampleLightPdf(
			LightSourceConstRef light,
			const luxrays::Point &p,
			const luxrays::Normal &n,
			const bool isVolume) const = 0;

	// Used for light emission
	virtual LightSourcePtr SampleLights(
		SceneConstRef, const float u, float *pdf
	) const = 0;

	// Transform the current object in Properties
	virtual luxrays::PropertiesUPtr ToProperties() const = 0;

	static LightStrategyType GetType(const luxrays::Properties &cfg);

	//--------------------------------------------------------------------------
	// Static methods used by LightStrategyRegistry
	//--------------------------------------------------------------------------

	// This method is not used at the moment
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	// Allocate a Object based on the cfg definition
	static LightStrategyUPtr FromProperties(const luxrays::Properties &cfg);
	// This method is not used at the moment
	static std::string FromPropertiesOCL(const luxrays::Properties &cfg);

	static LightStrategyType String2LightStrategyType(const std::string &type);
	static std::string LightStrategyType2String(const LightStrategyType type);

protected:
	static luxrays::PropertiesUPtr GetDefaultProps();

	LightStrategy(const LightStrategyType t) : type(t) { }

	SceneConstPtr scene;  // I think this could be a (mandatory) reference but
									 // for now, I keep it as a (optional) pointer

private:
	const LightStrategyType type;
};

}

#endif	/* _SLG_LIGHTSTRATEGY_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
