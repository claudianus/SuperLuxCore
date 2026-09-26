/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 *   you may not use this file except in compliance with the License.       *
 *   You may obtain a copy of the License at                               *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                           *
 ***************************************************************************/

#ifndef _SLG_LIGHTSTRATEGY_RESTIRDI_H
#define	_SLG_LIGHTSTRATEGY_RESTIRDI_H

#include "slg/lights/strategies/logpower.h"

#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace slg {

//------------------------------------------------------------------------------
// LightStrategyRestirDI
//
// Resampled Importance Sampling for direct lighting ("Spatiotemporal
// Reservoir Resampling for Real-time Ray Tracing with Dynamic Direct
// Lighting" / ReSTIR DI).
//
// Stage 1: reservoir sampling over power-based candidates.
// Stage 2: IsAlwaysInShadow() zero-weight culling.
// Stage 3: contribution-aware target via Illuminate() with BSDF context
//          (measured: ~4.1x quality-per-time on manylights).
// Stage 4 (this): SPATIAL reservoir reuse. Each shade point merges the
//          finished reservoirs from nearby cells of a per-thread hash
//          grid into its own candidate stream, re-evaluating each
//          neighbor's light against the CURRENT point's target
//          function (unbiased: the neighbor's light is just another
//          candidate drawn with its own source pdf). Cells also
//          persist across passes, so earlier winners keep entering
//          later candidate streams (candidate-level temporal reuse).
//          Thread safety: per-thread grids, no locks; a thread only
//          ever writes its own grid and reads grids of OTHER threads
//          whose entries are light pointers + floats (a torn read can
//          at worst pick a stale light, which stays an unbiased
//          candidate because the pdf is re-evaluated here).
// Stage 5 (this): unbiased RIS output weight. The estimator is the
//          ReSTIR paper's W-form: (wSum/M) * g(tau)/target(tau) with
//          g = f/(q*dpw). Three properties make it correct:
//          1. The target carries the geometry term 1/directPdfW from
//             Illuminate() - without it a close dim light can win with
//             a tiny selection probability and the compensation
//             explodes (was: ~1600x bright blobs on manylights-4spot).
//          2. M is the TOTAL proposal-draw count (effectiveCandidate-
//             Count), including candidates culled to zero target -
//             counting only survivors inflates the mean by M/M_surv
//             (was: 4x on manylights-4spot).
//          3. The RIS factor travels in a separate risScale out-param,
//             NOT folded into *pdf: the pick pdf stays the source q so
//             the direct-hit MIS (which uses SampleLightPdf() = q)
//             stays consistent - folding it into the pdf de-synchronizes
//             the two MIS sides (was: ~10% mean offset on mesh-light
//             scenes). Accept decisions use hashed per-call randoms so
//             they don't pair deterministically with the stratified
//             proposal draws.
//          (True M-coordinate temporal reservoirs with GRIS MIS
//          weights need per-pixel reservoir plumbing through the path
//          tracer API - scheduled with the channel-count refactor.)
//------------------------------------------------------------------------------

class LightStrategyRestirDI : public LightStrategyLogPower {
public:
	// candidates == 0 selects adaptive candidate count (scaled with the
	// number of directly-sampleable lights in Preprocess)
	LightStrategyRestirDI(const u_int candidates = 0);

	virtual ~LightStrategyRestirDI();

	virtual void Preprocess(SceneConstRef scene, const LightStrategyTask taskType,
			const bool useRTMode);

	// ReSTIR only changes TASK_ILLUMINATE; emission keeps the
	// power-based distribution from the parent class
	virtual LightSourcePtr SampleLights(
			SceneConstRef scene,
			const float u,
			const luxrays::Point &p, const luxrays::Normal &n,
			const bool isVolume,
			float *pdf) const;

	// Stage 3+4: contribution-aware reservoir with BSDF context and
	// spatial/temporal reuse through the per-thread reservoir grid.
	// *pdf returns the source selection pdf (MIS-consistent with
	// SampleLightPdf); the RIS output weight W/(M*target) is returned
	// through *risScale.
	virtual LightSourcePtr SampleLightsBSDF(
			SceneConstRef scene,
			const BSDF &bsdf,
			const float time,
			const float u,
			float *pdf,
			float *risScale = nullptr,
			float *lightSurfaceUs = nullptr) const;

	virtual LightStrategyType GetType() const { return GetObjectType(); }
	virtual std::string GetTag() const { return GetObjectTag(); }

	//--------------------------------------------------------------------------
	// Static methods used by LightStrategyRegistry
	//--------------------------------------------------------------------------

	static LightStrategyType GetObjectType() { return TYPE_RESTIR_DI; }
	static std::string GetObjectTag() { return "RESTIR_DI"; }
	static luxrays::PropertiesUPtr ToProperties(const luxrays::Properties &cfg);
	static LightStrategyUPtr FromProperties(const luxrays::Properties &cfg);
	static luxrays::PropertiesUPtr GetDefaultProps();

	u_int GetCandidateCount() const { return candidateCount; }
	u_int GetEffectiveCandidateCount() const { return effectiveCandidateCount; }

	// P1-1: temporal reuse is currently a GPU-side feature (the per-pixel
	// reservoir buffer of the PATHOCL kernels). The CPU strategy exposes
	// the flag so the GPU config mirrors the scene configuration.
	bool IsTemporalReuseEnabled() const { return temporalReuseEnable; }
	// Stage 4 spatial merge flag, consumed by the GPU kernels
	// (lightstrategy.restir.spatialreuse.enable, default false)
	bool IsSpatialReuseEnabled() const { return spatialReuseEnable; }
	// E2a: visibility-weighted RIS target (traces K candidate shadow
	// rays; GPU: through the shared rays buffer resolved by the
	// MK_RT_RESTIR micro-kernel; CPU: traced inline on the scene
	// accelerator). (lightstrategy.restir.visibility.enable, default false)
	bool IsVisibilityEnabled() const { return visibilityEnable; }

	// Stage 4 stats (accessed by tests/benchmarks)
	u_int GetSpatialReuseHits() const { return spatialReuseHits.load(); }
	u_int GetSpatialReuseQueries() const { return spatialReuseQueries.load(); }

protected:
	// Configured candidate count; 0 means "adaptive" (see Preprocess)
	u_int candidateCount;
	// Candidate count actually used this render (set in Preprocess)
	u_int effectiveCandidateCount;

	// ---- Stage 4: spatial/temporal reservoir cache ----
	// A finished reservoir stored per hash-grid cell.
	struct ReservoirEntry {
		u_int lightIndex;        // scene light index (NULL_CELL_LIGHT = empty)
		float wSum;              // sum of target weights seen
		u_int sampleCount;        // candidates merged into it (M)
		// Winner's target at the storing point (GRIS spatial merge
		// normalizes the reused sum by old->new target ratio)
		float targetAtStore;
		// Winner's light-surface sample (E2c reconnection shift, GPU
		// parity): the merge replays the SAME emitter point at the
		// current shade point, so pi_new/pi_old tracks the shading
		// change only. Zero-filled entries replay (0,0,0) - the old
		// fixed-point behaviour.
		float lsU, lsV, lsP;
	};
	static constexpr u_int NULL_CELL_LIGHT = 0xffffffffu;

	// Open-addressing hash grid keyed by floor(worldPos / cellSize).
	// One grid per render thread: written only by its owner, read by
	// everyone (entries are plain data).
	static constexpr u_int GRID_HASH_SIZE = 1u << 14; // 16384 buckets
	struct ReservoirGrid {
		std::array<ReservoirEntry, GRID_HASH_SIZE> buckets;
		ReservoirGrid();
	};

	// Fixed cell size: lights visible from one point stay highly
	// correlated within this radius in most production scenes.
	static constexpr float CELL_SIZE = .5f;

	// Set in Preprocess (grids are allocated per active thread)
	std::vector<std::unique_ptr<ReservoirGrid>> grids;
	mutable std::atomic<u_int> spatialReuseHits, spatialReuseQueries;

	// Stage 4 spatial merge is opt-in (GRIS pairwise combine)
	bool spatialReuseEnable;

	// P1-1: temporal reuse flag, consumed by the GPU kernels
	// (lightstrategy.restir.temporal.enable, default false)
	bool temporalReuseEnable;

	// E2a: visibility-weighted target flag, consumed by the GPU kernels
	// (lightstrategy.restir.visibility.enable, default false)
	bool visibilityEnable;

	u_int GetThreadGridIndex() const;
	ReservoirGrid *GetThreadGrid() const;

	static u_int GridHash(const int x, const int y, const int z);
};


}

#endif	/* _SLG_LIGHTSTRATEGY_RESTIRDI_H */
