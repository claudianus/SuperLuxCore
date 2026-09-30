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

#include <memory>
#include <filesystem>
#include <regex>
#include <mutex>

#include <boost/lexical_cast.hpp>
#include <boost/algorithm/string.hpp> 
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/unique_ptr.hpp>
#include <boost/serialization/optional.hpp>

#include "luxrays/usings.h"
#include "luxrays/utils/murmurhash.h"
#include "luxrays/utils/serializationutils.h"
#include "slg/usings.h"
#include "slg/renderconfig.h"
#include "slg/materials/materialdefs.h"
#include "slg/engines/renderengine.h"
#include "slg/film/film.h"

#include "slg/samplers/random.h"
#include "slg/samplers/sobol.h"
#include "slg/samplers/metropolis.h"

#include "slg/film/filters/box.h"
#include "slg/film/filters/gaussian.h"
#include "slg/film/filters/mitchell.h"
#include "slg/film/filters/mitchellss.h"
#include "slg/film/filters/blackmanharris.h"

#include "slg/engines/rtpathocl/rtpathocl.h"
#include "slg/engines/rtpathcpu/rtpathcpu.h"
#include "slg/engines/lightcpu/lightcpu.h"
#include "slg/engines/pathcpu/pathcpu.h"
#include "slg/engines/bidircpu/bidircpu.h"
#include "slg/engines/bidirvmcpu/bidirvmcpu.h"
#include "slg/engines/filesaver/filesaver.h"
#include "slg/engines/tilepathcpu/tilepathcpu.h"
#include "slg/engines/tilepathocl/tilepathocl.h"
#include "slg/lights/strategies/lightstrategyregistry.h"
#include "slg/lights/lightsourcedefs.h"
#include "slg/utils/filenameresolver.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Helpers
//------------------------------------------------------------------------------

static void PrintConfig(PropertiesRPtr props) {

	if (not props) return;

	SLG_LOG("Configuration: ");
	const auto& keys = props->GetAllNames();
	for (const auto& key : keys)
		SLG_LOG("  " << props->Get(key));

	SLG_FileNameResolver.Print();
}


//------------------------------------------------------------------------------
// RenderConfig
//------------------------------------------------------------------------------

static std::mutex defaultPropertiesMutex;
static std::unique_ptr<Properties> defaultProperties;

//BOOST_CLASS_EXPORT_IMPLEMENT(slg::RenderConfig)


// Case #1: a scene is provided by caller
RenderConfig::RenderConfig(Private p, PropertiesRPtr props, SceneRef scn)
	:
	cfg(std::make_unique<Properties>()),
	sceneRef(&scn)
{
	InitDefaultProperties();

	PrintConfig(props);

	if (!GetScene().HasCamera()) {
		throw std::runtime_error(
			"You can not build a RenderConfig with a scene not including a camera"
		);
	}

	// Parse the configuration
	Parse(*props);
}


// Case #2: no scene is provided by caller, RenderConfig has to create one
RenderConfig::RenderConfig(Private p, PropertiesRPtr props)
	:
	cfg(std::make_unique<Properties>()),
	sceneRef(nullptr)  // Temporary, awaiting scene construction
{
	InitDefaultProperties();

	assert(props);

	PrintConfig(props);

	// No scene has been provided by caller, create one
	const auto defaultSceneName = GetDefaultProperties()->Get("scene.file").Get<string>();
	const auto sceneFileName = SLG_FileNameResolver.ResolveFile(
		props->Get(Property("scene.file")(defaultSceneName)).Get<string>()
	);

	SDL_LOG("Reading scene: " << sceneFileName);
	internalScene = std::make_unique<Scene>(
		std::make_unique<Properties>(sceneFileName),
		props
	);
	sceneRef = internalScene.get();

	if (!GetScene().HasCamera()) {
		throw std::runtime_error(
			"You can not build a RenderConfig with a scene not including a camera"
		);
	}

	// Parse the configuration
	Parse(*props);
}

// Special private constructor for deserialization
RenderConfig::RenderConfig(
	PropertiesUPtr&& p_cfg, SceneRef p_scn, SceneUPtr&& p_internalscene
) :
	cfg(std::move(p_cfg)),
	sceneRef(&p_scn),
	internalScene(std::move(p_internalscene))
{
	if (internalScene) {
		sceneRef = internalScene.get();
	}
}


void RenderConfig::InitDefaultProperties() {
	// Check if I have to initialize the default Properties
	if (!defaultProperties.get()) {
		std::unique_lock<std::mutex> lock(defaultPropertiesMutex);
		if (!defaultProperties.get()) {
			auto props = std::make_unique<Properties>();
			*props << *RenderConfig::ToProperties(Properties());
	
			defaultProperties = std::move(props);
		}
	}
}


PropertiesRPtr RenderConfig::GetDefaultProperties() {
	InitDefaultProperties();

	return defaultProperties;
}


bool RenderConfig::HasCachedKernels() {
#if !defined(LUXRAYS_DISABLE_OPENCL)
	const string type = GetConfig().Get(
		Property("renderengine.type")(PathCPURenderEngine::GetObjectTag())
	).Get<string>();

	if ((type == "PATHOCL") ||
			(type == "RTPATHOCL") ||
			(type == "TILEPATHOCL")) {
		return PathOCLBaseRenderEngine::HasCachedKernels(*this);
	} else
		return true;
#else
	return true;
#endif
}

// We return an instance rather than a ref, to avoid issues in multithreaded
// context
const Property RenderConfig::GetProperty(const string &name) const {
	return ToProperties()->Get(name);
}

void RenderConfig::Parse(const Properties &props) {
	// I can not use GetProperty() here because it triggers a ToProperties() and it can
	// be a problem with OpenCL disabled (PATHOCL is not defined, etc.)
	if (GetConfig().Get(Property("debug.renderconfig.parse.print")(false)).Get<bool>()) {
		SDL_LOG("====================RenderConfig::Parse()======================"
				<< endl <<
				props);
		SDL_LOG("===============================================================");
	}

	// Reset the properties cache
	propsCache->Clear();

	GetConfig().Set(props);
	// I can not use GetProperty() here because it triggers a ToProperties() and it can
	// be a problem with OpenCL disabled (PATHOCL is not defined, etc.)
	GetScene().SetEnableParsePrint(GetConfig().Get(Property("debug.scene.parse.print")(false)).Get<bool>());

	// Scene-signature defaults before any strategy reader
	ApplyAutoLightTracing();

	UpdateFilmProperties(props);

	// Scene epsilon is read directly from the cfg properties inside
	// render engine Start() method

	// Accelerator settings are read directly from the cfg properties inside
	// the render engine

	// Light strategy
	GetScene().GetLightSources().SetLightStrategy(*cfg);

	// Update the Camera
	u_int filmFullWidth, filmFullHeight, filmSubRegion[4];
	u_int *subRegion = Film::GetFilmSize(*cfg, &filmFullWidth, &filmFullHeight, filmSubRegion) ?
		filmSubRegion : NULL;
	GetScene().GetCamera().Update(filmFullWidth, filmFullHeight, subRegion);
}

// Can this scene form caustic-class (hard-for-the-eye) paths at all?
// Any material with a SPECULAR or GLOSSY lobe - the eye-side partition's
// own condition - or a scattering volume counts; at least one emitter
// must exist for a light pass to make sense. Volumes live in matDefs
// (Volume : Material), so one scan covers them.
static bool SceneHasCausticCapablePaths(SceneConstRef scene) {
	if (scene.GetLightSources().GetSize() == 0)
		return false;
	const auto &mats = scene.GetMaterials();
	for (u_int i = 0; i < mats.GetSize(); ++i) {
		const auto &m = mats.GetMaterial(i);
		// NULLMAT reports SPECULAR|TRANSMIT but only passes rays
		// through - it focuses nothing, so it can't make a caustic
		if (m.GetType() == NULLMAT)
			continue;
		if (m.GetEventTypes() & (SPECULAR | GLOSSY))
			return true;
		if (m.IsVolume() && (m.GetType() != CLEAR_VOL))
			return true;
	}
	return false;
}

// Zero-config caustic stack (path.lighttracing.auto / path.mnee.auto,
// both default on): when the user hasn't pinned the flags, enable the
// light pass + the MNEE solver only on engines with a light-task
// population and only when the scene can form caustic-class paths - on
// diffuse-only scenes the tail tasks would deposit nothing (wasted
// budget), so auto keeps the eye side at 100%.
void RenderConfig::ApplyAutoLightTracing() {
	const string engineType = GetConfig().Get(
		Property("renderengine.type")("PATHCPU")).Get<string>();
	// TILEPATHCPU has no light-pass machinery at all (no light sampler,
	// no splatter, no screen-normalized channel): enabling lt there would
	// only switch eye-side caustic suppression on with nothing to deposit
	// - the same class of hole as the GPU zero-light-task tail. MNEE is
	// still allowed: it lives inside the eye path.
	const bool ltCapable = (engineType == "PATHCPU") ||
			(engineType == "RTPATHCPU") || (engineType == "PATHOCL") ||
			(engineType == "TILEPATHOCL") || (engineType == "RTPATHOCL");
	const bool mneeCapable = ltCapable || (engineType == "TILEPATHCPU");
	if (!mneeCapable)
		return;
	// path.lighttracing.only is a debug/validation mode: an explicit
	// request implies enable even on scenes whose signature is not
	// caustic-capable (the user asked for light paths, period)
	if (ltCapable &&
			GetConfig().Get(Property("path.lighttracing.only")(false)).Get<bool>() &&
			!GetConfig().IsDefined("path.lighttracing.enable")) {
		GetConfig().Set(Property("path.lighttracing.enable")(true));
		SDL_LOG("path.lighttracing.only: enabled light tracing"
				" (explicit light-only request)");
	}
	if (!SceneHasCausticCapablePaths(GetScene()))
		return;
	if (ltCapable &&
			GetConfig().Get(Property("path.lighttracing.auto")(true)).Get<bool>() &&
			!GetConfig().IsDefined("path.lighttracing.enable")) {
		GetConfig().Set(Property("path.lighttracing.enable")(true));
		SDL_LOG("path.lighttracing.auto: enabled light tracing"
				" (caustic-capable scene signature)");
	}
	// MNEE covers the eye-side half of the same class: delta/glossy
	// chains blocking a direct-light connect. It is near-free on scenes
	// that never trigger it, and the solver gates itself per connect.
	if (GetConfig().Get(Property("path.mnee.auto")(true)).Get<bool>() &&
			!GetConfig().IsDefined("path.mnee.enable")) {
		GetConfig().Set(Property("path.mnee.enable")(true));
		SDL_LOG("path.mnee.auto: enabled the MNEE solver"
				" (caustic-capable scene signature)");
	}
	// PSR (path.regularization.auto, default on): on caustic-capable
	// scenes the first passes are the expensive ones - SDS chains take
	// thousands of samples to resolve unblurred. Seed a conservative
	// sigma + a 64-spp halflife decay so early frames resolve caustics
	// fast and the blur vanishes into the noise floor (Kaplanyan
	// decaying schedule: asymptotically unbiased, sigma snaps to 0
	// under 1e-5). Any explicit path.regularization.* property wins.
	if (GetConfig().Get(Property("path.regularization.auto")(true)).Get<bool>()) {
		if (!GetConfig().IsDefined("path.regularization.sigma") &&
				!GetConfig().IsDefined("path.regularization.halflife")) {
			GetConfig().Set(Property("path.regularization.sigma")(0.03f));
			GetConfig().Set(Property("path.regularization.halflife")(64.0f));
			SDL_LOG("path.regularization.auto: seeded PSR sigma=0.03 "
					"halflife=64spp (caustic-capable scene signature)");
		}
	}
}

void RenderConfig::DeleteAllFilmImagePipelinesProperties() {
	GetConfig().DeleteAll(GetConfig().GetAllNamesRE("film\\.imagepipeline\\.[0-9]+\\..*"));
	GetConfig().DeleteAll(GetConfig().GetAllNamesRE("film\\.imagepipelines\\.[0-9]+\\.[0-9]+\\..*")); 
}

void RenderConfig::UpdateFilmProperties(const luxrays::Properties &props) {
	// I can not use GetProperty() here because it triggers a ToProperties() and it can
	// be a problem with OpenCL disabled (PATHOCL is not defined, etc.)
	if (GetConfig().Get(Property("debug.renderconfig.parse.print")(false)).Get<bool>()) {
		SDL_LOG("=============RenderConfig::UpdateFilmProperties()==============" << endl <<
				props);
		SDL_LOG("===============================================================");
	}

	//--------------------------------------------------------------------------
	// Check if there was a new image pipeline definition
	//--------------------------------------------------------------------------

	if (props.HaveNamesRE("film\\.imagepipeline\\.[0-9]+\\.type") ||
			props.HaveNamesRE("film\\.imagepipelines\\.[0-9]+\\.[0-9]+\\.type")) {
		// Delete the old image pipeline properties
		GetConfig().DeleteAll(GetConfig().GetAllNamesRE("film\\.imagepipeline\\.[0-9]+\\..*"));
		GetConfig().DeleteAll(GetConfig().GetAllNamesRE("film\\.imagepipelines\\.[0-9]+\\.[0-9]+\\..*"));

		// Update the RenderConfig properties with the new image pipeline definition
		std::regex reOldSyntax("film\\.imagepipeline\\.[0-9]+\\..*");
		std::regex reNewSyntax("film\\.imagepipelines\\.[0-9]+\\.[0-9]+\\..*");
		for(string propName: props.GetAllNames()) {
			if (std::regex_match(propName, reOldSyntax) ||
					std::regex_match(propName, reNewSyntax))
				GetConfig().Set(props.Get(propName));
		}
		
		// Reset the properties cache
		propsCache->Clear();
	}

	//--------------------------------------------------------------------------
	// Check if there are new radiance group scales
	//--------------------------------------------------------------------------

	if (props.HaveNames("film.imagepipeline.radiancescales.") ||
			props.HaveNamesRE("film\\.imagepipelines\\.[0-9]+\\.radiancescales\\..*")) {
		// Delete the old image pipeline properties
		GetConfig().DeleteAll(GetConfig().GetAllNames("film.imagepipeline.radiancescales."));
		GetConfig().DeleteAll(GetConfig().GetAllNamesRE("film\\.imagepipelines\\.[0-9]+\\.radiancescales\\..*"));

		// Update the RenderConfig properties with the new image pipeline definition
		std::regex reNewSyntax("film\\.imagepipelines\\.[0-9]+\\.radiancescales\\..*");
		for(string propName: props.GetAllNames()) {
			if (propName.starts_with("film.imagepipeline.radiancescales.") ||
					std::regex_match(propName, reNewSyntax))
				GetConfig().Set(props.Get(propName));
		}

		// Reset the properties cache
		propsCache->Clear();
	}

	//--------------------------------------------------------------------------
	// Check if there were new outputs definition
	//--------------------------------------------------------------------------

	if (props.HaveNames("film.outputs.")) {
		// Delete old radiance groups scale properties
		GetConfig().DeleteAll(GetConfig().GetAllNames("film.outputs."));
		
		// Update the RenderConfig properties with the new outputs definition properties
		for(string propName: props.GetAllNames()) {
			if (propName.starts_with("film.outputs."))
				GetConfig().Set(props.Get(propName));
		}

		// Reset the properties cache
		propsCache->Clear();
	}

	//--------------------------------------------------------------------------
	// Check if there is a new film size definition
	//--------------------------------------------------------------------------

	const bool filmWidthDefined = props.IsDefined("film.width");
	const bool filmHeightDefined = props.IsDefined("film.height");
	if (filmWidthDefined || filmHeightDefined) {
		if (filmWidthDefined)
			GetConfig().Set(props.Get("film.width"));
		if (filmHeightDefined)
			GetConfig().Set(props.Get("film.height"));
		
		// Reset the properties cache
		propsCache->Clear();
	}
}

void RenderConfig::Delete(const string &prefix) {
	// Reset the properties cache
	propsCache->Clear();

	GetConfig().DeleteAll(GetConfig().GetAllNames(prefix));
}

FilterUPtr RenderConfig::AllocPixelFilter() const {
	return Filter::FromProperties(*cfg);
}

FilmUPtr RenderConfig::AllocFilm() const {
	auto film = Film::FromProperties(cfg);

	// Add the channels required by the Sampler
	Film::FilmChannels channels;
	Sampler::AddRequiredChannels(channels, *cfg);
	for (auto const c : channels)
		film->AddChannel(c);

	InjectCryptomatteManifests(*film);

	return film;
}

// Cryptomatte manifests: the scene owns the name->id mapping, the film
// only stores opaque metadata for EXR output. Injected whenever the
// matching channel is enabled so Film stays scene-agnostic.
void RenderConfig::InjectCryptomatteManifests(Film &film) const {
	if (film.HasChannel(Film::CRYPTOMATTE_OBJECT)) {
		const string key = CryptoManifestKey("CryptoObject");
		film.SetMetadata("cryptomatte/" + key + "/manifest",
				sceneRef->GetCryptomatteManifest(true));
	}
	if (film.HasChannel(Film::CRYPTOMATTE_MATERIAL)) {
		const string key = CryptoManifestKey("CryptoMaterial");
		film.SetMetadata("cryptomatte/" + key + "/manifest",
				sceneRef->GetCryptomatteManifest(false));
	}
}

std::unique_ptr<SamplerSharedData> RenderConfig::AllocSamplerSharedData(
	const RandomGeneratorUPtr & rndGen, FilmRef film
) const {
	return SamplerSharedData::FromProperties(*cfg, rndGen, FilmPtr(&film));
}
std::unique_ptr<SamplerSharedData> RenderConfig::AllocSamplerSharedData(
	const RandomGeneratorUPtr & rndGen, FilmPtr film
) const {
	return SamplerSharedData::FromProperties(*cfg, rndGen, film);
}

std::unique_ptr<Sampler> RenderConfig::AllocSampler(
	const std::unique_ptr<RandomGenerator> & rndGen,
	FilmPtr film,
	FilmSampleSplatterRPtr flmSplatter,
	const std::shared_ptr<SamplerSharedData> sharedData,
	const Properties &additionalProps
) const {
	auto& props = *cfg;
	props << additionalProps;

	return Sampler::FromProperties(props, rndGen, film, flmSplatter, sharedData);
}

std::unique_ptr<Sampler> RenderConfig::AllocSampler(
	const std::unique_ptr<RandomGenerator> & rndGen,
	FilmRef film,
	FilmSampleSplatterRPtr flmSplatter,
	const std::shared_ptr<SamplerSharedData> sharedData,
	const Properties &additionalProps
) const {
	auto& props = *cfg;
	props << additionalProps;

	return Sampler::FromProperties(props, rndGen, FilmPtr(&film), flmSplatter, sharedData);
}

RenderEngineUPtr RenderConfig::AllocRenderEngine() {
#if defined(LUXRAYS_DISABLE_OPENCL)
	// This is a specific test for OpenCL-less version in order to print
	// a more clear error
	const string type = GetConfig().Get(Property("renderengine.type")(PathCPURenderEngine::GetObjectTag())).Get<string>();
	if ((type == "PATHOCL") ||
			(type == "RTPATHOCL") ||
			(type == "TILEPATHOCL"))
		throw runtime_error(type + " render engine is not supported by OpenCL-less version of the binaries. Download the OpenCL-enabled version or change the render engine used.");
#endif

	// Backend consistency guard: ReSTIR DI reservoir resampling is
	// implemented inside the CPU path tracers' direct light sampling
	// (pathtracer.cpp) and inside the GPU path tracers' kernels
	// (DirectLight_Illuminate() in pathoclbase_funcs.cl, enabled by
	// CompilePathTracer()). Any other engine silently degrades to the
	// parent power-based distribution (e.g. BIDIRCPU never calls
	// SampleLightsBSDF), so fail loudly instead of rendering with a
	// different algorithm than requested.
	if (LightStrategy::GetType(GetConfig()) == TYPE_RESTIR_DI) {
		const string engineType = GetConfig().Get(
			Property("renderengine.type")(PathCPURenderEngine::GetObjectTag())
		).Get<string>();

		if (engineType != PathCPURenderEngine::GetObjectTag() &&
				engineType != TilePathCPURenderEngine::GetObjectTag() &&
				engineType != RTPathCPURenderEngine::GetObjectTag() &&
				engineType != "PATHOCL" &&
				engineType != "TILEPATHOCL" &&
				engineType != "RTPATHOCL")
			throw runtime_error(
				"lightstrategy.type = RESTIR_DI is only supported by the "
				"PATHCPU, TILEPATHCPU, RTPATHCPU, PATHOCL, TILEPATHOCL and "
				"RTPATHOCL render engines (engine used: "
				+ engineType + "). Either switch to a path tracer engine or "
				"use another light strategy."
			);
	}

	return RenderEngine::FromProperties(*this);
}

PropertiesRPtr RenderConfig::ToProperties() const {
	if (!propsCache->GetSize())
		propsCache = ToProperties(*cfg);

	return propsCache;
}

PropertiesUPtr RenderConfig::ToProperties(const Properties &cfg) {
	auto props_ptr = std::make_unique<Properties>();

	Properties& props = *props_ptr;

	// LuxRays context
	props << cfg.Get(Property("context.verbose")(true));

	// Ray intersection accelerators
	props << cfg.Get(Property("accelerator.type")("AUTO"));
	props << cfg.Get(Property("accelerator.instances.enable")(true));
	props << cfg.Get(Property("accelerator.motionblur.enable")(true));
	// (M)BVH accelerator
	props << cfg.Get(Property("accelerator.bvh.builder.type")("EMBREE_BINNED_SAH"));
	props << cfg.Get(Property("accelerator.bvh.treetype")(4));
	props << cfg.Get(Property("accelerator.bvh.costsamples")(0));
	props << cfg.Get(Property("accelerator.bvh.isectcost")(80));
	props << cfg.Get(Property("accelerator.bvh.travcost")(10));
	props << cfg.Get(Property("accelerator.bvh.emptybonus")(.5));

	// Scene epsilon
	props << cfg.Get(Property("scene.epsilon.min")(DEFAULT_EPSILON_MIN));
	props << cfg.Get(Property("scene.epsilon.max")(DEFAULT_EPSILON_MAX));

	props << cfg.Get(Property("scene.file")("scenes/luxball/luxball.scn"));
	props << cfg.Get(Property("scene.images.resizepolicy.type")("NONE"));

	// LightStrategy
	props << *LightStrategy::ToProperties(cfg);

	// RenderEngine (includes PixelFilter and Sampler where applicable)
	props << *RenderEngine::ToProperties(cfg);

	// Film
	props << *Film::ToProperties(cfg);

	// Periodic saving
	props << cfg.Get(Property("periodicsave.film.outputs.period")(0.f));
	props << cfg.Get(Property("periodicsave.film.period")(0.f));
	props << cfg.Get(Property("periodicsave.film.filename")("film.flm"));
	props << cfg.Get(Property("periodicsave.resumerendering.period")(0.f));
	props << cfg.Get(Property("periodicsave.resumerendering.filename")("rendering.rsm"));

	props << cfg.Get(Property("resumerendering.filesafe")(true));

	// Debug
	props << cfg.Get(Property("debug.renderconfig.parse.print")(false));
	props << cfg.Get(Property("debug.scene.parse.print")(false));

	//--------------------------------------------------------------------------

	// This property isn't really used by LuxCore but is useful for GUIs.
	props << cfg.Get(Property("screen.refresh.interval")(100u));
	// This property isn't really used by LuxCore but is useful for GUIs.
	props << cfg.Get(Property("screen.tool.type")("CAMERA_EDIT"));

	props << cfg.Get(Property("screen.tiles.pending.show")(true));
	props << cfg.Get(Property("screen.tiles.converged.show")(false));
	props << cfg.Get(Property("screen.tiles.notconverged.show")(false));

	props << cfg.Get(Property("screen.tiles.passcount.show")(false));
	props << cfg.Get(Property("screen.tiles.error.show")(false));

	return std::move(props_ptr);
}

//------------------------------------------------------------------------------
// Serialization methods
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
// Serialization entry points
//------------------------------------------------------------------------------

RenderConfigUPtr RenderConfig::LoadSerialized(const std::string &fileName) {
	SerializationInputFile sif(fileName);

	// Pointer root matching SaveSerialized()'s RenderConfig* record
	RenderConfig *renderConfig = nullptr;
	sif.GetArchive() >> renderConfig;
	if (!renderConfig)
		throw runtime_error(
			"Error while loading serialized render configuration: " + fileName
		);

	if (!sif.IsGood())
		throw runtime_error(
			"Error while loading serialized render configuration: " + fileName
		);

	return RenderConfigUPtr(renderConfig);
}

// Save serialized method - pointer argument
void RenderConfig::SaveSerialized(
	const std::string &fileName,
	const RenderConfigUPtr& renderConfig
) {
	Properties emptyProps;
	SaveSerialized(fileName, renderConfig, emptyProps);
}

void RenderConfig::SaveSerialized(
	const std::string &fileName,
	const RenderConfigUPtr& renderConfig,
	const luxrays::Properties &additionalCfg
) {
	SerializationOutputFile sof(fileName);

	// This is quite a trick
	renderConfig->saveAdditionalCfg.Clear();
	renderConfig->saveAdditionalCfg.Set(additionalCfg);

	// Serialize through a pointer root: LoadSerialized() reads a raw
	// RenderConfig* record (see the ConstRef overload below).
	sof.GetArchive() << renderConfig.get();

	renderConfig->saveAdditionalCfg.Clear();

	if (!sof.IsGood())
		throw runtime_error(
			"Error while saving serialized render configuration: " + fileName
		);

	sof.Flush();

	SLG_LOG(
		"Render configuration saved: "
		<< (sof.GetPosition() / 1024)
		<< " Kbytes"
	);
}

// Save serialized method - reference argument
void RenderConfig::SaveSerialized(
	const std::string &fileName,
	const RenderConfigConstRef renderConfig,
	const luxrays::Properties &additionalCfg
) {
	SerializationOutputFile sof(fileName);

	// This is quite a trick
	renderConfig.saveAdditionalCfg.Clear();
	renderConfig.saveAdditionalCfg.Set(additionalCfg);

	// Serialize through a pointer root: LoadSerialized() and the .rsm
	// loader read a raw RenderConfig* record; a by-value root wrote an
	// object record that no loader could consume.
	const RenderConfig *renderConfigPtr = &renderConfig;
	sof.GetArchive() << renderConfigPtr;

	renderConfig.saveAdditionalCfg.Clear();

	if (!sof.IsGood())
		throw runtime_error(
			"Error while saving serialized render configuration: " + fileName
		);

	sof.Flush();

	SLG_LOG(
		"Render configuration saved: " << (sof.GetPosition() / 1024) << " Kbytes"
	);
}

//------------------------------------------------------------------------------
// Non-default constructor handlers
//------------------------------------------------------------------------------

BOOST_CLASS_EXPORT_IMPLEMENT(slg::RenderConfig)

template<class Archive>
void slg::RenderConfig::save_construct_data(
    Archive & ar, const RenderConfig * t, const unsigned int file_version
) {
    // save data required to construct instance

	// Save Configuration (cfg merged with the additional properties set
	// by SaveSerialized())
	PropertiesUPtr completeCfg = std::make_unique<Properties>();
	completeCfg->Set(*t->cfg);
	completeCfg->Set(t->saveAdditionalCfg);
	ar << completeCfg;

	// Save internal Scene
	ar << t->internalScene;

    // Save SceneRef (as a pointer)
    ar << t->sceneRef;
}

template<class Archive>
void slg::RenderConfig::load_construct_data(
    Archive & ar, RenderConfig * t, const unsigned int file_version
) {
    // retrieve data from archive required to construct new instance
    // create and load data through pointer to object
    // tracking handles issues of duplicates.
	PropertiesUPtr cfg;
	ar >> cfg;

	SceneUPtr sptr;
	ar >> sptr;

	decltype(sceneRef) sref;  // Load reference as a pointer
	ar >> sref;

    // invoke inplace constructor to initialize instance of RenderConfig
	::new(t) RenderConfig(std::move(cfg), *sref, std::move(sptr));  // NB: this is a placement new

}

template<typename Archive>
void slg::RenderConfig::serialize(Archive& ar, const unsigned int version) {
}

namespace slg {
// Explicit instantiations for portable archives
template void RenderConfig::serialize(LuxOutputArchive &ar, const u_int version);
template void RenderConfig::serialize(LuxInputArchive &ar, const u_int version);
template void RenderConfig::serialize(LuxOutputArchiveText &ar, const u_int version);
template void RenderConfig::serialize(LuxInputArchiveText &ar, const u_int version);

template void RenderConfig::load_construct_data(LuxInputArchive &ar, RenderConfig *, const u_int version);
template void RenderConfig::save_construct_data(LuxOutputArchive &ar, const RenderConfig *, const u_int version);
template void RenderConfig::load_construct_data(LuxInputArchiveText &ar, RenderConfig *, const u_int version);
template void RenderConfig::save_construct_data(LuxOutputArchiveText &ar, const RenderConfig *, const u_int version);
}

// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
