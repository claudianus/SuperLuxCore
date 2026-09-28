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

#include <math.h>

#include <boost/format.hpp>
#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <chrono>
#include <filesystem>

#include "luxrays/utils/thread.h"
#include "luxrays/utils/strutils.h"

#include "slg/samplers/sobol.h"
#include "slg/utils/pathdepthinfo.h"
#include "slg/cameras/camera.h"
#include "slg/engines/caches/photongi/photongicache.h"
#include "slg/engines/caches/photongi/tracephotonsthread.h"
#include "slg/utils/pathinfo.h"
#include "slg/scene/scene.h"
#include "slg/volumes/homogenous.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// PGICBeamIndex: segment AABBs of caustic photon beams in a boost R-tree.
// Point queries test the lookup sphere against each candidate segment.
//------------------------------------------------------------------------------

namespace bgi = boost::geometry::index;

class slg::PGICBeamIndex {
public:
	typedef boost::geometry::model::point<float, 3, boost::geometry::cs::cartesian> BPoint;
	typedef boost::geometry::model::box<BPoint> BBox;
	typedef std::pair<BBox, u_int> Item;
	typedef bgi::rtree<Item, bgi::quadratic<8> > RT;

	PGICBeamIndex(const std::vector<PhotonBeam> &beams) {
		std::vector<Item> items;
		items.reserve(beams.size());
		for (u_int i = 0; i < beams.size(); ++i) {
			const PhotonBeam &b = beams[i];
			const Point p1 = b.p0 + b.length * b.d;
			items.push_back(Item(BBox(
					BPoint(Min(b.p0.x, p1.x), Min(b.p0.y, p1.y), Min(b.p0.z, p1.z)),
					BPoint(Max(b.p0.x, p1.x), Max(b.p0.y, p1.y), Max(b.p0.z, p1.z))), i));
		}
		rt = RT(items.begin(), items.end());
	}

	RT rt;
};

//------------------------------------------------------------------------------
// PhotonGICache
//------------------------------------------------------------------------------

PhotonGICache::PhotonGICache() :
		scene(nullptr),
		visibilityParticlesKdTree(nullptr),
		radiancePhotonsBVH(nullptr) ,
		causticPhotonsBVH(nullptr) {
}

PhotonGICache::PhotonGICache(SceneConstRef scn, const PhotonGICacheParams &p) :
		scene(&scn), params(p),
		visibilityParticlesKdTree(nullptr),
		radiancePhotonsBVH(nullptr) ,
		indirectPhotonTracedCount(0),
		causticPhotonsBVH(nullptr),
		causticPhotonTracedCount(0),
		causticPhotonPass(0) {
}

PhotonGICache::~PhotonGICache() {
	// Join a possibly in-flight background update before freeing state
	updateThread.reset();

	delete visibilityParticlesKdTree;

	delete causticPhotonsBVH;
	delete radiancePhotonsBVH;

	delete updateCausticPhotonsBVH;
}

bool PhotonGICache::IsPhotonGIEnabled(const BSDF &bsdf) const {
	const BSDFEvent eventTypes = bsdf.GetEventTypes();

	if ((eventTypes & TRANSMIT) || (eventTypes & SPECULAR) ||
			((eventTypes & GLOSSY) && (bsdf.GetGlossiness() < params.glossinessUsageThreshold)))
		return false;
	else if (bsdf.IsVolume() && params.caustic.enabled && params.caustic.volumeBeams)
		// Medium scatter vertices are caustic(-beam) receivers regardless of
		// the per-volume photongi.enable flag (volumes default it to false):
		// they must run cache queries. They do NOT need visibility
		// particles (IsVisibilityEnabled governs those) - beams are
		// visibility-independent and point deposits in media stay gated by
		// whatever particles exist.
		return true;
	else
		return bsdf.IsPhotonGIEnabled();
}

bool PhotonGICache::IsVisibilityEnabled(const BSDF &bsdf) const {
	const BSDFEvent eventTypes = bsdf.GetEventTypes();

	// Upstream semantics: a medium scatter vertex only counts when the
	// volume explicitly opts into PhotonGI (scene.volumes.*.photongi.enable).
	if ((eventTypes & TRANSMIT) || (eventTypes & SPECULAR) ||
			((eventTypes & GLOSSY) && (bsdf.GetGlossiness() < params.glossinessUsageThreshold)))
		return false;
	else
		return bsdf.IsPhotonGIEnabled();
}

bool PhotonGICache::UseFrustumCulling() const {
	// RTPM-style photon culling replaces the visibility pre-pass: deposits
	// outside the camera frustum are rejected at trace time. Only valid for
	// caustic-only caches on projective cameras - indirect deposits need the
	// visibility map, and environment cameras have no film frame to test.
	const Camera::CameraType camType = scene->GetCamera().GetType();
	return params.caustic.enabled && !params.indirect.enabled &&
			((camType == Camera::PERSPECTIVE) || (camType == Camera::ORTHOGRAPHIC));
}

float PhotonGICache::GetIndirectUsageThreshold(const BSDFEvent lastBSDFEvent,
		const float lastGlossiness, const float u0) const {
	// Decide if the glossy surface is "nearly specular"

	if ((lastBSDFEvent & GLOSSY) && (lastGlossiness < params.glossinessUsageThreshold)) {
		// Disable the cache, the surface is "nearly specular"
		return numeric_limits<float>::infinity();
	} else {
		// Use a larger blend zone for glossy surface
		const float scale = (lastBSDFEvent & GLOSSY) ? 2.f : 1.f;

		// Enable the cache for diffuse or glossy "nearly diffuse" but only after
		// the threshold (before I brute force and cache between 0x and 1x the threshold)
		return scale * u0 * params.indirect.usageThresholdScale * params.indirect.lookUpRadius;
	}
}

bool PhotonGICache::IsDirectLightHitVisible(const EyePathInfo &pathInfo,
		const bool photonGICausticCacheUsed) const {
	// This is a specific check to cut fireflies created by some glossy or
	// specular bounce
	if (!(pathInfo.lastBSDFEvent & DIFFUSE) && (pathInfo.depth.diffuseDepth > 0))
		return false;
	else if (!params.caustic.enabled || !photonGICausticCacheUsed)
		return true;
	else if (!pathInfo.IsCausticPath() && (params.debugType == PGIC_DEBUG_NONE))
		return true;
	else
		return false;
}

void PhotonGICache::TracePhotons(const u_int seedBase, const u_int photonTracedCount,
		const bool indirectCacheDone, const bool causticCacheDone,
		std::atomic<u_int> &globalIndirectPhotonsTraced, std::atomic<u_int> &globalCausticPhotonsTraced,
		std::atomic<u_int> &globalIndirectSize, std::atomic<u_int> &globalCausticSize,
		SpillableArray<Photon> &dstCausticPhotons, std::vector<PhotonBeam> &dstCausticBeams) {
	const size_t renderThreadCount = GetHardwareThreadCount();
	using TracePhotonsThreadUPtr = std::unique_ptr<TracePhotonsThread>;
	std::vector<TracePhotonsThreadUPtr> renderThreads;

	std::atomic<u_int> globalPhotonsCounter(0);

	// Create the photon tracing threads
	renderThreads.reserve(renderThreadCount);
	for (size_t i = 0; i < renderThreadCount; ++i) {
		renderThreads.emplace_back(
			std::make_unique<TracePhotonsThread>(
				*this, i, seedBase, photonTracedCount,
				indirectCacheDone, causticCacheDone,
				globalPhotonsCounter, globalIndirectPhotonsTraced,
				globalCausticPhotonsTraced, globalIndirectSize,
				globalCausticSize
			)
		);
	}

	// Start photon tracing threads
	for (size_t i = 0; i < renderThreadCount; ++i)
		renderThreads[i]->Start();

	// Wait for the end of photon tracing threads
	u_int indirectPhotonStored = 0;
	u_int causticPhotonStored = 0;
	for (size_t i = 0; i < renderThreadCount; ++i) {
		renderThreads[i]->Join();

		// Copy all photons
		for (auto const &p : renderThreads[i]->indirectPhotons) {
			PGICVisibilityParticle &vp = visibilityParticles[p.visibilityParticelIndex];

			vp.alphaAccumulated.Add(p.lightID, p.alpha);
		}
		indirectPhotonStored += renderThreads[i]->indirectPhotons.size();

		dstCausticPhotons.insert(dstCausticPhotons.end(), renderThreads[i]->causticPhotons.begin(),
				renderThreads[i]->causticPhotons.end());
		causticPhotonStored += renderThreads[i]->causticPhotons.size();

		dstCausticBeams.insert(dstCausticBeams.end(), renderThreads[i]->causticBeams.begin(),
				renderThreads[i]->causticBeams.end());

		renderThreads[i].reset();
	}

	// Update the count only if I have traced this kind of photons
	if (!indirectCacheDone)
		indirectPhotonTracedCount = globalIndirectPhotonsTraced;
	// Update the count only if I have traced this kind of photons
	if (!causticCacheDone)
		causticPhotonTracedCount = globalCausticPhotonsTraced;

	SLG_LOG("PhotonGI additional indirect photon stored: " << indirectPhotonStored);
	SLG_LOG("PhotonGI additional caustic photon stored: " << causticPhotonStored);
	// photonReacedCount isn't exactly but it is quite near
	SLG_LOG("PhotonGI total photon traced: " << Max(indirectPhotonTracedCount, causticPhotonTracedCount));
}

//------------------------------------------------------------------------------
// Stall-free cache updates
//
// The periodic caustic update re-traces photons and rebuilds all indices
// on a dedicated worker thread while render threads keep querying the
// live cache. Only the pointer swap requires synchronization and it runs
// inside the sync barrier completion step (completion_t), where every
// render thread is already parked and can not be inside a query.

void PhotonGICache::UpdateWorker() {
	const double startTime = WallClockTime();
	SLG_LOG("Updating PhotonGI caustic cache (Pass " << causticPhotonPass << ")");

	try {
		// Reduce the look up radius, capped by the minimum
		updateLookUpRadius = params.caustic.lookUpRadius /
				powf(float(causticPhotonPass + 1), .5f * (1.f - params.caustic.radiusReduction));
		updateLookUpRadius = Max(updateLookUpRadius, params.caustic.minLookUpRadius);

		// Trace photons into the shadow copy (never touches the live cache)
		TracePhotons(false, params.caustic.enabled,
				&updateCausticPhotons, &updateCausticBeams);

		if (updateCausticPhotons.size() > 0) {
			SLG_LOG("PhotonGI building caustic photons BVH");
			updateCausticPhotonsBVH = new PGICPhotonBvh(&updateCausticPhotons,
					causticPhotonTracedCount, updateLookUpRadius, params.caustic.lookUpNormalAngle);
		}
		BuildCausticBeamsIndex(updateCausticBeams, updateCausticBeamsIndex);
	} catch (std::exception &e) {
		SLG_LOG("ERROR: PhotonGI cache background update failed: " << e.what());
		updateFailed = true;
	}

	SLG_LOG("PhotonGI caustic cache update traced in: " << std::setprecision(3) <<
			(WallClockTime() - startTime) << " secs");

	// Release the shadow cache for the barrier completion swap
	updatePendingSwap = true;
}

void PhotonGICache::ApplyPendingUpdate() noexcept {
	// Exactly-once guard: the barrier completion step runs every phase
	if (!updatePendingSwap.exchange(false))
		return;

	try {
		if (updateFailed) {
			updateFailed = false;
		} else {
			delete causticPhotonsBVH;
			causticPhotonsBVH = updateCausticPhotonsBVH;
			updateCausticPhotonsBVH = nullptr;

			causticPhotons = std::move(updateCausticPhotons);
			causticBeams = std::move(updateCausticBeams);
			causticBeamsIndex = std::move(updateCausticBeamsIndex);

			params.caustic.lookUpRadius = updateLookUpRadius;
			params.caustic.lookUpRadius2 = Sqr(updateLookUpRadius);
			SLG_LOG("New PhotonGI caustic cache lookup radius: " << params.caustic.lookUpRadius);

			lastUpdateSpp = updateFilmSPP;
			++causticPhotonPass;

			if (updateCallback) {
				updateCallback();
				updateCallback = nullptr;
			}
		}
	} catch (...) {
		// Must not throw from a barrier completion step
	}

	updateInFlight = false;
	updateThread.reset();
}

void PhotonGICache::completion_t::operator()() noexcept {
	if (cache)
		cache->ApplyPendingUpdate();
}

void PhotonGICache::TracePhotons(const bool indirectEnabled, const bool causticEnabled,
		SpillableArray<Photon> *dstCausticPhotons, std::vector<PhotonBeam> *dstCausticBeams) {
	if (!dstCausticPhotons)
		dstCausticPhotons = &causticPhotons;
	if (!dstCausticBeams)
		dstCausticBeams = &causticBeams;

	const size_t renderThreadCount = GetHardwareThreadCount();

	std::atomic<u_int> globalIndirectPhotonsTraced(0);
	std::atomic<u_int> globalCausticPhotonsTraced(0);
	std::atomic<u_int> globalIndirectSize(0);
	std::atomic<u_int> globalCausticSize(0);

	// Update the count only if I have traced this kind of photons
	if (indirectEnabled)
		indirectPhotonTracedCount = 0;
	// Update the count only if I have traced this kind of photons
	if (causticEnabled)
		causticPhotonTracedCount = 0;

	if (indirectEnabled && (params.indirect.maxSize == 0)) {
		// Automatic indirect cache convergence test is required

		const u_int photonTracedStep = 2000000;
		u_int photonTracedCount = 0;
		vector<SpectrumGroup> lastAlpha(visibilityParticles.size());
		vector<SpectrumGroup> currentAlpha(visibilityParticles.size());
		while (photonTracedCount < params.photon.maxTracedCount) {
			//------------------------------------------------------------------
			// Trace additional photons
			//------------------------------------------------------------------

			TracePhotons(updateSeedBase, photonTracedStep, false, !causticEnabled,
				globalIndirectPhotonsTraced, globalCausticPhotonsTraced,
				globalIndirectSize, globalCausticSize,
				*dstCausticPhotons, *dstCausticBeams);
			photonTracedCount += photonTracedStep;

			//------------------------------------------------------------------
			// Check the convergence if it is not the first step
			//------------------------------------------------------------------

			if (photonTracedCount > photonTracedStep) {
				// Compute current alpha

				for (u_int i = 0; i < visibilityParticles.size(); ++i) {
					const PGICVisibilityParticle vp = visibilityParticles[i];

					currentAlpha[i] = vp.ComputeRadiance(params.indirect.lookUpRadius2, indirectPhotonTracedCount);
				}

				// Filter outgoing radiance

				if (params.indirect.filterRadiusScale > 0.f) {
					vector<SpectrumGroup> filteredCurrentAlpha(visibilityParticles.size());
					FilterVisibilityParticlesRadiance(currentAlpha, filteredCurrentAlpha);

					currentAlpha = filteredCurrentAlpha;
				}

				// Compute the scale for an auto-linear-like tone mapping of values

				float Y = 0.f;
				for (u_int i = 0; i < visibilityParticles.size(); ++i)
					Y += currentAlpha[i].Sum().Y();
				Y /= visibilityParticles.size();

				const float alphaScale = (Y > 0.f) ? (1.25f / Y * powf(118.f / 255.f, 2.2f)) : 1.f;
				for (u_int i = 0; i < visibilityParticles.size(); ++i)
					currentAlpha[i] *= alphaScale;

				// Look for the max. error

				float maxError = 0.f;
				for (u_int i = 0; i < visibilityParticles.size(); ++i) {
					if (!currentAlpha[i].Black()) {
						SpectrumGroup alpha = currentAlpha[i];
						alpha -= lastAlpha[i];

						for (u_int j = 0; j < alpha.Size(); ++j) {
							const float currentError = alpha[j].Abs().Max();

							maxError = Max(maxError, currentError);
						}
					}

					// Update last alpha cache entries
					lastAlpha[i] = currentAlpha[i];
				}

				SLG_LOG(boost::format("PhotonGI estimated current indirect photon error: %.2f%%") % (100.f * maxError));

				// If the error is under the threshold, stop tracing photons for indirect cache
				if (maxError < params.indirect.haltThreshold) {
					// Finish the work for caustic cache too
					if (causticEnabled &&
							(causticPhotons.size() < params.caustic.maxSize) &&
							(photonTracedCount < params.photon.maxTracedCount)) {
						updateSeedBase += renderThreadCount;

						TracePhotons(updateSeedBase,
								params.photon.maxTracedCount - photonTracedCount, true, false,
								globalIndirectPhotonsTraced, globalCausticPhotonsTraced,
								globalIndirectSize, globalCausticSize,
								*dstCausticPhotons, *dstCausticBeams);
					}

					break;
				}
			} else {
				// Update last alpha cache entries

				for (u_int i = 0; i < visibilityParticles.size(); ++i) {
					const PGICVisibilityParticle vp = visibilityParticles[i];

					lastAlpha[i] = vp.ComputeRadiance(params.indirect.lookUpRadius2, indirectPhotonTracedCount);
				}
			}
			
			updateSeedBase += renderThreadCount;
		}
	} else {
		// Just trace the asked amount of photon paths
		TracePhotons(updateSeedBase, params.photon.maxTracedCount, !indirectEnabled, !causticEnabled,
				globalIndirectPhotonsTraced, globalCausticPhotonsTraced,
				globalIndirectSize, globalCausticSize,
				*dstCausticPhotons, *dstCausticBeams);

	}

	updateSeedBase += renderThreadCount;

	dstCausticPhotons->shrink_to_fit();
}

void PhotonGICache::FilterVisibilityParticlesRadiance(const vector<SpectrumGroup> &radianceValues,
			vector<SpectrumGroup> &filteredRadianceValues) const {
	const float lookUpRadius2 = Sqr(params.indirect.filterRadiusScale * params.indirect.lookUpRadius);
	const float lookUpCosNormalAngle = cosf(Radians(params.indirect.lookUpNormalAngle));

	#pragma omp parallel for
	for (
			// Visual C++ 2013 supports only OpenMP 2.5
#if _OPENMP >= 200805
			unsigned
#endif
			int index = 0; index < visibilityParticles.size(); ++index) {
		// Look for all near particles

		std::vector<size_t> nearParticleIndices;
		const PGICVisibilityParticle &vp = visibilityParticles[index];
		// I can use visibilityParticlesKdTree to get radiance photons indices
		// because there is a one on one correspondence 
		visibilityParticlesKdTree->GetAllNearEntries(
			nearParticleIndices,
			vp.p, vp.n, vp.isVolume,
			lookUpRadius2, lookUpCosNormalAngle);

		if (nearParticleIndices.size() > 0) {
			SpectrumGroup &filtered = filteredRadianceValues[index];
			for (auto nearIndex : nearParticleIndices)
				filtered += radianceValues[nearIndex];

			filtered /= nearParticleIndices.size();
		} 
	}
}

void PhotonGICache::CreateRadiancePhotons() {
	//--------------------------------------------------------------------------
	// Compute the outgoing radiance for each visibility entry
	//--------------------------------------------------------------------------

	vector<SpectrumGroup> outgoingRadianceValues(visibilityParticles.size());

	for (u_int index = 0 ; index < visibilityParticles.size(); ++index) {
		const PGICVisibilityParticle &vp = visibilityParticles[index];

		outgoingRadianceValues[index] = vp.ComputeRadiance(params.indirect.lookUpRadius2, indirectPhotonTracedCount);
		assert (outgoingRadianceValues[index].IsValid());
	}

	//--------------------------------------------------------------------------
	// Filter outgoing radiance
	//--------------------------------------------------------------------------

	if (params.indirect.filterRadiusScale > 0.f) {
		SLG_LOG("PhotonGI filtering radiance photons");

		vector<SpectrumGroup> filteredOutgoingRadianceValues(visibilityParticles.size());
		FilterVisibilityParticlesRadiance(outgoingRadianceValues, filteredOutgoingRadianceValues);

		outgoingRadianceValues = filteredOutgoingRadianceValues;
	}

	//--------------------------------------------------------------------------
	// Create a radiance map entry for each visibility entry
	//--------------------------------------------------------------------------

	for (u_int index = 0 ; index < visibilityParticles.size(); ++index) {
		if (!outgoingRadianceValues[index].Black()) {
			const PGICVisibilityParticle &vp = visibilityParticles[index];

			radiancePhotons.push_back(RadiancePhoton(vp.p,
					vp.n, outgoingRadianceValues[index], vp.isVolume));
		}
	}
	radiancePhotons.shrink_to_fit();
	
	SLG_LOG("PhotonGI total radiance photon stored: " << radiancePhotons.size());
}

void PhotonGICache::Preprocess(const u_int threadCnt) {
	threadCount = threadCnt;
	threadsSyncBarrier.reset(new std::barrier(threadCount, completion_t{this}));
	lastUpdateSpp = 0;
	updateSeedBase = 1;
	finishUpdateFlag = false;

	if (params.persistent.fileName != "") {
		// Check if the file already exist
		if (std::filesystem::exists(params.persistent.fileName)) {
			// Load the cache from the file
			LoadPersistentCache(params.persistent.fileName);

			return;
		}
		
		// The file doesn't exist so I have to go trough normal pre-processing
	}

	//--------------------------------------------------------------------------
	// Evaluate best radius if required
	//--------------------------------------------------------------------------

	// The eye-pass footprint probe is deterministic: run it once and share
	// the result across every radius that asked for auto (0).
	float autoRadius = 0.f;
	if ((params.indirect.enabled && (params.indirect.lookUpRadius == 0.f)) ||
			(params.caustic.enabled && (params.caustic.lookUpRadius == 0.f))) {
		autoRadius = EvaluateBestRadius();
		if (params.indirect.enabled && (params.indirect.lookUpRadius == 0.f)) {
			params.indirect.lookUpRadius = autoRadius;
			SLG_LOG("PhotonGI best indirect cache radius: " << autoRadius);
		}
		if (params.caustic.enabled && (params.caustic.lookUpRadius == 0.f)) {
			params.caustic.lookUpRadius = autoRadius;
			SLG_LOG("PhotonGI best caustic cache radius: " << autoRadius);
		}
	}
	if (params.caustic.enabled && (params.caustic.minLookUpRadius == 0.f))
		// The radius-reduction floor scales with the resolved radius
		// (explicit radius or auto-derived), not an absolute value.
		params.caustic.minLookUpRadius = params.caustic.lookUpRadius * .02f;

	if (params.caustic.enabled && (params.caustic.maxSize == 0)) {
		// Automatic cache capacity: ~2 photons per film pixel, bounded
		// [256K, 16M]. Caustic detail requirements scale with resolution
		// (viewport renders stay light), so the film area is the natural
		// budget axis - the same scheme Corona's caustics solver uses.
		const u_int filmPixels = Max(1u, scene->GetCamera().filmWidth *
				scene->GetCamera().filmHeight);
		params.caustic.maxSize = Max(262144u, Min(16777216u, 2u * filmPixels));
		SLG_LOG("PhotonGI automatic caustic cache max size: " << params.caustic.maxSize);
	}

	//--------------------------------------------------------------------------
	// Initialize all parameters
	//--------------------------------------------------------------------------

	if (!params.indirect.enabled)
		params.indirect.maxSize = 0;

	if (!params.caustic.enabled)
		params.caustic.maxSize = 0;

	if (params.indirect.enabled) {
		// I must use indirect cache parameters for Visibility particles if the
		// cache is enabled
		params.visibility.lookUpRadius = params.indirect.lookUpRadius;
		params.visibility.lookUpNormalAngle = params.indirect.lookUpNormalAngle;
	} else {
		if (params.visibility.lookUpRadius == 0.f) {
			if (params.caustic.enabled) {
				// Caustic radius is too small for visibility check
				params.visibility.lookUpRadius = (autoRadius > 0.f) ? autoRadius : EvaluateBestRadius();
				params.visibility.lookUpNormalAngle = params.caustic.lookUpNormalAngle;
			} else
				throw runtime_error("Indirect and/or caustic cache must be enabled in PhotonGI");
		}
	}
	SLG_LOG("PhotonGI visibility lookup radius: " << params.visibility.lookUpRadius);
	params.visibility.lookUpNormalCosAngle = cosf(Radians(params.visibility.lookUpNormalAngle));

	params.visibility.lookUpRadius2 = params.visibility.lookUpRadius * params.visibility.lookUpRadius;
	params.indirect.lookUpRadius2 = params.indirect.lookUpRadius * params.indirect.lookUpRadius;
	params.caustic.lookUpRadius2 = params.caustic.lookUpRadius * params.caustic.lookUpRadius;

	//--------------------------------------------------------------------------
	// Trace visibility particles
	//--------------------------------------------------------------------------

	if (UseFrustumCulling()) {
		// Caustic-only caches on projective cameras do not need the
		// visibility pass at all: photon deposits are frustum-culled instead
		// (RTPM-style photon culling). This removes the dominant pre-pass
		// cost for the common "just caustics" configuration.
		SLG_LOG("PhotonGI visibility pass skipped: frustum-culled caustic deposits");
	} else {
		TraceVisibilityParticles();
		if (visibilityParticles.size() == 0) {
			if (!(params.caustic.enabled && params.caustic.volumeBeams)) {
				SLG_LOG("PhotonGI WARNING: nothing is visible and/or cache enabled.");
				return;
			}
			// Caustic beams deposit on specular-prefix medium flights without
			// any visibility gate, so photon tracing stays useful even with no
			// particles (visibilityParticlesKdTree remains null and point
			// deposits are simply skipped).
			SLG_LOG("PhotonGI no visibility particles: caustic beams proceed anyway");
		}
	}

	//--------------------------------------------------------------------------
	// Fill all photon vectors
	//--------------------------------------------------------------------------

	// I build indirect and caustic caches with 2 different steps in order to
	// have Metropolis work at beast for the 2 different tasks

	if (params.indirect.enabled) {
		SLG_LOG("PhotonGI tracing indirect cache photons");
		TracePhotons(true, false);
	}

	if (params.caustic.enabled) {
		SLG_LOG("PhotonGI tracing caustic cache photons");
		TracePhotons(false, true);
	}

	//--------------------------------------------------------------------------
	// Radiance photon map
	//--------------------------------------------------------------------------

	if (params.indirect.enabled) {	
		SLG_LOG("PhotonGI building radiance photon data");
		CreateRadiancePhotons();

		if (radiancePhotons.size() > 0) {
			SLG_LOG("PhotonGI building radiance photons BVH");
			radiancePhotonsBVH = new PGICRadiancePhotonBvh(&radiancePhotons,
					params.indirect.lookUpRadius, params.indirect.lookUpNormalAngle);
		}
	}

	//--------------------------------------------------------------------------
	// Caustic photon map
	//--------------------------------------------------------------------------

	if ((causticPhotons.size() > 0) && params.caustic.enabled) {
		SLG_LOG("PhotonGI building caustic photons BVH");
		causticPhotonsBVH = new PGICPhotonBvh(&causticPhotons, causticPhotonTracedCount,
				params.caustic.lookUpRadius, params.caustic.lookUpNormalAngle);
	}

	BuildCausticBeamsIndex();

	//--------------------------------------------------------------------------
	// Free visibility map (only if it is not required for a further update
	//--------------------------------------------------------------------------

	if (!params.caustic.enabled || (params.caustic.updateSpp == 0)) {
		delete visibilityParticlesKdTree;
		visibilityParticlesKdTree = nullptr;
		visibilityParticles.clear();
		visibilityParticles.shrink_to_fit();
	}

	//--------------------------------------------------------------------------
	// Print some statistics about memory usage
	//--------------------------------------------------------------------------

	size_t totalMemUsage = 0;

	if (causticPhotonsBVH) {
		SLG_LOG("PhotonGI caustic cache photons memory usage: " << ToMemString(causticPhotons.size() * sizeof(Photon)));
		SLG_LOG("PhotonGI caustic cache BVH memory usage: " << ToMemString(causticPhotonsBVH->GetMemoryUsage()));

		totalMemUsage += causticPhotons.size() * sizeof(Photon) + causticPhotonsBVH->GetMemoryUsage();
	}

	if (radiancePhotonsBVH) {
		SLG_LOG("PhotonGI indirect cache photons memory usage: " << ToMemString(radiancePhotons.size() * sizeof(RadiancePhoton)));
		SLG_LOG("PhotonGI indirect cache BVH memory usage: " << ToMemString(radiancePhotonsBVH->GetMemoryUsage()));

		totalMemUsage += radiancePhotons.size() * sizeof(Photon) + radiancePhotonsBVH->GetMemoryUsage();
	}

	SLG_LOG("PhotonGI total memory usage: " << ToMemString(totalMemUsage));

	//--------------------------------------------------------------------------
	// Spill the read-only photon arrays to file-backed storage
	//--------------------------------------------------------------------------

	// The caches are read-only during rendering once their BVH has been built:
	// swapping them for copy-on-write file mappings lets the kernel evict the
	// untouched pages under pressure while lookups page them back on demand.
	// Skipped when the caustic cache is periodically updated, because Update()
	// clears and rebuilds the array (Reset() would just drop the mapping, but
	// spilling an array that is about to be rebuilt is wasted work).
	if (scene->GeoSpillEnabled() &&
			(!params.caustic.enabled || (params.caustic.updateSpp == 0))) {
		const std::string dir = scene->GeoSpillDir() + "/" + std::to_string(
				std::chrono::steady_clock::now().time_since_epoch().count()) +
				"-" + std::to_string(reinterpret_cast<uintptr_t>(this));
		const size_t minBytes = scene->GeoSpillMinBytes();
		size_t spilled = 0;

		if (radiancePhotonsBVH &&
				(radiancePhotons.size() * sizeof(RadiancePhoton) >= minBytes)) {
			std::filesystem::create_directories(dir);
			spilled += radiancePhotons.Spill(dir + "/radiancephotons.bin");
		}
		if (causticPhotonsBVH &&
				(causticPhotons.size() * sizeof(Photon) >= minBytes)) {
			std::filesystem::create_directories(dir);
			spilled += causticPhotons.Spill(dir + "/causticphotons.bin");
		}

		if (spilled > 0)
			SLG_LOG("PhotonGI cache spilled to file-backed storage: " << ToMemString(spilled));
	}

	//--------------------------------------------------------------------------
	// Check if I have to save the persistent cache
	//--------------------------------------------------------------------------

	if (params.persistent.fileName != "")
		SavePersistentCache(params.persistent.fileName);
}

const SpectrumGroup *PhotonGICache::GetIndirectRadiance(const BSDF &bsdf) const {
	assert (IsPhotonGIEnabled(bsdf));

	if (radiancePhotonsBVH) {
		// Flip the normal if required
		const Normal n = (bsdf.hitPoint.intoObject ? 1.f: -1.f) * bsdf.hitPoint.geometryN;
		const RadiancePhoton *radiancePhoton = radiancePhotonsBVH->GetNearestEntry(bsdf.hitPoint.p, n, bsdf.IsVolume());

		if (radiancePhoton) {
			const SpectrumGroup *result = &radiancePhoton->outgoingRadiance;

			assert (result->IsValid());
			assert (DistanceSquared(radiancePhoton->p, bsdf.hitPoint.p) < radiancePhotonsBVH->GetEntryRadius());
			assert (bsdf.IsVolume() == radiancePhoton->isVolume);
			assert (radiancePhoton->isVolume || (Dot(radiancePhoton->n, n) > radiancePhotonsBVH->GetEntryNormalCosAngle()));

			return result;
		}
	}
	
	return nullptr;
}

void PhotonGICache::BuildCausticBeamsIndex() {
	BuildCausticBeamsIndex(causticBeams, causticBeamsIndex);
}

void PhotonGICache::BuildCausticBeamsIndex(const std::vector<PhotonBeam> &src,
		std::unique_ptr<PGICBeamIndex> &dst) {
	dst.reset();
	if (params.caustic.volumeBeams && (src.size() > 0)) {
		SLG_LOG("PhotonGI building caustic beams index (" << src.size() << " beams)");
		dst = std::make_unique<PGICBeamIndex>(src);
	}
}

SpectrumGroup PhotonGICache::ConnectCausticBeams(const BSDF &bsdf) const {
	SpectrumGroup result;

	const Point &x = bsdf.hitPoint.p;
	const float r = params.caustic.lookUpRadius;
	const float r2 = params.caustic.lookUpRadius2;

	std::vector<PGICBeamIndex::Item> hits;
	causticBeamsIndex->rt.query(bgi::intersects(PGICBeamIndex::BBox(
			PGICBeamIndex::BPoint(x.x - r, x.y - r, x.z - r),
			PGICBeamIndex::BPoint(x.x + r, x.y + r, x.z + r))),
			back_inserter(hits));

	for (auto const &hit : hits) {
		const PhotonBeam &beam = causticBeams[hit.second];
		// Segment/ball overlap: the range of beam parameter t for which
		// p0 + t*d lies inside the lookup ball around x. Solving the
		// quadratic gives t = s +/- sqrt(r^2 - d2) where s is the unclamped
		// projection of x on the beam line and d2 the squared point-line
		// distance. End-cap culling is handled by the [lo, hi] interval.
		const float s = Dot(x - beam.p0, beam.d);
		const float d2 = DistanceSquared(x, beam.p0 + s * beam.d);
		if (d2 >= r2)
			continue;

		const float half = sqrtf(r2 - d2);
		const float lo = Max(0.f, s - half);
		const float hi = Min(beam.length, s + half);
		if (hi <= lo)
			continue;

		// The point-photon volume kernel (4/3*pi*r^3) integrated along the
		// beam direction is exactly this overlap length.
		const Spectrum beamT = beam.volume->TransmittanceEstimate(
				Ray(beam.p0, beam.d, 0.f, .5f * (lo + hi)), .5f);

		BSDFEvent event;
		float directPdfW;
		Spectrum bsdfEval = bsdf.Evaluate(-beam.d, &event, &directPdfW, nullptr);
		bsdfEval /= directPdfW;

		// Flux per unit length times the integrated kernel footprint
		result.Add(beam.lightID, beam.alpha * beamT * bsdfEval * ((hi - lo) / beam.length));
	}

	// Kernel-consistent with the point-photon volume estimator (the beam
	// spreads the packet density along its length instead of a point)
	result /= causticPhotonTracedCount * (4.f / 3.f * M_PI * r2 * r);

	assert (result.IsValid());
	return result;
}

SpectrumGroup PhotonGICache::ConnectWithCausticPaths(const BSDF &bsdf) const {
	assert (IsPhotonGIEnabled(bsdf));

	SpectrumGroup result;
	// Volume vertices in homogeneous media are answered by the beam
	// estimator: every caustic point deposit in such a medium also produced
	// a beam, so the two estimates stay disjoint. All other vertices
	// (surfaces, heterogeneous/clear volumes) use the point-photon kernel.
	if (bsdf.IsVolume() && params.caustic.volumeBeams && causticBeamsIndex &&
			dynamic_observer_cast<const HomogeneousVolume>(bsdf.GetMaterial())) {
		result = ConnectCausticBeams(bsdf);
	} else if (causticPhotonsBVH) {
		result = causticPhotonsBVH->ConnectAllNearEntries(bsdf);
	}

	assert (result.IsValid());
	return result;
}

PhotonGISamplerType PhotonGICache::String2SamplerType(const string &type) {
	if (type == "RANDOM")
		return PhotonGISamplerType::PGIC_SAMPLER_RANDOM;
	else if (type == "METROPOLIS")
		return PhotonGISamplerType::PGIC_SAMPLER_METROPOLIS;
	else
		throw runtime_error("Unknown PhotonGI cache sampler type: " + type);
}

string PhotonGICache::SamplerType2String(const PhotonGISamplerType type) {
	switch (type) {
		case PhotonGISamplerType::PGIC_SAMPLER_RANDOM:
			return "RANDOM";
		case PhotonGISamplerType::PGIC_SAMPLER_METROPOLIS:
			return "METROPOLIS";
		default:
			throw runtime_error("Unsupported sampler type in PhotonGICache::SamplerType2String(): " + ToString(type));
	}
}

PhotonGIDebugType PhotonGICache::String2DebugType(const string &type) {
	if (type == "none")
		return PhotonGIDebugType::PGIC_DEBUG_NONE;
	else if (type == "showindirect")
		return PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECT;
	else if (type == "showcaustic")
		return PhotonGIDebugType::PGIC_DEBUG_SHOWCAUSTIC;
	else if (type == "showindirectpathmix")
		return PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECTPATHMIX;
	else
		throw runtime_error("Unknown PhotonGI cache debug type: " + type);
}

string PhotonGICache::DebugType2String(const PhotonGIDebugType type) {
	switch (type) {
		case PhotonGIDebugType::PGIC_DEBUG_NONE:
			return "none";
		case PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECT:
			return "showindirect";
		case PhotonGIDebugType::PGIC_DEBUG_SHOWCAUSTIC:
			return "showcaustic";
		case PhotonGIDebugType::PGIC_DEBUG_SHOWINDIRECTPATHMIX:
			return "showindirectpathmix";
		default:
			throw runtime_error("Unsupported wrap type in PhotonGICache::DebugType2String(): " + ToString(type));
	}
}


// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
