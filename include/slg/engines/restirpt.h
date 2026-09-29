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

#ifndef _SLG_RESTIRPT_H
#define	_SLG_RESTIRPT_H

// ReSTIR PT (PT-1): per-pixel path-suffix reservoir for whole-tail
// reuse. Design doc: doc/engineering/restir-pt-design.md.
// References: Lin, Kettunen & Wyman, ReSTIR PT Enhanced (PACMCGIT
// 9(1), 2026); Lin et al. 2022 (GRIS); Ouyang 2021 (ReSTIR GI).
//
// Where ReSTIR GI resamples which x2 the first bounce continues
// through (winner is always retraced), PT stores the *measured* suffix
// radiance L_suf(x2) of the winning path - all radiance delivered from
// x2 onward. When a stored suffix wins resampling, the path does not
// continue: the estimate f_reconnect*G*Vis*L_suf*W is contributed
// directly (one shadow ray instead of a full suffix retrace).
//
// Bias position: reusing a noisy measured suffix as payoff is the
// bounded-bias "empirical reuse" regime - kept in check by the same
// M-cap, representative-winner gate and target-ratio clamp the GI/DI
// code already uses. Not strictly unbiased; convergence parity is
// enforced by the e-test tolerance.

#include <vector>

#include "luxrays/luxrays.h"
#include "luxrays/core/geometry/point.h"
#include "luxrays/core/geometry/normal.h"
#include "luxrays/core/geometry/vector.h"
#include "luxrays/core/color/color.h"
#include "luxrays/core/intersectiondevice.h"
#include "slg/slg.h"
#include "slg/bsdf/bsdfevents.h"

namespace slg {

class Scene;
class BSDF;
class PathGuidingCache;
class PathVolumeInfo;

class RestirPT {
public:
	// One reservoir per film pixel (screen-space, GI precedent).
	// pass is a seqlock stamp - same publication protocol as
	// RestirGI::Reservoir (invalidate, write payload, stamp last;
	// readers accept strictly-older passes for temporal merges and
	// reject torn entries by bracket comparison).
	struct Reservoir {
		float x1[3];		// prefix vertex (shift source)
		float x1n[3];		// primary geometric normal
		float x2[3];		// reconnection vertex
		float x2n[3];		// x2 geometric normal (Jacobian cos)
		float dir[3];		// x1 -> x2 (or the miss direction)
		float lsuf[3];		// MEASURED suffix radiance at x2
		float wSum;			// accumulated RIS weight
		float target;		// pi_hat of the stored winner (measured base)
		u_int m;			// accumulated candidate count
		u_int isMiss;		// 1 = winner was a miss (env direction)
		u_int pass;			// seqlock stamp
	};

	// Outcome of ResampleSuffix - caller carries it to Commit() at
	// path end so the reservoir record can carry the measured suffix.
	struct Pick {
		luxrays::Vector dir;			// winning direction (fresh or reconnected)
		luxrays::Spectrum eval;		// f*|cos| * W - the bounce multiplier
		luxrays::Spectrum fcos;		// f*|cos| of the winning edge (stored target)
		luxrays::Spectrum lsuf;		// stored winner only: measured suffix radiance
		luxrays::Spectrum connThr;	// stored winner only: volume transmittance of
							// the reconnected edge (1 for miss/fresh)
		float pdfW;			// RIS marginal selection density (1/W)
		BSDFEvent event;
		luxrays::Point x2;
		luxrays::Normal x2n;
		float wSum;			// post-merge totals (estimator only)
		float storeEps;		// support floor used by this call
		u_int m;
		// Store-side totals: pre-spatial merge wSum/m. Storing the
		// post-spatial values would feed neighbour-inflated weight back
		// into the same entries next pass - the merge-explosion
		// pathology the GI pre-spatial store fixed (see the GPU
		// RestirGI_Resolve comment).
		float storeWSum;
		u_int storeM;
		u_int miss;
		bool consumed;		// true = stored suffix, path ends here
		bool storeable;		// true = caller may Commit() this pick
	};

	RestirPT() : filmW(0), filmH(0) { }
	~RestirPT() { }

	void Init(const u_int width, const u_int height);
	void Reset();

	// nullptr when out of the film bounds
	Reservoir *Lookup(const u_int px, const u_int py) {
		return ((px < filmW) && (py < filmH)) ?
				&entries[py * filmW + px] : nullptr;
	}

	// Resample the first-bounce continuation - with whole-suffix reuse.
	// Mirrors RestirGI::ResampleFirstBounce. On success returns true and
	// fills pick: pick.consumed=true means a stored suffix won - the
	// caller adds pathThroughput * pick.eval * pick.lsuf to the pixel
	// and TERMINATES the path (the suffix is already measured).
	// consumed=false is the GI-like case: continue the path along
	// pick.dir with the RIS-weighted multiplier pick.eval, and call
	// Commit() at path end to publish the measured suffix.
	bool ResampleSuffix(
			luxrays::IntersectionDeviceRef device, SceneConstRef scene,
			const float time, const BSDF &bsdf,
			const PathVolumeInfo &volInfo,
			const luxrays::Point &x1,
			const u_int pixelX, const u_int pixelY, const u_int pass,
			const u_int candidateCount, const bool temporalEnable,
			const bool spatialEnable,
			const PathGuidingCache *guideCache, const float guideStrength,
			Pick *pick);

	// Publish the measured suffix for a non-consumed pick. lsuf is the
	// radiance the path delivered from x2 onward per unit x1->x2 edge
	// weight (caller computes it as radiance delta / landing
	// throughput, guarded per channel).
	void Commit(const u_int pixelX, const u_int pixelY,
			const u_int pass, const Pick &pick,
			const luxrays::Point &x1, const luxrays::Normal &x1n,
			const luxrays::Spectrum &lsuf);

private:
	u_int filmW, filmH;
	std::vector<Reservoir> entries;
};

}

#endif	/* _SLG_RESTIRPT_H */
