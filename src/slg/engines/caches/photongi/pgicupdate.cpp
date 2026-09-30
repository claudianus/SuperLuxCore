/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <stop_token>

#include "slg/engines/caches/photongi/photongicache.h"

using namespace std;
using namespace luxrays;
using namespace slg;

bool PhotonGICache::Update(const u_int threadIndex, const u_int filmSPP,
		const std::function<void()> &threadZeroCallback) {
	if (!params.caustic.enabled || (params.caustic.updateSpp == 0) || finishUpdateFlag)
		return false;

	// Launch a background re-trace once the update period expires. The
	// worker fills a shadow copy of the caustic cache while render
	// threads keep querying the live one: the update no longer stalls
	// rendering for the whole photon tracing + index rebuild time.
	if ((threadIndex == 0) && !updateInFlight &&
			(initialUpdatePending ||
			// filmSPP is u_int: a film reset mid-session can drop it
			// below lastUpdateSpp and the subtraction would wrap into
			// a huge delta, relaunching a worker every poll
			((filmSPP > lastUpdateSpp) &&
			((filmSPP - lastUpdateSpp) > params.caustic.updateSpp)))) {
		// Saturation backoff (CPU re-trace mode, LUX_PGIC_SATBACKOFF=0
		// disables): a generation retraces the whole photon budget and
		// rebuilds the index - seconds of worker CPU competing with
		// render threads. Once the radius refinement has reached its
		// floor (the compounding pass^-0.02 decay is capped by
		// minLookUpRadius) a swap only replaces the population with an
		// equal-size fresh one at the same radius: statistically
		// identical cache quality, pure waste (e56: ~4s/pass with the
		// radius pinned at its floor). Skip while nothing can change; a
		// scene edit recreates the whole cache (engine restart), so a
		// static session stays correct. GPU ingest mode is untouched
		// (its per-pass cost is an index rebuild only).
		static const bool saturationBackoffOn = !getenv("LUX_PGIC_SATBACKOFF") ||
				(getenv("LUX_PGIC_SATBACKOFF")[0] != '0');
		if (saturationBackoffOn && !initialUpdatePending && !ingestOnly) {
			const float nextRadius = Max(params.caustic.lookUpRadius /
					powf(float(causticPhotonPass + 1),
					.5f * (1.f - params.caustic.radiusReduction)),
					params.caustic.minLookUpRadius);
			if (nextRadius >= params.caustic.lookUpRadius * (1.f - 1e-4f)) {
				if (!saturationBackoffLogged) {
					saturationBackoffLogged = true;
					SLG_LOG("PhotonGI caustic cache lookup radius at its "
							"floor: skipping further update passes "
							"(LUX_PGIC_SATBACKOFF=0 restores)");
				}
				lastUpdateSpp = filmSPP;
				return false;
			}
		}
		// A safety check to avoid the update if visibility map has been
		// deallocated (caustic beams and frustum-culled deposits do not
		// need it)
		if ((visibilityParticles.size() == 0) &&
				!params.caustic.volumeBeams && !UseFrustumCulling()) {
			SLG_LOG("ERROR: Updating PhotonGI caustic cache is not possible without visibility information");
			lastUpdateSpp = filmSPP;
			initialUpdatePending = false;
		} else {
			// Drop leftovers of a previously failed update round. In
			// ingest mode (GPU photon deposits) the shadow buffers hold
			// the accumulated population - refresh them from the live
			// cache so the worker's BVH sees the full set (old + new
			// staging absorb).
			delete updateCausticPhotonsBVH;
			updateCausticPhotonsBVH = nullptr;
			if (ingestOnly) {
				updateCausticPhotons.clear();
				updateCausticPhotons.insert(updateCausticPhotons.end(),
						causticPhotons.begin(), causticPhotons.end());
				updateCausticBeams = causticBeams;
			} else {
				updateCausticPhotons.clear();
				updateCausticBeams.clear();
			}

			updateInFlight = true;
			updateFailed = false;
			updateAbortRequested = false;
			initialUpdatePending = false;
			updateFilmSPP = filmSPP;
			updateCallback = threadZeroCallback;
			// Seed the shadow traced count from the live one: ingest
			// generations accumulate (trace generations reset it
			// inside TracePhotons anyway)
			updateCausticPhotonTracedCount = causticPhotonTracedCount;

			{
				std::lock_guard<std::mutex> lock(updateThreadMutex);
				updateThread = std::make_unique<JThread>(
						[this](std::stop_token stopToken) {
							// Join/reset requests stop on the jthread:
							// forward it to the flag the photon-tracing
							// loops poll (their own jthread token is
							// never signaled - Join() only waits)
							std::stop_callback abortCb(stopToken, [this] {
								updateAbortRequested.store(true);
							});
							UpdateWorker();
						});
			}
		}
	}

	// The deferred initial generation (armed in Preprocess, launched by
	// the first Update) carries no callback: adopt thread 0's callback
	// so the swap still notifies GPU-side recompilation exactly once.
	if ((threadIndex == 0) && updateInFlight && !updateCallback)
		updateCallback = threadZeroCallback;

	if (!updatePendingSwap)
		return false;

	// All threads park here; the barrier completion step performs the
	// shadow-cache swap while no query can be in flight.
	// Two arrivals per participant, paired like FinishUpdate()'s two
	// phases: a barrier phase only completes when every party has
	// arrived, so all barrier entries must come in same-call pairs
	// (Update's pending-swap path and FinishUpdate's drain path
	// contribute 2 each). A single arrival here would consume a
	// FinishUpdate thread's first phase and strand its second one -
	// finishUpdateFlag is set between its phases and blocks every new
	// arrival, hanging engine stop.
	threadsSyncBarrier->arrive_and_wait();
	threadsSyncBarrier->arrive_and_wait();

	return (threadIndex == 0);
}

void PhotonGICache::FinishUpdate(const u_int threadIndex) {
	// Wait for a possibly in-flight background update before the
	// barrier dance; a ready shadow copy is then swapped in by the
	// barrier completion step as usual.
	{
		// All render threads pass through here (and ApplyPendingUpdate
		// may concurrently reset inside this barrier's completion
		// step): serialize the join+reset, the first thread to get
		// the lock performs it
		std::lock_guard<std::mutex> lock(updateThreadMutex);
		if (updateThread)
			updateThread.reset();
	}

	for (;;) {
		if (finishUpdateFlag)
			return;

		threadsSyncBarrier->arrive_and_wait();
		finishUpdateFlag = true;
		threadsSyncBarrier->arrive_and_wait();
	}
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
