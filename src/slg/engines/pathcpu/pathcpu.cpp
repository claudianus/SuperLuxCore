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

#include "slg/engines/pathcpu/pathcpu.h"
#include "slg/engines/pathcpu/pathcpurenderstate.h"
#include "slg/film/filters/filter.h"
#include "slg/samplers/sobol.h"
#include "slg/samplers/metropolis.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PathCPURenderEngine
//------------------------------------------------------------------------------

PathCPURenderEngine::PathCPURenderEngine(RenderConfigRef rcfg) :
		CPUNoTileRenderEngine(rcfg), photonGICache(nullptr),
		pathGuidingCache(nullptr), restirGI(nullptr),
		lightSampleSplatter(nullptr), lightSamplerSharedData(nullptr) {
}

PathCPURenderEngine::~PathCPURenderEngine() {
	delete photonGICache;
	delete pathGuidingCache;
	delete restirGI;
}

void PathCPURenderEngine::InitFilm() {
	GetFilm().AddChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED);

	// pathTracer has not yet been initialized
	const bool hybridBackForwardEnable = renderConfig.GetConfig().Get(PathTracer::GetDefaultProps()->
			Get("path.hybridbackforward.enable")).Get<bool>();
	if (hybridBackForwardEnable)
		GetFilm().AddChannel(Film::RADIANCE_PER_SCREEN_NORMALIZED);

	GetFilm().SetRadianceGroupCount(renderConfig.GetScene().GetLightSources().GetLightGroupCount());
	GetFilm().SetThreadCount(renderThreads.size());
	GetFilm().Init();
}

RenderStateSPtr PathCPURenderEngine::GetRenderState() {
	return std::make_shared<PathCPURenderState>(bootStrapSeed, photonGICache);
}

void PathCPURenderEngine::StartLockLess() {
	auto& cfg = renderConfig.GetConfig();

	//--------------------------------------------------------------------------
	// Check to have the right sampler settings
	//--------------------------------------------------------------------------

	if (GetType() == RTPATHCPU) {
		const string samplerType = cfg.Get(Property("sampler.type")(SobolSampler::GetObjectTag())).Get<string>();
		if (samplerType != "RTPATHCPUSAMPLER")
			throw runtime_error("RTPATHCPU render engine can use only RTPATHCPUSAMPLER");
	} else
		CheckSamplersForNoTile(RenderEngineType2String(GetType()), cfg);

	//--------------------------------------------------------------------------
	// Check to have the right sampler settings
	//--------------------------------------------------------------------------

	const string samplerType = cfg.Get(Property("sampler.type")(SobolSampler::GetObjectTag())).Get<string>();
	if (GetType() == RTPATHCPU) {
		if (samplerType != "RTPATHCPUSAMPLER")
			throw runtime_error("RTPATHCPU render engine can use only RTPATHCPUSAMPLER");
	} else {
		if (samplerType == "RTPATHCPUSAMPLER")
			throw runtime_error("PATHCPU render engine can not use RTPATHCPUSAMPLER");
	}

	//--------------------------------------------------------------------------
	// Restore render state if there is one
	//--------------------------------------------------------------------------

	if (startRenderState) {
		// Check if the render state is of the right type
		startRenderState->CheckEngineTag(GetObjectTag());

		auto rs = static_pointer_cast<PathCPURenderState>(startRenderState);

		// Use a new seed to continue the rendering
		const u_int newSeed = rs->bootStrapSeed + 1;
		SLG_LOG("Continuing the rendering with new PATHCPU seed: " + ToString(newSeed));
		SetSeed(newSeed);

		// Transfer the ownership of PhotonGI cache pointer
		photonGICache = rs->photonGICache;
		rs->photonGICache = nullptr;

		// I have to set the scene pointer in photonGICache because it is not
		// saved by serialization
		if (photonGICache)
			photonGICache->SetScene(renderConfig.GetScene());

		startRenderState = nullptr;
	}

	//--------------------------------------------------------------------------
	// Allocate PhotonGICache if enabled
	//--------------------------------------------------------------------------

	// note: photonGICache could have been restored from the render state
	if ((GetType() != RTPATHCPU) && !photonGICache) {
		photonGICache = PhotonGICache::FromProperties(renderConfig.GetScene(), cfg);

		// photonGICache will be nullptr if the cache is disabled
		if (photonGICache)
			photonGICache->Preprocess(renderThreads.size());
	}

	//--------------------------------------------------------------------------
	// Initialize the PathTracer class with rendering parameters
	//--------------------------------------------------------------------------

	pathTracer.ParseOptions(cfg, *GetDefaultProps());

	if (pathTracer.hybridBackForwardEnable)
		lightSamplerSharedData = MetropolisSamplerSharedData::FromProperties(Properties(), seedBaseGenerator, GetFilm());

	pathTracer.InitPixelFilterDistribution(GetPixelFilter());

	lightSampleSplatter.reset();
	if (pathTracer.hybridBackForwardEnable)
		lightSampleSplatter = std::make_unique<FilmSampleSplatter>(GetPixelFilter());

	pathTracer.SetPhotonGICache(photonGICache);

	//--------------------------------------------------------------------------
	// Allocate path guiding cache if enabled (P1-3 M1, CPU only)
	//--------------------------------------------------------------------------

	delete pathGuidingCache;
	pathGuidingCache = nullptr;
	if (cfg.Get(PathTracer::GetDefaultProps()->Get("path.guiding.enable")).Get<bool>()) {
		// Optional warm start: path.guiding.tablefile loads a previously
		// dumped read tree (LUX_PG_DUMP) and keeps training on top of it.
		const string tableFile = cfg.Get(Property("path.guiding.tablefile")("")).Get<string>();
		if (!tableFile.empty()) {
			pathGuidingCache = PathGuidingCache::Load(tableFile);
			if (pathGuidingCache) {
				SLG_LOG("[PathCPURenderEngine] Path guiding table loaded: " << tableFile);
			} else {
				SLG_LOG("WARNING: unable to load path guiding table file: " << tableFile);
			}
		}
		if (!pathGuidingCache) {
			const BSphere &bsphere = renderConfig.GetScene().GetSceneBSphere();
			const Point cubeMin(bsphere.center.x - bsphere.rad,
					bsphere.center.y - bsphere.rad,
					bsphere.center.z - bsphere.rad);
			pathGuidingCache = new PathGuidingCache(cubeMin, 2.f * bsphere.rad);
		}
		SLG_LOG("[PathCPURenderEngine] Path guiding (M4) enabled");
	}
	pathTracer.SetPathGuidingCache(pathGuidingCache);

	//--------------------------------------------------------------------------
	// ReSTIR GI (G1): per-pixel first-bounce reservoir, shared by all
	// render threads (advisory lock-free access inside).
	//--------------------------------------------------------------------------

	delete restirGI;
	restirGI = nullptr;
	if (cfg.Get(PathTracer::GetDefaultProps()->Get("path.restir.gi.enable")).Get<bool>()) {
		restirGI = new RestirGI();
		restirGI->Init(GetFilm().GetWidth(), GetFilm().GetHeight());
		SLG_LOG("[PathCPURenderEngine] ReSTIR GI enabled (candidates=" <<
				cfg.Get(PathTracer::GetDefaultProps()->Get(
					"path.restir.gi.candidates")).Get<int>() << ")");
	}
	pathTracer.SetRestirGI(restirGI);

	//--------------------------------------------------------------------------

	CPUNoTileRenderEngine::StartLockLess();
}

void PathCPURenderEngine::StopLockLess() {
	CPUNoTileRenderEngine::StopLockLess();

	pathTracer.DeletePixelFilterDistribution();

	// Table export for GPU training flow (M2b): dump the frozen read
	// side when LUX_PG_DUMP is set.
	if (pathGuidingCache) {
		const char *dumpPath = getenv("LUX_PG_DUMP");
		if (dumpPath && dumpPath[0])
			pathGuidingCache->Save(dumpPath);
	}

	delete photonGICache;
	photonGICache = nullptr;
}

void PathCPURenderEngine::EndSceneEditLockLess(const EditActionList &editActions) {
	if (lightSamplerSharedData)
		lightSamplerSharedData->Reset();

	// Reservoirs key on film pixels; a scene edit invalidates the
	// stored x2's (bounded-bias reuse would still be safe via the
	// Jacobian + V test, but a reset is cheaper than stale entries).
	if (restirGI)
		restirGI->Reset();

	CPURenderEngine::EndSceneEditLockLess(editActions);
}

//------------------------------------------------------------------------------
// Static methods used by RenderEngineRegistry
//------------------------------------------------------------------------------

PropertiesUPtr PathCPURenderEngine::ToProperties(const Properties &cfg) {
	PropertiesUPtr props = std::make_unique<Properties>();

	*props << *CPUNoTileRenderEngine::ToProperties(cfg) <<
				cfg.Get(GetDefaultProps()->Get("renderengine.type")) <<
			PathTracer::ToProperties(cfg) <<
			PhotonGICache::ToProperties(cfg);

	return props;
}

RenderEngine *PathCPURenderEngine::FromProperties(RenderConfigRef rcfg) {
	return new PathCPURenderEngine(rcfg);
}

PropertiesUPtr PathCPURenderEngine::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			CPUNoTileRenderEngine::GetDefaultProps() <<
			Property("renderengine.type")(GetObjectTag()) <<
			PathTracer::GetDefaultProps() <<
			PhotonGICache::GetDefaultProps();

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
