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

// ReSTIR PT (PT-1): per-pixel path-suffix reservoir, CPU
// implementation. Design: doc/engineering/restir-pt-design.md.
// Structure mirrors restirgi.cpp - the estimator framework (GRIS
// merge, seqlock, M-cap, representative-winner gate, Jacobian shift,
// binary-V reconnection) is identical; what changed is the payload
// (measured L_suf instead of proxy L_hat) and the consumed-winner
// semantics (no suffix retrace).

#include <atomic>

#include "slg/engines/restirpt.h"
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

void RestirPT::Init(const u_int width, const u_int height) {
	filmW = width;
	filmH = height;
	entries.assign((size_t)width * height, Reservoir());
	Reset();
}

void RestirPT::Reset() {
	for (auto &e : entries) {
		e.wSum = 0.f;
		e.target = 0.f;
		e.m = 0;
		e.isMiss = 0;
		e.pass = 0;
	}
}

//------------------------------------------------------------------------------
// Resampling
//------------------------------------------------------------------------------

namespace {

// Bounded-bias merge clamp, same constant as GI/DI
// (RESTIR_MERGE_MAX_TARGET_RATIO).
const float RESTIR_PT_MAX_TARGET_RATIO = 64.f;

inline float PTRandom(const u_int seed, const u_int stream) {
	return SobolSequence::BlueNoiseHash(seed ^ (stream * 0x85EBCA6Bu)) *
			(1.f / 4294967296.f);
}

// Proxy radiance for a fresh candidate that hit a surface: emission
// toward x1 (free) plus a one-sample NEE estimate. Same cheap-target
// recipe as GI - the *stored* candidates are rated by their measured
// suffix instead, so the proxy only gates fresh-direction selection.
// (Copy of the GI helper; the PT proxy may gain a guiding-field term
// later - keep them independent.)
Spectrum PT_ProxyHitRadiance(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const BSDF &x2bsdf,
		const PathVolumeInfo &volInfo, const u_int seed) {
	Spectrum lHat;
	if (x2bsdf.IsLightSource())
		lHat += x2bsdf.GetEmittedRadiance();

	auto &lightStrategy = scene.GetLightSources().GetIlluminateLightStrategy();
	float pickPdf;
	const Normal landingNormal = x2bsdf.hitPoint.GetLandingShadeN();
	LightSourcePtr light = lightStrategy.SampleLights(scene,
			PTRandom(seed, 0x72u), x2bsdf.hitPoint.p,
			landingNormal, x2bsdf.IsVolume(), &pickPdf);
	if (light && (pickPdf > 0.f) &&
			!light->IsAlwaysInShadow(scene, x2bsdf.hitPoint.p,
					landingNormal)) {
		Ray shadowRay;
		float directPdfW;
		const Spectrum lightRadiance = light->Illuminate(scene, x2bsdf,
				time, PTRandom(seed, 0x73u), PTRandom(seed, 0x74u),
				PTRandom(seed, 0x75u), shadowRay, directPdfW);
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
						&shadowVolInfo, PTRandom(seed, 0x76u),
						&shadowRay, &shadowRayHit, &shadowBsdf,
						&shadowThroughput, nullptr, nullptr, true))
					lHat += lightRadiance * eval2 / (directPdfW * pickPdf);
			}
		}
	}

	return lHat;
}

} // anonymous namespace

bool RestirPT::ResampleSuffix(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const BSDF &bsdf,
		const PathVolumeInfo &volInfo,
		const Point &x1,
		const u_int pixelX, const u_int pixelY, const u_int pass,
		const u_int candidateCount, const bool temporalEnable,
		const bool spatialEnable,
		Pick *pick) {
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
	// Identical to GI - including the M bookkeeping (culled candidates
	// count as proposal draws).
	//----------------------------------------------------------------------
	for (u_int i = 0; i < K; ++i) {
		const u_int seed = baseSeed ^ (i * 0x9E3779B9u);
		++mTotal;

		float pdfW, cosDir;
		const Spectrum bsdfSample = bsdf.Sample(&dirs[i],
				PTRandom(seed, 0x01u), PTRandom(seed, 0x02u),
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
		PathDepthInfo candDepthInfo;
		candDepthInfo.depth = bsdf.hitPoint.rayDepth;
		candDepthInfo.diffuseDepth = bsdf.hitPoint.rayDiffuseDepth;
		candDepthInfo.glossyDepth = bsdf.hitPoint.rayGlossyDepth;
		candDepthInfo.specularDepth = bsdf.hitPoint.raySpecularDepth;
		candDepthInfo.transmitDepth = bsdf.hitPoint.rayTransmissionDepth;
		candDepthInfo.transparentDepth = bsdf.hitPoint.rayTransparentDepth;
		candDepthInfo.IncDepths(events[i]);
		if (scene.Intersect(IntersectionDevicePtr(&device), EYE_RAY | INDIRECT_RAY, &rayVolInfo,
				PTRandom(seed, 0x03u), &ray, &rayHit, &x2bsdf,
				&connectionThroughput, nullptr, nullptr, false,
				&candDepthInfo, events[i])) {
			x2s[i] = x2bsdf.hitPoint.p;
			x2ns[i] = x2bsdf.hitPoint.geometryN;
			lHats[i] = PT_ProxyHitRadiance(device, scene, time,
					x2bsdf, volInfo, seed);
		} else {
			misses[i] = 1u;
			for (EnvLightSource &envLight :
					scene.GetLightSources().GetEnvLightSources())
				lHats[i] += envLight.GetRadiance(scene, &bsdf, -dirs[i]);
		}
	}

	// Support floor (GI rule): 5% of the brightest proxy keeps the
	// target's support equal to the integrand's.
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
		const float r = PTRandom(baseSeed ^ (i * 0x9E3779B9u), 0x04u);
		if ((winner < 0) || (r < w / wSum))
			winner = (int)i;
	}

	//----------------------------------------------------------------------
	// Temporal merge: reconnect the pixel's stored suffix to this x1.
	// Same Jacobian reconnection + binary-V as GI; the target side uses
	// the STORED MEASURED L_suf (the merge's whole point: its payoff
	// estimate is exact, only the prefix edge changes).
	//----------------------------------------------------------------------
	Reservoir *stored = Lookup(pixelX, pixelY);
	Reservoir storedSnap = Reservoir();
	Vector storedDir;
	Spectrum storedEval;
	Spectrum storedConnThr(1.f);
	BSDFEvent storedEvent = (BSDFEvent)0;
	float storedTargetNew = 0.f;
	Spectrum storedLsuf;
	const u_int p0 = stored ?
		std::atomic_ref<u_int>(stored->pass).load(
				std::memory_order_acquire) : 0xFFFFFFFFu;
	if (temporalEnable && (p0 != 0xFFFFFFFFu) && (p0 < pass)) {
		const Reservoir snap = *stored;
		if ((snap.m > 0) && (snap.wSum > 0.f) && (snap.target > 0.f) &&
				(snap.target >= 0.05f * snap.wSum / (float)snap.m) &&
				(std::atomic_ref<u_int>(stored->pass).load(
						std::memory_order_acquire) == p0)) {
			float piNew = 0.f;
			float J = 1.f;
			bool visible = true;
			bool hasDir = false;
			if (snap.isMiss) {
				storedDir = Vector(snap.dir[0], snap.dir[1],
						snap.dir[2]);
				hasDir = true;
			} else {
				const Point x2(snap.x2[0], snap.x2[1], snap.x2[2]);
				const Vector dv = x2 - x1;
				const float dCur = dv.Length();
				if (dCur > MachineEpsilon::E(x1)) {
					storedDir = dv / dCur;
					hasDir = true;

					Ray vRay(bsdf.GetRayOrigin(storedDir), storedDir,
							0.f, dCur - MachineEpsilon::E(x2), time);
					PathVolumeInfo vVolInfo = volInfo;
					RayHit vHit;
					BSDF vBsdf;
					Spectrum vThroughput;
					visible = !scene.Intersect(IntersectionDevicePtr(&device), EYE_RAY | SHADOW_RAY,
							&vVolInfo, PTRandom(baseSeed, 0x40u), &vRay,
							&vHit, &vBsdf, &vThroughput, nullptr, nullptr,
							true);
					// Volume transmittance of the reconnected segment -
					// the walked path's own connThr cannot apply to the
					// new edge, so the consumed contribution folds it in.
					if (visible)
						storedConnThr = vThroughput;

					const Vector toSrc = Point(snap.x1[0], snap.x1[1],
							snap.x1[2]) - x2;
					const float dSrc = toSrc.Length();
					if (dSrc > 0.f) {
						const Normal n2(snap.x2n[0], snap.x2n[1],
								snap.x2n[2]);
						const float cosCur = fabsf(Dot(n2, -storedDir));
						const float cosSrc = fabsf(Dot(n2, toSrc / dSrc));
						const float denom = cosSrc * dCur * dCur;
						if (denom > 0.f)
							J = (cosCur * dSrc * dSrc) / denom;
					}
				}
			}
			if (hasDir && visible) {
				// Failed shifts are rejected without counting their
				// mass (GI rule - counting inflates M with impossible
				// draws and measurably darkens the output).
				mTotal += snap.m;

				float pdfS;
				storedEval = bsdf.Evaluate(storedDir, &storedEvent, &pdfS);
				// Measured suffix as the target radiance term
				storedLsuf = Spectrum(snap.lsuf[0], snap.lsuf[1],
						snap.lsuf[2]);
				piNew = storedEval.Y() * (storedLsuf.Y() + eps);
			}

			const float ratio = Min((piNew / snap.target) * J,
					RESTIR_PT_MAX_TARGET_RATIO);
			const float bNbr = snap.wSum * ratio;
			wSum += bNbr;
			const float r = PTRandom(baseSeed, 0x41u);
			if (((winner < 0) && (bNbr > 0.f)) || (r < bNbr / wSum)) {
				winner = (int)K;
				storedSnap = snap;
			}
			storedTargetNew = piNew;
		}
	}

	// M cap (DI rule): reuse count bounded at 2x candidate count,
	// rescale keeps W = wSum/M exact.
	const u_int mCap = 2u * K;
	if (mTotal > mCap) {
		wSum *= (float)mCap / (float)mTotal;
		mTotal = mCap;
	}

	// Pre-spatial store totals (GI parity): the reservoir record must
	// carry the state BEFORE the spatial merges below, else a
	// neighbour-inflated wSum feeds back into the same entries next
	// pass (the merge-explosion pathology). The estimator's W still
	// uses the post-merge totals.
	float storeWSum = wSum;
	u_int storeM = mTotal;

	struct WinRec {
		Vector dir;
		Spectrum fcos;
		float target;
		Spectrum lsuf;		// measured (stored) or proxy (fresh)
		Spectrum connThr;	// reconnected-edge transmittance (measured)
		Point x2;
		Normal x2n;
		u_int miss;
		BSDFEvent event;
		float pdfW;
		bool measured;		// stored candidate -> lsuf is measured
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
			out.lsuf = storedLsuf;
			out.connThr = storedConnThr;
			out.miss = storedSnap.isMiss;
			out.event = storedEvent;
			out.pdfW = 0.f;
			out.measured = true;
			if (!out.miss) {
				out.x2 = Point(storedSnap.x2[0], storedSnap.x2[1],
						storedSnap.x2[2]);
				out.x2n = Normal(storedSnap.x2n[0], storedSnap.x2n[1],
						storedSnap.x2n[2]);
			}
		} else {
			out.dir = dirs[winner];
			out.fcos = fcos[winner];
			out.target = targets[winner];
			out.lsuf = lHats[winner];
			out.x2 = x2s[winner];
			out.x2n = x2ns[winner];
			out.miss = misses[winner];
			out.event = events[winner];
			out.pdfW = pdfs[winner];
			out.measured = false;
		}
	}

	//----------------------------------------------------------------------
	// Spatial merge (E2b pattern): up to 2 pseudo-random neighbours in a
	// 5x5 window, same-surface gated on x1, Jacobian shift + binary-V.
	// A spatial win is measured too - it consumes the path the same way
	// a temporal win does.
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
			const u_int np0 = std::atomic_ref<u_int>(nbr->pass).
					load(std::memory_order_acquire);
			if (np0 == 0xFFFFFFFFu)
				continue;
			const Reservoir nSnap = *nbr;
			if (!(nSnap.m > 0) || !(nSnap.wSum > 0.f) ||
					!(nSnap.target > 0.f))
				continue;
			if (nSnap.target < 0.05f * nSnap.wSum / (float)nSnap.m)
				continue;
			const float ddx = nSnap.x1[0] - x1.x,
					ddy = nSnap.x1[1] - x1.y,
					ddz = nSnap.x1[2] - x1.z;
			if (ddx * ddx + ddy * ddy + ddz * ddz > maxDist2)
				continue;
			const float dn = nSnap.x1n[0] * curX1n.x +
					nSnap.x1n[1] * curX1n.y +
					nSnap.x1n[2] * curX1n.z;
			if (dn < 0.9063f)
				continue;
			if (std::atomic_ref<u_int>(nbr->pass).load(
					std::memory_order_acquire) != np0)
				continue;

			Vector nbDir;
			Spectrum nbConnThr(1.f);
			float J = 1.f;
			bool visible = true;
			bool hasDir = false;
			if (nSnap.isMiss) {
				nbDir = Vector(nSnap.dir[0], nSnap.dir[1], nSnap.dir[2]);
				hasDir = true;
			} else {
				const Point nx2(nSnap.x2[0], nSnap.x2[1], nSnap.x2[2]);
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
							PTRandom(baseSeed, 0x50u + k), &vRay,
							&vHit, &vBsdf, &vThroughput, nullptr,
							nullptr, true);
					if (visible)
						nbConnThr = vThroughput;

					const Vector toSrc = Point(nSnap.x1[0],
							nSnap.x1[1], nSnap.x1[2]) - nx2;
					const float dSrc = toSrc.Length();
					if (dSrc > 0.f) {
						const Normal n2(nSnap.x2n[0], nSnap.x2n[1],
								nSnap.x2n[2]);
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
			Spectrum nbLsuf;
			if (hasDir && visible) {
				mTotal += nSnap.m;

				float pdfS;
				nbEval = bsdf.Evaluate(nbDir, &nbEvent, &pdfS);
				nbLsuf = Spectrum(nSnap.lsuf[0], nSnap.lsuf[1],
						nSnap.lsuf[2]);
				piNew = nbEval.Y() * (nbLsuf.Y() + eps);
			}

			const float ratio = Min((piNew / nSnap.target) * J,
					RESTIR_PT_MAX_TARGET_RATIO);
			const float bNbr = nSnap.wSum * ratio;
			wSum += bNbr;
			const float r = PTRandom(baseSeed, 0x60u + k);
			if ((!haveOut && (bNbr > 0.f)) || (r < bNbr / wSum)) {
				out.dir = nbDir;
				out.fcos = nbEval;
				out.target = piNew;
				out.lsuf = nbLsuf;
				out.connThr = nbConnThr;
				out.miss = nSnap.isMiss;
				out.event = nbEvent;
				out.pdfW = 0.f;
				out.measured = true;
				if (!out.miss) {
					out.x2 = Point(nSnap.x2[0], nSnap.x2[1],
							nSnap.x2[2]);
					out.x2n = Normal(nSnap.x2n[0], nSnap.x2n[1],
							nSnap.x2n[2]);
				}
				haveOut = true;
				outIsFresh = false;
			}
		}

		if (mTotal > mCap) {
			wSum *= (float)mCap / (float)mTotal;
			mTotal = mCap;
		}
	}

	//----------------------------------------------------------------------
	// Winner resolution -> output pick
	//----------------------------------------------------------------------
	if (!haveOut)
		return false;

	float W = 0.f;
	if ((out.target > 0.f) && (wSum > 0.f))
		W = wSum / (mTotal * out.target);
	if (!isfinite(W) || !(W > 0.f)) {
		if (!outIsFresh)
			return false;
		// Degenerate W on a fresh winner: fall back to the plain
		// proposal payoff (W = 1/pdfW), unbiased (GI rule).
		W = 1.f / out.pdfW;
	}

	pick->dir = out.dir;
	pick->eval = out.fcos * W;
	pick->fcos = out.fcos;
	pick->lsuf = out.measured ? out.lsuf : Spectrum();
	pick->connThr = out.measured ? out.connThr : Spectrum(1.f);
	pick->pdfW = 1.f / W;
	pick->event = out.event;
	pick->x2 = out.x2;
	pick->x2n = out.x2n;
	pick->wSum = wSum;
	pick->m = mTotal;
	pick->storeWSum = storeWSum;
	pick->storeM = storeM;
	pick->miss = out.miss;
	pick->consumed = out.measured;
	pick->storeEps = eps;
	// Storeable candidates: a fresh winner records after its suffix is
	// measured at path end; a consumed winner republishes its stored
	// record immediately (wSum/m updated). A fresh winner that never
	// lands (RR/max-depth kill before x2) simply isn't committed.
	pick->storeable = true;

	return true;
}

void RestirPT::Commit(const u_int pixelX, const u_int pixelY,
		const u_int pass, const Pick &pick,
		const Point &x1, const Normal &x1n,
		const Spectrum &lsuf) {
	Reservoir *slot = Lookup(pixelX, pixelY);
	if (!slot || !pick.storeable)
		return;

	// For a consumed pick the suffix is already measured (pick.lsuf);
	// for a fresh pick the caller supplies the just-measured value.
	const Spectrum &suf = pick.consumed ? pick.lsuf : lsuf;
	// Stored target is rebased to the measured suffix so next pass's
	// merge ratio (piNew / snap.target) compares measured targets.
	// A zero target keeps support correctness: it can never merge.
	const float tgt = pick.fcos.Y() * (suf.Y() + pick.storeEps);

	// Seqlock publish (GI parity)
	std::atomic_ref<u_int>(slot->pass).store(0xFFFFFFFFu,
			std::memory_order_relaxed);
	slot->x1[0] = x1.x; slot->x1[1] = x1.y; slot->x1[2] = x1.z;
	slot->x1n[0] = x1n.x; slot->x1n[1] = x1n.y; slot->x1n[2] = x1n.z;
	slot->x2[0] = pick.x2.x; slot->x2[1] = pick.x2.y;
	slot->x2[2] = pick.x2.z;
	slot->x2n[0] = pick.x2n.x; slot->x2n[1] = pick.x2n.y;
	slot->x2n[2] = pick.x2n.z;
	slot->dir[0] = pick.dir.x; slot->dir[1] = pick.dir.y;
	slot->dir[2] = pick.dir.z;
	slot->lsuf[0] = suf.c[0]; slot->lsuf[1] = suf.c[1];
	slot->lsuf[2] = suf.c[2];
	slot->wSum = pick.storeWSum;
	slot->target = tgt;
	slot->m = pick.storeM;
	slot->isMiss = pick.miss;
	std::atomic_ref<u_int>(slot->pass).store(pass,
			std::memory_order_release);
}
