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

void PhotonGICache::LaunchUpdate(const u_int filmSPP) {
	// Launch a background re-trace once the update period expires. The
	// worker fills a shadow copy of the caustic cache while render
	// threads keep querying the live one: the update no longer stalls
	// rendering for the whole photon tracing + index rebuild time.
	if (!updateInFlight &&
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
				return;
			}
		}
		// A safety check to avoid the update if visibility map has been
		// deallocated (caustic beams and caustic-only caches do not
		// need it)
		if ((visibilityParticles.size() == 0) &&
				!params.caustic.volumeBeams && !SkipsVisibilityPass()) {
			SLG_LOG("ERROR: Updating PhotonGI caustic cache is not possible without visibility information");
			lastUpdateSpp = filmSPP;
			initialUpdatePending = false;
		} else {
			// Drop leftovers of a previously failed update round. The
			// worker refills the shadow buffers: a CPU re-trace, or (in
			// ingest mode) the GPU deposit window
			delete updateCausticPhotonsBVH;
			updateCausticPhotonsBVH = nullptr;
			updateCausticPhotons.clear();
			updateCausticBeams.clear();

			updateInFlight = true;
			updateFailed = false;
			updateAbortRequested = false;
			initialUpdatePending = false;
			updateFilmSPP = filmSPP;
			updateCallback = nullptr;
			updateCausticPhotonTracedCount = 0;

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
}

bool PhotonGICache::Update(const u_int threadIndex, const u_int filmSPP,
		const std::function<void()> &threadZeroCallback) {
	if (!params.caustic.enabled || (params.caustic.updateSpp == 0) || finishUpdateFlag)
		return false;

	if (threadIndex == 0)
		LaunchUpdate(filmSPP);

	// LaunchUpdate() leaves the callback empty (the deferred initial
	// generation is launched with none either): adopt thread 0's
	// callback so the swap still notifies GPU-side recompilation
	// exactly once.
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
	// contribute 2 each, FinishUpdate drops its seat with its single
	// arrival). A single arrival here would leave the parties out of
	// step and strand the second phase.
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

	// Stop new update rounds, then leave the barrier for good. A
	// finished thread must not keep a seat: the old "arrive twice,
	// return once the flag is up" exit let a peer that was still
	// rendering (a GPU thread mid-batch, a generation swap landing
	// after the CPU threads halted) arrive at a later swap alone and
	// hang engine stop forever (e102 zero-config PATHOCL). The drop
	// counts as this phase's arrival, so peers parked in Update()'s
	// pair still complete it, and every later phase needs only the
	// threads still rendering. Every party drops at most once per
	// barrier (threads restarted without Preprocess() find updates
	// disabled by the flag and the seats gone).
	finishUpdateFlag = true;
	if (finishDroppedParties.fetch_add(1) < threadCount)
		threadsSyncBarrier->arrive_and_drop();
}

//------------------------------------------------------------------------------
// Barrier-free update protocol (RTPATHCPU viewport)
//------------------------------------------------------------------------------

// The RT engine renders through its own pause barrier and restarts its
// film on every edit: its threads never enter threadsSyncBarrier. The
// main thread polls instead (RTPathCPURenderEngine::UpdateFilmLockLess)
// and swaps with the render threads parked on the engine's barrier.
bool PhotonGICache::PollUpdate(const u_int filmSPP) {
	if (!params.caustic.enabled || (params.caustic.updateSpp == 0) || finishUpdateFlag)
		return false;

	LaunchUpdate(filmSPP);

	return updatePendingSwap;
}

void PhotonGICache::AbortUpdate() {
	{
		// Stop + join: the trace loops poll updateAbortRequested and
		// the worker discards the generation (UpdateWorker clears
		// updateInFlight). A worker that had already finished leaves
		// its swap pending (and updateInFlight up until the swap: a
		// relaunch would clear the shadow buffers under it).
		std::lock_guard<std::mutex> lock(updateThreadMutex);
		updateThread.reset();
	}

	// An aborted first generation must be relaunched: nothing else
	// would ever arm it again
	if (!IsCausticReady() && !updatePendingSwap)
		initialUpdatePending = true;
}

void PhotonGICache::RestartUpdateClock() {
	// The film restarts from 0 spp (an edit, or the viewport reset on
	// the first generation): the period counts from there
	lastUpdateSpp = 0;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
