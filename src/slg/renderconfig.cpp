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
#include "slg/materials/cyclesbssrdf.h"
#include "slg/materials/mix.h"
#include "slg/materials/twosided.h"
#include "slg/volumes/homogenous.h"
#include "slg/volumes/heterogenous.h"
#include "slg/scene/sceneobjectdefs.h"
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
	ApplyAutoCausticCache();
	ApplyAutoSSSDepth();

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

// Any surface the MNEE solver can walk: delta specular (mirror, smooth
// glass/dielectric)
static bool SceneHasDeltaSurface(SceneConstRef scene) {
	const auto &mats = scene.GetMaterials();
	for (u_int i = 0; i < mats.GetSize(); ++i) {
		const auto &m = mats.GetMaterial(i);
		if ((m.GetType() == NULLMAT) || m.IsVolume())
			continue;
		if ((m.GetEventTypes() & SPECULAR) || m.IsDelta())
			return true;
	}
	return false;
}

// Narrower signature for the light pass: can the eye/light partition
// (PathInfo::IsAdaptiveCausticPath / IsAdaptiveTerminalHard) hand any
// path class to it? A surface terminal must be delta or glossy at or
// under glossyThreshold; a medium terminal only counts once the chain
// crossed a non-diffuse surface (causticHasSurface), so fog or SSS
// alone among matte surfaces owns nothing. Without such a class the
// light pass's task share is pure cost: 300 tiny bulbs over rough brass
// ~30% more noise at equal time, a matte fog room half its eye passes.
static bool SceneHasLightPassPaths(SceneConstRef scene, const float glossyThreshold) {
	const auto &lightDefs = scene.GetLightSources();
	if (lightDefs.GetSize() == 0)
		return false;
	// A glossy (non-delta) terminal only needs the light pass when some
	// emitter is invisible to eye rays (point, spot, sphere, distant...):
	// area and environment emitters are reached by the eye's own glossy
	// bounce under MIS, so there the light pass is pure cost - 1.6-2x
	// lower equal-time efficiency on area-lit rough-metal / thin-film
	// scenes, with identical converged images. (Names: the lights vector needs
	// Preprocess(), which hasn't run yet.)
	bool eyeHiddenLight = false;
	for (const auto &name : lightDefs.GetLightSourceNames()) {
		const auto &l = lightDefs.GetLightSource(name);
		if (!l.IsIntersectable() && !l.IsEnvironmental()) {
			eyeHiddenLight = true;
			break;
		}
	}
	bool scatteringVolume = false;
	bool nonDiffuseSurface = false;
	const auto &mats = scene.GetMaterials();
	for (u_int i = 0; i < mats.GetSize(); ++i) {
		const auto &m = mats.GetMaterial(i);
		if (m.GetType() == NULLMAT)
			continue;
		if (m.IsVolume()) {
			scatteringVolume |= (m.GetType() != CLEAR_VOL);
			continue;
		}
		const BSDFEvent events = m.GetEventTypes();
		if ((events & SPECULAR) || m.IsDelta())
			return true;
		if (events & GLOSSY) {
			if (eyeHiddenLight && (m.GetGlossiness() <= glossyThreshold))
				return true;
			nonDiffuseSurface = true;
		}
	}
	return scatteringVolume && nonDiffuseSurface;
}

// Glossiness above which the eye/light partition never hands a glossy
// surface terminal to the light pass (PathInfo::IsAdaptiveTerminalHard /
// IsCausticPath)
static float LightPassGlossinessThreshold(const Properties &cfg) {
	const auto defaultProps = PathTracer::GetDefaultProps();
	if (cfg.Get(defaultProps->Get("path.hybridbackforward.adaptivecaustic")).Get<bool>())
		return Clamp(cfg.Get(defaultProps->Get("path.hybridbackforward.terminalglossiness")).Get<float>(), 0.f, 1.f);
	if (cfg.Get(defaultProps->Get("path.hybridbackforward.enable")).Get<bool>())
		return Clamp(cfg.Get(defaultProps->Get("path.hybridbackforward.glossinessthreshold")).Get<float>(), 0.f, 1.f);
	return .05f;
}

// Zero-config caustic stack (path.lighttracing.auto / path.mnee.auto,
// both default on): when the user hasn't pinned the flags, enable the
// light pass + the MNEE solver only on engines with a light-task
// population and only when the scene can form caustic-class paths - on
// diffuse-only scenes the tail tasks would deposit nothing (wasted
// budget), so auto keeps the eye side at 100%. The light pass itself
// uses the narrower SceneHasLightPassPaths(); MNEE/PSR keep the wider
// signature.
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
	// Vertex connection (GPU BDPT connects) is not yet consistent in
	// scattering media: its MIS with the hybrid eye/light estimators
	// overestimated a furnace fog sphere by ~40% (subsurface too). Keep
	// those scenes on the verified estimators; VC stays on elsewhere.
	if (GetConfig().Get(Property("path.vertexconnection.enable")(false)).Get<bool>()) {
		const auto &mats = GetScene().GetMaterials();
		for (u_int i = 0; i < mats.GetSize(); ++i) {
			const auto &m = mats.GetMaterial(i);
			if (m.IsVolume() && (m.GetType() != CLEAR_VOL)) {
				GetConfig().Set(Property("path.vertexconnection.enable")(false));
				SDL_LOG("path.vertexconnection.enable: disabled - the scene has"
						" scattering media, where vertex connection is not"
						" yet unbiased");
				break;
			}
		}
	}
	if (!SceneHasCausticCapablePaths(GetScene()))
		return;
	if (ltCapable &&
			GetConfig().Get(Property("path.lighttracing.auto")(true)).Get<bool>() &&
			!GetConfig().IsDefined("path.lighttracing.enable") &&
			SceneHasLightPassPaths(GetScene(), LightPassGlossinessThreshold(GetConfig()))) {
		GetConfig().Set(Property("path.lighttracing.enable")(true));
		SDL_LOG("path.lighttracing.auto: enabled light tracing"
				" (caustic-capable scene signature)");
	}
	// MNEE covers the eye-side half of the same class: delta chains
	// blocking a direct-light connect. The solver only ever walks delta
	// specular vertices (pathtracer_mnee.cpp), so a glossy-only scene can
	// never trigger it - there it was pure cost: the MNEE kernel stage
	// plus ~1.9KB/task of GPU solver state (1GB at 512K tasks).
	if (GetConfig().Get(Property("path.mnee.auto")(true)).Get<bool>() &&
			!GetConfig().IsDefined("path.mnee.enable") &&
			SceneHasDeltaSurface(GetScene())) {
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

// A refracting caster that focuses light: delta or near-delta TRANSMIT.
// Architectural glass passes rays straight (no focusing) and NULLMAT only
// passes them through, so neither can cast a caustic.
static bool SceneHasRefractiveCaster(SceneConstRef scene, const float glossyThreshold) {
	const auto &mats = scene.GetMaterials();
	for (u_int i = 0; i < mats.GetSize(); ++i) {
		const auto &m = mats.GetMaterial(i);
		if ((m.GetType() == NULLMAT) || (m.GetType() == ARCHGLASS) || m.IsVolume())
			continue;
		const BSDFEvent events = m.GetEventTypes();
		if (!(events & TRANSMIT))
			continue;
		if ((events & SPECULAR) || m.IsDelta() ||
				((events & GLOSSY) && (m.GetGlossiness() < glossyThreshold)))
			return true;
	}
	return false;
}

// A volume that scatters (constant zero scattering - absorption-only water -
// does not count)
static bool SceneHasScatteringVolume(SceneConstRef scene) {
	const auto &mats = scene.GetMaterials();
	for (u_int i = 0; i < mats.GetSize(); ++i) {
		const auto &m = mats.GetMaterial(i);
		const Texture *sigmaS = nullptr;
		if (m.GetType() == HOMOGENEOUS_VOL)
			sigmaS = &static_cast<const HomogeneousVolume &>(m).GetSigmaS();
		else if (m.GetType() == HETEROGENEOUS_VOL)
			sigmaS = &static_cast<const HeterogeneousVolume &>(m).GetSigmaS();
		else
			continue;
		if (!(((sigmaS->GetType() == CONST_FLOAT) || (sigmaS->GetType() == CONST_FLOAT3)) &&
				(sigmaS->Filter() <= 0.f)))
			return true;
	}
	return false;
}

// Zero-config SDS caustics (path.photongi.caustic.auto, default on). A
// caustic seen through the caster itself - a pool floor through the water
// surface, a table top through a glass - is a light -> S -> D -> S -> eye
// path: the light pass can not splat it (the eye end is specular) and the
// eye's NEE can not connect it (the light end is specular; MNEE only solves
// delta emitters, not the sun's cone). The photon caustic cache is the one
// estimator that covers the class, so enable it when the scene has a
// refracting caster and caustic.enabled was not given explicitly - the same
// rule as the Blender add-on's caustic_mode = auto.
void RenderConfig::ApplyAutoCausticCache() {
	if (!GetConfig().Get(Property("path.photongi.caustic.auto")(true)).Get<bool>() ||
			GetConfig().IsDefined("path.photongi.caustic.enabled"))
		return;
	// Path engines. RTPATHCPU (CPU viewport) builds the cache in the
	// background and keeps it across camera edits; RTPATHOCL has no
	// swap protocol yet
	const string engineType = GetConfig().Get(
		Property("renderengine.type")("PATHCPU")).Get<string>();
	if ((engineType != "PATHCPU") && (engineType != "TILEPATHCPU") &&
			(engineType != "PATHOCL") && (engineType != "TILEPATHOCL") &&
			(engineType != "RTPATHCPU"))
		return;
	// Filter Glossy blurs the eye path's glass lobes; the photons are traced
	// unblurred and the cache replaces those eye paths, so the sharp caustic
	// would come back several times brighter than the blurred reference
	if (GetConfig().Get(Property("path.filterglossy")(0.f)).Get<float>() > 0.f)
		return;
	// Light-only debug renders have no eye path to query the cache
	if (GetConfig().Get(Property("path.lighttracing.only")(false)).Get<bool>())
		return;
	if (GetScene().GetLightSources().GetSize() == 0)
		return;
	const float glossyThreshold = GetConfig().Get(
			Property("path.photongi.glossinessusagethreshold")(.05f)).Get<float>();
	if (!SceneHasRefractiveCaster(GetScene(), glossyThreshold))
		return;
	// Not validated with scattering media: a glass ball in world fog
	// (showcase fogbeam) came out 3.4x a BDPT reference with the cache
	// (11x through the ball), 1.08x without it. The light pass and the
	// medium tools already cover volume caustics there.
	if (SceneHasScatteringVolume(GetScene())) {
		SDL_LOG("path.photongi.caustic.auto: skipped (scattering volume in the scene)");
		return;
	}

	GetConfig().Set(Property("path.photongi.caustic.enabled")(true));
	// The legacy photon depth (4) is sized for the indirect cache: it
	// leaves three delta interfaces before the landing vertex, and the
	// eye side cuts the light hit the cache replaces, so every longer
	// chain renders dark (three glass slabs, 6 interfaces: floor 0.875x
	// of the reference, e116). Same depth the Blender add-on exports.
	if (!GetConfig().IsDefined("path.photongi.photon.maxdepth"))
		GetConfig().Set(Property("path.photongi.photon.maxdepth")(16));
	SDL_LOG("path.photongi.caustic.auto: enabled the caustic cache"
			" (refracting caster: caustics seen through it need it)");
}

void RenderConfig::ApplyAutoSSSDepth() {
	const string engineType = GetConfig().Get(
		Property("renderengine.type")("PATHCPU")).Get<string>();
	const bool isOCL = (engineType == "PATHOCL") ||
			(engineType == "TILEPATHOCL") || (engineType == "RTPATHOCL");
	if (!isOCL && (engineType != "PATHCPU") && (engineType != "RTPATHCPU") &&
			(engineType != "TILEPATHCPU"))
		return;
	if (!GetConfig().Get(Property("path.pathdepth.volume.auto")(true)).Get<bool>())
		return;

	bool hasSSS = false;
	const auto &mats = GetScene().GetMaterials();
	for (u_int i = 0; i < mats.GetSize(); ++i) {
		const auto &m = mats.GetMaterial(i);
		if ((m.GetType() == HOMOGENEOUS_VOL) &&
				static_cast<const HomogeneousVolume &>(m).IsSSSParametrized()) {
			hasSSS = true;
			break;
		}
	}
	if (!hasSSS)
		return;

	// A subsurface random walk is one path vertex per scatter, and with
	// the engine defaults (total 6, volume 0 = scatters count as diffuse,
	// diffuse 4) the walk is cut after ~4 steps: an OpenPBR sphere with
	// subsurface_color .8 and a radius 1/100 of its size rendered .13 in
	// a white furnace instead of .78 (together with the boundary
	// reflections, see BSDF::IsVolumeDepthVertex()). The walk albedo after the CB15
	// remap is ~.99+, so the step count has a long tail: a volume depth
	// of 256 still lost 5%, 512 lost 1.4%, 1024 converged (render time
	// +45% over 256 on that furnace - walks end on their own, the cap
	// only bounds the tail).
	const int sssVolumeDepth = 1024;
	if (GetConfig().IsDefined("path.pathdepth.volume")) {
		// Explicit volume budget: only fill a missing total around it
		const int volume = GetConfig().Get("path.pathdepth.volume").Get<int>();
		if (volume <= 0) {
			SDL_LOG("WARNING: the scene has subsurface scattering media but"
					" path.pathdepth.volume = 0 counts their random-walk steps"
					" as diffuse bounces: the walks are truncated and dark");
			return;
		}
		if (!GetConfig().IsDefined("path.pathdepth.total") &&
				!GetConfig().IsDefined("path.maxdepth")) {
			GetConfig().Set(Property("path.pathdepth.total")(6 + volume));
			SDL_LOG("path.pathdepth.volume.auto: total depth " << (6 + volume) <<
					" (default surface budget + volume depth " << volume << ")");
		}
		return;
	}
	// The Metropolis sampler on the GPU stores every sample dimension
	// per task (2 x 10 floats per path vertex x task count): a 1000+
	// vertex budget does not fit, leave the choice to the user
	if (isOCL && (GetConfig().Get(Property("sampler.type")("SOBOL")).Get<string>() ==
			"METROPOLIS")) {
		SDL_LOG("WARNING: the scene has subsurface scattering media; their"
				" random walks are truncated by the path depth, which is not"
				" raised automatically with the GPU Metropolis sampler - set"
				" path.pathdepth.volume and path.pathdepth.total");
		return;
	}

	// No volume budget given (legacy 0: walk steps count as diffuse
	// bounces and against the total): give the walk its own budget on
	// top of the surface one, so an explicit total keeps its meaning
	// (Blender exports always set it) and surface paths are unchanged
	int surfaceTotal = 6;
	if (GetConfig().IsDefined("path.pathdepth.total"))
		surfaceTotal = GetConfig().Get("path.pathdepth.total").Get<int>();
	else if (GetConfig().IsDefined("path.maxdepth") &&
			!GetConfig().IsDefined("path.pathdepth.diffuse") &&
			!GetConfig().IsDefined("path.pathdepth.glossy") &&
			!GetConfig().IsDefined("path.pathdepth.specular")) {
		// Legacy single depth: setting path.pathdepth.total turns off the
		// path.maxdepth compatibility in PathTracer::ParseOptions(), so
		// spell out the per-event depths it would have used
		surfaceTotal = GetConfig().Get("path.maxdepth").Get<int>();
		for (const string k : {"diffuse", "glossy", "specular"})
			GetConfig().Set(Property("path.pathdepth." + k)(surfaceTotal));
	}
	const int total = surfaceTotal + sssVolumeDepth;
	GetConfig().Set(Property("path.pathdepth.volume")(sssVolumeDepth));
	GetConfig().Set(Property("path.pathdepth.total")(total));
	SDL_LOG("path.pathdepth.volume.auto: volume depth " << sssVolumeDepth <<
			", total depth " << total << " (subsurface scattering media)");
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

void RenderConfig::ValidateCyclesBSSRDF() const {
	bool experimentalBSSRDF = false;
	const auto &props = GetConfig();
	const string engine = props.Get(Property("renderengine.type")("PATHCPU")).Get<string>();
	const bool adjointOptIn =
			props.Get(Property("path.cyclesbssrdf.experimental.adjoint.enable")(false)).Get<bool>();
	const bool deviceDiagnostic = engine == "PATHOCL" &&
			props.Get(Property("path.cyclesbssrdf.experimental.device.enable")(false)).Get<bool>();
	const bool deviceAdjointDiagnostic = deviceDiagnostic && adjointOptIn &&
			(props.Get(Property("path.hybridbackforward.enable")(true)).Get<bool>() ||
			 props.Get(Property("path.lighttracing.enable")(false)).Get<bool>());
	// CPU hybrid uses the same reverse walk as LIGHTCPU, with its existing
	// disjoint eye/caustic partition. Both ways of promoting PATHCPU to
	// hybrid must validate the reverse kernel before worker creation.
	const bool cpuHybridDiagnostic = engine == "PATHCPU" && adjointOptIn &&
			(props.Get(Property("path.hybridbackforward.enable")(true)).Get<bool>() ||
			 props.Get(Property("path.lighttracing.enable")(false)).Get<bool>());
	const bool adjointDiagnostic = (engine == "LIGHTCPU" && adjointOptIn) || cpuHybridDiagnostic || deviceAdjointDiagnostic;
	const auto &objects = GetScene().GetObjects();
	for (u_int i = 0; i < objects.GetSize(); ++i) {
		const Material &root = objects.GetSceneObject(i).GetMaterial();
		std::unordered_set<const Material *> referenced;
		root.AddReferencedMaterials(referenced);
		bool contains = root.GetType() == CYCLES_BSSRDF;
		for (const auto material : referenced)
			contains |= material->GetType() == CYCLES_BSSRDF;
		if (contains) {
			if (adjointDiagnostic && (root.GetType() != CYCLES_BSSRDF || root.GetBumpTexture() ||
					root.GetFrontTransparencyTexture() || root.GetBackTransparencyTexture() ||
					root.GetInteriorVolume() || root.GetExteriorVolume() || GetScene().HasDefaultWorldVolume()))
				throw runtime_error("Experimental cyclesbssrdf adjoint mixed, partitioned, Normal/Bump, opacity and Volume kernels remain unsupported");
			if (adjointDiagnostic) {
				const auto &group = objects.GetSceneObject(i).GetSubsurfaceGroup();
				if (!group.empty()) {
					for (u_int j = 0; j < objects.GetSize(); ++j)
						if (objects.GetSceneObject(j).GetSubsurfaceGroup() == group &&
								&objects.GetSceneObject(j).GetMaterial() != &root)
							throw runtime_error("Experimental cyclesbssrdf adjoint partitioned groups require nonlocal kernel reconstruction");
				}
			}
			referenced.insert(&root);
			bool mixed = false;
			for (const auto material : referenced)
				mixed |= material->GetType() == MIX && material->HasCyclesBSSRDF();
			// A selected component currently shares the entry frame. Reject
			// explicit opacity overrides and contexts requiring a new frame.
			if (mixed) {
				for (const auto material : referenced)
					if ((material->GetType() != NULLMAT && (material->GetFrontTransparencyTexture() ||
							material->GetBackTransparencyTexture())) || material->GetBumpTexture() ||
							material->GetInteriorVolume() || material->GetExteriorVolume() ||
							GetScene().HasDefaultWorldVolume())
						throw runtime_error("Experimental cyclesbssrdf mixed closures with explicit opacity, Normal/Bump or Volume contexts remain unsupported: " + material->GetName());
			}
			std::vector<std::pair<const Material *, unsigned>> pending{{&root, 0u}};
			while (!pending.empty()) {
				const auto entry = pending.back();
				pending.pop_back();
				const auto material = entry.first;
				if (!material->HasCyclesBSSRDF() && !material->HasNullLobes()) continue;
				if (entry.second >= 64u)
					throw runtime_error("Experimental cyclesbssrdf closure selection exceeds 64 levels");
				if (material->GetType() == MIX) {
					const auto &mix = static_cast<const MixMaterial &>(*material);
					pending.emplace_back(&mix.GetMaterialA(), entry.second + 1u);
					pending.emplace_back(&mix.GetMaterialB(), entry.second + 1u);
				} else if (material->GetType() == TWOSIDED) {
					const auto &two = static_cast<const TwoSidedMaterial &>(*material);
					pending.emplace_back(two.GetFrontMaterial().get(), entry.second + 1u);
					pending.emplace_back(two.GetBackMaterial().get(), entry.second + 1u);
				}
			}
			for (const auto material : referenced) {
				if (material->GetType() == CYCLES_BSSRDF) {
					const auto &closure = static_cast<const CyclesBSSRDFMaterial &>(*material);
					const auto failure = closure.ExperimentalParameterFailure(
							GetConfig().Get(Property("path.spectral.enable")(false)).Get<bool>());
					if (!failure.empty())
						throw runtime_error("Experimental cyclesbssrdf material " + closure.GetName() + ": " + failure);
					if (adjointDiagnostic) {
						const auto adjointFailure = closure.ExperimentalAdjointParameterFailure(
								props.Get(Property("path.spectral.enable")(false)).Get<bool>());
						if (!adjointFailure.empty())
							throw runtime_error("Experimental cyclesbssrdf material " + closure.GetName() + ": " + adjointFailure);
						if (deviceAdjointDiagnostic) {
							HitPoint hit;
							hit.Init();
							const auto p = closure.Freeze(hit);
							if (p.roughness == 0.f && (p.radiusRGB.c[0] >= 1e-8f ||
									p.radiusRGB.c[1] >= 1e-8f || p.radiusRGB.c[2] >= 1e-8f))
								throw runtime_error("Experimental cyclesbssrdf GPU sharp boundary camera connections remain unimplemented");
						}
					}
					continue;
				}
				if (material->GetType() == TWOSIDED || material->GetType() == MIX)
					continue;
				std::unordered_set<const Material *> children;
				material->AddReferencedMaterials(children);
				for (const auto child : children)
					if (child->GetType() == CYCLES_BSSRDF)
						throw runtime_error("Experimental cyclesbssrdf mixed closures require nonlocal selection/PDF support");
			}
		}
		experimentalBSSRDF |= contains;
	}
	if (experimentalBSSRDF) {
		if (deviceDiagnostic) {
			for (u_int i = 0; i < objects.GetSize(); ++i) {
				const auto &material = objects.GetSceneObject(i).GetMaterial();
				std::unordered_set<const Material *> referenced;
				material.AddReferencedMaterials(referenced);
				referenced.insert(&material);
				for (const auto child : referenced)
					// Grouped exits can land on other material partitions too.
					if ((child->GetType() == CYCLES_BSSRDF || !objects.GetSceneObject(i).GetSubsurfaceGroup().empty()) &&
							(child->GetBumpTexture() || child->GetInteriorVolume() || child->GetExteriorVolume() || GetScene().HasDefaultWorldVolume()))
						throw runtime_error("Experimental cyclesbssrdf device exit Normal/Bump and explicit Volume contexts remain unverified: " + child->GetName());
			}
			if ((props.Get(Property("path.vertexconnection.enable")(false)).Get<bool>() ||
					props.Get(Property("path.vertexconnect.enable")(false)).Get<bool>()))
				throw runtime_error("Experimental cyclesbssrdf device vertex connection is not implemented yet");
		}
		if (!props.Get(Property("path.cyclesbssrdf.experimental.enable")(false)).Get<bool>() ||
				(engine != "PATHCPU" && !deviceDiagnostic && !adjointDiagnostic) ||
				(props.Get(Property("path.hybridbackforward.enable")(true)).Get<bool>() && !cpuHybridDiagnostic && !deviceAdjointDiagnostic) ||
				(props.Get(Property("path.lighttracing.enable")(false)).Get<bool>() && !cpuHybridDiagnostic && !deviceAdjointDiagnostic) ||
				(props.Get(Property("path.lighttracing.only")(false)).Get<bool>() && !deviceAdjointDiagnostic) ||
				(props.Get(Property("path.vertexconnection.enable")(false)).Get<bool>() ||
					props.Get(Property("path.vertexconnect.enable")(false)).Get<bool>()) ||
				props.Get(Property("path.restir.gi.enable")(false)).Get<bool>() ||
				props.Get(Property("path.restir.pt.enable")(false)).Get<bool>() ||
				props.Get(Property("path.photongi.caustic.enabled")(false)).Get<bool>() ||
				props.Get(Property("path.photongi.indirect.enabled")(false)).Get<bool>())
			throw runtime_error("Experimental cyclesbssrdf requires explicit eye-only diagnostics or adjoint opt-in for LIGHTCPU/CPU hybrid and PATHOCL device light/hybrid; PATHOCL requires device opt-in, and BIDIR, vertex connections and caches remain unsupported");
	}
}

RenderEngineUPtr RenderConfig::AllocRenderEngine() {
	ValidateCyclesBSSRDF();
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
