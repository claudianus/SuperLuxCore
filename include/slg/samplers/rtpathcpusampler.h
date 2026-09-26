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

#ifndef _SLG_RTPATHCPU_SAMPLER_H
#define	_SLG_RTPATHCPU_SAMPLER_H

#include <atomic>
#include <string>
#include <vector>

#include <barrier>

#include "luxrays/core/randomgen.h"
#include "slg/slg.h"
#include "slg/film/film.h"
#include "slg/samplers/sampler.h"

namespace slg {

//------------------------------------------------------------------------------
// RTPathCPU specific sampler data
//
// Used to share sampler specific data across multiple threads
//------------------------------------------------------------------------------
	
class RTPathCPUSamplerSharedData : public SamplerSharedData {
public:
	struct PixelCoord {
		u_int x, y;
	};

	RTPathCPUSamplerSharedData(FilmPtr flm, const u_int zoomFactor);
	virtual ~RTPathCPUSamplerSharedData() { }

	virtual void Reset();

	void Reset(FilmPtr flm);

	// Runtime update of the decimation factor: stored immediately and the
	// coarse sequence is rebuilt lazily at the next Reset() (a thread-safe
	// point in the RT flow).
	void SetZoomFactor(const u_int zf) { zoomFactor = luxrays::Max(1u, zf); }

	static std::unique_ptr<SamplerSharedData> FromProperties(
		const luxrays::Properties &cfg,
		const luxrays::RandomGeneratorUPtr &  rndGen, FilmPtr film);

	FilmPtr engineFilm;
	std::atomic<u_int> step;
	u_int filmSubRegion[4], filmSubRegionWidth, filmSubRegionHeight;
	std::vector<PixelCoord> pixelRenderSequence;
	// Coarse first-frame pixels (zoomFactor-spaced, subregion-local coords)
	// in shuffled order: the preview pass covers the whole image at once
	// instead of filling rows bottom-to-top.
	std::atomic<u_int> zoomFactor;
	// Factor the sequences were last built with; Reset() rebuilds when it
	// differs from zoomFactor so a runtime change takes effect on the next
	// reset without racing the in-flight first frame.
	u_int builtZoomFactor;
	std::vector<PixelCoord> firstFrameSequence;
};

//------------------------------------------------------------------------------
// RTPathCPU specific sampler
//------------------------------------------------------------------------------

class RTPathCPURenderEngine;

class RTPathCPUSampler : public Sampler {
public:
	RTPathCPUSampler(
		const luxrays::RandomGeneratorUPtr & rnd,
		FilmPtr flm,
		const FilmSampleSplatterUPtr& flmSplatter,
		SamplerSharedDataSPtr samplerSharedData
	);
	virtual ~RTPathCPUSampler();

	virtual SamplerType GetType() const { return GetObjectType(); }
	virtual std::string GetTag() const { return GetObjectTag(); }

	virtual float GetSample(const u_int index);
	virtual void NextSample(const std::vector<SampleResult> &sampleResults);

	void SetRenderEngine(RTPathCPURenderEngine *engine);
	void Reset(FilmPtr flm);

	//--------------------------------------------------------------------------
	// Static methods used by SamplerRegistry
	//--------------------------------------------------------------------------

	static SamplerType GetObjectType() { return RTPATHCPUSAMPLER; }
	static std::string GetObjectTag() { return "RTPATHCPUSAMPLER"; }
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static SamplerUPtr FromProperties(
		const luxrays::Properties &cfg, const luxrays::RandomGeneratorUPtr & rndGen,
		FilmPtr film, const FilmSampleSplatterUPtr& flmSplatter,
		SamplerSharedDataSPtr sharedData);
	static slg::ocl::Sampler *FromPropertiesOCL(const luxrays::Properties &cfg);
	static void AddRequiredChannels(Film::FilmChannels &channels, const luxrays::Properties &cfg);

private:
	static luxrays::PropertiesUPtr GetDefaultProps();

	void NextPixel();

	std::shared_ptr<RTPathCPUSamplerSharedData> sharedData;
	RTPathCPURenderEngine *engine;

	u_int myStep, frameHeight;
	u_int currentX, currentY, linesDone;
	bool firstFrameDone;
};

}

#endif	/* _SLG_RTPATHCPU_SAMPLER_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
