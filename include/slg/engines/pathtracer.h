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

#ifndef _SLG_PATHTRACER_H
#define	_SLG_PATHTRACER_H

#include "slg/slg.h"
#include "slg/engines/cpurenderengine.h"
#include "slg/samplers/sampler.h"
#include "slg/film/film.h"
#include "slg/film/filmsamplesplatter.h"
#include "slg/bsdf/bsdf.h"
#include "slg/engines/caches/photongi/photongicache.h"
#include "slg/engines/pathguiding.h"
#include "slg/engines/restirgi.h"
#include "slg/utils/pathinfo.h"

namespace slg {

// OpenCL data types
namespace ocl {
#include "slg/engines/pathtracer_types.cl"
}

//------------------------------------------------------------------------------
// Path Tracing render code
//
// All the methods of this class must be thread-safe because they will be used
// by render threads	
//------------------------------------------------------------------------------

class PathTracer;
class VarianceClamping;

class PathTracerThreadState {
public:
	PathTracerThreadState(
		luxrays::IntersectionDeviceRef device,
		const SamplerUPtr& eyeSampler,
		const SamplerUPtr& lightSampler,
		SceneConstRef scene, FilmRef film,
		const VarianceClamping *varianceClamping,
		const bool useFilmSplat = false
	);
	virtual ~PathTracerThreadState();

	SamplerRef GetEyeSampler() { return *eyeSampler; }
	SamplerRef GetLightSampler() { return *lightSampler; }

	luxrays::IntersectionDeviceRef device;

	SceneConstRef scene;
	FilmRef GetFilm() { return film; }
	FilmConstRef GetFilm() const { return film; }
	const VarianceClamping *varianceClamping;

	std::vector<SampleResult> & GetEyeSampleResults() { return std::ref(eyeSampleResults); }
	std::vector<SampleResult> & GetLightSampleResults() { return std::ref(lightSampleResults); }

	// Used for hybrid rendering
	double eyeSampleCount, lightSampleCount;

private:
	const SamplerUPtr& eyeSampler;
	const SamplerUPtr& lightSampler;
	FilmRef film;
	std::vector<SampleResult> eyeSampleResults, lightSampleResults;
};

class PhotonGICache;

class PathTracer {
public:
	typedef enum {
		ILLUMINATED, SHADOWED, NOT_VISIBLE
	} DirectLightResult;

	typedef std::function<void(const LightPathInfo &pathInfo,
			const BSDF &, const u_int, const luxrays::Spectrum &,
			std::vector<SampleResult> &sampleResults)> ConnectToEyeCallBackType;

	PathTracer();
	virtual ~PathTracer();

	void InitPixelFilterDistribution(const FilterUPtr& pixelFilter);
	void DeletePixelFilterDistribution();

	void SetPhotonGICache(const PhotonGICache *cache) { photonGICache = cache; }
	const PhotonGICache *GetPhotonGICache() const { return photonGICache; }

	void SetPathGuidingCache(const PathGuidingCache *cache) { pathGuidingCache = cache; }
	const PathGuidingCache *GetPathGuidingCache() const { return pathGuidingCache; }

	void SetRestirGI(RestirGI *gi) { restirGI = gi; }
	RestirGI *GetRestirGI() const { return restirGI; }

	void ParseOptions(
		luxrays::PropertiesConstRef cfg,
		const luxrays::Properties &defaultProps
	);

	DirectLightResult DirectLightSampling(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const float u0,
		const float u1, const float u2,
		const float u3, const float u4,
		const EyePathInfo &pathInfo, const luxrays::Spectrum &pathThrouput,
		const BSDF &bsdf, SampleResult *sampleResult,
		const bool useBSDFEVal = true) const;

	void RenderEyePath(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene,
		Sampler& sampler,
		EyePathInfo &pathInfo,
		luxrays::Ray &eyeRay, const luxrays::Spectrum &eyeTroughput,
		std::vector<SampleResult> &sampleResults) const;
	void RenderEyeSample(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene, FilmConstRef film,
		Sampler& sampler,
		std::vector<SampleResult> &sampleResults) const;

	void RenderLightSample(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene,
		FilmConstRef film,
		Sampler& sampler,
		std::vector<SampleResult> &sampleResults,
		const ConnectToEyeCallBackType &ConnectToEyeCallBack) const;
	void RenderLightSample(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene,
		FilmConstRef film,
		Sampler& sampler,
		std::vector<SampleResult> &sampleResults
	) const {
		static const ConnectToEyeCallBackType noCallback;
		RenderLightSample(device, scene, film, sampler, sampleResults, noCallback);
	}

	bool HasToRenderEyeSample(PathTracerThreadState &state) const;
	void ApplyVarianceClamp(const PathTracerThreadState &state,
			std::vector<SampleResult> &sampleResults) const;
	void RenderSample(PathTracerThreadState &state) const;

	static void InitEyeSampleResults(FilmConstRef film, std::vector<SampleResult> &sampleResults,
			const bool useFilmSplat = false);
	static void ResetEyeSampleResults(std::vector<SampleResult> &sampleResults);
	static SampleResult &AddLightSampleResult(std::vector<SampleResult> &sampleResults,
			FilmConstRef film);

	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static luxrays::PropertiesUPtr GetDefaultProps();

	// Used for Sampler indices
	u_int eyeSampleBootSize, eyeSampleStepSize, eyeSampleSize;
	u_int lightSampleBootSize, lightSampleStepSize, lightSampleSize;

	// Path depth settings
	PathDepthInfo maxPathDepth;

	u_int rrDepth;
	float rrImportanceCap;

	// Clamping settings
	float sqrtVarianceClampMaxValue;

	// Hybrid backward/forward path tracing settings
	float hybridBackForwardPartition, hybridBackForwardGlossinessThreshold;

	// Albedo AOV settings
	AlbedoSpecularSetting albedoSpecularSetting;
	float albedoSpecularGlossinessThreshold;
	
	// Option flags
	bool forceBlackBackground, hybridBackForwardEnable;

	// Hero-wavelength spectral transport (P2-1): when enabled, each path
	// draws 3 stratified wavelengths and Spectrum channels carry spectral
	// bins instead of RGB primaries. CPU path engines only.
	bool spectralEnable;

	// MNEE (Manifold Next Event Estimation) direct light sampling through
	// delta specular surfaces (Hanika et al. 2015, single specular vertex;
	// Zeltner et al. 2020 for chains of more than one vertex).
	bool mneeEnable;
	u_int mneeMaxIterations;
	// Maximum number of delta specular vertices in the MNEE chain. 1 selects
	// the single vertex solver alone (the default: no behavior change), 2 and
	// more also enable the multi-specular chain that closed glass slabs and
	// glass balls need.
	u_int mneeMaxSpecular;
	// Manifold seed cache (GPU only, path.mnee.seedcache): converged
	// single-vertex solutions are cached in a hashed world-space grid and
	// reused as Newton warm-start seeds for nearby attempts.
	bool mneeSeedCacheEnable;

	// ReSTIR GI (G1) settings (path.restir.gi.*): the CPU implementation
	// runs through the restirGI store below; the GPU kernels read the
	// mirrored taskConfig.pathTracer.restirGI copy.
	bool restirGIEnable;
	u_int restirGICandidates;
	bool restirGITemporalEnable;
	bool restirGISpatialEnable;

private:
	void GenerateEyeRay(CameraConstRef camera, FilmConstRef film,
			luxrays::Ray &eyeRay, PathVolumeInfo &volInfo,
			Sampler& sampler,
			SampleResult &sampleResult) const;

	// RenderEyeSample methods

	void DirectHitFiniteLight(SceneConstRef scene, const EyePathInfo &pathInfo,
			const luxrays::Spectrum &pathThrouput, const luxrays::Ray &ray,
			const float distance, const BSDF &bsdf,
			SampleResult *sampleResult) const;
	void DirectHitInfiniteLight(SceneConstRef scene, const EyePathInfo &pathInfo,
			const luxrays::Spectrum &pathThrouput, const luxrays::Ray &ray,
			const BSDF *bsdf, SampleResult *sampleResult) const;
	bool CheckDirectHitVisibilityFlags(LightSourceConstRef lightSource,
			const PathDepthInfo &depthInfo,	const BSDFEvent lastBSDFEvent) const;

	// MNEE: solve the single specular chain x0 -> x1 (delta specular occluder)
	// -> y (positional delta light) and add the contribution to sampleResult.
	// Returns true if a contribution was added. See pathtracer_mnee.cpp and
	// dev-tools/mnee_design.md.
	bool MNEEDirectSampling(
			luxrays::IntersectionDeviceRef device,
			SceneConstRef scene,
			const float time,
			const EyePathInfo &pathInfo,
			const luxrays::Spectrum &pathThrouput,
			const BSDF &bsdf,
			LightSourceConstRef light, const float lightPickPdf, const float risScale,
			const luxrays::Ray &shadowRay, const float directPdfW0,
			const luxrays::RayHit &shadowRayHit,
			const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
			const float u1, const float u2, const float u3, const float u4,
			SampleResult *sampleResult) const;

	// MNEE, multi-specular variant: solve the chain x0 -> x1 -> ... -> xN
	// (delta specular occluders) -> y and add the contribution to
	// sampleResult. Only attempted when the single vertex solve found no
	// solution. Returns true if a contribution was added. See
	// pathtracer_mnee.cpp and dev-tools/mnee_design.md section 4b.
	bool MNEEMultiDirectSampling(
			luxrays::IntersectionDeviceRef device,
			SceneConstRef scene,
			const float time,
			const EyePathInfo &pathInfo,
			const luxrays::Spectrum &pathThrouput,
			const BSDF &bsdf,
			LightSourceConstRef light, const float lightPickPdf, const float risScale,
			const luxrays::Ray &shadowRay, const float directPdfW0,
			const luxrays::RayHit &shadowRayHit,
			const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
			const float u1, const float u2, const float u3, const float u4,
			SampleResult *sampleResult) const;

	// RenderLightSample methods

	void ConnectToEye(luxrays::IntersectionDeviceRef device,
			SceneConstRef scene,
			FilmConstRef film, const float time,
			const float u0, const float u1, const float u2,
			const LightSource &light,  const BSDF &bsdf,
			const luxrays::Spectrum &flux, const LightPathInfo &pathInfo,
			std::vector<SampleResult> &sampleResults) const;

	FilterDistribution *pixelFilterDistribution;
	const PhotonGICache *photonGICache;
	// Path guiding (P1-3 M1 CPU; M2b GPU): owned by the engine, shared by
	// all render threads (lock-free inside). Null when disabled.
	const PathGuidingCache *pathGuidingCache;
	// Guiding on/off (path.guiding.enable) + frozen table file for GPU
	// sampling (path.guiding.tablefile, empty = inline CPU training).
	bool guidingEnable;
	std::string guidingTableFile;

	// ReSTIR GI (G1): per-pixel first-bounce reservoir, owned by the
	// engine and shared by all render threads (advisory lock-free
	// access). Null when disabled. Path-level state - not a light
	// strategy - so it lives here, not in LightStrategy.
	RestirGI *restirGI;

	static const Film::FilmChannels eyeSampleResultsChannels;
	static const Film::FilmChannels lightSampleResultsChannels;
};

}

#endif	/* _SLG_PATHTRACER_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
