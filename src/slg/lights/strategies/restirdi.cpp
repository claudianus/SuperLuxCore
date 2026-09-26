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

#include "slg/lights/strategies/restirdi.h"
#include "slg/samplers/sobolsequence.h"
#include "luxrays/utils/properties.h"
#include "luxrays/core/dataset.h"
#include "slg/scene/scene.h"
#include "slg/bsdf/bsdf.h"

#include <memory>
#include <thread>
#include <cstring>

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Stage 4 infrastructure: per-thread reservoir grids
//------------------------------------------------------------------------------

LightStrategyRestirDI::ReservoirGrid::ReservoirGrid() {
	for (auto &e : buckets) {
		e.lightIndex = NULL_CELL_LIGHT;
		e.wSum = 0.f;
		e.sampleCount = 0;
		e.targetAtStore = 0.f;
		e.lsU = e.lsV = e.lsP = 0.f;
	}
}

// Round the thread count up to what the render can use; grids are
// allocated generously so late-spawned threads also find a slot.
u_int LightStrategyRestirDI::GetThreadGridIndex() const {
	// hash the thread id into a stable slot in [0, grids.size())
	static thread_local const size_t tid = std::hash<std::thread::id>()(
			std::this_thread::get_id());
	return (u_int)(tid % Max(1ul, grids.size()));
}

LightStrategyRestirDI::ReservoirGrid *LightStrategyRestirDI::GetThreadGrid() const {
	if (grids.empty())
		return nullptr;
	return grids[GetThreadGridIndex()].get();
}

u_int LightStrategyRestirDI::GridHash(const int x, const int y, const int z) {
	// integer hash (xorshift-style mixing), mapped into the bucket range
	u_int h = (u_int)x * 73856093u ^ (u_int)y * 19349663u ^ (u_int)z * 83492791u;
	h ^= h >> 16;
	h *= 2246822519u;
	h ^= h >> 13;
	return h & (GRID_HASH_SIZE - 1u);
}

//------------------------------------------------------------------------------
// LightStrategyRestirDI
//------------------------------------------------------------------------------

LightStrategyRestirDI::LightStrategyRestirDI(const u_int candidates) :
		LightStrategyLogPower(), candidateCount(candidates),
		effectiveCandidateCount(Max(1u, candidates)),
		spatialReuseHits(0), spatialReuseQueries(0),
		spatialReuseEnable(false), temporalReuseEnable(false),
		visibilityEnable(false) {
	// Stage 4: allocate one grid per potential render thread. Threads
	// pick a grid by hashing their thread id, so two threads may share
	// a grid only when there are more threads than grids; sharing is
	// still safe (entries are data-only, worst case a stale light read
	// as an extra unbiased candidate).
	const u_int hwThreads = Max(1u, std::thread::hardware_concurrency() * 2u);
	grids.reserve(hwThreads);
	for (u_int i = 0; i < hwThreads; ++i)
		grids.push_back(std::make_unique<ReservoirGrid>());
}

LightStrategyRestirDI::~LightStrategyRestirDI() {
}

void LightStrategyRestirDI::Preprocess(SceneConstRef scene,
		const LightStrategyTask taskType, const bool useRTMode) {
	// The candidate distribution is the LOG_POWER distribution; the
	// reservoir logic in SampleLights() does the resampling on top.
	LightStrategyLogPower::Preprocess(scene, taskType, useRTMode);

	// Stage 4: clear the reservoir grids for a fresh render (a reused
	// session must not leak reservoirs from a previous scene)
	for (auto &g : grids) {
		for (auto &e : g->buckets) {
			e.lightIndex = NULL_CELL_LIGHT;
			e.wSum = 0.f;
			e.sampleCount = 0;
			e.lsU = e.lsV = e.lsP = 0.f;
		}
	}
	spatialReuseHits.store(0);
	spatialReuseQueries.store(0);

	// Stage 4: adapt the candidate count to the scene. With few lights,
	// a fixed count of 8 mostly resamples the same light over and over
	// (pure overhead); with many lights, more candidates mean better
	// coverage of the contribution landscape. We scale with the
	// number of directly-sampleable lights, clamped to [2, 32].
	// (candidateCount of 1 disables resampling - not desired, so min 2.)
	const u_int lightCount = scene.GetLightSources().GetSize();
	u_int directLightCount = 0;
	for (u_int i = 0; i < lightCount; ++i) {
		if (scene.GetLightSources().GetLightSource(i).IsDirectLightSamplingEnabled())
			++directLightCount;
	}

	// Heuristic: sqrt scaling - a handful of lights -> ~2-3 candidates,
	// 100 lights -> ~17, 1000+ -> capped at 32 (diminishing returns;
	// each candidate costs an Illuminate() evaluation)
	u_int adaptive = 2 + (u_int)(sqrtf((float)directLightCount) * 1.5f);
	// An explicit lightstrategy.restir.candidates > 0 overrides the
	// heuristic; 0 (the default) means adaptive
	if (candidateCount > 0)
		adaptive = candidateCount;
	effectiveCandidateCount = Clamp(adaptive, 2u, 32u);
}


namespace {
// Decorrelated accept randoms. The proposal draws are stratified over
// u (u_i = u + i/M), so an accept test keyed directly on u would pair
// deterministically with the candidate identities and bias the
// reservoir (observed as a ~10% mean offset on scenes with many
// competing mesh lights). Hashing (u, per-call counter, candidate
// index) breaks that coupling.
inline u_int AcceptSeed(const float u, const u_int callCounter) {
	u_int uBits;
	std::memcpy(&uBits, &u, sizeof(uBits));
	return SobolSequence::BlueNoiseHash(uBits ^ (callCounter * 0x9E3779B9u));
}
inline float AcceptRand(const u_int seed, const u_int i) {
	return SobolSequence::BlueNoiseHash(seed ^ (i * 0x85EBCA6Bu)) *
			(1.f / 4294967296.f);
}
}

LightSourcePtr LightStrategyRestirDI::SampleLights(
		SceneConstRef scene,
		const float u, const Point &p, const Normal &n,
		const bool isVolume, float *pdf) const {
	// Without a BSDF there is no contribution target to resample
	// against: the old flat-target reservoir (w = 1/q, i.e. p-hat = 1)
	// converges to a UNIFORM light pick compensated by W = wSum/M -
	// strictly worse than the log-power proposal it draws from, and it
	// still pays effectiveCandidateCount proposal evaluations for it.
	// Plain log-power sampling is the better proposal here.
	return LightStrategyLogPower::SampleLights(
			scene, u, p, n, isVolume, pdf);
}

LightSourcePtr LightStrategyRestirDI::SampleLightsBSDF(
		SceneConstRef scene, const BSDF &bsdf, const float time,
		const float u, float *pdf, float *risScale,
		float *lightSurfaceUs) const {
	// Stage 3: contribution-aware reservoir. Each candidate's target
	// weight is its estimated direct contribution at this shade point:
	// Illuminate() evaluates radiance x geometry (it builds the shadow
	// ray but does not trace it), which is exactly the ReSTIR target
	// function. The cost is one Illuminate() per candidate - bounded
	// by candidateCount and still far cheaper than the shadow rays a
	// wrong pick would waste.
	//
	// Stage 4: SPATIAL/TEMPORAL REUSE. Before running the fresh
	// candidate loop, the reservoirs stored in the 3x3x3 neighborhood
	// of hash-grid cells around this shade point are merged in as
	// EXTRA candidates. Each neighbor's stored light is re-evaluated
	// against the CURRENT point's target (Illuminate() here), with its
	// own source pdf, so the estimator stays unbiased - a reused
	// light is just another weighted candidate. This is what makes
	// ReSTIR tick: points near each other have correlated light
	// contributions, so yesterday's winners are today's good guesses.
	// Because cells persist for the whole render, later passes
	// automatically reuse earlier ones (temporal reuse).

	const Normal landingNormal = bsdf.hitPoint.intoObject ?
		bsdf.hitPoint.shadeN : -bsdf.hitPoint.shadeN;

	ReservoirGrid *grid = GetThreadGrid();

	float wSum = 0.f;
	LightSourcePtr reservoirLight = nullptr;
	float reservoirPdf = 0.f;
	float reservoirTarget = 0.f;
	// Winning FRESH candidate's light-surface sample (visibility path):
	// the candidate's own sample doubles as the contribution sample, so
	// it is returned through lightSurfaceUs and the caller's payoff
	// Illuminate() reuses it verbatim - the binary V term folded into
	// its target and the payoff then cover exactly the same surface
	// point. Merged winners carry no stored sample: they are
	// re-evaluated with the caller's own sample (same semantics as the
	// GPU resolve path), so the out-parameter stays untouched.
	float winU1 = 0.f, winU2 = 0.f, winU3 = 0.f;
	bool winnerIsFresh = false;

	static thread_local u_int acceptCounter = 0;
	const u_int acceptSeed = AcceptSeed(u, acceptCounter++);

	// ---- Stage 4: GRIS spatial merge of neighbor reservoirs ----
	// A neighbor's winner was NOT drawn from this point's proposal q, so
	// treating it as one more q-draw (the old code) over-represents
	// lights that win often nearby: deterministic spots inflated by the
	// copy count. The GRIS-correct combine (Bitterli et al.) resamples
	// between the reservoirs with MIS weights built from each side's
	// output weight W (= wSum/(M*target), proposal-normalized) times the
	// target re-evaluated HERE:
	//   b_nbr = M_nbr * W_nbr * pi_new(tau_nbr)
	//         = wSum_nbr * (pi_new / pi_old)
	//   P(take nbr) = b_nbr / (wSum_cur + b_nbr)
	//   wSum = wSum_cur + b_nbr, M = M_cur + M_nbr
	// Fresh candidates are the M=1 special case of the same rule, so the
	// stream below is untouched. No shift mapping is applied (first-order
	// spatial reuse): exact when neighbor targets match, negligible drift
	// otherwise at 0.5m cells. Stays behind
	// lightstrategy.restir.spatialreuse.enable (default off).
	static constexpr u_int MAX_NEIGHBOR_MERGES = 2;
	// Statistical sample count: ACTUAL fresh draws below (incl. culled)
	// plus reused neighbor counts. (Claiming the full budget here while
	// merges displace fresh draws undercounts W = wSum/M = dark bias.)
	u_int mTotal = 0;
	u_int mergeCount = 0;

	// The own-cell merge (offset 0) is the CPU counterpart of the GPU
	// per-pixel temporal reservoir: the same bucket is re-read across
	// passes at nearby shade points. Neighbour cells are the spatial
	// merge. temporal.enable alone therefore runs the own-cell merge
	// too (previously the flag was parsed but never consumed - a
	// silent no-op on CPU while the GPU honoured it).
	if (grid && (spatialReuseEnable || temporalReuseEnable)) {
		const Point &p = bsdf.hitPoint.p;
		const int cx = (int)floorf(p.x / CELL_SIZE);
		const int cy = (int)floorf(p.y / CELL_SIZE);
		const int cz = (int)floorf(p.z / CELL_SIZE);

		// Visit ONLY the cell containing this point (plus 6 face
		// neighbors when empty). A 27-cell sweep costs 27 hash probes
		// per shade point even when nearly all cells are empty - the
		// probe loop itself became the bottleneck at 100+ lights.
		static const int OFFS[7][3] = {
			{0, 0, 0}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
			{0, -1, 0}, {0, 0, 1}, {0, 0, -1},
		};
		u_int merges = 0;
		// M2c-era experiment (env LUX_RIS_TEMPORAL_ONLY=1): merge only the
		// entry in THIS cell (temporal history, same target distribution),
		// skip neighbor buckets (spatial reuse needs shifts to pay).
		static const bool kTemporalOnly = (getenv("LUX_RIS_TEMPORAL_ONLY") != nullptr);
		// With visibility-weighted targets the GPU kernels merge only the
		// temporal reservoir and skip cross-cell neighbor merges entirely:
		// the candidates' exact-sample V terms do not transfer across
		// shading points, and the neighbors' V-free target re-evaluation
		// would mix two different target measures into one reservoir (it
		// also amplified stale wSum feedback into runaway output weights).
		// Mirror that here: under visibility the own-cell merge (the CPU
		// temporal equivalent - same bucket across passes) still runs.
		const int oMax = (kTemporalOnly || visibilityEnable ||
				!spatialReuseEnable) ? 1 : 7;
		for (int o = 0; o < oMax && merges < MAX_NEIGHBOR_MERGES; ++o) {
			const int dx = OFFS[o][0], dy = OFFS[o][1], dz = OFFS[o][2];
			{
				const u_int slot = GridHash(cx + dx, cy + dy, cz + dz);
					const ReservoirEntry &entry = grid->buckets[slot];
					if (entry.lightIndex == NULL_CELL_LIGHT)
						continue;

					// Resolve the stored light through the scene index
					if (entry.lightIndex >= scene.GetLightSources().GetSize())
						continue;
					LightSourcePtr nbLight = scene.GetLightSources().
							GetLightSourcePtr(entry.lightIndex);
					if (!nbLight)
						continue;
					// Unusable normalization: skip (a zero old target
					// would divide by zero below).
					if (!(entry.targetAtStore > 0.f) || entry.sampleCount == 0u)
						continue;

					spatialReuseQueries.fetch_add(1, std::memory_order_relaxed);

					// Source pdf: the same power-based pdf the reservoir
					// originally drew this light with (the engine's own
					// per-light pdf query)
					const float nbSourcePdf = SampleLightPdf(
							*nbLight, p, landingNormal, bsdf.IsVolume());
					if (nbSourcePdf <= 0.f)
						continue;

					if (nbLight->IsAlwaysInShadow(scene, p, landingNormal))
						continue;

					// Reconnection shift (E2c, GPU parity): replay the
					// stored winner's light-surface sample so pi_new is
					// measured at the same emitter point that produced
					// pi_old - the ratio tracks the shading change only.
					// Entries predating the store of ls* carry (0,0,0),
					// the old fixed-point behaviour.
					Ray nbShadowRay;
					float nbDirectPdfW;
					const Spectrum nbRadiance = nbLight->Illuminate(
							scene, bsdf, time,
							entry.lsU, entry.lsV, entry.lsP,
							nbShadowRay, nbDirectPdfW);
					if (nbRadiance.Black() || nbDirectPdfW <= 0.f)
						continue;

					// Target re-evaluated HERE (pi_new); the entry's
					// targetAtStore is pi_old at the storing point.
					const float nbTargetNew = nbRadiance.Y() /
							(nbSourcePdf * nbDirectPdfW);
					if (nbTargetNew <= 0.f)
						continue;

					// GRIS combine weight: neighbor sum converted to
					// this point's target measure. NO ratio clamp here:
					// dropping merges by weight is biased (dark). The
					// tiny-old-target pathology is handled at the STORE
					// instead (unrepresentative winners are not shared).
					const float bNbr = entry.wSum *
							(nbTargetNew / entry.targetAtStore);

					wSum += bNbr;
					mTotal += entry.sampleCount;
					++merges;
					++mergeCount;

					const float accept = (wSum > 0.f) ?
							(bNbr / wSum) : 0.f;
					const float r = AcceptRand(acceptSeed,
							1000u + (dx + 3) + 7u * (dy + 3) + 49u * (dz + 3));
					if (!reservoirLight || r < accept) {
						reservoirLight = nbLight;
						reservoirPdf = nbSourcePdf;
						reservoirTarget = nbTargetNew;
						// Propagate the replayed sample so the payoff
						// Illuminate() and a later store stay attached
						// to the point the merge evaluated
						winU1 = entry.lsU;
						winU2 = entry.lsV;
						winU3 = entry.lsP;
						winnerIsFresh = false;
						spatialReuseHits.fetch_add(1, std::memory_order_relaxed);
					}
				}
		}
	}

	// Fresh candidate stream. The neighbor merges above already spent
	// the same Illuminate() budget that MAX_NEIGHBOR_MERGES fresh
	// candidates would have; subtract them so the TOTAL evaluation
	// count per shade point stays at effectiveCandidateCount. This
	// keeps the per-pass cost at stage-3 levels while the reservoir
	// still benefits from proven-good neighbor picks.
	// M2c-era experiment (env LUX_RIS_NOBUDGET=1): skip the budget
	// subtraction (full fresh stream + merges on top). Costs extra
	// Illuminates; tests whether reuse carries ANY information here.
	static const bool kNoBudget = (getenv("LUX_RIS_NOBUDGET") != nullptr);
	const u_int mergesSpent = kNoBudget ? 0u : mergeCount; // == number of merged neighbors
	u_int candCountBSDF = (effectiveCandidateCount > mergesSpent) ?
			(effectiveCandidateCount - mergesSpent) : 1u;
	for (u_int i = 0; i < candCountBSDF; ++i) {
		++mTotal; // every fresh draw counts (culled ones too)
		const float u_i = fmod(u + i * (1.f / candCountBSDF), 1.f);

		float candidatePdf;
		LightSourcePtr candidate = LightStrategyLogPower::SampleLights(
				scene, u_i, bsdf.hitPoint.p, landingNormal,
				bsdf.IsVolume(), &candidatePdf);

		if (!candidate || candidatePdf <= 0.f)
			continue;

		// Provably occluded candidates can never contribute.
		// DIAG (env LUX_RIS_NOCULL=1): skip the cull to test whether it
		// wrongly removes contributing lights (clustered dark bias suspect).
		static const bool kNoCull = (getenv("LUX_RIS_NOCULL") != nullptr);
		if (!kNoCull && candidate->IsAlwaysInShadow(scene, bsdf.hitPoint.p, landingNormal))
			continue;

		// Contribution target: the light's radiance x geometry at this
		// point. Black means the light cannot reach the surface here at
		// all -> zero weight.
		//
		// Visibility-weighted variant (E2a, GPU parity): the candidate's
		// OWN light-surface sample doubles as the eventual contribution
		// sample - if it wins, its sample is returned to the caller and
		// reused verbatim for the payoff Illuminate(), so the binary V
		// tested below covers exactly the point the payoff evaluates
		// (re-sampling the winner would decouple them: a real bias on
		// area/mesh lights). Each candidate needs a proper independent
		// 3D sample hashed off (u, i) - the same SobolSequence blue-noise
		// hash scheme as the GPU kernel - instead of the (u_i,0,0)
		// shorthand (a fixed corner on area lights) or one shared sample
		// (correlated targets, worse variance).
		float su = u_i, sv = 0.f, sp = 0.f;
		if (visibilityEnable) {
			u_int uBits;
			std::memcpy(&uBits, &u, sizeof(uBits));
			const u_int h = SobolSequence::BlueNoiseHash(
					uBits ^ (i * 0x9E3779B9u + 0x27D4EB2Fu));
			su = SobolSequence::BlueNoiseHash(h ^ 0x165667B1u) *
					(1.f / 4294967296.f);
			sv = SobolSequence::BlueNoiseHash(h ^ 0x9E3779B9u) *
					(1.f / 4294967296.f);
			sp = SobolSequence::BlueNoiseHash(h ^ 0x85EBCA6Bu) *
					(1.f / 4294967296.f);
		}

		Ray shadowRay;
		float directPdfW;
		const Spectrum lightRadiance = candidate->Illuminate(
				scene, bsdf, time, su, sv, sp, shadowRay, directPdfW);

		if (lightRadiance.Black())
			continue;

		// Unnormalized target ~ radiance x GEOMETRY / pdf_source. The
		// geometry factor (1/directPdfW) is ESSENTIAL: without it the
		// target ignores distance/attenuation, a close but dim light can
		// win the reservoir with a tiny selection probability and the
		// 1/P(tau) compensation explodes (observed as ~1600x bright
		// blobs on the manylights-4spot scene). With it the target
		// tracks the actual contribution and the estimator variance
		// stays bounded.
		const float lum = lightRadiance.Y();
		if (directPdfW <= 0.f)
			continue;

		// Binary visibility at the candidate's own surface point. The
		// ray Illuminate() built already ends at the sampled light
		// point (maxt), so a hit means occluded -> V = 0 -> culled
		// candidate (still counted in mTotal above, like the other
		// culls). Traced on the scene's Embree accelerator - a plain
		// any-hit test; the winner's contribution still goes through
		// the full transparent-shadow path, so alpha/shadow-catcher
		// behavior is unchanged.
		if (visibilityEnable) {
			RayHit shadowRayHit;
			if (scene.GetDataSet().GetAccelerator(ACCEL_EMBREE)->
					Intersect(&shadowRay, &shadowRayHit))
				continue;
		}

		const float targetWeight = lum / (candidatePdf * directPdfW);

		wSum += targetWeight;

		// Weighted reservoir update (single-pass algorithm from the
		// ReSTIR paper): accept candidate i with prob w_i / wSum
		const float accept = (wSum > 0.f) ? (targetWeight / wSum) : 0.f;
		const float r = AcceptRand(acceptSeed, i);
		if (!reservoirLight || r < accept) {
			reservoirLight = candidate;
			reservoirPdf = candidatePdf;
			reservoirTarget = targetWeight;
			winU1 = su;
			winU2 = sv;
			winU3 = sp;
			winnerIsFresh = true;
		}
	}

	// Cap reuse counts at 2x (measured: 20x lets stale winners dominate
	// ~95% and T2 degrades 1.14x -> 1.39x; without shifts, reused winners
	// are often irrelevant no matter how big M gets). Rescale keeps
	// W = wSum/M exact. Done BEFORE the store so entries never carry
	// runaway counts either.
	// NOTE: mTotal counts actual fresh draws (incl. culled) plus reused
	// neighbor counts; wSum only sums evaluated mass. Both sides of
	// the cap keep the output weight consistent.
	{
		const u_int mCap = 2u * Max(effectiveCandidateCount, 1u);
		if (mTotal > mCap) {
			wSum *= (float)mCap / (float)mTotal;
			mTotal = mCap;
		}
	}

	// ---- Stage 4: store the finished reservoir for future reuse ----
	// Only representative winners are shared: a tiny winner target
	// under normal mass makes W = wSum/(M*target) astronomical, and the
	// next merge divides by it (1e26 image means observed). Not storing
	// cannot bias anyone (fewer future candidates, all counted).
	// The store is also skipped when no reuse mode is on - nothing
	// would ever read it back.
	if (grid && (spatialReuseEnable || temporalReuseEnable) &&
			reservoirLight && wSum > 0.f &&
			reservoirTarget >= .05f * wSum / (float)Max(mTotal, 1u)) {
		const Point &p = bsdf.hitPoint.p;
		const u_int slot = GridHash(
				(int)floorf(p.x / CELL_SIZE),
				(int)floorf(p.y / CELL_SIZE),
				(int)floorf(p.z / CELL_SIZE));
		ReservoirEntry &entry = grid->buckets[slot];
		entry.lightIndex = reservoirLight->lightSceneIndex;
		entry.wSum = wSum;
		entry.sampleCount = mTotal;
		entry.targetAtStore = reservoirTarget;
		// E2c: keep the winning sample's light-surface draws so a future
		// merge replays the same emitter point (reconnection shift)
		entry.lsU = winU1;
		entry.lsV = winU2;
		entry.lsP = winU3;
	}

	if (reservoirLight) {
		// Stage 5: RIS output weight (Bitterli et al. 2020, W = wSum/M).
		// The estimator the path tracer must build is
		//   (wSum / (M * target(tau))) * g(tau)
		// with g = f/(q*dpw): unbiased for the full direct integral with
		// bounded variance (target ~ g/bsdf). M is the TOTAL number of
		// proposal draws (candidates culled to zero target contribute 0
		// to wSum but still count in M) = effectiveCandidateCount.
		//
		// The RIS factor travels in *risScale, NOT in *pdf: the MIS on
		// intersectable/environment lights pairs this estimator with the
		// direct-hit estimator whose density uses SampleLightPdf() = q.
		// Each side is individually unbiased and the MIS weights sum to
		// 1, so the combination stays unbiased; folding the RIS factor
		// into *pdf would de-synchronize the two MIS sides (observed as
		// a ~10% mean offset on mesh-light scenes).
		if (pdf)
			*pdf = reservoirPdf;
		if (risScale)
			*risScale = wSum / ((float)Max(mTotal, 1u) * reservoirTarget);
		// Visibility path: hand the caller the winning sample's
		// light-surface draws so the payoff Illuminate() reuses the
		// exact point the V term covered. Fresh winners carry their
		// candidate sample; merged winners the stored reservoir sample
		// replayed by the merge (E2c) - either way V and the payoff
		// stay attached to the same emitter point.
		if (lightSurfaceUs && visibilityEnable) {
			lightSurfaceUs[0] = winU1;
			lightSurfaceUs[1] = winU2;
			lightSurfaceUs[2] = winU3;
		}
		// DIAG GRIS explosion (revert): trap absurd output weights
		{
			static const bool kTrap = (getenv("LUX_RIS_TRAP") != nullptr);
			if (kTrap && *risScale > 1e6f) {
				static std::atomic<int> kCount{0};
				if (kCount.fetch_add(1) < 5)
					fprintf(stderr, "[RIS] risScale=%.4g wSum=%.4g mTotal=%u tgt=%.4g pdf=%.4g merges=%u\n",
						*risScale, wSum, mTotal, reservoirTarget, reservoirPdf, mergeCount);
			}
		}

		return reservoirLight;
	}

	// Empty reservoir (no contributing candidate) - fall back
	return nullptr;
}

// Static methods used by LightStrategyRegistry

PropertiesUPtr LightStrategyRestirDI::ToProperties(const Properties &cfg) {
	PropertiesUPtr props = std::make_unique<Properties>();

	*props <<
				cfg.Get(GetDefaultProps()->Get("lightstrategy.type")) <<
				cfg.Get(GetDefaultProps()->Get("lightstrategy.restir.candidates")) <<
				cfg.Get(GetDefaultProps()->Get("lightstrategy.restir.spatialreuse.enable")) <<
				cfg.Get(GetDefaultProps()->Get("lightstrategy.restir.temporal.enable")) <<
				cfg.Get(GetDefaultProps()->Get("lightstrategy.restir.visibility.enable"));

	return props;
}

LightStrategyUPtr LightStrategyRestirDI::FromProperties(const Properties &cfg) {
	// 0 (the default) selects the adaptive candidate count
	const u_int candidates = cfg.Get(
		Property("lightstrategy.restir.candidates")(0)
	).Get<u_int>();
	// Stage 4 spatial merge (EXPERIMENTAL - see restirdi.h)
	const bool spatialReuse = cfg.Get(
		Property("lightstrategy.restir.spatialreuse.enable")(false)
	).Get<bool>();
	// P1-1 temporal reuse (consumed by the GPU kernels)
	const bool temporalReuse = cfg.Get(
		Property("lightstrategy.restir.temporal.enable")(false)
	).Get<bool>();
	// E2a visibility-weighted target (GPU: MK_RT_RESTIR ray batch;
	// CPU: inline trace on the scene accelerator in this strategy)
	const bool visibility = cfg.Get(
		Property("lightstrategy.restir.visibility.enable")(false)
	).Get<bool>();

	auto strategy = std::make_unique<LightStrategyRestirDI>(candidates);
	strategy->spatialReuseEnable = spatialReuse;
	strategy->temporalReuseEnable = temporalReuse;
	strategy->visibilityEnable = visibility;
	return strategy;
}

PropertiesUPtr LightStrategyRestirDI::GetDefaultProps() {
	auto props = std::make_unique<Properties>();
		*props <<
				LightStrategy::GetDefaultProps() <<
				Property("lightstrategy.type")(GetObjectTag()) <<
				Property("lightstrategy.restir.candidates")(0) <<
				Property("lightstrategy.restir.spatialreuse.enable")(false) <<
				Property("lightstrategy.restir.temporal.enable")(false) <<
				Property("lightstrategy.restir.visibility.enable")(false);

	return props;
}
