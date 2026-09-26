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

#include <atomic>
#include <memory>
#include <mutex>

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
		const bool useBSDFEVal = true,
		// M4b RIS product guiding: this path's candidate-weight mean
		// Zhat (>0 activates the pHat = t/Zhat bounce density in MIS;
		// 0 keeps the plain/mixture competitor).
		const float risZhat = 0.f) const;

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

	// LPE: accumulate r into the lpeRadiance slots of every expression
	// whose NFA accepts the terminal symbol sym (see slg/utils/lpe.h).
	// The (vSym, sym) overload is for next-event connections: the
	// receiving vertex's own event steps before the terminal.
	static void AccumulateLPE(SampleResult *sampleResult, const EyePathInfo &pathInfo,
			const u_int sym, const luxrays::Spectrum &r);
	static void AccumulateLPE(SampleResult *sampleResult, const EyePathInfo &pathInfo,
			const u_int vSym, const u_int sym, const luxrays::Spectrum &r);

	// Used for Sampler indices
	u_int eyeSampleBootSize, eyeSampleStepSize, eyeSampleSize;
	u_int lightSampleBootSize, lightSampleStepSize, lightSampleSize;

	// Path depth settings
	PathDepthInfo maxPathDepth;

	u_int rrDepth;
	float rrImportanceCap;

	// Clamping settings
	float sqrtVarianceClampMaxValue;
	// Adaptive Robust Clamping (see varianceclamping.h): 0/1 flags and
	// scope enum match the CL kernel layout (0 = all, 1 = indirect,
	// 2 = direct)
	int varianceClampAdaptive;
	int varianceClampScope;
	float varianceClampSigma;

	// Hybrid backward/forward path tracing settings
	float hybridBackForwardPartition, hybridBackForwardGlossinessThreshold;
	// Adaptive caustic partition: widened chain membership (any
	// non-diffuse vertex) plus a per-path terminal difficulty gate.
	float hybridBackForwardTerminalGlossiness, hybridBackForwardConnectProb;
	bool hybridBackForwardAdaptiveCaustic;

	// Albedo AOV settings
	AlbedoSpecularSetting albedoSpecularSetting;
	float albedoSpecularGlossinessThreshold;
	
	// Option flags
	bool forceBlackBackground, hybridBackForwardEnable;

	// Hero-wavelength spectral transport (P2-1): when enabled, each path
	// draws 3 stratified wavelengths and Spectrum channels carry spectral
	// bins instead of RGB primaries. CPU path engines only.
	bool spectralEnable;
	// RGB->SPD upsampling model (path.spectral.upsampling): false = Smits
	// basis (default, zero regression), true = Jakob-Hanika 2019 sigmoid
	// model (rgb2spec coefficient table). GPU backends read the flag to
	// upload the JH2019 table buffer; kernels switch on the buffer
	// pointer itself.
	bool spectralUpsamplingJH2019;

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
	// Manifold seed cache (path.mnee.seedcache): converged single-vertex
	// solutions are cached in a hashed world-space grid and reused as
	// Newton warm-start seeds for nearby attempts on the same occluder.
	// The light-side (LMNEE) solves namespace their entries with a
	// sentinel light index, exactly like the GPU mneeSeeds table.
	bool mneeSeedCacheEnable;
	struct MneeSeedEntry {
		std::atomic<float> vx{0.f}, vy{0.f}, vz{0.f};
		std::atomic<float> nx{0.f}, ny{0.f}, nz{0.f};
		std::atomic<u_int> lightIndex{0}, meshIndex{0};
		std::atomic<u_int> mirrorMode{0}, valid{0};
	};
	// The table is shared by all render threads: seeds are last-writer-wins
	// hints (a stale entry only wastes Newton iterations, the constraint is
	// still verified), so relaxed atomics are sufficient.
	std::unique_ptr<MneeSeedEntry[]> mneeSeeds;

	// ReSTIR GI (G1) settings (path.restir.gi.*): the CPU implementation
	// runs through the restirGI store below; the GPU kernels read the
	// mirrored taskConfig.pathTracer.restirGI copy.
	bool restirGIEnable;
	u_int restirGICandidates;
	bool restirGITemporalEnable;
	bool restirGISpatialEnable;

	// RIS product-guiding candidate count (path.guiding.risk): K > 0
	// resamples K mixture-proposal draws against the product target
	// f*|cos|*Lhat; K = 1 degenerates to the plain mixture draw.
	int guidingRisK;

	// Guiding artist gates (P5, path.guiding.*): earliest bounce depth
	// the guide may sample (early bounces are DL-covered), glossiness
	// cutoff skipping lobes the coarse field cannot resolve, the
	// pure-diffuse opt-in, and a strength scale on the guide-side
	// selection probability. All four mirror into
	// taskConfig->pathTracer for the kernels.
	int guidingMinDepth;
	float guidingGlossiness;
	bool guidingDiffuse;
	float guidingStrength;

	// GPU light tracing (path.lighttracing.*): a second task population
	// on PATHOCL/RTPATHOCL traces light sub-paths and splats their
	// vertices into RADIANCE_PER_SCREEN_NORMALIZED via camera projection
	// (doc/features/gpu_lighttracing.md). Implies hybridBackForward on
	// the eye side (caustic suppression, same contract as CPU hybrid).
	bool lightTracingEnable;
	float lightTracingTaskFraction;

	// Vertex connection (M6, path.vertexconnection.enable): GPU port of
	// the BIDIRCPU eye x light vertex connect against a per-light-task
	// vertex cache. Requires a light-task population; when set, the
	// caustic-only splat gate and the hybrid diffuse-cut are replaced by
	// the SmallVCM MIS weights (misVm/misVc = 0 -> pure BPT).
	bool vertexConnectEnable;
	// Probabilistic connection (M7): expected connect budget per eye
	// vertex (0 = every candidate), the number of light tasks pooled
	// per eye vertex (1 = paired task only) and whether the budget is
	// scaled by measured per-tile connect efficiency.
	u_int vertexConnectBudget, vertexConnectPoolTasks;
	bool vertexConnectAdaptive;
	// Vertex merging (M7): merge radius as a fraction of the scene
	// bounding-sphere radius (0 disables merging)
	float vertexConnectMergeRadius;
	// Temporal connect reuse (M7d): replay each eye task's best
	// connect vertex as an extra deterministic candidate
	bool vertexConnectReuse;
	// Caustic focus cache: guided light emission toward remembered
	// productive targets (see LIGHT_FOCUS_K in pathoclbase_datatypes.cl
	// for the GPU ring; the CPU table below mirrors it). Unbiased
	// mixture: pdf = (1-ratio)*native + ratio*aim.
	bool lightFocusEnable;
	float lightFocusRatio;
	float lightFocusRadiusFrac;
	// Per-light ring of the last K world-space targets that produced a
	// screen contribution through a delta interface, plus the write
	// cursor. Entries are last-writer-wins hints shared by all render
	// threads: a torn read folds to the broad aim radius (same contract
	// as the GPU ring), so relaxed atomics are sufficient.
	static constexpr u_int lightFocusK = 32;
	struct LightFocusEntry {
		std::atomic<float> x{0.f}, y{0.f}, z{0.f}, aimR{0.f};
	};
	mutable std::once_flag lightFocusInitOnce;
	mutable std::unique_ptr<LightFocusEntry[]> lightFocusTable;
	mutable std::unique_ptr<std::atomic<u_int>[]> lightFocusCounts;
	mutable u_int lightFocusLightCount = 0;
	// Delta-specular caster bounding spheres (world space). Distant-family
	// lights have no steerable emission direction, so LightFocusEmit
	// instead re-origins their rays onto the casters' discs projected
	// onto the emit plane: without this a directional light's rays are
	// spread over the whole scene disc and almost none reach the glass.
	mutable std::vector<luxrays::Point> lightFocusCasterCenters;
	mutable std::vector<float> lightFocusCasterRadii;

private:
	friend class CompiledScene;
	friend class PathOCLBaseOCLRenderThread;
	friend class BiDirCPURenderThread;

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

	// LMNEE: light-side manifold connect x0 -> specular vertex -> camera
	// lens, the CPU port of the GPU LMnee_* driver. Called by ConnectToEye
	// when the visibility ray hits a delta specular occluder. Returns true
	// if a contribution was splatted. See pathtracer_mnee.cpp.
	bool LMNEEConnectToEye(
			luxrays::IntersectionDeviceRef device,
			SceneConstRef scene,
			FilmConstRef film, const float time,
			const LightSource &light, const BSDF &bsdf,
			const luxrays::Spectrum &flux, const LightPathInfo &pathInfo,
			const luxrays::RayHit &shadowRayHit,
			const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
			BSDF &warmV0, BSDF &warmV1, bool &warmOk,
			std::vector<SampleResult> &sampleResults) const;

	// LMNEE, multi-specular variant: x0 -> x1 -> ... -> xN -> lens for
	// closed dielectrics (slabs, spheres) that need more than one
	// refracting interface. Mirrors MNEEMultiDirectSampling. warmV0/warmV1
	// optionally carry a consistent [solved vertex, seg2 blocker] seed
	// pair from the failed single-vertex solve.
	bool LMNEEMultiConnectToEye(
			luxrays::IntersectionDeviceRef device,
			SceneConstRef scene,
			FilmConstRef film, const float time,
			const LightSource &light, const BSDF &bsdf,
			const luxrays::Spectrum &flux, const LightPathInfo &pathInfo,
			const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
			const BSDF *warmV0, const BSDF *warmV1,
			std::vector<SampleResult> &sampleResults) const;

	// Caustic focus cache (CPU side of the GPU lightFocus rings):
	// lazy allocation against the scene light count, hotspot crediting
	// and the guided-emission mixture applied to a freshly emitted ray.
	void LightFocusEnsureInit(SceneConstRef scene) const;
	void LightFocusCredit(SceneConstRef scene, const u_int lightIndex,
			const luxrays::Point &p) const;
	void LightFocusEmit(SceneConstRef scene, const LightSource &light,
			Sampler &sampler, const float time,
			luxrays::Ray &ray, luxrays::Spectrum &flux,
			float &emissionPdfW) const;
	// U-variants take the four focus draws (coin, slot, cone x2) as
	// explicit arguments: engines whose sampler layout differs from
	// PathTracer's boot dims (BIDIRCPU draws them at 13-16) can drive
	// the same guided-emission mixture with their own dimensions.
	void LightFocusEmitU(SceneConstRef scene, const LightSource &light,
			const float time,
			luxrays::Ray &ray, luxrays::Spectrum &flux,
			float &emissionPdfW, const float uCoin, const float uSlot,
			const float uCone0, const float uCone1) const;
	// Distant-family branch of the caustic focus: the light direction is
	// (nearly) fixed, so instead of re-aiming the direction the ray origin
	// is re-sampled on a caster disc projected onto the emit plane.
	void LightFocusEmitDistant(SceneConstRef scene, const LightSource &light,
			Sampler &sampler, const float time,
			luxrays::Ray &ray, float &emissionPdfW) const;
	void LightFocusEmitDistantU(SceneConstRef scene, const LightSource &light,
			const float time,
			luxrays::Ray &ray, float &emissionPdfW,
			const float uCoin, const float uSlot,
			const float uCone0, const float uCone1) const;
	// Mixture emission pdf of a light-subpath direction: anywhere the
	// light strategy's own pdf appears as the ALTERNATIVE strategy in a
	// MIS weight (BIDIR NEE weightCamera) it must evaluate the same
	// (1-g)*native + g*aim mixture or the partition leaks energy.
	float LightFocusEmissionPdfW(SceneConstRef scene, const LightSource &light,
			const luxrays::Vector &emitDir, const float nativePdfW) const;
	// Aim-branch directional density at emitDir (per solid angle,
	// normalized by the slot count); shared by the emit-time pdf and the
	// alternative-strategy lookup above.
	float LightFocusAimPdf(const u_int lightIndex, const u_int focusN,
			const luxrays::Point &rayOrig, const luxrays::Vector &emitDir,
			const float nativePdf) const;

	FilterDistribution *pixelFilterDistribution;
	const PhotonGICache *photonGICache;
	// Path guiding (P1-3 M1 CPU; M2b GPU): owned by the engine, shared by
	// all render threads (lock-free inside). Null when disabled.
	const PathGuidingCache *pathGuidingCache;
	// Guiding on/off (path.guiding.enable) + frozen table file for GPU
	// sampling (path.guiding.tablefile, empty = inline CPU training).
	bool guidingEnable;
	std::string guidingTableFile;

	// Portal-guided bounce sampling (M5, path.portal.*): artist-placed
	// planar rects marking apertures through which light enters a space
	// (window, doorway, slit). With probability portalShare the bounce
	// direction is proposed by aiming at a uniform point on a rect - a
	// directional technique with an analytic solid-angle density,
	// folded into the one-sample MIS alongside BSDF/guide. This is the
	// routing fix for scenes whose variance lives in "did the path
	// transit the opening" - directional guiding fields cannot resolve
	// features narrower than their lobe width.
	struct PortalRect {
		luxrays::Point v0;		// one rect corner
		luxrays::Vector e1, e2;	// edge vectors from v0
		luxrays::Vector n;		// unit normal
		float invArea;
		// 2x2 Gram coefficients for the inside-rect solve
		float g11, g12, g22, invDet;

		luxrays::Point SamplePoint(const float u, const float v) const {
			return v0 + u * e1 + v * e2;
		}
		// Solid-angle density of direction d from p under this rect's
		// uniform-area proposal: r^2/(A*|cos_s|), zero when the ray
		// misses the rect or runs parallel to its plane.
		float PdfW(const luxrays::Point &p, const luxrays::Vector &d) const {
			const float dn = luxrays::Dot(d, n);
			if (dn == 0.f)
				return 0.f;
			const float t = luxrays::Dot(v0 - p, n) / dn;
			if (!(t > 0.f))
				return 0.f;
			const luxrays::Vector ds = (p + t * d) - v0;
			const float u = invDet * (g22 * luxrays::Dot(ds, e1) - g12 * luxrays::Dot(ds, e2));
			const float v = invDet * (g11 * luxrays::Dot(ds, e2) - g12 * luxrays::Dot(ds, e1));
			if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f)
				return 0.f;
			return t * t * invArea / fabsf(dn);
		}
	};
	std::vector<PortalRect> portals;
	float portalShare;

	// Aggregate portal proposal density: the bounce picks one rect
	// uniformly, so the marginal is (1/N)*sum of the per-rect pdfs.
	float PortalPdfW(const luxrays::Point &p, const luxrays::Vector &d) const {
		float sum = 0.f;
		for (u_int i = 0; i < portals.size(); ++i)
			sum += portals[i].PdfW(p, d);
		return sum / portals.size();
	}
	// False when p lies on every portal's plane (the proposal would be
	// degenerate: all candidate directions run in-plane). Must be
	// mirrored exactly between the bounce side and the DL-side density.
	bool PortalUsableAt(const luxrays::Point &p) const {
		for (u_int i = 0; i < portals.size(); ++i)
			if (fabsf(luxrays::Dot(p - portals[i].v0, portals[i].n)) > 1e-4f)
				return true;
		return false;
	}
	// Half-space gate for the portal proposal (env LUX_PG_PORTALSIDE):
	// +1 fires only on the +n side of a portal, -1 only on -n, 0
	// (default) on both. Lets a one-sided aperture skip wasting draws
	// on vertices behind the portal.
	float portalSideGate = 0.f;
	bool PortalSideOK(const luxrays::Point &p) const {
		if (portalSideGate == 0.f)
			return true;
		for (u_int i = 0; i < portals.size(); ++i) {
			const float s = luxrays::Dot(p - portals[i].v0, portals[i].n);
			if (s * portalSideGate > 1e-4f)
				return true;
		}
		return false;
	}
	// The aperture must sit in the surface's upper hemisphere - a portal
	// behind the shading normal can never deliver light to this vertex,
	// so proposing it would burn the share on zero-contribution draws.
	bool PortalFacingOK(const luxrays::Point &p, const luxrays::Vector &n) const {
		for (u_int i = 0; i < portals.size(); ++i) {
			const luxrays::Vector d = portals[i].v0 +
					.5f * (portals[i].e1 + portals[i].e2) - p;
			if (luxrays::Dot(d, n) > 0.f)
				return true;
		}
		return false;
	}
	// Adaptive portal share (M5): the technique earns the fraction of
	// the leaf's incident field arriving through the aperture -
	// Sum_i Omega_i * Lhat(d_i) / leafTotal, capped by portalShare. A
	// slit-dominated leaf gets the full share; a leaf lit mostly by
	// interreflection keeps its bounce budget. Falls back to the full
	// share while the field is untrained (early exploration).
	// Env LUX_PG_PORTALADAPT=0 disables the adaptation (fixed share).
	bool portalAdapt = true;
	float PortalShareAt(const luxrays::Point &p) const;

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
