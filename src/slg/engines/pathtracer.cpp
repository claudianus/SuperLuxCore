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


#include "luxrays/usings.h"
#include "luxrays/utils/mc.h"
#include "luxrays/utils/properties.h"
#include "luxrays/core/color/spectral.h"
#include "slg/lights/light.h"
#include "slg/lights/pointlight.h"
#include "slg/lights/spotlight.h"
#include "slg/lights/distantlight.h"
#include "slg/lights/sharpdistantlight.h"
#include "slg/lights/sunlight.h"
#include "slg/lights/spherelight.h"
#include "slg/lights/trianglelight.h"
#include "slg/scene/sceneobject.h"
#include "slg/usings.h"
#include "slg/engines/pathtracer.h"
#include "slg/engines/caches/photongi/photongicache.h"
#include "slg/engines/pathguiding.h"
#include "slg/samplers/metropolis.h"
#include "slg/utils/varianceclamping.h"
#include "slg/cameras/camera.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PathTracer
//------------------------------------------------------------------------------

PathTracerThreadState::PathTracerThreadState(IntersectionDeviceRef dev,
		const SamplerUPtr& eSampler,
		const SamplerUPtr& lSampler,
		SceneConstRef scn, FilmRef flm,
		const VarianceClamping *varClamping,
		const bool useFilmSplat) : device(dev),
		eyeSampler(eSampler), lightSampler(lSampler), scene(scn), film(flm),
		varianceClamping(varClamping) {
	// Initialize Eye SampleResults
	eyeSampleResults.resize(1);
	PathTracer::InitEyeSampleResults(film, eyeSampleResults, useFilmSplat);

	eyeSampleCount = 0.0;
	// Using 1.0 instead of 0.0 to avoid a division by zero
	lightSampleCount = 1.0;
}

PathTracerThreadState::~PathTracerThreadState() {
}

// Adaptive caustic partition: canonical solid angle of a light as seen
// from a vertex (definition near DirectHitFiniteLight, used by
// DirectLightSampling earlier in this file)
static float LightConnectionSolidAngle(const LightSource &light,
		const Point &P);

//------------------------------------------------------------------------------
// PathTracer
//------------------------------------------------------------------------------

const Film::FilmChannels PathTracer::eyeSampleResultsChannels({
	Film::RADIANCE_PER_PIXEL_NORMALIZED, Film::ALPHA, Film::DEPTH,
	Film::POSITION, Film::GEOMETRY_NORMAL, Film::SHADING_NORMAL, Film::MATERIAL_ID,
	Film::DIRECT_DIFFUSE, Film::DIRECT_DIFFUSE_REFLECT, Film::DIRECT_DIFFUSE_TRANSMIT,
	Film::DIRECT_GLOSSY, Film::DIRECT_GLOSSY_REFLECT, Film::DIRECT_GLOSSY_TRANSMIT,
	Film::EMISSION,
	Film::INDIRECT_DIFFUSE, Film::INDIRECT_DIFFUSE_REFLECT, Film::INDIRECT_DIFFUSE_TRANSMIT,
	Film::INDIRECT_GLOSSY, Film::INDIRECT_GLOSSY_REFLECT, Film::INDIRECT_GLOSSY_TRANSMIT,
	Film::INDIRECT_SPECULAR, Film::INDIRECT_SPECULAR_REFLECT, Film::INDIRECT_SPECULAR_TRANSMIT,
	Film::DIRECT_SHADOW_MASK, Film::INDIRECT_SHADOW_MASK, Film::UV, Film::RAYCOUNT,
	Film::IRRADIANCE, Film::OBJECT_ID, Film::SAMPLECOUNT, Film::CONVERGENCE,
	Film::MATERIAL_ID_COLOR, Film::ALBEDO, Film::AVG_SHADING_NORMAL, Film::NOISE
});

const Film::FilmChannels PathTracer::lightSampleResultsChannels({
	Film::RADIANCE_PER_SCREEN_NORMALIZED
}); 

PathTracer::PathTracer() : pixelFilterDistribution(nullptr),
		photonGICache(nullptr), pathGuidingCache(nullptr),
		guidingEnable(false), spectralEnable(false),
		spectralUpsamplingJH2019(false),
		restirGI(nullptr), restirGIEnable(false), restirGICandidates(4),
		restirGITemporalEnable(true), restirGISpatialEnable(true) {
}

// Path guiding (P1-3 M1): independent bin-pick uniform. The pick must be
// uniform and independent of the jitter uniforms, but sampler dims come
// in at most two Cranley-Patterson shift parities, so any third guide
// dim shares a shift with a jitter dim (correlated triple = biased pdf).
// Hash (pixel, pass, vertex) instead: uniform, shift-independent,
// deterministic.
// M2c: earliest bounce depth at which the guide may sample (env
// LUX_PG_MINDEPTH, default 2). Early-bounce incident is direct-dominated
// and DL already covers it, so guiding there only dilutes; the guide's
// headroom is indirect (deeper bounces). Measured: mindepth 0/1/2 RMSE
// 0.0088/0.0077/0.0068 at small scale (plain 0.0044).
static int GuidingMinDepth() {
	static const int kMinDepth = []() {
		const char *e = getenv("LUX_PG_MINDEPTH");
		return e ? atoi(e) : 2;
	}();
	return kMinDepth;
}
// M2c indirect-only training (default on; LUX_PG_INDIRECT=0 opts out).
// M2c: skip near-smooth glossy (env LUX_PG_GLOSS, default .3). A 128-bin
// field cannot resolve a tight lobe; guiding there only dilutes against
// near-perfect BSDF sampling. Rough-glossy/diffuse keep the guide.
static float GuidingGloss() {
	static const float kGloss = []() {
		const char *e = getenv("LUX_PG_GLOSS");
		return e ? (float)atof(e) : .3f;
	}();
	return kGloss;
}

// M2c E3: guide diffuse bounces too (default off; LUX_PG_DIFFUSE=1).
// M1 excluded them (cosine-BSDF near-optimal, blunt field only adds
// noise); with indirect-only + smoothed + adaptive field it may help.
static bool GuidingDiffuse() {
	static const bool kDiffuse = (getenv("LUX_PG_DIFFUSE") != nullptr);
	return kDiffuse;
}

static bool GuidingIndirect() {
	static const bool kIndirect = []() {
		const char *e = getenv("LUX_PG_INDIRECT");
		return !e || (atoi(e) != 0);
	}();
	return kIndirect;
}

// Is this vertex's BSDF a usable guiding target? The glossiness cutoff
// exists only to skip near-smooth lobes the coarse field cannot resolve
// - it is meaningless for a purely diffuse surface, which previously made
// the LUX_PG_DIFFUSE opt-in unreachable (matte reports glossiness 0 and
// always failed the cutoff). A pure-diffuse bounce is guided when the
// flag is on; a glossy bounce still needs enough roughness for the field.
static bool GuidableBsdf(const BSDF &bsdf) {
	return ((bsdf.GetEventTypes() & GLOSSY) != 0) ?
			(bsdf.GetGlossiness() >= GuidingGloss()) : GuidingDiffuse();
}

static u_int GuidingHash(u_int x) {
	// murmur3 32-bit finalizer (same as SobolSequence::BlueNoiseHash)
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

PathTracer::~PathTracer() {
	delete pixelFilterDistribution;
}

void PathTracer::InitPixelFilterDistribution(const FilterUPtr& pixelFilter) {
	// Compile sample distribution
	delete pixelFilterDistribution;
	pixelFilterDistribution = new FilterDistribution(pixelFilter, 64);
}

void  PathTracer::DeletePixelFilterDistribution() {
	delete pixelFilterDistribution;
	pixelFilterDistribution = NULL;
}

void PathTracer::InitEyeSampleResults(FilmConstRef film, vector<SampleResult> &sampleResults,
		const bool useFilmSplat) {
	SampleResult &sampleResult = sampleResults[0];

	sampleResult.Init(&eyeSampleResultsChannels, film.GetRadianceGroupCount());
	sampleResult.useFilmSplat = useFilmSplat;
}

void PathTracer::ResetEyeSampleResults(vector<SampleResult> &sampleResults) {
	SampleResult &sampleResult = sampleResults[0];

	// Set to 0.0 all result colors
	sampleResult.emission = Spectrum();
	for (u_int i = 0; i < sampleResult.radiance.Size(); ++i)
		sampleResult.radiance[i] = Spectrum();
	sampleResult.directDiffuseReflect = Spectrum();
	sampleResult.directDiffuseTransmit = Spectrum();
	sampleResult.directGlossyReflect = Spectrum();
	sampleResult.directGlossyTransmit = Spectrum();
	sampleResult.indirectDiffuseReflect = Spectrum();
	sampleResult.indirectDiffuseTransmit = Spectrum();
	sampleResult.indirectGlossyReflect = Spectrum();
	sampleResult.indirectGlossyTransmit = Spectrum();
	sampleResult.indirectSpecularReflect = Spectrum();
	sampleResult.indirectSpecularTransmit = Spectrum();
	sampleResult.directShadowMask = 1.f;
	sampleResult.indirectShadowMask = 1.f;
	sampleResult.irradiance = Spectrum();
	sampleResult.albedo = Spectrum();
	sampleResult.isHoldout = false;

	sampleResult.rayCount = 0.f;
}

//------------------------------------------------------------------------------
// RenderEyeSample methods
//------------------------------------------------------------------------------

PathTracer::DirectLightResult PathTracer::DirectLightSampling(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time,
		const float u0, const float u1, const float u2,
		const float u3, const float u4,
		const EyePathInfo &pathInfo, const Spectrum &pathThroughput,
		const BSDF &bsdf, SampleResult *sampleResult,
		const bool useBSDFEVal) const {
	if (!bsdf.IsDelta()) {
		// Select the light strategy to use
		auto& lightStrategy =
			bsdf.IsShadowCatcherOnlyInfiniteLights() ?
			scene.GetLightSources().GetInfiniteLightStrategy() :
			scene.GetLightSources().GetIlluminateLightStrategy();

		// Pick a light source to sample (BSDF-aware path: lets ReSTIR-style
		// strategies weight candidates by estimated contribution).
		// risScale carries a resampling (RIS) output weight separate from
		// the MIS pick pdf so both MIS sides stay consistent.
		float lightPickPdf;
		float risScale = 1.f;
		// The strategy may override the light-surface sample with the
		// winning candidate's own (ReSTIR visibility-weighted targets):
		// the binary V folded into the candidate's target and the payoff
		// below must cover the same surface point.
		float lightSurfaceUs[3] = {u1, u2, u3};
		auto light = lightStrategy.SampleLightsBSDF(
			scene,
			bsdf,
			time,
			u0,
			&lightPickPdf,
			&risScale,
			lightSurfaceUs
		);

		if (light) {
			Ray shadowRay;
			float directPdfW;
			Spectrum lightRadiance = light->Illuminate(
				scene, bsdf, time, lightSurfaceUs[0], lightSurfaceUs[1],
				lightSurfaceUs[2], shadowRay, directPdfW
			);
			verify (!lightRadiance.IsNaN() && !lightRadiance.IsInf());

			if (!lightRadiance.Black()) {
				verify (!isnan(directPdfW) && !isinf(directPdfW));

				BSDFEvent event;
				float bsdfPdfW;
				Spectrum bsdfEval;
				
				if (useBSDFEVal)
					bsdfEval = bsdf.Evaluate(shadowRay.d, &event, &bsdfPdfW);
				else {
					// This is used by BAKECPU and must be aligned with its BSDF sampling
					bsdfEval = Spectrum(Dot(shadowRay.d, bsdf.hitPoint.shadeN) * INV_PI);
					bsdfPdfW = INV_TWOPI;
					event = DIFFUSE | REFLECT;
				}
				verify (!bsdfEval.IsNaN() && !bsdfEval.IsInf());

				if (!bsdfEval.Black() &&
						(!hybridBackForwardEnable ||
						(hybridBackForwardAdaptiveCaustic ?
							// Adaptive partition: the vertex connecting
							// to the light is the terminal; its lobe vs
							// the light's solid angle decides difficulty
							!pathInfo.IsAdaptiveCausticPath(event,
									bsdf.GetGlossiness(),
									hybridBackForwardTerminalGlossiness,
									hybridBackForwardConnectProb,
									LightConnectionSolidAngle(*light, bsdf.hitPoint.p)) :
							!pathInfo.IsCausticPath(event, bsdf.GetGlossiness(), hybridBackForwardGlossinessThreshold)))) {
					verify (!isnan(bsdfPdfW) && !isinf(bsdfPdfW));
					
					// Create a new PathDepthInfo for the path to the light source
					PathDepthInfo directLightDepthInfo = pathInfo.depth;
					directLightDepthInfo.IncDepths(event);
					
					RayHit shadowRayHit;
					BSDF shadowBsdf;
					Spectrum connectionThroughput;
					// Create a new PathVolumeInfo for the path to the light source
					PathVolumeInfo volInfo = pathInfo.volume;
					// Check if the light source is visible
					if (!scene.Intersect(IntersectionDevicePtr(&device), EYE_RAY | SHADOW_RAY, &volInfo, u4, &shadowRay,
							&shadowRayHit, &shadowBsdf, &connectionThroughput, nullptr,
							nullptr, true, &directLightDepthInfo, NONE)) {
						// Add the light contribution only if it is not a shadow catcher
						// (because, if the light is visible, the material will be
						// transparent in the case of a shadow catcher).

						if (!bsdf.IsShadowCatcher()) {
							// I'm ignoring volume emission because it is not sampled in
							// direct light step.
							const float directLightSamplingPdfW = directPdfW * lightPickPdf;
							const float factor = risScale / directLightSamplingPdfW;

							// Path guiding (P1-3 M1): when the bounce at this
							// vertex is mixture-sampled, the competing
							// technique for the DL/BSDF MIS is the mixture
							// (0.5*BSDF + 0.5*guide), not pure BSDF. Using
							// the pure BSDF pdf here overstates the
							// competitor wherever the guide is concentrated
							// elsewhere and slashes DL contributions (bias).
							// (CanGuide is monotonic, so a DL-time/ bounce-
							// time flip is a negligible transient.)
							float bouncePdfW = bsdfPdfW;
							if (guidingEnable && pathGuidingCache && !bsdf.IsDelta() &&
								GuidableBsdf(bsdf) &&
									((int)pathInfo.depth.depth >= GuidingMinDepth()) &&
									pathGuidingCache->CanGuide(bsdf.hitPoint.p)) {
								const float wDl = PathGuidingCache::MixWeight(
										pathGuidingCache->ReadTotal(bsdf.hitPoint.p));
								bouncePdfW = (1.f - wDl) * bsdfPdfW + wDl * pathGuidingCache->Pdf(
										bsdf.hitPoint.p, bsdf.hitPoint.shadeN, shadowRay.d);
							}

							if (directLightDepthInfo.GetRRDepth() >= rrDepth) {
								// Russian Roulette
								bouncePdfW *= RenderEngine::RussianRouletteProb(bsdfEval, rrImportanceCap);
							}

							// Account for material transparency
							bouncePdfW *= light->GetAvgPassThroughTransparency();

							// MIS between direct light sampling and BSDF sampling
							//
							// Note: I have to avoid MIS on the last path vertex
							const bool misEnabled = !sampleResult->lastPathVertex &&
								(light->IsEnvironmental() || light->IsIntersectable()) &&
								CheckDirectHitVisibilityFlags(*light, directLightDepthInfo, event) &&
								!shadowBsdf.hitPoint.throughShadowTransparency;

							const float weight = misEnabled ? PowerHeuristic(directLightSamplingPdfW, bouncePdfW) : 1.f;
							const Spectrum incomingRadiance = bsdfEval * (weight * factor) * connectionThroughput * lightRadiance;

							sampleResult->AddDirectLight(light->GetID(), event, pathThroughput, incomingRadiance, 1.f);

							// The first path vertex is not handled by AddDirectLight(). This is valid
							// for irradiance AOV only if it is not a SPECULAR material.
							//
							// Note: irradiance samples the light sources only here (i.e. no
							// direct hit, no MIS, it would be useless)
							//
							// Note: RR is ignored here because it can not happen on first path vertex
							if ((sampleResult->firstPathVertex) && !(bsdf.GetEventTypes() & SPECULAR))
								sampleResult->irradiance =
										(INV_PI * fabsf(Dot(bsdf.hitPoint.shadeN, shadowRay.d)) *
										factor) * connectionThroughput * lightRadiance;
						}

						return ILLUMINATED;
					} else {
						// The shadow ray was blocked by a surface. MNEE: if the
						// blocker is a delta specular material and the light is a
						// positional delta emitter, try to solve the specular chain
						// x0 -> x1 -> y (Hanika et al. 2015 / Zeltner et al. 2020).
						// The plain estimator is 0 on these paths and forward BSDF
						// sampling can not hit a positional delta light, so the
						// estimators are disjoint and no MIS is required.
						//
						// hybridBackForward: every MNEE path is caustic-class
						// (light -> specular chain -> diffuse -> eye), which the
						// light-tracing pass owns. The isNearlyCaustic gate above
						// only sees speculars in the EYE prefix, not in the
						// connection leg, so MNEE must be suppressed explicitly -
						// measured double counting: interior luminance summed the
						// two estimators exactly (0.0299 = 0.0154 + 0.0149 on
						// tinycaster).
						const LightSourceType mneeLightType = light->GetType();
						// Only a delta-direction emitter is disjoint from forward
						// BSDF sampling (a sharpdistant direction can never be hit
						// by a sampled direction). A distant/sun/environment cone
						// has finite solid angle: forward refraction CAN reach it,
						// so MNEE would double-count the caustic without MIS.
						const bool mneeLightIsDir =
								(mneeLightType == TYPE_SHARPDISTANT);
						const bool mneeLightIsPoint = (mneeLightType == TYPE_POINT ||
								mneeLightType == TYPE_SPOT ||
								mneeLightType == TYPE_MAPPOINT);
						if (mneeEnable && !hybridBackForwardEnable &&
								useBSDFEVal && !bsdf.IsShadowCatcher() &&
								(mneeLightIsPoint || mneeLightIsDir) &&
								!shadowBsdf.IsVolume() &&
								shadowBsdf.IsDelta() &&
								(shadowBsdf.GetEventTypes() & SPECULAR)) {
							if (MNEEDirectSampling(device, scene, time, pathInfo,
									pathThroughput, bsdf, *light, lightPickPdf, risScale,
									shadowRay, directPdfW, shadowRayHit, shadowBsdf, volInfo,
									u1, u2, u3, u4, sampleResult))
								return ILLUMINATED;

							// The single vertex solve found no solution (e.g. the
							// light is not visible from that vertex because a
							// second refracting face is in the way): try the
							// multi-specular chain (closed glass slab / glass ball
							// caustics). The two are different path structures
							// (one specular vertex vs N), so they never both
							// contribute to the same path. Directional endpoints
							// are solved in direction space, same as the single
							// vertex solver.
							if (mneeMaxSpecular > 1 &&
									MNEEMultiDirectSampling(device, scene, time, pathInfo,
										pathThroughput, bsdf, *light, lightPickPdf, risScale,
										shadowRay, directPdfW, shadowRayHit, shadowBsdf, volInfo,
										u1, u2, u3, u4, sampleResult))
								return ILLUMINATED;
						}

						return SHADOWED;
					}
				}
			}
		}
	}

	return NOT_VISIBLE;
}

bool PathTracer::CheckDirectHitVisibilityFlags(LightSourceConstRef lightSource, const PathDepthInfo &depthInfo,
		const BSDFEvent lastBSDFEvent) const {
	if (depthInfo.depth == 0)
		return true;

	if ((lastBSDFEvent & DIFFUSE) && lightSource.IsVisibleIndirectDiffuse())
		return true;
	if ((lastBSDFEvent & GLOSSY) && lightSource.IsVisibleIndirectGlossy())
		return true;
	if ((lastBSDFEvent & SPECULAR) && lightSource.IsVisibleIndirectSpecular())
		return true;

	return false;
}

// Canonical solid angle subtended by the light at vertex P, for the
// adaptive caustic partition - the CPU port of
// Light_ConnectionSolidAngle() (light_funcs.cl). A pure function of
// (P, light) so eye and light paths classify the same connection
// identically: infinity for environment lights (always easy for the
// eye path), 0 for positional emitters (direct light sampling covers
// them, BSDF sampling can never hit them) and the cone/planar
// subtended angle otherwise.
static float LightConnectionSolidAngle(const LightSource &light,
		const Point &P) {
	switch (light.GetType()) {
		case slg::TYPE_TRIANGLE: {
			const TriangleLight &tl = static_cast<const TriangleLight &>(light);
			const Vector toV = Vector(P) - Vector(tl.worldCentroid);
			const float dist2 = toV.LengthSquared();
			if (dist2 <= 0.f)
				return INFINITY;
			const float cosT = fabsf(Dot(tl.worldGeometryNormal, toV)) / sqrtf(dist2);
			return tl.GetTriangleArea() * cosT / dist2;
		}
		case slg::TYPE_SUN: {
			const SunLight &sl = static_cast<const SunLight &>(light);
			return 2.f * M_PI * (1.f - sl.GetCosThetaMax());
		}
		case slg::TYPE_DISTANT: {
			const DistantLight &dl = static_cast<const DistantLight &>(light);
			return 2.f * M_PI * (1.f - dl.GetCosThetaMax());
		}
		case slg::TYPE_SPHERE:
		case slg::TYPE_MAPSPHERE: {
			const SphereLight &sl = static_cast<const SphereLight &>(light);
			const float dist = Distance(P, sl.GetAbsolutePosition());
			if (dist <= sl.radius)
				return 4.f * M_PI;
			const float sinT = sl.radius / dist;
			return 2.f * M_PI * (1.f - sqrtf(Max(0.f, 1.f - sinT * sinT)));
		}
		case slg::TYPE_IL:
		case slg::TYPE_IL_SKY:
		case slg::TYPE_IL_SKY2:
		case slg::TYPE_IL_CONSTANT:
			return INFINITY;
		default:
			// Point, mappoint, spot, projection, laser, sharpdistant
			return 0.f;
	}
}

void PathTracer::DirectHitFiniteLight(SceneConstRef scene,
		const EyePathInfo &pathInfo,
		const Spectrum &pathThroughput, const Ray &ray,
		const float distance, const BSDF &bsdf,
		SampleResult *sampleResult) const {

	auto lightSource = bsdf.GetLightSource();

	// Check if the light source is visible according the settings
	if (!CheckDirectHitVisibilityFlags(*lightSource, pathInfo.depth, pathInfo.lastBSDFEvent) ||
			// If the material is shadow transparent, Direct Light sampling
			// will take care of transporting all emitted light
			bsdf.hitPoint.throughShadowTransparency)
		return;

	float directPdfA;
	const Spectrum emittedRadiance = bsdf.GetEmittedRadiance(&directPdfA);

	if (!emittedRadiance.Black()) {
		float weight;
		if (!(pathInfo.lastBSDFEvent & SPECULAR)) {
			auto& lightStrategy = scene.GetLightSources().GetIlluminateLightStrategy();
			// RESTIR_DI culls provably-shadowed lights from DL sampling
			// (Stage 2/3 IsAlwaysInShadow). A culled light has NO DL-side
			// coverage, so the direct hit is the sole covering technique
			// and its MIS weight must be 1: weighting it against a DL
			// density that can never produce it drops energy (measured as
			// a clustered -1.7% dark bias on mesh scenes). lastShadeN is
			// exactly the landing normal the DL-side cull used at this
			// vertex (pathinfo.cpp), so the decisions agree exactly.
			// Other strategies never cull: weight unchanged for them.
			if (lightStrategy.GetType() == TYPE_RESTIR_DI &&
					lightSource->IsAlwaysInShadow(scene, ray.o, pathInfo.lastShadeN)) {
				weight = 1.f;
			} else {
				const float lightPickProb = lightStrategy.SampleLightPdf(
					*lightSource,
					ray.o, pathInfo.lastShadeN, pathInfo.lastFromVolume);

			// This is a specific check to avoid fireflies with DLSC
			if ((lightPickProb == 0.f) && lightSource->IsDirectLightSamplingEnabled() &&
					(lightStrategy.GetType() == TYPE_DLS_CACHE))
				return;

			const float directPdfW = PdfAtoW(directPdfA, distance,
					AbsDot(bsdf.hitPoint.fixedDir, bsdf.hitPoint.shadeN));

			// MIS between BSDF sampling and direct light sampling
			weight = PowerHeuristic(pathInfo.lastBSDFPdfW * lightSource->GetAvgPassThroughTransparency(), directPdfW * lightPickProb);
			}
		} else
			weight = 1.f;

		sampleResult->AddEmission(bsdf.GetLightID(), pathThroughput, weight * emittedRadiance);
	}
}

void PathTracer::DirectHitInfiniteLight(SceneConstRef scene,
		const EyePathInfo &pathInfo, const Spectrum &pathThroughput,
		const Ray &ray, const BSDF *bsdf, SampleResult *sampleResult) const {
	// If the material is shadow transparent, Direct Light sampling
	// will take care of transporting all emitted light
	if (bsdf && bsdf->hitPoint.throughShadowTransparency)
		return;

	for(EnvLightSource& envLight: scene.GetLightSources().GetEnvLightSources()) {
		// Check if the light source is visible according the settings
		if (!CheckDirectHitVisibilityFlags(envLight, pathInfo.depth, pathInfo.lastBSDFEvent))
			continue;

		float directPdfW;
		const Spectrum envRadiance = envLight.GetRadiance(scene, bsdf, -ray.d, &directPdfW);
		if (!envRadiance.Black()) {
			float weight;
			if (!(pathInfo.lastBSDFEvent & SPECULAR)) {
				const float lightPickProb = scene.GetLightSources().GetIlluminateLightStrategy().
						SampleLightPdf(envLight, ray.o, pathInfo.lastShadeN, pathInfo.lastFromVolume);

				// MIS between BSDF sampling and direct light sampling
				weight = PowerHeuristic(pathInfo.lastBSDFPdfW, directPdfW * lightPickProb);
			} else
				weight = 1.f;

			sampleResult->AddEmission(envLight.GetID(), pathThroughput, weight * envRadiance);
		}
	}	
}

void PathTracer::GenerateEyeRay(CameraConstRef camera, FilmConstRef film, Ray &eyeRay,
		PathVolumeInfo &volInfo, Sampler& sampler, SampleResult &sampleResult) const {
	const float filmX = sampler.GetSample(0);
	const float filmY = sampler.GetSample(1);

	// Use fast pixel filtering, like the one used in TILEPATH.

	const u_int *subRegion = film.GetSubRegion();
	sampleResult.pixelX = Min(Floor2UInt(filmX), subRegion[1]);
	sampleResult.pixelY = Min(Floor2UInt(filmY), subRegion[3]);
	assert (sampleResult.pixelX >= subRegion[0]);
	assert (sampleResult.pixelX <= subRegion[1]);
	assert (sampleResult.pixelY >= subRegion[2]);
	assert (sampleResult.pixelY <= subRegion[3]);

	const float uSubPixelX = filmX - sampleResult.pixelX;
	const float uSubPixelY = filmY - sampleResult.pixelY;

	// Sample according the pixel filter distribution
	float distX, distY;
	pixelFilterDistribution->SampleContinuous(uSubPixelX, uSubPixelY, &distX, &distY);

	sampleResult.filmX = sampleResult.pixelX + .5f + distX;
	sampleResult.filmY = sampleResult.pixelY + .5f + distY;

	const float timeSample = sampler.GetSample(4);
	const float time = camera.GenerateRayTime(timeSample);

	camera.GenerateRay(time, sampleResult.filmX, sampleResult.filmY, &eyeRay, &volInfo,
		sampler.GetSample(2), sampler.GetSample(3));
}

//------------------------------------------------------------------------------
// RenderEyePath methods
//------------------------------------------------------------------------------

void PathTracer::RenderEyePath(IntersectionDeviceRef device,
		SceneConstRef scene, Sampler& sampler, EyePathInfo &pathInfo,
		Ray &eyeRay,  const luxrays::Spectrum &eyeTroughput,
		vector<SampleResult> &sampleResults) const {
	// To keep track of the number of rays traced
	const double deviceRayCount = device.GetTotalRaysCount();

	// This is used by light strategy
	pathInfo.lastShadeN = Normal(eyeRay.d);

	SampleResult &sampleResult = sampleResults[0];
	bool photonGIShowIndirectPathMixUsed = false;
	bool photonGICausticCacheUsed = false;
	bool photonGICacheEnabledOnLastHit = false;
	float radianceVertStart = 0.f;
	float directVertStart = 0.f;
	bool albedoToDo = true;
	sampleResult.albedo = Spectrum(); // Just in case albedoToDo is never true
	sampleResult.shadingNormal = Normal();
	Spectrum pathThroughput(eyeTroughput);
	BSDF bsdf;
	for (;;) {
		sampleResult.firstPathVertex = (pathInfo.depth.depth == 0);
		// Path guiding (P1-3 M1): snapshot the accumulated radiance so the
		// arrival record below can credit this vertex with its local value
		// (direct light + emission added during this vertex), normalized by
		// the arrival throughput into an incident-radiance estimate. (Plain
		// throughput recording would learn path density, not radiance.)
		radianceVertStart = sampleResult.radiance.Sum().Filter();
		directVertStart = sampleResult.directDiffuseReflect.Filter() +
				sampleResult.directDiffuseTransmit.Filter() +
				sampleResult.directGlossyReflect.Filter() +
				sampleResult.directGlossyTransmit.Filter() +
				sampleResult.emission.Filter();
		const u_int sampleOffset = eyeSampleBootSize + pathInfo.depth.depth * eyeSampleStepSize;

		RayHit eyeRayHit;
		Spectrum connectionThroughput;
		const float passThrough = sampler.GetSample(sampleOffset);
		const bool hit = scene.Intersect(
				IntersectionDevicePtr(&device),
				EYE_RAY | (sampleResult.firstPathVertex ? CAMERA_RAY : INDIRECT_RAY),
				&pathInfo.volume, passThrough,
				&eyeRay, &eyeRayHit, &bsdf, &connectionThroughput,
				&pathThroughput, &sampleResult, false,
				&pathInfo.depth, pathInfo.lastBSDFEvent);
		pathThroughput *= connectionThroughput;
		// Note: pass-through check is done inside Scene::Intersect()

		const bool checkDirectLightHit =
				// Avoid to render caustic path if PhotonGI caustic cache is enabled
				(!photonGICache ||
					photonGICache->IsDirectLightHitVisible(pathInfo, photonGICausticCacheUsed));

		// Hybrid caustic suppression is per-emitter in the adaptive
		// partition: env hits have infinite solid angle so only a delta
		// terminal makes the connection eye-hard
		const bool suppressInfiniteHit = hybridBackForwardEnable &&
				(hybridBackForwardAdaptiveCaustic ?
					pathInfo.IsAdaptiveCausticHitPath(
							hybridBackForwardTerminalGlossiness,
							hybridBackForwardConnectProb, INFINITY) :
					pathInfo.IsCausticPath());

		if (!hit) {
			// Nothing was hit, look for env. lights
			if ((!(forceBlackBackground && pathInfo.isPassThroughPath) || !pathInfo.isPassThroughPath) &&
					checkDirectLightHit && !suppressInfiniteHit) {
				DirectHitInfiniteLight(scene, pathInfo, pathThroughput,
						eyeRay, sampleResult.firstPathVertex ? nullptr : &bsdf,
						&sampleResult);
			}

			if (sampleResult.firstPathVertex) {
				sampleResult.alpha = 0.f;
				sampleResult.depth = numeric_limits<float>::infinity();
				sampleResult.position = Point(
						numeric_limits<float>::infinity(),
						numeric_limits<float>::infinity(),
						numeric_limits<float>::infinity());
				sampleResult.geometryNormal = Normal();
				sampleResult.shadingNormal = Normal();
				sampleResult.materialID = 0;
				sampleResult.objectID = 0;
				sampleResult.uv = UV(numeric_limits<float>::infinity(),
						numeric_limits<float>::infinity());
			} else if (!sampleResult.isHoldout && pathInfo.isTransmittedPath) {
				// I set to 0.0 also the alpha all purely transmitted paths hitting nothing
				sampleResult.alpha = 0.f;
			}
			break;
		}

		// Something was hit

		if (albedoToDo && bsdf.IsAlbedoEndPoint(albedoSpecularSetting, albedoSpecularGlossinessThreshold)) {
			sampleResult.albedo = pathThroughput * bsdf.Albedo();
			sampleResult.shadingNormal = bsdf.hitPoint.shadeN;
			albedoToDo = false;
		}

		if (sampleResult.firstPathVertex) {
			// The alpha value can be changed if the material is a shadow catcher (see below)
			sampleResult.alpha = bsdf.IsHoldout() ? 0.f : 1.f;
			sampleResult.depth = eyeRayHit.t;
			sampleResult.position = bsdf.hitPoint.p;
			sampleResult.geometryNormal = bsdf.hitPoint.geometryN;
			sampleResult.materialID = bsdf.GetMaterialID();
			sampleResult.objectID = bsdf.GetObjectID();
			sampleResult.uv = bsdf.hitPoint.GetUV(0);
			sampleResult.isHoldout = bsdf.IsHoldout();
		}
		sampleResult.lastPathVertex = pathInfo.depth.IsLastPathVertex(maxPathDepth, bsdf.GetEventTypes());

		//----------------------------------------------------------------------
		// Check if it is a baked material
		//----------------------------------------------------------------------

		if (bsdf.HasBakeMap(COMBINED)) {
			sampleResult.radiance[0] += pathThroughput * bsdf.GetBakeMapValue();
			break;
		} else if (bsdf.HasBakeMap(LIGHTMAP)) {
			sampleResult.radiance[0] += pathThroughput * bsdf.Albedo() * bsdf.GetBakeMapValue();
			break;
		}

		//----------------------------------------------------------------------
		// Check if it is a light source and I have to add light emission
		//----------------------------------------------------------------------

		if (bsdf.IsLightSource() && checkDirectLightHit &&
				(!hybridBackForwardEnable ||
				(hybridBackForwardAdaptiveCaustic ?
					// The terminal vertex is the ray origin (it scattered
					// the path into this emitter)
					!pathInfo.IsAdaptiveCausticHitPath(
							hybridBackForwardTerminalGlossiness,
							hybridBackForwardConnectProb,
							LightConnectionSolidAngle(*bsdf.GetLightSource(),
									eyeRay.o)) :
					!pathInfo.IsCausticPath()))) {
			DirectHitFiniteLight(scene, pathInfo, pathThroughput,
					eyeRay, eyeRayHit.t, bsdf, &sampleResult);
		}

		//----------------------------------------------------------------------
		// Check if I can use the photon cache
		//----------------------------------------------------------------------

		if (photonGICache) {
			const bool isPhotonGIEnabled = photonGICache->IsPhotonGIEnabled(bsdf);

			// Check if one of the debug modes is enabled
			if (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECT) {
				if (isPhotonGIEnabled) {
					const SpectrumGroup *group = photonGICache->GetIndirectRadiance(bsdf);
					if (group)
						sampleResult.radiance += *group;
				}
				break;
			} else if (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWCAUSTIC) {
				if (isPhotonGIEnabled)
					sampleResult.radiance += photonGICache->ConnectWithCausticPaths(bsdf);
				break;
			} else if (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECTPATHMIX) {
				// Check if the cache is enabled for this material
				if (isPhotonGIEnabled) {
					if (photonGICacheEnabledOnLastHit &&
							(eyeRayHit.t > photonGICache->GetIndirectUsageThreshold(pathInfo.lastBSDFEvent,
								pathInfo.lastGlossiness,
								passThrough))) {
						sampleResult.radiance[0] = Spectrum(0.f, 0.f, 1.f);
						photonGIShowIndirectPathMixUsed = true;
						break;
					}

					photonGICacheEnabledOnLastHit = true;
				} else
					photonGICacheEnabledOnLastHit = false;
			} else {
				// Check if the cache is enabled for this material
				if (isPhotonGIEnabled) {
					// TODO: add support for AOVs (possible ?)

					if (photonGICache->IsCausticEnabled() && (!hybridBackForwardEnable || pathInfo.depth.depth != 0)) {
						const SpectrumGroup causticRadiance = photonGICache->ConnectWithCausticPaths(bsdf);

						if (!causticRadiance.Black()) {
							sampleResult.radiance.AddWeighted(pathThroughput, causticRadiance);
							photonGICausticCacheUsed = true;
						}
					}

					if (photonGICache->IsIndirectEnabled() && photonGICacheEnabledOnLastHit &&
							(eyeRayHit.t > photonGICache->GetIndirectUsageThreshold(pathInfo.lastBSDFEvent,
								pathInfo.lastGlossiness,
								// I hope to not introduce strange sample correlations
								// by using passThrough here
								passThrough))) {
						const SpectrumGroup *group = photonGICache->GetIndirectRadiance(bsdf);
						if (group)
							sampleResult.radiance.AddWeighted(pathThroughput, *group);
						// I can terminate the path, all done
						break;
					}

					photonGICacheEnabledOnLastHit = true;
				} else
					photonGICacheEnabledOnLastHit = false;
			}
		}

		//------------------------------------------------------------------
		// Direct light sampling
		//------------------------------------------------------------------

		// I avoid to do DL on the last vertex otherwise it introduces a lot of
		// noise because I can not use MIS.
		// I handle as a special case when the path vertex is both the first
		// and the last: I do direct light sampling without MIS.
		if (sampleResult.lastPathVertex && !sampleResult.firstPathVertex)
			break;

		const DirectLightResult directLightResult = DirectLightSampling(
				device, scene,
				eyeRay.time,
				sampler.GetSample(sampleOffset + 1),
				sampler.GetSample(sampleOffset + 2),
				sampler.GetSample(sampleOffset + 3),
				sampler.GetSample(sampleOffset + 4),
				sampler.GetSample(sampleOffset + 5),
				pathInfo, 
				pathThroughput, bsdf, &sampleResult);

		if (sampleResult.lastPathVertex)
			break;

		//------------------------------------------------------------------
		// Build the next vertex path ray
		//------------------------------------------------------------------

		Vector sampledDir;
		float cosSampledDir;
		Spectrum bsdfSample;
		float bsdfPdfW;
		BSDFEvent bsdfEvent;
		if (bsdf.IsShadowCatcher() && (directLightResult != SHADOWED)) {
			bsdfSample = bsdf.ShadowCatcherSample(&sampledDir, &bsdfPdfW, &cosSampledDir, &bsdfEvent);

			if (sampleResult.firstPathVertex) {
				// In this case I have also to set the value of the alpha channel to 0.0
				sampleResult.alpha = 0.f;
			}
		} else {
			const Spectrum &shadowTransparency = bsdf.GetPassThroughShadowTransparency();
			if (!sampleResult.firstPathVertex && !shadowTransparency.Black() && !pathInfo.IsSpecularPath()) {
				sampledDir = -bsdf.hitPoint.fixedDir;
				bsdfSample = shadowTransparency;
				bsdfPdfW = pathInfo.lastBSDFPdfW;
				cosSampledDir = -1.f;
				bsdfEvent = pathInfo.lastBSDFEvent;
			} else {
				// ReSTIR GI (G1): at a depth-0 non-delta vertex the
				// first-bounce continuation is resampled from the
				// per-pixel reservoir + K fresh candidates. On success
				// the winner's direction replaces the BSDF draw below;
				// outEval already carries the RIS weight W and outPdfW
				// is the marginal selection density (1/W) used for MIS
				// bookkeeping at later vertices and env hits.
				bool giSelected = false;
				if (restirGI && restirGIEnable &&
						sampleResult.firstPathVertex && !bsdf.IsDelta()) {
					Vector giDir;
					float giPdfW;
					BSDFEvent giEvent;
					Spectrum giEval;
					if (restirGI->ResampleFirstBounce(device, scene,
							eyeRay.time, bsdf, pathInfo.volume,
							bsdf.hitPoint.p,
							sampleResult.pixelX, sampleResult.pixelY,
							sampler.GetPass(), restirGICandidates,
							restirGITemporalEnable, restirGISpatialEnable,
							&giDir, &giEval, &giPdfW, &giEvent)) {
						sampledDir = giDir;
						bsdfSample = giEval;
						bsdfPdfW = giPdfW;
						cosSampledDir = fabsf(Dot(bsdf.hitPoint.shadeN, giDir));
						bsdfEvent = giEvent;
						giSelected = true;
					}
				}
				// Path guiding (P1-3 M1, CPU only): train on every
				// non-delta arrival, but guide glossy bounces only.
				// Rationale: diffuse cosine-BSDF sampling is already
				// near-optimal, so a blunt fixed-grid guide can only add
				// noise there; glossy BSDF sampling is poor and benefits.
				// One-sample MIS with randomized 50/50 technique selection:
				// u6 doubles as the selector (rescaled afterwards to a
				// uniform draw, so no new sample dimensions); the mixture
				// pdf in the denominator keeps every branch unbiased.
				// (A deterministic selection with mixture weights would be
				// biased: the weights must match the selection probabilities.)
				// NOTE: the books (bsdfEvent for depth counting) come from
				// a shadow BSDF draw with the same uniforms, not from
				// Evaluate's event superset (which would consume specular
				// depth on every guided bounce and terminate paths early).
				bool guided = false;
				const bool tryGuide = pathGuidingCache && !bsdf.IsDelta() &&
						GuidableBsdf(bsdf) &&
						((int)pathInfo.depth.depth >= GuidingMinDepth()) &&
						pathGuidingCache->CanGuide(bsdf.hitPoint.p);
				const float uSelRaw = sampler.GetSample(sampleOffset + 6);
				// M2c adaptive mixture: selection probability from the
				// read-side (frozen-in-round) cell total, so bounce-time
				// and DL-time weights agree. Any w in (0,1) is exact.
				const float wGuide = (guidingEnable && tryGuide) ?
						PathGuidingCache::MixWeight(
							pathGuidingCache->ReadTotal(bsdf.hitPoint.p)) : .5f;
				const bool takeGuideSide = (uSelRaw < wGuide);
				// Both mixture sides must rescale the selector to a full
				// [0,1) conditional uniform. Reusing the raw draw on the
				// BSDF side would restrict it to [0,.5) (!takeGuideSide
				// conditions uSelRaw<.5 for stateless samplers), silently
				// replacing the BSDF density with its low-u0 half (bias).
				const float uSelRescaled = takeGuideSide ?
						uSelRaw / Max(wGuide, 1e-6f) :
						(uSelRaw - wGuide) / Max(1.f - wGuide, 1e-6f);
				if (pathGuidingCache && !bsdf.IsDelta()) {
					// Incident-radiance training target: local value added
					// at this vertex (direct light + emission since vertex
					// start), divided by the arrival throughput. Unbiased
					// incident estimate; Record clamps fireflies.
					// M2c (env LUX_PG_INDIRECT=1): train on indirect only
					// (total minus direct light added this vertex). DL
					// covers direct better than any guide; the guide's
					// headroom is indirect transport, and a direct-peak
					// field only duplicates DL's job at 50% cost.
					const float arrival = Max(pathThroughput.Filter(), 1e-3f);
					float localValue = sampleResult.radiance.Sum().Filter() - radianceVertStart;
					if (GuidingIndirect()) {
						const float dlAdded = sampleResult.directDiffuseReflect.Filter() +
								sampleResult.directDiffuseTransmit.Filter() +
								sampleResult.directGlossyReflect.Filter() +
								sampleResult.directGlossyTransmit.Filter() +
								sampleResult.emission.Filter() - directVertStart;
						localValue -= dlAdded;
					}
					pathGuidingCache->Record(bsdf.hitPoint.p, -eyeRay.d,
							localValue / arrival);
					if (guidingEnable && tryGuide && takeGuideSide && !giSelected) {
						float guidePdfW;
						Vector guideDir;
						// Sample() is total under tryGuide (table miss falls
						// back to cosine sampling inside), so the guide side
						// always draws from a valid distribution with exact
						// pdf: the one-sample MIS stays exact. Falling back
						// to a BSDF resample here (under mixture weights)
						// would be biased; a zero-contribution guide draw
						// kills the path instead (also unbiased).
						// Independent bin pick (see GuidingHash note above).
						// M2c E4 (env LUX_PG_UBIN=9): stratified Sobol dim 9
						// instead of the hash (dim 9 is padding inside the
						// 10-dim eye step; dims 0-8 are taken). Stratified
						// across pixels, still an independent dimension
						// from the selector (dim 6). Default stays hashed
						// (pass-decorrelated) until measured better.
						static const bool kStratBin = (getenv("LUX_PG_UBIN") != nullptr);
						const float uBin = kStratBin ?
							sampler.GetSample(sampleOffset + 9) : GuidingHash(
								(sampleResult.pixelX * 73856093u) ^
								(sampleResult.pixelY * 19349663u) ^
								(sampler.GetPass() * 83492791u) ^
								(sampleOffset * 2971215073u)) *
								(1.f / 4294967296.f);
						if (pathGuidingCache->Sample(bsdf.hitPoint.p,
								bsdf.hitPoint.shadeN,
								uBin,
								uSelRescaled,
								sampler.GetSample(sampleOffset + 7),
								&guideDir, &guidePdfW) && (guidePdfW > 0.f)) {
							// Shadow BSDF draw with the same uniforms: only
							// its event is kept (single-lobe books for depth
							// counting); the direction/pdf are the guide's.
							// (If the shadow draw absorbs, fall back to the
							// Evaluate event.)
							Vector discardDir;
							float discardPdfW, discardCos;
							BSDFEvent shadowEvent = (BSDFEvent)0;
							const Spectrum discardEval = bsdf.Sample(&discardDir,
									uSelRescaled,
									sampler.GetSample(sampleOffset + 7),
									&discardPdfW, &discardCos, &shadowEvent);
							BSDFEvent guideEvent;
							float guideBsdfPdfW, guideReversePdfW;
							const Spectrum guideEvalDouble = bsdf.Evaluate(guideDir,
									&guideEvent, &guideBsdfPdfW, &guideReversePdfW);
							// DisneyMaterial::Evaluate double-counts the cosine
							// (its DisneyEvaluate already includes |cos| and the
							// wrapper multiplies by |cos| again, unlike Sample
							// which is single-cos like the other materials'
							// Evaluate). Reduce to the single-cos numerator the
							// mixture needs (upstream DL quirk, out of scope).
							const float cosLocal = fabsf(bsdf.GetFrame().ToLocal(guideDir).z);
							const Spectrum guideEval = (cosLocal > 1e-3f) ?
									guideEvalDouble / cosLocal : Spectrum();
							if (!guideEval.Black()) {
								// bsdfEval already holds f * cos (like the
								// BSDF branch factor); divide by the mixture.
								const float mixPdfW = (1.f - wGuide) * guideBsdfPdfW + wGuide * guidePdfW;
								if (mixPdfW > 0.f) {
									sampledDir = guideDir;
									bsdfSample = guideEval / mixPdfW;
									bsdfPdfW = mixPdfW;
									cosSampledDir = fabsf(Dot(bsdf.hitPoint.shadeN, sampledDir));
									bsdfEvent = discardEval.Black() ? guideEvent : shadowEvent;
									guided = true;
								} else {
									// Zero density under both techniques:
									// the draw contributes nothing; kill the
									// path (unbiased, keeps MIS exact).
									guided = true;
									bsdfSample = Spectrum();
								}
							} else {
								// Valid guide draw, zero BSDF contribution
								// (e.g. across a shading/geometry normal
								// side): the path contributes nothing here.
								// Kill it instead of resampling the BSDF
								// under mixture weights (biased).
								guided = true;
								bsdfSample = Spectrum();
							}
						} else {
							// Unreachable under tryGuide (Sample is total
							// there); kill the path rather than resample.
							guided = true;
							bsdfSample = Spectrum();
						}
					}
				}
				if (!guided && !giSelected) {
					// Inside the mixture (tryGuide) either side uses the
					// rescaled conditional uniform (a full [0,1) uniform
					// given the selector outcome); elsewhere the raw draw
					// keeps stock sampler behavior bit-for-bit.
					const float uBsdf = (guidingEnable && tryGuide) ?
							uSelRescaled : sampler.GetSample(sampleOffset + 6);
					bsdfSample = bsdf.Sample(&sampledDir,
							uBsdf,
							sampler.GetSample(sampleOffset + 7),
							&bsdfPdfW, &cosSampledDir, &bsdfEvent);
					if (guidingEnable && tryGuide) {
						// Every BSDF-side sample under tryGuide (whichever
						// way the selector fell) must be reweighted to the
						// mixture: one-sample MIS divides by the marginal
						// sampling density on both sides. Gating this on
						// takeGuideSide instead leaves the !take side at
						// full BSDF weight (bias).
						const float guidePdfW = pathGuidingCache->Pdf(
								bsdf.hitPoint.p, bsdf.hitPoint.shadeN, sampledDir);
						const float mixPdfW = .5f * bsdfPdfW + .5f * guidePdfW;
						if (mixPdfW > 0.f) {
							// bsdfSample here holds f * cos / bsdfPdfW
							// (material convention); reweight to the mixture.
							bsdfSample *= bsdfPdfW / mixPdfW;
							bsdfPdfW = mixPdfW;
						}
					}
				}
				pathInfo.isPassThroughPath = false;
			}
		}

		verify (!bsdfSample.IsNaN() && !bsdfSample.IsInf() && !bsdfSample.IsNeg());
		if (bsdfSample.Black())
			break;

		if (sampleResult.firstPathVertex)
			sampleResult.firstPathVertexEvent = bsdfEvent;

		pathInfo.AddVertex(bsdf, bsdfEvent, bsdfPdfW, hybridBackForwardGlossinessThreshold);

		// Russian Roulette
		float rrProb = 1.f;
		if (pathInfo.UseRR(rrDepth)) {
			 rrProb = RenderEngine::RussianRouletteProb(bsdfSample, rrImportanceCap);
			if (rrProb < sampler.GetSample(sampleOffset + 8))
				break;

			// Increase path contribution
			bsdfSample /= rrProb;
		}

		pathThroughput *= bsdfSample;
		verify (!pathThroughput.IsNaN() && !pathThroughput.IsInf());

		// This is valid for irradiance AOV only if it is not a SPECULAR material and
		// first path vertex. Set or update sampleResult.irradiancePathThroughput
		if (sampleResult.firstPathVertex) {
			if (!(bsdf.GetEventTypes() & SPECULAR))
				sampleResult.irradiancePathThroughput = INV_PI * AbsDot(bsdf.hitPoint.shadeN, sampledDir) / rrProb;
			else
				sampleResult.irradiancePathThroughput = Spectrum();
		} else
			sampleResult.irradiancePathThroughput *= bsdfSample;

		eyeRay.Update(bsdf.GetRayOrigin(sampledDir), sampledDir);
	}

	sampleResult.rayCount += static_cast<float>(device.GetTotalRaysCount() - deviceRayCount);

	if (sampleResult.isHoldout) {
		sampleResult.radiance.Clear();
		sampleResult.albedo = Spectrum();
	}

	if (photonGICache && (photonGICache->GetDebugType() == PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECTPATHMIX) &&
			!photonGIShowIndirectPathMixUsed)
		sampleResult.radiance[0] = Spectrum(1.f, 0.f, 0.f);
}

//------------------------------------------------------------------------------
// RenderEyeSample
//------------------------------------------------------------------------------

// Project every spectral color field of a SampleResult (wavelength bins)
// back to film RGB under the CIE matching functions. Data fields (positions,
// normals, IDs, alpha, masks) are left untouched.
static void ProjectSampleResultToRGB(SampleResult &sr, const PathWavelengths &sw) {
	for (u_int i = 0; i < sr.radiance.Size(); ++i)
		sr.radiance[i] = Spectral::ProjectToRGB(sr.radiance[i], sw);
	sr.directDiffuseReflect = Spectral::ProjectToRGB(sr.directDiffuseReflect, sw);
	sr.directDiffuseTransmit = Spectral::ProjectToRGB(sr.directDiffuseTransmit, sw);
	sr.directGlossyReflect = Spectral::ProjectToRGB(sr.directGlossyReflect, sw);
	sr.directGlossyTransmit = Spectral::ProjectToRGB(sr.directGlossyTransmit, sw);
	sr.emission = Spectral::ProjectToRGB(sr.emission, sw);
	sr.indirectDiffuseReflect = Spectral::ProjectToRGB(sr.indirectDiffuseReflect, sw);
	sr.indirectDiffuseTransmit = Spectral::ProjectToRGB(sr.indirectDiffuseTransmit, sw);
	sr.indirectGlossyReflect = Spectral::ProjectToRGB(sr.indirectGlossyReflect, sw);
	sr.indirectGlossyTransmit = Spectral::ProjectToRGB(sr.indirectGlossyTransmit, sw);
	sr.indirectSpecularReflect = Spectral::ProjectToRGB(sr.indirectSpecularReflect, sw);
	sr.indirectSpecularTransmit = Spectral::ProjectToRGB(sr.indirectSpecularTransmit, sw);
	sr.irradiance = Spectral::ProjectToRGB(sr.irradiance, sw);
	sr.irradiancePathThroughput = Spectral::ProjectToRGB(sr.irradiancePathThroughput, sw);
	sr.albedo = Spectral::ProjectToRGB(sr.albedo, sw);
}

void PathTracer::RenderEyeSample(
	IntersectionDeviceRef device,
	SceneConstRef scene, FilmConstRef film,
	Sampler& sampler,
	vector<SampleResult> &sampleResults
) const {
	ResetEyeSampleResults(sampleResults);

	// Spectral transport: draw the path wavelengths (the extra boot
	// dimension allocated by ParseOptions) and activate them for the
	// duration of the path on this thread
	PathWavelengths sw;
	if (spectralEnable)
		sw.Sample(sampler.GetSample(eyeSampleBootSize - 1));
	const Spectral::ScopeWavelengths wlScope(sw);

	EyePathInfo pathInfo;
	Ray eyeRay;
	GenerateEyeRay(scene.GetCamera(), film, eyeRay, pathInfo.volume, sampler, sampleResults[0]);

	RenderEyePath(device, scene, sampler, pathInfo, eyeRay, Spectrum(1.f), sampleResults);

	if (wlScope.Active()) {
		for (auto &sr : sampleResults)
			ProjectSampleResultToRGB(sr, sw);
	}
}

//------------------------------------------------------------------------------
// RenderLightSample methods
//------------------------------------------------------------------------------

SampleResult &PathTracer::AddLightSampleResult(vector<SampleResult> &sampleResults,
		FilmConstRef film) {
	const u_int size = sampleResults.size();
	sampleResults.resize(size + 1);

	SampleResult &sampleResult = sampleResults[size];
	sampleResult.Init(&lightSampleResultsChannels, film.GetRadianceGroupCount());

	return sampleResult;
}

void PathTracer::ConnectToEye(IntersectionDeviceRef device,
		SceneConstRef scene,
		FilmConstRef film, const float time,
		const float u0, const float u1, const float u2,
		const LightSource &light, const BSDF &bsdf, 
		const Spectrum &flux, const LightPathInfo &pathInfo,
		vector<SampleResult> &sampleResults) const {
	// I don't connect camera invisible objects with the eye
	if (bsdf.IsCameraInvisible() || bsdf.IsDelta())
		return;

	float filmX, filmY;
	bool sampleSuccess;
	Ray eyeRay;

	Vector eyeDir;
	float eyeDistance = 0;
	Point lensPoint = pathInfo.lensPoint;
    if (scene.GetCamera().GetType() == Camera::ORTHOGRAPHIC){
		// Orthographic camera need to be handled separately,
		// lensPoint can not be pre-calculated in this case
		Point p = bsdf.hitPoint.p;
		eyeDir = scene.GetCamera().GetDir();
		// calculate distance from vertex to camera plane
		const float D = -eyeDir.x*lensPoint.x - eyeDir.y*lensPoint.y - eyeDir.z*lensPoint.z;
		eyeDistance = eyeDir.x*p.x + eyeDir.y*p.y + eyeDir.z*p.z + D;
		eyeDistance = fabsf(eyeDistance);

		eyeRay = Ray(bsdf.hitPoint.p, eyeDir,
			0.f,
			eyeDistance,
			time);
		sampleSuccess = scene.GetCamera().ProjectToImage(&eyeRay, &filmX, &filmY);
	} else {
		eyeDir = Vector(bsdf.hitPoint.p - lensPoint);
		eyeDistance = eyeDir.Length();
		eyeDir /= eyeDistance;

		eyeRay = Ray(lensPoint, eyeDir,
			0.f,
			eyeDistance,
			time);
		sampleSuccess = scene.GetCamera().GetSamplePosition(&eyeRay, &filmX, &filmY);
	}

	if (sampleSuccess) {
		BSDFEvent event;
		const Spectrum bsdfEval = bsdf.Evaluate(-eyeDir, &event);

		// The visibility ray is needed even when the straight-line
		// receiver eval is black: a surface whose normal faces away from
		// the lens can still be reached through a specular interface
		// (e.g. a table top seen only through a glass sphere). LMNEE
		// evaluates the receiver toward the solved vertex, not the lens.
		if (!bsdfEval.Black() || mneeEnable) {
			// I have to flip the direction of the traced ray because
			// the information inside PathVolumeInfo are about the path from
			// the light toward the camera (i.e. ray.o would be in the wrong
			// place).
			Ray traceRay(bsdf.GetRayOrigin(-eyeRay.d), -eyeRay.d,
					eyeDistance - eyeRay.maxt,
					eyeDistance - eyeRay.mint,
					time);
			traceRay.UpdateMinMaxWithEpsilon();
			RayHit traceRayHit;

			BSDF bsdfConn;
			Spectrum connectionThroughput;
			// Create a new PathVolumeInfo for the path to the light source
			PathVolumeInfo volInfo = pathInfo.volume;
			// Copy of the light path depth info: the connection ray may
			// cross transparent surfaces (transparentDepth is updated by
			// Scene::Intersect()) but the change must not leak into the
			// light path state
			PathDepthInfo connDepthInfo = pathInfo.depth;
			// SHADOW_RAY lets the connection ray pass through
			// transparency.shadow materials (e.g. a refractive enclosure
			// around the vertex), mirroring eye-path shadow rays
			if (!scene.Intersect(
					luxrays::make_observer<IntersectionDevice>(device),
					LIGHT_RAY | CAMERA_RAY | SHADOW_RAY,
					&volInfo, u0, &traceRay, &traceRayHit, &bsdfConn,
					&connectionThroughput, nullptr, nullptr, false,
					&connDepthInfo, NONE)) {
				// Nothing was hit, the light path vertex is visible
				if (!bsdfEval.Black()) {

				float fluxToRadianceFactor;
				scene.GetCamera().GetPDF(eyeRay, eyeDistance, filmX, filmY, nullptr, &fluxToRadianceFactor);

				SampleResult &sampleResult = AddLightSampleResult(sampleResults, film);
				sampleResult.filmX = filmX;
				sampleResult.filmY = filmY;

				sampleResult.pixelX = Floor2UInt(filmX);
				sampleResult.pixelY = Floor2UInt(filmY);

#if !defined(NDEBUG)
				const u_int *subRegion = film.GetSubRegion();
#endif
				assert (sampleResult.pixelX >= subRegion[0]);
				assert (sampleResult.pixelX <= subRegion[1]);
				assert (sampleResult.pixelY >= subRegion[2]);
				assert (sampleResult.pixelY <= subRegion[3]);

				sampleResult.isCaustic = hybridBackForwardAdaptiveCaustic ?
						pathInfo.IsAdaptiveCausticPath(event,
								hybridBackForwardTerminalGlossiness,
								hybridBackForwardConnectProb,
								LightConnectionSolidAngle(light,
										pathInfo.firstVertexP)) :
						pathInfo.IsCausticPath(event, bsdf.GetGlossiness(),
								hybridBackForwardGlossinessThreshold);

				// Add radiance from the light source
				sampleResult.radiance[light.GetID()] = connectionThroughput * flux * fluxToRadianceFactor * bsdfEval;
				}
			} else {
				if (mneeEnable && !bsdfConn.IsVolume() &&
					bsdfConn.IsDelta() &&
					(bsdfConn.GetEventTypes() & SPECULAR)) {
				// The connect was blocked by a delta specular surface.
				// LMNEE: solve the specular manifold x0 -> x1 -> lens and
				// splat the refracted/reflected contribution (the CPU port
				// of the GPU LMnee_* driver; the plain estimator is 0 on
				// these paths, so the estimators are disjoint and no MIS
				// is needed). The solver starts from the clean path volume
				// state, not volInfo which the connect march mutated.
				PathVolumeInfo mneeVolInfo = pathInfo.volume;
				BSDF warmV0, warmV1;
				bool warmOk = false;
				if (!LMNEEConnectToEye(device, scene, film, time, light,
						bsdf, flux, pathInfo, traceRayHit, bsdfConn,
						mneeVolInfo, warmV0, warmV1, warmOk,
						sampleResults) && (mneeMaxSpecular > 1)) {
					// The single vertex solve found no solution. When it
					// converged but its second segment re-blocked, warmV0/
					// warmV1 carry the solved vertex + the re-blocker: a
					// consistent seed pair on the solved ray's path.
					PathVolumeInfo mneeChainVolInfo = pathInfo.volume;
					LMNEEMultiConnectToEye(device, scene, film, time, light,
							bsdf, flux, pathInfo, bsdfConn, mneeChainVolInfo,
							warmOk ? &warmV0 : nullptr,
							warmOk ? &warmV1 : nullptr,
							sampleResults);
				}
			}
		}
	}
	}
}

//------------------------------------------------------------------------------
// Caustic focus cache (CPU side)
//
// CPU port of the GPU lightFocus/lightFocusCount rings
// (pathoclbase_kernels_micro.cl): each light keeps a ring of the last
// lightFocusK world-space targets that produced a screen contribution
// through a delta interface. Emission is steered toward a remembered
// hotspot with probability lightFocusRatio, weighted by the one-sample
// mixture pdf (1-g)*native + g*aim over BOTH branches so the estimator
// stays unbiased. The aim radius of a new entry is its distance to the
// nearest existing target, clamped to [focusRadiusFrac, 1] * worldRadius.
//------------------------------------------------------------------------------

void PathTracer::LightFocusEnsureInit(SceneConstRef scene) const {
	std::call_once(lightFocusInitOnce, [&]() {
		lightFocusLightCount = scene.GetLightSources().GetSize();
		lightFocusTable.reset(new LightFocusEntry[lightFocusLightCount * lightFocusK]);
		lightFocusCounts.reset(new std::atomic<u_int>[lightFocusLightCount]);
		for (u_int i = 0; i < lightFocusLightCount; ++i)
			lightFocusCounts[i].store(0u, std::memory_order_relaxed);

		// Delta-specular caster bounding spheres for distant-light
		// caustic focusing: every object able to produce a delta
		// specular event (glass, mirror, ...) is a potential caustic
		// caster for a directional light.
		for (u_int i = 0; i < scene.GetObjects().GetSize(); ++i) {
			SceneObjectConstRef obj = scene.GetObjects().GetSceneObject(i);
			MaterialConstRef mat = obj.GetMaterial();
			if (!mat.IsDelta() || !(mat.GetEventTypes() & SPECULAR))
				continue;
			const BBox &bb = obj.GetExtMesh().GetBBox();
			const Point c = (bb.pMin + bb.pMax) * .5f;
			lightFocusCasterCenters.push_back(c);
			lightFocusCasterRadii.push_back((bb.pMax - c).Length());
		}
	});
}

void PathTracer::LightFocusCredit(SceneConstRef scene, const u_int lightIndex,
		const Point &p) const {
	if (!lightFocusEnable)
		return;
	// Cheap after the first call; keeps the table pointer write
	// sequenced-before any ring access on every thread
	LightFocusEnsureInit(scene);
	if (lightIndex >= lightFocusLightCount)
		return;

	const float worldRadius = scene.GetDataSet().GetBSphere().rad;
	const float minR = lightFocusRadiusFrac * worldRadius;
	const float maxR = worldRadius;

	LightFocusEntry *ring = &lightFocusTable[lightIndex * lightFocusK];
	const u_int cursor = lightFocusCounts[lightIndex].fetch_add(1u,
			std::memory_order_relaxed);
	const u_int nValid = Min(cursor, lightFocusK);

	// Aim radius = distance to the nearest existing target (GPU
	// FocusAimRadius). A torn read yields NaN; fold it to the broad cap.
	float nn2 = numeric_limits<float>::infinity();
	for (u_int e = 0; e < nValid; ++e) {
		const float dx = ring[e].x.load(std::memory_order_relaxed) - p.x;
		const float dy = ring[e].y.load(std::memory_order_relaxed) - p.y;
		const float dz = ring[e].z.load(std::memory_order_relaxed) - p.z;
		nn2 = Min(nn2, dx * dx + dy * dy + dz * dz);
	}
	const float r = sqrtf(nn2);
	const float aimR = (r == r) ? Clamp(r, minR, maxR) : maxR;

	LightFocusEntry &slot = ring[cursor % lightFocusK];
	slot.x.store(p.x, std::memory_order_relaxed);
	slot.y.store(p.y, std::memory_order_relaxed);
	slot.z.store(p.z, std::memory_order_relaxed);
	slot.aimR.store(aimR, std::memory_order_relaxed);
}

// SpotLight::LocalFalloff twin (spotlight.cpp keeps it file-static)
static float LightFocusSpotFalloff(const Vector &w,
		const float cosTotalWidth, const float cosFalloffStart) {
	const float cosT = CosTheta(w);
	if (cosT < cosTotalWidth)
		return 0.f;
	if (cosT > cosFalloffStart)
		return 1.f;
	const float delta = (cosT - cosTotalWidth) / (cosFalloffStart - cosTotalWidth);
	return powf(delta, 4);
}

void PathTracer::LightFocusEmit(SceneConstRef scene, const LightSource &light,
		Sampler &sampler, const float time,
		Ray &ray, Spectrum &flux, float &emissionPdfW) const {
	if (!lightFocusEnable)
		return;
	LightFocusEnsureInit(scene);

	const u_int lightIndex = light.lightSceneIndex;
	if (lightIndex >= lightFocusLightCount)
		return;
	const LightSourceType lightType = light.GetType();
	// Distant-family lights have a (nearly) fixed direction, so focusing
	// cannot steer it; it acts on the ray ORIGIN instead. Native emission
	// spreads origins uniformly over the whole scene disc, while only the
	// fraction crossing a delta-specular caster produces caustics.
	if ((lightType == TYPE_DISTANT) || (lightType == TYPE_SHARPDISTANT)) {
		LightFocusEmitDistant(scene, light, sampler, time, ray, emissionPdfW);
		return;
	}

	const u_int focusN = Min(lightFocusCounts[lightIndex].load(
			std::memory_order_relaxed), lightFocusK);
	// Positional emitters only (GPU parity): re-aiming the direction of
	// an area emitter needs the emission frame at the sampled point,
	// handled on the GPU side; distant/env lights have no steerable
	// origin either.
	if ((focusN == 0) || ((lightType != TYPE_POINT) && (lightType != TYPE_SPOT)))
		return;

	const float g = lightFocusRatio;
	const float worldRadius = scene.GetDataSet().GetBSphere().rad;
	// Focus dims sit right after the 9 fixed boot dims (the spectral
	// wavelength draw keeps occupying the last boot slot)
	const float uCoin = sampler.GetSample(9);
	const float uSlot = sampler.GetSample(10);
	const float uCone0 = sampler.GetSample(11);
	const float uCone1 = sampler.GetSample(12);

	LightFocusEntry *ring = &lightFocusTable[lightIndex * lightFocusK];

	float nativePdf = 0.f;
	if (lightType == TYPE_POINT)
		nativePdf = UniformSpherePdf();
	// SPOT native pdf is evaluated below from the local direction

	const Point &rayOrig = ray.o;
	if (uCoin < g) {
		// Re-aim the emitted direction at a remembered hotspot: uniform
		// slot pick + uniform cone sample inside its aim radius
		const u_int slot = Min((u_int)(uSlot * focusN), focusN - 1);
		const LightFocusEntry &hp = ring[slot];
		const Point target(hp.x.load(std::memory_order_relaxed),
				hp.y.load(std::memory_order_relaxed),
				hp.z.load(std::memory_order_relaxed));
		const float aimR = hp.aimR.load(std::memory_order_relaxed);
		const Vector toTarget = target - rayOrig;
		const float targetDist = toTarget.Length();
		if (targetDist > aimR) {
			const float sinMax = aimR / targetDist;
			const float cosMax = sqrtf(Max(0.f, 1.f - sinMax * sinMax));
			const Vector axis = toTarget / targetDist;
			Vector axX, axY;
			CoordinateSystem(axis, &axX, &axY);
			const Vector newDir = UniformSampleCone(uCone0, uCone1,
					cosMax, axX, axY, axis);
			ray.d = newDir;

			// Recompute the emission flux for the redirected ray
			if (lightType == TYPE_POINT) {
				float ef[3];
				static_cast<const PointLight &>(light).
						GetPreprocessedData(nullptr, nullptr, ef);
				flux = Spectral::Emission(Spectrum(ef[0], ef[1], ef[2])) *
						(1.f / (4.f * M_PI));
			} else {
				float ef[3], cosTotalWidth, cosFalloffStart;
				const Transform *l2w;
				static_cast<const SpotLight &>(light).GetPreprocessedData(
						ef, nullptr, &cosTotalWidth, &cosFalloffStart, &l2w);
				const Vector localDir = Normalize(Inverse(*l2w) * newDir);
				// Outside the cone the falloff is 0 and the path dies,
				// exactly like the GPU's leaked rim sample
				flux = Spectral::Emission(Spectrum(ef[0], ef[1], ef[2])) *
						(LightFocusSpotFalloff(localDir, cosTotalWidth,
						cosFalloffStart) / fabsf(CosTheta(localDir)));
			}
		}
	}

	// Mixture pdf of the final direction over both strategies (the aim
	// strategy covers every slot whose cone contains the direction;
	// degenerate slots contribute the native density, GPU parity)
	const Vector emitDir = Normalize(ray.d);
	if (lightType == TYPE_SPOT) {
		float cosTotalWidth, cosFalloffStart;
		const Transform *l2w;
		static_cast<const SpotLight &>(light).GetPreprocessedData(
				nullptr, nullptr, &cosTotalWidth, &cosFalloffStart, &l2w);
		const Vector localDir = Normalize(Inverse(*l2w) * emitDir);
		nativePdf = (CosTheta(localDir) >= cosTotalWidth) ?
				UniformConePdf(cosTotalWidth) : 0.f;
	}
	float aimPdf = 0.f;
	for (u_int k = 0; k < focusN; ++k) {
		const LightFocusEntry &hk = ring[k];
		const Vector tk = Point(hk.x.load(std::memory_order_relaxed),
				hk.y.load(std::memory_order_relaxed),
				hk.z.load(std::memory_order_relaxed)) - rayOrig;
		const float aimR = hk.aimR.load(std::memory_order_relaxed);
		const float dk = tk.Length();
		if (dk > aimR) {
			const float sk = aimR / dk;
			const float ck = sqrtf(Max(0.f, 1.f - sk * sk));
			if (Dot(emitDir, tk / dk) >= ck)
				aimPdf += UniformConePdf(ck);
		} else
			aimPdf += nativePdf;
	}
	aimPdf /= focusN;

	emissionPdfW = (1.f - g) * nativePdf + g * aimPdf;
}

void PathTracer::LightFocusEmitDistant(SceneConstRef scene,
		const LightSource &light, Sampler &sampler, const float time,
		Ray &ray, float &emissionPdfW) const {
	const u_int casterN = lightFocusCasterCenters.size();
	if (casterN == 0)
		return;

	// Emit-plane frame of the light (the direction itself is not steered)
	Vector dirV, ax, ay;
	float ld[3], lx[3], ly3[3];
	if (light.GetType() == TYPE_SHARPDISTANT)
		static_cast<const SharpDistantLight &>(light).GetPreprocessedData(
				ld, lx, ly3);
	else
		static_cast<const DistantLight &>(light).GetPreprocessedData(
				ld, lx, ly3, nullptr, nullptr);
	dirV = Vector(ld[0], ld[1], ld[2]);
	ax = Vector(lx[0], lx[1], lx[2]);
	ay = Vector(ly3[0], ly3[1], ly3[2]);

	const Point worldCenter = scene.GetDataSet().GetBSphere().center;
	const float envR = InfiniteLightSource::GetEnvRadius(scene);
	const float invR = 1.f / envR;

	// Emit-plane coordinates are unit-disc: a caster sphere (C, r)
	// projects to disc center proj(C)/envR with radius r/envR.
	float sumR2 = 0.f;
	for (u_int i = 0; i < casterN; ++i) {
		const float r = lightFocusCasterRadii[i];
		sumR2 += r * r;
	}
	if (sumR2 <= 0.f)
		return;

	const float g = lightFocusRatio;
	const float uCoin = sampler.GetSample(9);
	const float uSlot = sampler.GetSample(10);
	const float uCone0 = sampler.GetSample(11);
	const float uCone1 = sampler.GetSample(12);

	if (uCoin < g) {
		// Pick a caster proportional to its disc area r^2, then sample a
		// point of its projected disc
		float t = uSlot * sumR2;
		u_int i = 0;
		for (; i + 1 < casterN; ++i) {
			const float w = lightFocusCasterRadii[i] *
					lightFocusCasterRadii[i];
			if (t < w)
				break;
			t -= w;
		}
		float dd1, dd2;
		ConcentricSampleDisk(uCone0, uCone1, &dd1, &dd2);
		const float rho = lightFocusCasterRadii[i] * invR;
		const Vector oc = lightFocusCasterCenters[i] - worldCenter;
		// Emit-plane disc coords map to the perpendicular offset with a
		// negative sign: ray.o = wc - R*(dir + d1*x + d2*y), so the caster
		// disc center sits at -perp(C - wc)/R.
		const float s1 = -Dot(oc, ax) * invR + rho * dd1;
		const float s2 = -Dot(oc, ay) * invR + rho * dd2;
		ray.Update(worldCenter - envR * (dirV + s1 * ax + s2 * ay),
				ray.d, ray.time);
	}

	// Exact mixture pdf on the final origin's emit-plane coordinates.
	// The area pdf of the native strategy is 1/(pi*envR^2) inside the
	// unit disc; each caster disc covering the origin contributes
	// (r_i^2/sumR2)/(pi*r_i^2) = 1/(pi*sumR2), so the area pdf is
	// (1-g)/(pi*envR^2)*[in disc] + g*coverN/(pi*sumR2). Multiplying the
	// already-evaluated native emissionPdfW by the ratio folds the
	// direction pdf through unchanged.
	const Vector oo = ray.o - worldCenter;
	const float s1 = -Dot(oo, ax) * invR;
	const float s2 = -Dot(oo, ay) * invR;
	u_int coverN = 0;
	for (u_int i = 0; i < casterN; ++i) {
		const Vector oc = lightFocusCasterCenters[i] - worldCenter;
		const float c1 = -Dot(oc, ax) * invR;
		const float c2 = -Dot(oc, ay) * invR;
		const float rho = lightFocusCasterRadii[i] * invR;
		const float e1 = s1 - c1, e2 = s2 - c2;
		if (e1 * e1 + e2 * e2 <= rho * rho)
			++coverN;
	}
	const float nativeArea = (s1 * s1 + s2 * s2 <= 1.f) ? 1.f : 0.f;
	const float mult = (1.f - g) * nativeArea +
			g * coverN * envR * envR / sumR2;
	static u_int dbgN = 0;
	if (dbgN < 8 && (dbgN++ , true))
		SLG_LOG("FocusDistant o=" << ray.o << " d=" << ray.d << " s=" << s1 << "," << s2 << " cover=" << coverN << " envR=" << envR << " mult=" << mult);
	emissionPdfW *= mult;
}

//------------------------------------------------------------------------------
// RenderLightSample
//------------------------------------------------------------------------------

void PathTracer::RenderLightSample(IntersectionDeviceRef device,
		SceneConstRef scene, FilmConstRef film,
		Sampler& sampler, vector<SampleResult> &sampleResults,
		const ConnectToEyeCallBackType &ConnectToEyeCallBack) const {
	sampleResults.clear();

	// Spectral transport: draw the path wavelengths (the extra boot
	// dimension allocated by ParseOptions) for the light path
	PathWavelengths sw;
	if (spectralEnable)
		sw.Sample(sampler.GetSample(lightSampleBootSize - 1));
	const Spectral::ScopeWavelengths wlScope(sw);

	Spectrum lightPathFlux;

	const float timeSample = sampler.GetSample(8);
	const float time = scene.GetCamera().GenerateRayTime(timeSample);

	// Select one light source
	float lightPickPdf;
	auto light = scene.GetLightSources().GetEmitLightStrategy().
			SampleLights(scene, sampler.GetSample(0), &lightPickPdf);

	if (light) {
		// Initialize the light path
		Ray nextEventRay;
		float lightEmitPdfW;
		lightPathFlux = light->Emit(scene,
				time, sampler.GetSample(1), sampler.GetSample(2),
				sampler.GetSample(3), sampler.GetSample(4), sampler.GetSample(5),
				nextEventRay, lightEmitPdfW);

		if (lightPathFlux.Black())
			return;

		// Caustic focus cache: with probability focusRatio re-aim the
		// emitted direction at a remembered productive target and fold
		// the one-sample mixture pdf into emissionPdfW (GPU parity)
		LightFocusEmit(scene, *light, sampler, time,
				nextEventRay, lightPathFlux, lightEmitPdfW);
		if (lightEmitPdfW <= 0.f)
			return;

		lightPathFlux /= lightEmitPdfW * lightPickPdf;
		assert (!lightPathFlux.IsNaN() && !lightPathFlux.IsInf());

		LightPathInfo pathInfo;

		// Caustic focus cache: position of the first delta-specular
		// vertex of this path (credited into the emitting light's
		// hotspot ring on a successful camera connect, GPU
		// hasDeltaVertex/firstDeltaP parity)
		bool hasDeltaVertex = false;
		Point firstDeltaP;

		/*
		// Sample a point on the camera lens
		if (!scene.GetCamera().SampleLens(time, sampler.GetSample(6), sampler.GetSample(7),
				&pathInfo.lensPoint))
			return;
		*/

		//----------------------------------------------------------------------
		// Trace the light path
		//----------------------------------------------------------------------

		while (pathInfo.depth.depth < maxPathDepth.depth) {
			const u_int sampleOffset = lightSampleBootSize +  pathInfo.depth.depth * lightSampleStepSize;

			RayHit nextEventRayHit;
			BSDF bsdf;
			Spectrum connectionThroughput;
			const bool hit = scene.Intersect(
				luxrays::make_observer(device),
				LIGHT_RAY | INDIRECT_RAY,
				&pathInfo.volume, sampler.GetSample(sampleOffset),
				&nextEventRay, &nextEventRayHit, &bsdf,
				&connectionThroughput, nullptr, nullptr, false,
				&pathInfo.depth, pathInfo.lastBSDFEvent
			);
			if (!hit) {
				// Ray lost in space...
				break;
			}

			// Check if it is something with a not black shadow transparency
			// and stop if it has. Direct light sampling will take care of
			// this kind of paths.
			if (!bsdf.GetPassThroughShadowTransparency().Black() & !bsdf.GetPassThroughShadowTransparencyOverride())
				break;

			// Something was hit

			lightPathFlux *= connectionThroughput;

			// Caustic focus cache: remember the first delta-specular
			// vertex of this path - a successful camera connect credits
			// it into the emitting light's hotspot ring
			if (!hasDeltaVertex && bsdf.IsDelta()) {
				firstDeltaP = bsdf.hitPoint.p;
				hasDeltaVertex = true;
			}

			//--------------------------------------------------------------
			// Try to connect the light path vertex with the eye
			//--------------------------------------------------------------

			scene.GetCamera().SampleLens(time, sampler.GetSample(6), sampler.GetSample(7),
				&pathInfo.lensPoint);

			const size_t sampleResultsBefore = sampleResults.size();
			if (ConnectToEyeCallBack){
				ConnectToEyeCallBack(pathInfo, bsdf, light->GetID(), lightPathFlux, sampleResults);
			} else {
				ConnectToEye(device, scene, film,
						nextEventRay.time,
						sampler.GetSample(sampleOffset + 1),
						sampler.GetSample(sampleOffset + 2),
						sampler.GetSample(sampleOffset + 3),
						*light, bsdf, lightPathFlux, pathInfo, sampleResults);
			}

			// A connect that produced a screen contribution after
			// crossing a delta surface is a productive target: remember
			// it (once per path, GPU parity - the flag clears so a later
			// second delta bounce can still be credited)
			if (hasDeltaVertex &&
					(sampleResults.size() > sampleResultsBefore)) {
				LightFocusCredit(scene, light->lightSceneIndex, firstDeltaP);
				hasDeltaVertex = false;
			}

			if (pathInfo.depth.depth == maxPathDepth.depth - 1)
				break;

			//--------------------------------------------------------------
			// Build the next vertex path ray
			//--------------------------------------------------------------

			float bsdfPdf = 0.f;
			Vector sampledDir;
			BSDFEvent bsdfEvent;
			float cosSampleDir;
			Spectrum bsdfSample = bsdf.Sample(&sampledDir,
						sampler.GetSample(sampleOffset + 4),
						sampler.GetSample(sampleOffset + 5),
					&bsdfPdf, &cosSampleDir, &bsdfEvent);
			if (bsdfSample.Black())
				break;

			pathInfo.AddVertex(bsdf, bsdfEvent, hybridBackForwardGlossinessThreshold);

			// If it isn't anymore a (nearly) specular path, I can stop.
			// Adaptive partition: the chain survives while vertices are
			// non-diffuse (boundary-glossy interior vertices included)
			if (hybridBackForwardEnable &&
					!(hybridBackForwardAdaptiveCaustic ?
						pathInfo.isAdaptiveS : pathInfo.IsSpecularPath()) &&
					// This condition is added to "stabilize" Metropolis sampler
					// used in light tracing part of hybrid rendering. In this case
					// I render also some not-caustic sample to make an "easy"
					// computation of the avg. image luminance.
					(pathInfo.depth.diffuseDepth + pathInfo.depth.glossyDepth > 1))
				break;

			// Russian Roulette
			if (pathInfo.UseRR(rrDepth)) {
				// Russian Roulette
				const float rrProb = RenderEngine::RussianRouletteProb(bsdfSample, rrImportanceCap);
				if (rrProb < sampler.GetSample(sampleOffset + 6))
					break;

				// Increase path contribution
				bsdfSample /= rrProb;
			}

			lightPathFlux *= bsdfSample;
			verify (!lightPathFlux.IsNaN() && !lightPathFlux.IsInf());

			nextEventRay.Update(bsdf.GetRayOrigin(sampledDir), sampledDir);
		}
	}

	if (wlScope.Active()) {
		for (auto &sr : sampleResults)
			ProjectSampleResultToRGB(sr, sw);
	}
}

//------------------------------------------------------------------------------
// RenderSample
//------------------------------------------------------------------------------

bool PathTracer::HasToRenderEyeSample(PathTracerThreadState &state) const {
	// Check if I have to trace an eye or light path
	if (hybridBackForwardEnable) {
		const double ratio = state.eyeSampleCount / state.lightSampleCount;
		if ((hybridBackForwardPartition == 1.f) ||
				(ratio < hybridBackForwardPartition)) {
			// Trace an eye path
			state.eyeSampleCount += 1.0;
			return true;
		} else {
			// Trace a light path
			state.lightSampleCount += 1.0;
			return false;
		}
	} else {
		state.eyeSampleCount += 1.0;
		return true;
	}
}

void PathTracer::ApplyVarianceClamp(const PathTracerThreadState &state,
		vector<SampleResult> &sampleResults) const {
	// Variance clamping
	if (state.varianceClamping->hasClamping()) {
		for(u_int i = 0; i < sampleResults.size(); ++i) {
			SampleResult &sampleResult = sampleResults[i];

			// I clamp only eye paths samples (variance clamping would cut
			// SDS path values due to high scale of PSR samples)
			if (sampleResult.HasChannel(Film::RADIANCE_PER_PIXEL_NORMALIZED))
				state.varianceClamping->Clamp(state.GetFilm(), sampleResult);
		}
	}
}

void PathTracer::RenderSample(PathTracerThreadState &state) const {

	auto Render = [&]() {

		// Check if I have to trace an eye or light path
		if (HasToRenderEyeSample(state)) {
			// Trace an eye path
			auto& sampler = state.GetEyeSampler();
			auto& sampleResults = state.GetEyeSampleResults();
			RenderEyeSample(
				state.device,
				state.scene,
				state.GetFilm(),
				sampler,
				sampleResults
			);
			return std::make_tuple(std::ref(sampler), std::ref(sampleResults));
		} else {
			// Otherwise trace a light path
			auto& sampler = state.GetLightSampler();
			auto& sampleResults = state.GetLightSampleResults();
			RenderLightSample(
				state.device,
				state.scene,
				state.GetFilm(),
				sampler,
				sampleResults
			);
			return std::make_tuple(std::ref(sampler), std::ref(sampleResults));
		}
	};  // lambda

	// Render sample
	auto [sampler, sampleResults] = Render();
	assert(&sampleResults == &state.GetEyeSampleResults() || &sampleResults == &state.GetLightSampleResults());

	// Apply variance clamping
	ApplyVarianceClamp(state, sampleResults);

	sampler.NextSample(sampleResults);
}

//------------------------------------------------------------------------------
// ParseOptions
//------------------------------------------------------------------------------

void PathTracer::ParseOptions(
	luxrays::PropertiesConstRef cfg,
	const luxrays::Properties &defaultProps
) {
	// Path depth settings
	maxPathDepth.depth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.total")).Get<int>());
	maxPathDepth.diffuseDepth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.diffuse")).Get<int>());
	maxPathDepth.glossyDepth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.glossy")).Get<int>());
	maxPathDepth.specularDepth = Max(0, cfg.Get(defaultProps.Get("path.pathdepth.specular")).Get<int>());

	// For compatibility with the past
	if (cfg.IsDefined("path.maxdepth") &&
			!cfg.IsDefined("path.pathdepth.total") &&
			!cfg.IsDefined("path.pathdepth.diffuse") &&
			!cfg.IsDefined("path.pathdepth.glossy") &&
			!cfg.IsDefined("path.pathdepth.specular")) {
		const u_int maxDepth = Max(0, cfg.Get("path.maxdepth").Get<int>());
		maxPathDepth.depth = maxDepth;
		maxPathDepth.diffuseDepth = maxDepth;
		maxPathDepth.glossyDepth = maxDepth;
		maxPathDepth.specularDepth = maxDepth;
	}

	// Russian Roulette settings
	rrDepth = (u_int)Max(1, cfg.Get(defaultProps.Get("path.russianroulette.depth")).Get<int>());
	rrImportanceCap = Clamp(cfg.Get(defaultProps.Get("path.russianroulette.cap")).Get<double>(), 0.0, 1.0);

	// Clamping settings
	// clamping.radiance.maxvalue is the old radiance clamping, now converted in variance clamping
	sqrtVarianceClampMaxValue = cfg.Get(Property("path.clamping.radiance.maxvalue")(0.0)).Get<double>();
	if (cfg.IsDefined("path.clamping.variance.maxvalue"))
		sqrtVarianceClampMaxValue = cfg.Get(defaultProps.Get("path.clamping.variance.maxvalue")).Get<double>();
	sqrtVarianceClampMaxValue = Max(0.f, sqrtVarianceClampMaxValue);

	forceBlackBackground = cfg.Get(defaultProps.Get("path.forceblackbackground.enable")).Get<bool>();
	
	hybridBackForwardEnable = cfg.Get(defaultProps.Get("path.hybridbackforward.enable")).Get<bool>();
	// hybridBackForwardGlossinessThreshold is used by LIGHTCPU when PSR is enabled
	// so I have always to set the value
	hybridBackForwardGlossinessThreshold = .05f;
	// Adaptive caustic partition: parsed unconditionally because
	// path.lighttracing.enable below can force hybrid on after this block
	hybridBackForwardAdaptiveCaustic = cfg.Get(defaultProps.Get("path.hybridbackforward.adaptivecaustic")).Get<bool>();
	hybridBackForwardTerminalGlossiness = Clamp(cfg.Get(defaultProps.Get("path.hybridbackforward.terminalglossiness")).Get<double>(), 0.0, 1.0);
	hybridBackForwardConnectProb = Clamp(cfg.Get(defaultProps.Get("path.hybridbackforward.connectprob")).Get<double>(), 0.0, 1.0);
	if (hybridBackForwardEnable) {
		hybridBackForwardPartition = Clamp(cfg.Get(defaultProps.Get("path.hybridbackforward.partition")).Get<double>(), 0.0, 1.0);
		hybridBackForwardGlossinessThreshold = Clamp(cfg.Get(defaultProps.Get("path.hybridbackforward.glossinessthreshold")).Get<double>(), 0.0, 1.0);
	}

	// GPU light tracing (PATHOCL/RTPATHOCL): a tail task population splats
	// light-path vertices into the screen-normalized channel. The light
	// pass owns the caustic-class contributions, so the eye side must run
	// with hybrid caustic suppression for the estimator to stay unbiased
	// (same contract as CPU hybrid).
	lightTracingEnable = cfg.Get(defaultProps.Get("path.lighttracing.enable")).Get<bool>();
	lightTracingTaskFraction = Clamp(cfg.Get(defaultProps.Get("path.lighttracing.taskfraction")).Get<double>(), 0.0, 0.9);
	// Caustic focus cache (GPU): guided emission mixture parameters
	lightFocusEnable = cfg.Get(defaultProps.Get("path.lighttracing.focus.enable")).Get<bool>();
	lightFocusRatio = Clamp(cfg.Get(defaultProps.Get("path.lighttracing.focus.ratio")).Get<double>(), 0.0, 0.9);
	lightFocusRadiusFrac = Clamp(cfg.Get(defaultProps.Get("path.lighttracing.focus.radius")).Get<double>(), 1e-5, 1.0);
	if (lightTracingEnable && !hybridBackForwardEnable) {
		hybridBackForwardEnable = true;
		hybridBackForwardPartition = cfg.Get(defaultProps.Get("path.hybridbackforward.partition")).Get<double>();
	}

	// Albedo AOV settings
	albedoSpecularSetting = String2AlbedoSpecularSetting(cfg.Get(defaultProps.Get("path.albedospecular.type")).Get<string>());
	albedoSpecularGlossinessThreshold = Max(cfg.Get(defaultProps.Get("path.albedospecular.glossinessthreshold")).Get<double>(), 0.0);

	// MNEE (specular chain direct light sampling)
	mneeEnable = cfg.Get(defaultProps.Get("path.mnee.enable")).Get<bool>();
	mneeMaxIterations = Max(1, cfg.Get(defaultProps.Get("path.mnee.maxiterations")).Get<int>());
	mneeMaxSpecular = Clamp(cfg.Get(defaultProps.Get("path.mnee.maxspecular")).Get<int>(), 1, 4);
	// Manifold seed cache: warm-start Newton from cached converged
	// solutions nearby on the same occluder/light (the GPU mneeSeeds
	// hashed grid, same size and key function).
	mneeSeedCacheEnable = cfg.Get(defaultProps.Get("path.mnee.seedcache")).Get<bool>();
	if (mneeSeedCacheEnable)
		mneeSeeds.reset(new MneeSeedEntry[1u << 14]());
	else
		mneeSeeds.reset();

	// Path guiding (P1-3 M1 CPU; M2b GPU samples a frozen table file)
	// (path.guiding.tablefile, empty = train inline (CPU) / unguided (GPU))
	guidingEnable = cfg.Get(defaultProps.Get("path.guiding.enable")).Get<bool>();
	guidingTableFile = cfg.Get(defaultProps.Get("path.guiding.tablefile")).Get<string>();

	// Hero-wavelength spectral transport (P2-1): Spectrum channels carry
	// spectral samples at the path wavelengths instead of RGB primaries
	spectralEnable = cfg.Get(defaultProps.Get("path.spectral.enable")).Get<bool>();
	Spectral::SetEnabled(spectralEnable);

	// RGB->SPD upsampling model: "smits" (default, zero regression) or
	// "jh2019" (Jakob-Hanika 2019 sigmoid basis, opt-in; see
	// doc/features/spectral.md). Canonical name follows the existing
	// path.spectral.* convention; the bare "spectral.upsampling" alias
	// is accepted too and takes precedence when set.
	const string upsamplingModel = cfg.IsDefined("spectral.upsampling") ?
		cfg.Get("spectral.upsampling").Get<string>() :
		cfg.Get(defaultProps.Get("path.spectral.upsampling")).Get<string>();
	if (upsamplingModel == "jh2019")
		spectralUpsamplingJH2019 = true;
	else if (upsamplingModel == "smits")
		spectralUpsamplingJH2019 = false;
	else
		throw runtime_error("Unknown path.spectral.upsampling value "
			"(expected \"smits\" or \"jh2019\"): " + upsamplingModel);
	Spectral::SetUpsamplingModel(spectralUpsamplingJH2019 ?
			Spectral::UPSAMPLING_JH2019 : Spectral::UPSAMPLING_SMITS);
	if (spectralUpsamplingJH2019 && !spectralEnable)
		SLG_LOG("WARNING: path.spectral.upsampling=jh2019 has no effect "
			"without path.spectral.enable=1");

	// ReSTIR GI (G1, CPU): per-pixel first-bounce reservoir. The store
	// itself is engine-owned; only the toggles live here.
	restirGIEnable = cfg.Get(defaultProps.Get("path.restir.gi.enable")).Get<bool>();
	restirGICandidates = Max(1, cfg.Get(defaultProps.Get("path.restir.gi.candidates")).Get<int>());
	restirGITemporalEnable = cfg.Get(defaultProps.Get("path.restir.gi.temporal.enable")).Get<bool>();
	restirGISpatialEnable = cfg.Get(defaultProps.Get("path.restir.gi.spatial.enable")).Get<bool>();

	// Update eye sample size (9 classic dims + 1 path-guiding bin pick)
	eyeSampleBootSize = 5 + (spectralEnable ? 1 : 0); // +1 wavelength draw
	eyeSampleStepSize = 10;
	eyeSampleSize =
		eyeSampleBootSize + // To generate eye ray
		(maxPathDepth.depth + 1) * eyeSampleStepSize; // For each path vertex

	// Update light sample size (the 4 caustic-focus dims sit right after
	// the 9 fixed dims; the wavelength draw stays last)
	lightSampleBootSize = 9 + (lightFocusEnable ? 4 : 0) + (spectralEnable ? 1 : 0);
	lightSampleStepSize = 7;
	lightSampleSize =
		lightSampleBootSize + // To generate eye ray
		maxPathDepth.depth * lightSampleStepSize; // For each path vertex
}

//------------------------------------------------------------------------------
// Static methods used by RenderEngineRegistry
//------------------------------------------------------------------------------

PropertiesUPtr PathTracer::ToProperties(const Properties &cfg) {
	auto props_ptr = std::make_unique<Properties>();
	auto& props = *props_ptr;

	if (cfg.IsDefined("path.maxdepth") &&
			!cfg.IsDefined("path.pathdepth.total") &&
			!cfg.IsDefined("path.pathdepth.diffuse") &&
			!cfg.IsDefined("path.pathdepth.glossy") &&
			!cfg.IsDefined("path.pathdepth.specular")) {
		const u_int maxDepth = Max(0, cfg.Get("path.maxdepth").Get<int>());
		props << 
				Property("path.pathdepth.total")(maxDepth) <<
				Property("path.pathdepth.diffuse")(maxDepth) <<
				Property("path.pathdepth.glossy")(maxDepth) <<
				Property("path.pathdepth.specular")(maxDepth);
	} else {
		props <<
				cfg.Get(GetDefaultProps()->Get("path.pathdepth.total")) <<
				cfg.Get(GetDefaultProps()->Get("path.pathdepth.diffuse")) <<
				cfg.Get(GetDefaultProps()->Get("path.pathdepth.glossy")) <<
				cfg.Get(GetDefaultProps()->Get("path.pathdepth.specular"));
	}

	props <<
			cfg.Get(GetDefaultProps()->Get("path.hybridbackforward.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.hybridbackforward.partition")) <<
			cfg.Get(GetDefaultProps()->Get("path.hybridbackforward.glossinessthreshold")) <<
			cfg.Get(GetDefaultProps()->Get("path.hybridbackforward.adaptivecaustic")) <<
			cfg.Get(GetDefaultProps()->Get("path.hybridbackforward.terminalglossiness")) <<
			cfg.Get(GetDefaultProps()->Get("path.hybridbackforward.connectprob")) <<
			cfg.Get(GetDefaultProps()->Get("path.lighttracing.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.lighttracing.taskfraction")) <<
			cfg.Get(GetDefaultProps()->Get("path.lighttracing.only")) <<
			cfg.Get(GetDefaultProps()->Get("path.lighttracing.focus.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.lighttracing.focus.ratio")) <<
			cfg.Get(GetDefaultProps()->Get("path.lighttracing.focus.radius")) <<
			cfg.Get(GetDefaultProps()->Get("path.mnee.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.mnee.maxiterations")) <<
			cfg.Get(GetDefaultProps()->Get("path.mnee.maxspecular")) <<
			cfg.Get(GetDefaultProps()->Get("path.mnee.seedcache")) <<
			cfg.Get(GetDefaultProps()->Get("path.guiding.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.guiding.tablefile")) <<
			cfg.Get(GetDefaultProps()->Get("path.restir.gi.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.restir.gi.candidates")) <<
			cfg.Get(GetDefaultProps()->Get("path.restir.gi.temporal.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.restir.gi.spatial.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.spectral.enable")) <<
			// Emit the effective value: the "spectral.upsampling" alias
			// takes precedence over the canonical name when set
			Property("path.spectral.upsampling")(
				cfg.IsDefined("spectral.upsampling") ?
					cfg.Get("spectral.upsampling").Get<string>() :
					cfg.Get(GetDefaultProps()->Get("path.spectral.upsampling")).Get<string>()) <<
			cfg.Get(GetDefaultProps()->Get("path.russianroulette.depth")) <<
			cfg.Get(GetDefaultProps()->Get("path.russianroulette.cap")) <<
			cfg.Get(GetDefaultProps()->Get("path.clamping.variance.maxvalue")) <<
			cfg.Get(GetDefaultProps()->Get("path.forceblackbackground.enable")) <<
			cfg.Get(GetDefaultProps()->Get("path.albedospecular.type")) <<
			cfg.Get(GetDefaultProps()->Get("path.albedospecular.glossinessthreshold")) <<
			*Sampler::ToProperties(cfg);

	return props_ptr;
}

PropertiesUPtr PathTracer::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
	*props <<
			Property("path.hybridbackforward.enable")(false) <<
			Property("path.hybridbackforward.partition")(0.8) <<
			Property("path.hybridbackforward.glossinessthreshold")(.05f) <<
			// Adaptive caustic partition (on by default): the light pass
			// owns path classes whose light-adjacent vertex is hard for
			// the eye path - delta, or a glossy lobe of glossiness <=
			// terminalglossiness facing a light covering less than
			// connectprob of the lobe solid angle (PI * g^2). connectProb
			// is the eye-connection success probability below which the
			// light pass takes over: paths the eye completes less than
			// half the time are noise-dominant, so 0.5 is the default.
			Property("path.hybridbackforward.adaptivecaustic")(true) <<
			Property("path.hybridbackforward.terminalglossiness")(.3f) <<
			Property("path.hybridbackforward.connectprob")(.5f) <<
			Property("path.lighttracing.enable")(false) <<
			Property("path.lighttracing.taskfraction")(0.25) <<
			Property("path.lighttracing.only")(false) <<
			Property("path.lighttracing.focus.enable")(true) <<
			Property("path.lighttracing.focus.ratio")(0.5) <<
			Property("path.lighttracing.focus.radius")(0.01f) <<
			Property("path.mnee.enable")(false) <<
			Property("path.mnee.maxiterations")(12) <<
			Property("path.mnee.maxspecular")(1) <<
			Property("path.mnee.seedcache")(true) <<
			Property("path.guiding.enable")(false) <<
			Property("path.guiding.tablefile")("") <<
			Property("path.restir.gi.enable")(false) <<
			Property("path.restir.gi.candidates")(4) <<
			Property("path.restir.gi.temporal.enable")(true) <<
			Property("path.restir.gi.spatial.enable")(true) <<
			Property("path.spectral.enable")(false) <<
			Property("path.spectral.upsampling")("smits") <<
			Property("path.pathdepth.total")(6) <<
			Property("path.pathdepth.diffuse")(4) <<
			Property("path.pathdepth.glossy")(4) <<
			Property("path.pathdepth.specular")(6) <<
			Property("path.russianroulette.depth")(3) <<
			Property("path.russianroulette.cap")(.5f) <<
			Property("path.clamping.variance.maxvalue")(0.f) <<
			Property("path.forceblackbackground.enable")(false) <<
			Property("path.albedospecular.type")("REFLECT_TRANSMIT") <<
			Property("path.albedospecular.glossinessthreshold")(.05f);

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
