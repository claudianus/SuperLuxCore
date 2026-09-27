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

#include "luxrays/core/color/color.h"
#include "slg/usings.h"
#include "slg/samplers/rtpathcpusampler.h"
#include "slg/engines/rtpathcpu/rtpathcpu.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// RTPathCPU specific sampler shared data
//------------------------------------------------------------------------------

RTPathCPUSamplerSharedData::RTPathCPUSamplerSharedData(FilmPtr film,
		const u_int zf) :
	engineFilm(film),
	SamplerSharedData()
{
	filmSubRegion[0] = 0;
	filmSubRegion[1] = 0;
	filmSubRegion[2] = 0;
	filmSubRegion[3] = 0;
	filmSubRegionWidth =  0;
	filmSubRegionHeight = 0;
	zoomFactor = Max(1u, zf);
	builtZoomFactor = 0;  // force the sequence build in Reset()

	Reset(film);
}

void RTPathCPUSamplerSharedData::Reset(FilmPtr film) {
	engineFilm = film;
	Reset();
}

void RTPathCPUSamplerSharedData::Reset() {
	const u_int *subRegion = engineFilm->GetSubRegion();

	// Check if something has changed (a runtime zoomFactor change also
	// rebuilds the sequences: Reset() runs at a thread-safe point)
	const bool zoomChanged = (builtZoomFactor != zoomFactor);
	if (zoomChanged ||
			(filmSubRegion[0] != subRegion[0]) || (filmSubRegion[1] != subRegion[1]) ||
			(filmSubRegion[2] != subRegion[2]) || (filmSubRegion[3] != subRegion[3])) {
		builtZoomFactor = zoomFactor;
		filmSubRegion[0] = subRegion[0];
		filmSubRegion[1] = subRegion[1];
		filmSubRegion[2] = subRegion[2];
		filmSubRegion[3] = subRegion[3];
		filmSubRegionWidth = subRegion[1] - subRegion[0] + 1;
		filmSubRegionHeight = subRegion[3] - subRegion[2] + 1;
		

		const u_int pixelCount = filmSubRegionWidth * filmSubRegionHeight;
		pixelRenderSequence.resize(pixelCount);

		for (u_int y = 0; y < filmSubRegionHeight; ++y) {
			for (u_int x = 0; x < filmSubRegionWidth; ++x) {
				const u_int index = x + y * filmSubRegionWidth;

				pixelRenderSequence[index].x = x + subRegion[0];
				pixelRenderSequence[index].y = y + subRegion[2];
			}
		}

		// Randomly shuffle elements
		RandomGenerator rnd(123);
		for (u_int i = 0; i < pixelCount; i++) {
			const u_int j = Min(Floor2UInt(rnd.floatValue() * pixelCount), pixelCount - 1);
			Swap(pixelRenderSequence[i], pixelRenderSequence[j]);
		}

		// Build the coarse first-frame pixel list (one sample per
		// zoomFactor x zoomFactor block) and shuffle it: a scattered order
		// covers the whole frame with the first pass, so the user sees a
		// dithered full image instead of a row band filling bottom-to-top.
		const u_int zf = Max(1u, zoomFactor.load());
		const u_int cw = (filmSubRegionWidth + zf - 1) / zf;
		const u_int ch = (filmSubRegionHeight + zf - 1) / zf;
		const u_int coarseCount = cw * ch;
		firstFrameSequence.resize(coarseCount);

		for (u_int y = 0; y < ch; ++y)
			for (u_int x = 0; x < cw; ++x) {
				PixelCoord &pc = firstFrameSequence[x + y * cw];
				pc.x = x * zf;
				pc.y = y * zf;
			}

		RandomGenerator rndFF(321);
		for (u_int i = 0; i < coarseCount; ++i) {
			const u_int j = Min(Floor2UInt(rndFF.floatValue() * coarseCount), coarseCount - 1);
			Swap(firstFrameSequence[i], firstFrameSequence[j]);
		}
	}

	step = 0;
}

std::unique_ptr<SamplerSharedData> RTPathCPUSamplerSharedData::FromProperties(
	const Properties &cfg, const RandomGeneratorUPtr & rndGen, FilmPtr film
) {
	// zoomFactor must reach the ctor: Reset() inside builds the coarse
	// first-frame pixel sequence with it
	const u_int zf = (u_int)Max(1,
		cfg.Get(Property("rtpathcpu.zoomphase.size")(4)).Get<int>());

	return std::make_unique<RTPathCPUSamplerSharedData>(film, zf);
}

//------------------------------------------------------------------------------
// RTPathCPU specific sampler
//------------------------------------------------------------------------------

RTPathCPUSampler::RTPathCPUSampler(
	const luxrays::RandomGeneratorUPtr & rnd,
	FilmPtr flm,
	const FilmSampleSplatterUPtr& flmSplatter,
	SamplerSharedDataSPtr samplerSharedData
) :
	Sampler(rnd, flm, flmSplatter, true),
	sharedData(dynamic_pointer_cast<RTPathCPUSamplerSharedData>(samplerSharedData)),
	adaptiveStrength(0.f), foveaStrength(0.f), foveaRadius(.4f),
	foveaDepthScale(0.f)
{
	film = flm;
	// Disable denoiser statistics collection
	film->GetDenoiser().SetEnabled(false);

	// NOTE: The sampler can not be used until the call of SetRenderEngine()
}

RTPathCPUSampler::~RTPathCPUSampler() {
}

void RTPathCPUSampler::SetRenderEngine(RTPathCPURenderEngine *re) {
	engine = re;

	Reset(film);
}

void RTPathCPUSampler::Reset(FilmPtr flm) {
	film = flm;
	// Disable denoiser statistics collection
	film->GetDenoiser().SetEnabled(false);

	myStep = sharedData->step.fetch_add(1);
	if (myStep < sharedData->firstFrameSequence.size()) {
		const auto &pc = sharedData->firstFrameSequence[myStep];
		currentX = pc.x;
		currentY = pc.y;
	} else {
		currentX = 0;
		currentY = 0;
	}
	linesDone = 0;
	firstFrameDone = false;
}

void RTPathCPUSampler::NextPixel() {
	if (!firstFrameDone) {
		// First frame: render one pixel every engine->zoomFactor x
		// engine->zoomFactor block, visiting the coarse pixels in a shuffled
		// order so the preview covers the whole image right away.
		myStep = sharedData->step.fetch_add(1);

		if (myStep < sharedData->firstFrameSequence.size()) {
			const auto &pc = sharedData->firstFrameSequence[myStep];
			currentX = pc.x;
			currentY = pc.y;

			// This should be done as atomic operation but it is only for statistics
			// (adding the effective number of samples rendered, not the pixels count)
			film->AddSampleCount(threadIndex, 1.0, 0.0);
		} else {
			// Signal the main thread after have finished the rendering
			// of the first frame
			std::unique_lock<std::mutex> lock(engine->firstFrameMutex);

			++(engine->firstFrameThreadDoneCount);

			engine->firstFrameCondition.notify_one();

			firstFrameDone = true;

			// Hand off to the normal path with a coherent state
			const u_int zf = Max(1u, engine->zoomFactor.load());
			currentX = 0;
			currentY = (myStep * zf) % RoundUp<u_int>(sharedData->filmSubRegionHeight, zf);
			linesDone = 0;
		}
	} else {
		// Normal rendering. With adaptiveStrength > 0 the walk keeps
		// skipping forward while the candidate pixel looks converged
		// (same floor semantics as the other adaptive samplers: every
		// pixel keeps a 1-strength acceptance probability).
		// Snapshot the decimation factor once per call: it can change at
		// any time via SetRuntimeResolutionReduction() without pausing
		// the render threads.
		const u_int zf = Max(1u, engine->zoomFactor.load());
		for (u_int tries = 0; ; ) {
			++currentX;

			if (currentX >= sharedData->filmSubRegionWidth) {
				currentX = 0;
				++linesDone;
				++currentY;

				if ((currentY >= sharedData->filmSubRegionHeight) || (linesDone >= zf)) {
					// This should be done as atomic operation but it is only for statistics
					film->AddSampleCount(threadIndex, sharedData->filmSubRegionWidth * linesDone, 0.0);

					myStep = sharedData->step.fetch_add(1);
					// Modulo against the LIVE padded height: a frameHeight
					// captured at Reset() goes stale across a runtime
					// zoomFactor change and (myStep * zf) % staleHeight
					// can land on a row >= filmSubRegionHeight, indexing
					// past the end of pixelRenderSequence
					currentY = (myStep * zf) % RoundUp<u_int>(sharedData->filmSubRegionHeight, zf);
					linesDone = 0;
				}
			}

			const bool useAdaptive = (adaptiveStrength > 0.f) && film->channel_NOISE;
			const bool useFovea = foveaStrength > 0.f;
			if (!useAdaptive && !useFovea)
				break;

			const auto &pc = sharedData->pixelRenderSequence[currentX + currentY * sharedData->filmSubRegionWidth];
			float threshold = 1.f;
			if (useAdaptive) {
				const float noise = *film->channel_NOISE->GetPixel(pc.x, pc.y);
				threshold = Max(std::isinf(noise) ? 1.f : noise, 1.f - adaptiveStrength);
			}
			if (useFovea) {
				// Same geometric importance as the OCL tilepath: radial
				// falloff to 1-strength at the frame corners, times a
				// near-depth gain when foveaDepthScale is set
				const float scx = .5f * (sharedData->filmSubRegion[0] + sharedData->filmSubRegion[1]);
				const float scy = .5f * (sharedData->filmSubRegion[2] + sharedData->filmSubRegion[3]);
				const float rw = sharedData->filmSubRegion[1] - sharedData->filmSubRegion[0];
				const float rh = sharedData->filmSubRegion[3] - sharedData->filmSubRegion[2];
				const float invHalfDiag = 2.f / Max(1.f, sqrtf(rw * rw + rh * rh));
				const float dx = (pc.x + .5f - scx) * invHalfDiag;
				const float dy = (pc.y + .5f - scy) * invHalfDiag;
				const float r = sqrtf(dx * dx + dy * dy);
				// smoothstep(foveaRadius, 1, r)
				const float t = Clamp((r - foveaRadius) / Max(1e-6f, 1.f - foveaRadius), 0.f, 1.f);
				float imp = 1.f - foveaStrength * (t * t * (3.f - 2.f * t));
				if (film->channel_DEPTH && (foveaDepthScale > 0.f)) {
					const float d = *film->channel_DEPTH->GetPixel(pc.x, pc.y);
					if (std::isfinite(d))
						imp *= Min(1.f, foveaDepthScale / Max(d, 1e-3f));
				}
				threshold *= imp;
			}
			if ((rndGen->floatValue() <= threshold) || (++tries >= 8))
				break;
		}
	}
}

float RTPathCPUSampler::GetSample(const u_int index) {
	assert (index < requestedSamples);

	float u;
	switch (index) {
		case 0: {
			const u_int px = firstFrameDone ?
				sharedData->pixelRenderSequence[currentX + currentY * sharedData->filmSubRegionWidth].x :
				(currentX + sharedData->filmSubRegion[0]);
			u = px + rndGen->floatValue();
			break;
		}
		case 1: {
			const u_int py = firstFrameDone ?
				sharedData->pixelRenderSequence[currentX + currentY * sharedData->filmSubRegionWidth].y :
				(currentY + sharedData->filmSubRegion[2]);
			u = py + rndGen->floatValue();
			break;
		}
		default:
			u = rndGen->floatValue();
			break;
	}
	
	return u;
}

void RTPathCPUSampler::NextSample(const vector<SampleResult> &sampleResults) {
	// film->AddSampleCount(1.0) is done in NextPixel()

	const SampleResult *sr = &sampleResults[0];
	
	// AddSamplesToFilm(sampleResults) is replaced by this special section of code to
	// to render 1 sample every engine->zoomFactor x engine->zoomFactor pixels on the first frame
	if (firstFrameDone)
		film->AddSample(sr->pixelX, sr->pixelY, *sr, 1.f);
	else {
		// Single-pixel write only: the VIEWPORT_INFILL imagepipeline plugin
		// reconstructs the gaps, which looks far better than zoomFactor x
		// zoomFactor blocks. A fake weight keeps the first frame easily
		// replaced once the steady sequence reaches the pixel.
		film->AddSample(sr->pixelX, sr->pixelY, *sr, engine->zoomWeight);
	}

	NextPixel();
}

//------------------------------------------------------------------------------
// Static methods used by SamplerRegistry
//------------------------------------------------------------------------------

PropertiesUPtr RTPathCPUSampler::ToProperties(const Properties &cfg) {
	PropertiesUPtr props = std::make_unique<Properties>();
	*props << cfg.Get(GetDefaultProps()->Get("sampler.type"));
	return props;
}

SamplerUPtr RTPathCPUSampler::FromProperties(const Properties &cfg, const RandomGeneratorUPtr & rndGen,
		FilmPtr film, const FilmSampleSplatterUPtr& flmSplatter, SamplerSharedDataSPtr sharedData) {
	auto s = std::make_unique<RTPathCPUSampler>(rndGen, film, flmSplatter, sharedData);
	s->adaptiveStrength = Clamp(
			cfg.Get(GetDefaultProps()->Get("sampler.rtpathcpusampler.adaptive.strength")).Get<double>(), 0.0, .95);
	s->foveaStrength = Clamp(
			cfg.Get(GetDefaultProps()->Get("sampler.rtpathcpusampler.fovea.strength")).Get<double>(), 0.0, .95);
	s->foveaRadius = Clamp(
			cfg.Get(GetDefaultProps()->Get("sampler.rtpathcpusampler.fovea.radius")).Get<double>(), 0.01, 1.0);
	s->foveaDepthScale = Max(0.0,
			cfg.Get(GetDefaultProps()->Get("sampler.rtpathcpusampler.fovea.depthscale")).Get<double>());
	return s;
}

slg::ocl::Sampler *RTPathCPUSampler::FromPropertiesOCL(const Properties &cfg) {
	// This can not happen
	throw runtime_error("Internal error in RTPathCPUSampler::FromPropertiesOCL()");
}

void RTPathCPUSampler::AddRequiredChannels(Film::FilmChannels &channels, const luxrays::Properties &cfg) {
	// Noise-guided steady sequence needs the film NOISE channel
	const float str = cfg.Get(GetDefaultProps()->Get("sampler.rtpathcpusampler.adaptive.strength")).Get<double>();
	if (str > 0.f)
		channels.insert(Film::NOISE);
	// The foveation depth term reads the first-hit DEPTH channel
	const float dScale = cfg.Get(GetDefaultProps()->Get("sampler.rtpathcpusampler.fovea.depthscale")).Get<double>();
	if (dScale > 0.0)
		channels.insert(Film::DEPTH);
}

PropertiesUPtr RTPathCPUSampler::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			Sampler::GetDefaultProps() <<
			Property("sampler.type")(GetObjectTag()) <<
			Property("sampler.rtpathcpusampler.adaptive.strength")(0.f) <<
			Property("sampler.rtpathcpusampler.fovea.strength")(0.f) <<
			Property("sampler.rtpathcpusampler.fovea.radius")(.4f) <<
			Property("sampler.rtpathcpusampler.fovea.depthscale")(0.f);

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
