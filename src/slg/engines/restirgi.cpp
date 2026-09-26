/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of SuperLuxCore (LuxCoreRender fork).               *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 *   LuxCoreRender is free software: you can redistribute it and/or modify *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 3 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   LuxCoreRender is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with LuxCoreRender.  If not, see <http://www.gnu.org/licenses/>.*
 *                                                                         *
 ***************************************************************************/

// ReSTIR GI (G1): per-pixel first-bounce reservoir, CPU implementation.
// See dev-tools/restir-gi-design.md for the estimator derivation and
// scope. GPU port is a follow-up (the candidate/merge tail-queue
// pattern already exists for the DI stage).

#include "slg/engines/restirgi.h"
#include "slg/scene/scene.h"
#include "slg/bsdf/bsdf.h"
#include "slg/lights/light.h"
#include "slg/lights/lightsourcedefs.h"
#include "slg/lights/strategies/lightstrategy.h"
#include "slg/samplers/sobolsequence.h"
#include "slg/utils/pathvolumeinfo.h"

using namespace std;
using namespace luxrays;
using namespace slg;

void RestirGI::Init(const u_int width, const u_int height) {
	filmW = width;
	filmH = height;
	entries.assign((size_t)width * height, Reservoir());
	Reset();
}

void RestirGI::Reset() {
	for (auto &e : entries) {
		e.wSum = 0.f;
		e.target = 0.f;
		e.m = 0;
		e.isMiss = 0;
	}
}

//------------------------------------------------------------------------------
// Resampling
//------------------------------------------------------------------------------

namespace {

// Bounded-bias merge clamp, same constant as the GPU spatial merge
// (RESTIR_MERGE_MAX_TARGET_RATIO): a stale tiny pi_old would otherwise
// amplify wSum through repeated merges.
const float RESTIR_GI_MAX_TARGET_RATIO = 64.f;

inline float GIRandom(const u_int seed, const u_int stream) {
	return SobolSequence::BlueNoiseHash(seed ^ (stream * 0x85EBCA6Bu)) *
			(1.f / 4294967296.f);
}

// Proxy radiance L_hat(x2) for a candidate that hit a surface: x2's
// emission toward x1 (free) plus a one-sample NEE estimate (light pick
// + binary-V shadow ray). This is the cheap part of the target - the
// real payoff still comes from tracing the winning continuation.
Spectrum GI_ProxyHitRadiance(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const BSDF &x2bsdf,
		const PathVolumeInfo &volInfo, const u_int seed) {
	Spectrum lHat;
	if (x2bsdf.IsLightSource())
		lHat += x2bsdf.GetEmittedRadiance();

	auto &lightStrategy = scene.GetLightSources().GetIlluminateLightStrategy();
	float pickPdf;
	LightSourcePtr light = lightStrategy.SampleLights(scene,
			GIRandom(seed, 0x72u), x2bsdf.hitPoint.p,
			x2bsdf.hitPoint.geometryN, x2bsdf.IsVolume(), &pickPdf);
	if (light && (pickPdf > 0.f) &&
			!light->IsAlwaysInShadow(scene, x2bsdf.hitPoint.p,
					x2bsdf.hitPoint.geometryN)) {
		Ray shadowRay;
		float directPdfW;
		const Spectrum lightRadiance = light->Illuminate(scene, x2bsdf,
				time, GIRandom(seed, 0x73u), GIRandom(seed, 0x74u),
				GIRandom(seed, 0x75u), shadowRay, directPdfW);
		if (!lightRadiance.Black() && (directPdfW > 0.f)) {
			BSDFEvent event2;
			float pdfW2;
			const Spectrum eval2 = x2bsdf.Evaluate(shadowRay.d,
					&event2, &pdfW2);
			if (!eval2.Black()) {
				PathVolumeInfo shadowVolInfo = volInfo;
				RayHit shadowRayHit;
				BSDF shadowBsdf;
				Spectrum shadowThroughput;
				if (!scene.Intersect(IntersectionDevicePtr(&device), EYE_RAY | SHADOW_RAY,
						&shadowVolInfo, GIRandom(seed, 0x76u),
						&shadowRay, &shadowRayHit, &shadowBsdf,
						&shadowThroughput, nullptr, nullptr, true))
					lHat += lightRadiance * eval2 / (directPdfW * pickPdf);
			}
		}
	}

	return lHat;
}

} // anonymous namespace

bool RestirGI::ResampleFirstBounce(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const BSDF &bsdf,
		const PathVolumeInfo &volInfo,
		const Point &x1,
		const u_int pixelX, const u_int pixelY, const u_int pass,
		const u_int candidateCount, const bool temporalEnable,
		const bool spatialEnable,
		Vector *outDir, Spectrum *outEval, float *outPdfW,
		BSDFEvent *outEvent) {
	const u_int baseSeed = (pixelX * 73856093u) ^
			(pixelY * 19349663u) ^ (pass * 83492791u);

	const u_int K = Max(1u, candidateCount);
	vector<Vector> dirs(K);
	vector<Spectrum> fcos(K);
	vector<float> pdfs(K, 0.f), targets(K, 0.f);
	vector<BSDFEvent> events(K, (BSDFEvent)0);
	vector<Spectrum> lHats(K);
	vector<Point> x2s(K);
	vector<Normal> x2ns(K);
	vector<u_int> misses(K, 0u);

	float wSum = 0.f;
	u_int mTotal = 0;
	int winner = -1;      // [0,K) fresh candidate, or K = stored reservoir

	//----------------------------------------------------------------------
	// Fresh candidates: K BSDF-sampled bounce directions, each evaluated
	// with the proxy target pi_hat = (f*cos)(dir) * (L_hat(x2) + eps).
	// Culled candidates still count toward M (M tracks proposal draws).
	//
	// The additive floor eps keeps the target's support equal to the
	// integrand's: L_hat is a one-sample estimate (emission + one NEE
	// shadow ray), so it reports 0 wherever the probe was occluded even
	// though x2's true radiance is nonzero (indirect). Without the floor
	// those directions could never win and their real contribution would
	// be dropped (systematic darkening); on the winner-take-all K=1 edge
	// the same zero target would instead force a conditioned redraw
	// (systematic brightening). eps is derived from the candidate set so
	// it stays proportional to the scene's proxy scale.
	//----------------------------------------------------------------------
	for (u_int i = 0; i < K; ++i) {
		const u_int seed = baseSeed ^ (i * 0x9E3779B9u);
		++mTotal;

		float pdfW, cosDir;
		const Spectrum bsdfSample = bsdf.Sample(&dirs[i],
				GIRandom(seed, 0x01u), GIRandom(seed, 0x02u),
				&pdfW, &cosDir, &events[i]);
		if (bsdfSample.Black() || !(pdfW > 0.f))
			continue;
		pdfs[i] = pdfW;
		fcos[i] = bsdfSample * pdfW;

		Ray ray(bsdf.GetRayOrigin(dirs[i]), dirs[i]);
		PathVolumeInfo rayVolInfo = volInfo;
		RayHit rayHit;
		BSDF x2bsdf;
		Spectrum connectionThroughput;
		if (scene.Intersect(IntersectionDevicePtr(&device), EYE_RAY | INDIRECT_RAY, &rayVolInfo,
				GIRandom(seed, 0x03u), &ray, &rayHit, &x2bsdf,
				&connectionThroughput)) {
			x2s[i] = x2bsdf.hitPoint.p;
			x2ns[i] = x2bsdf.hitPoint.geometryN;
			lHats[i] = GI_ProxyHitRadiance(device, scene, time,
					x2bsdf, volInfo, seed);
		} else {
			misses[i] = 1u;
			// Miss: proxy = environment radiance along the direction
			// (the env contribution is part of the indirect integral)
			for (EnvLightSource &envLight :
					scene.GetLightSources().GetEnvLightSources())
				lHats[i] += envLight.GetRadiance(scene, &bsdf, -dirs[i]);
		}
	}

	// Support floor: 5% of the brightest proxy in the set. Any positive
	// eps restores unbiasedness (target > 0 wherever f*cos > 0); the
	// fraction only trades selection probability of dark-proxy
	// directions against variance.
	float lHatMax = 0.f;
	for (u_int i = 0; i < K; ++i)
		lHatMax = Max(lHatMax, lHats[i].Y());
	const float eps = 0.05f * lHatMax;

	for (u_int i = 0; i < K; ++i) {
		if (!(pdfs[i] > 0.f))
			continue;
		targets[i] = fcos[i].Y() * (lHats[i].Y() + eps);
		const float w = targets[i] / pdfs[i];
		wSum += w;
		const float r = GIRandom(baseSeed ^ (i * 0x9E3779B9u), 0x04u);
		if ((winner < 0) || (r < w / wSum))
			winner = (int)i;
	}

	//----------------------------------------------------------------------
	// Temporal merge: the pixel's stored reservoir is reconnected to the
	// current x1. The reconnection shift replays the stored x2 (same
	// endpoint), so pi_new/pi_old carries the Jacobian of the
	// solid-angle reparametrization between the two origins:
	//   J = (|cos_x2(x2->x1_cur)| / d_cur^2) /
	//       (|cos_x2(x2->x1_src)| / d_src^2)
	// (J == 1 on static geometry where x1_cur == x1_src). Visibility of
	// the reconnected segment is tested - occluded transfers get
	// pi_new = 0 (the E2d rule, cheap on CPU where traces are inline).
	//----------------------------------------------------------------------
	Reservoir *stored = Lookup(pixelX, pixelY);
	Vector storedDir;
	Spectrum storedEval;
	BSDFEvent storedEvent = (BSDFEvent)0;
	float storedTargetNew = 0.f;
	if (temporalEnable && stored && (stored->m > 0) &&
			(stored->wSum > 0.f) && (stored->target > 0.f)) {
		mTotal += stored->m;

		float piNew = 0.f;
		float J = 1.f;
		bool visible = true;
		bool hasDir = false;
		if (stored->isMiss) {
			storedDir = Vector(stored->dir[0], stored->dir[1],
					stored->dir[2]);
			hasDir = true;
		} else {
			const Point x2(stored->x2[0], stored->x2[1], stored->x2[2]);
			const Vector dv = x2 - x1;
			const float dCur = dv.Length();
			if (dCur > MachineEpsilon::E(x1)) {
				storedDir = dv / dCur;
				hasDir = true;

				// Binary visibility of the reconnected segment
				Ray vRay(bsdf.GetRayOrigin(storedDir), storedDir,
						0.f, dCur - MachineEpsilon::E(x2), time);
				PathVolumeInfo vVolInfo = volInfo;
				RayHit vHit;
				BSDF vBsdf;
				Spectrum vThroughput;
				visible = !scene.Intersect(IntersectionDevicePtr(&device), EYE_RAY | SHADOW_RAY,
						&vVolInfo, GIRandom(baseSeed, 0x40u), &vRay,
						&vHit, &vBsdf, &vThroughput, nullptr, nullptr,
						true);

				// Jacobian of the solid-angle shift src -> cur
				const Vector toSrc = Point(stored->x1[0], stored->x1[1],
						stored->x1[2]) - x2;
				const float dSrc = toSrc.Length();
				if (dSrc > 0.f) {
					const Normal n2(stored->x2n[0], stored->x2n[1],
							stored->x2n[2]);
					const float cosCur = fabsf(Dot(n2, -storedDir));
					const float cosSrc = fabsf(Dot(n2, toSrc / dSrc));
					const float denom = cosSrc * dCur * dCur;
					if (denom > 0.f)
						J = (cosCur * dSrc * dSrc) / denom;
				}
			}
		}
		if (hasDir && visible) {
			float pdfS;
			storedEval = bsdf.Evaluate(storedDir, &storedEvent, &pdfS);
			// Same support floor as the fresh candidates (see above);
			// stored->target already carries its own pass's eps.
			piNew = storedEval.Y() * (Spectrum(stored->lHat[0],
					stored->lHat[1], stored->lHat[2]).Y() + eps);
		}

		// GRIS merge weight with the Jacobian and the defensive clamp
		const float ratio = Min((piNew / stored->target) * J,
				RESTIR_GI_MAX_TARGET_RATIO);
		const float bNbr = stored->wSum * ratio;
		wSum += bNbr;
		const float r = GIRandom(baseSeed, 0x41u);
		if ((winner < 0) || (r < bNbr / wSum))
			winner = (int)K;
		storedTargetNew = piNew;
	}

	//----------------------------------------------------------------------
	// Incumbent resolution (fresh + temporal only) and pre-spatial store.
	// The stored reservoir must carry the state BEFORE the spatial merges
	// below: storing post-spatial wSum/M lets a neighbour-inflated weight
	// feed back into the same entries next pass (the DI merge-explosion
	// pathology, E2b).
	//----------------------------------------------------------------------

	// Cap the reuse count at 2x the candidate count (DI rule): a runaway
	// M would let stale winners dominate; the rescale keeps W = wSum/M
	// exact.
	const u_int mCap = 2u * K;
	if (mTotal > mCap) {
		wSum *= (float)mCap / (float)mTotal;
		mTotal = mCap;
	}

	struct WinRec {
		Vector dir;
		Spectrum fcos;
		float target;
		Spectrum lHat;
		Point x2;
		Normal x2n;
		u_int miss;
		BSDFEvent event;
		float pdfW;		// proposal pdf (fresh candidates only)
	};
	WinRec out;
	bool haveOut = false;
	bool outIsFresh = false;

	if (winner >= 0) {
		haveOut = true;
		outIsFresh = (winner < (int)K);
		if (winner == (int)K) {
			out.dir = storedDir;
			out.fcos = storedEval;
			out.target = storedTargetNew;
			out.lHat = Spectrum(stored->lHat[0], stored->lHat[1],
					stored->lHat[2]);
			out.miss = stored->isMiss;
			out.event = storedEvent;
			out.pdfW = 0.f;
			if (!out.miss) {
				out.x2 = Point(stored->x2[0], stored->x2[1],
						stored->x2[2]);
				out.x2n = Normal(stored->x2n[0], stored->x2n[1],
						stored->x2n[2]);
			}
		} else {
			out.dir = dirs[winner];
			out.fcos = fcos[winner];
			out.target = targets[winner];
			out.lHat = lHats[winner];
			out.x2 = x2s[winner];
			out.x2n = x2ns[winner];
			out.miss = misses[winner];
			out.event = events[winner];
			out.pdfW = pdfs[winner];
		}
	}

	//----------------------------------------------------------------------
	// Store the post-temporal reservoir for the next pass. An incumbent
	// with target 0 is stored as-is - it just cannot be merged next pass
	// (the merge requires target > 0), which also clears stale entries.
	//----------------------------------------------------------------------
	Reservoir *slot = Lookup(pixelX, pixelY);
	if (slot && haveOut) {
		slot->x1[0] = x1.x; slot->x1[1] = x1.y; slot->x1[2] = x1.z;
		slot->x1n[0] = bsdf.hitPoint.geometryN.x;
		slot->x1n[1] = bsdf.hitPoint.geometryN.y;
		slot->x1n[2] = bsdf.hitPoint.geometryN.z;
		slot->x2[0] = out.x2.x; slot->x2[1] = out.x2.y;
		slot->x2[2] = out.x2.z;
		slot->x2n[0] = out.x2n.x; slot->x2n[1] = out.x2n.y;
		slot->x2n[2] = out.x2n.z;
		slot->dir[0] = out.dir.x; slot->dir[1] = out.dir.y;
		slot->dir[2] = out.dir.z;
		slot->lHat[0] = out.lHat.c[0]; slot->lHat[1] = out.lHat.c[1];
		slot->lHat[2] = out.lHat.c[2];
		slot->wSum = wSum;
		slot->target = out.target;
		slot->m = mTotal;
		slot->isMiss = out.miss;
	}

	//----------------------------------------------------------------------
	// Spatial merge (G1-b): up to 2 pseudo-random neighbour pixels in a
	// 5x5 window (E2b constants), same-surface gated on x1 (2% of the
	// world radius, ~25 degree normals) and shifted by the same
	// Jacobian-corrected reconnection as the temporal merge, including
	// the binary-V test on the reconnected segment (E2d rule). Entries
	// are advisory data shared between render threads: a torn read
	// yields a bounded wrong-weight merge, never a crash.
	//----------------------------------------------------------------------
	if (spatialEnable) {
		const Normal curX1n = bsdf.hitPoint.geometryN;
		const float worldRadius = scene.GetDataSet().GetBSphere().rad;
		const float maxDist2 = 0.02f * 0.02f * worldRadius * worldRadius;

		for (u_int k = 0; k < 2u; ++k) {
			const u_int h = SobolSequence::BlueNoiseHash(baseSeed ^
					(k * 0x85EBCA6Bu) ^ 0x5A5A5A5Au);
			u_int off = h % 25u;
			if (off == 12u)
				off = 24u; // skip the centre cell (self)
			const int nx = (int)pixelX + (int)(off % 5u) - 2;
			const int ny = (int)pixelY + (int)(off / 5u) - 2;
			Reservoir *nbr = ((nx >= 0) && (ny >= 0)) ?
					Lookup((u_int)nx, (u_int)ny) : nullptr;
			if (!nbr || (nbr == stored))
				continue;
			if (!(nbr->m > 0) || !(nbr->wSum > 0.f) ||
					!(nbr->target > 0.f))
				continue;
			// Representative-winner gate (E2b): a fluke tiny-target
			// winner explodes pi_new/pi_old on re-evaluation.
			if (nbr->target < 0.05f * nbr->wSum / (float)nbr->m)
				continue;
			// Same-surface gate on the primary vertex
			const float ddx = nbr->x1[0] - x1.x,
					ddy = nbr->x1[1] - x1.y,
					ddz = nbr->x1[2] - x1.z;
			if (ddx * ddx + ddy * ddy + ddz * ddz > maxDist2)
				continue;
			const float dn = nbr->x1n[0] * curX1n.x +
					nbr->x1n[1] * curX1n.y + nbr->x1n[2] * curX1n.z;
			if (dn < 0.9063f)
				continue;

			mTotal += nbr->m;

			// Reconnect the neighbour's x2 to this x1 (same shift as the
			// temporal merge above)
			Vector nbDir;
			float J = 1.f;
			bool visible = true;
			bool hasDir = false;
			if (nbr->isMiss) {
				nbDir = Vector(nbr->dir[0], nbr->dir[1], nbr->dir[2]);
				hasDir = true;
			} else {
				const Point nx2(nbr->x2[0], nbr->x2[1], nbr->x2[2]);
				const Vector dv = nx2 - x1;
				const float dCur = dv.Length();
				if (dCur > MachineEpsilon::E(x1)) {
					nbDir = dv / dCur;
					hasDir = true;

					Ray vRay(bsdf.GetRayOrigin(nbDir), nbDir,
							0.f, dCur - MachineEpsilon::E(nx2), time);
					PathVolumeInfo vVolInfo = volInfo;
					RayHit vHit;
					BSDF vBsdf;
					Spectrum vThroughput;
					visible = !scene.Intersect(
							IntersectionDevicePtr(&device),
							EYE_RAY | SHADOW_RAY, &vVolInfo,
							GIRandom(baseSeed, 0x50u + k), &vRay,
							&vHit, &vBsdf, &vThroughput, nullptr,
							nullptr, true);

					const Vector toSrc = Point(nbr->x1[0], nbr->x1[1],
							nbr->x1[2]) - nx2;
					const float dSrc = toSrc.Length();
					if (dSrc > 0.f) {
						const Normal n2(nbr->x2n[0], nbr->x2n[1],
								nbr->x2n[2]);
						const float cosCur = fabsf(Dot(n2, -nbDir));
						const float cosSrc = fabsf(Dot(n2,
								toSrc / dSrc));
						const float denom = cosSrc * dCur * dCur;
						if (denom > 0.f)
							J = (cosCur * dSrc * dSrc) / denom;
					}
				}
			}

			float piNew = 0.f;
			Spectrum nbEval;
			BSDFEvent nbEvent = (BSDFEvent)0;
			if (hasDir && visible) {
				float pdfS;
				nbEval = bsdf.Evaluate(nbDir, &nbEvent, &pdfS);
				piNew = nbEval.Y() * (Spectrum(nbr->lHat[0],
						nbr->lHat[1], nbr->lHat[2]).Y() + eps);
			}

			const float ratio = Min((piNew / nbr->target) * J,
					RESTIR_GI_MAX_TARGET_RATIO);
			const float bNbr = nbr->wSum * ratio;
			wSum += bNbr;
			const float r = GIRandom(baseSeed, 0x60u + k);
			if (!haveOut || (r < bNbr / wSum)) {
				out.dir = nbDir;
				out.fcos = nbEval;
				out.target = piNew;
				out.lHat = Spectrum(nbr->lHat[0], nbr->lHat[1],
						nbr->lHat[2]);
				out.miss = nbr->isMiss;
				out.event = nbEvent;
				out.pdfW = 0.f;
				if (!out.miss) {
					out.x2 = Point(nbr->x2[0], nbr->x2[1],
							nbr->x2[2]);
					out.x2n = Normal(nbr->x2n[0], nbr->x2n[1],
							nbr->x2n[2]);
				}
				haveOut = true;
				outIsFresh = false;
			}
		}

		// Re-apply the M cap for the post-spatial totals
		if (mTotal > mCap) {
			wSum *= (float)mCap / (float)mTotal;
			mTotal = mCap;
		}
	}

	//----------------------------------------------------------------------
	// Winner resolution -> output
	//----------------------------------------------------------------------
	if (!haveOut)
		return false;

	float W = 0.f;
	if ((out.target > 0.f) && (wSum > 0.f))
		W = wSum / (mTotal * out.target);
	if (!isfinite(W) || !(W > 0.f)) {
		if (!outIsFresh)
			return false;
		// All-zero targets (or a degenerate W) leave no resampling
		// decision - but the fresh winner is still a plain proposal draw,
		// so returning it with the BSDF payoff (W = 1/pdfW) is unbiased.
		// Falling back to a caller redraw instead would condition the
		// sample on "the proxy saw darkness" and bias the estimate.
		W = 1.f / out.pdfW;
	}

	// RIS marginal selection density of the winning direction - the
	// honest "pdf" for MIS bookkeeping at later vertices / env hits
	// (equals pdfW on the payoff-fallback path above).
	const float risPdfW = 1.f / W;

	*outDir = out.dir;
	*outEval = out.fcos * W;
	*outPdfW = risPdfW;
	*outEvent = out.event;

	return true;
}
