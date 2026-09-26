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

#ifndef _SLG_RESTIRGI_H
#define	_SLG_RESTIRGI_H

// ReSTIR GI (G1): per-pixel first-bounce reservoir for indirect reuse.
// Design doc: dev-tools/restir-gi-design.md. References: Ouyang 2021
// (ReSTIR GI), Lin 2022 (GRIS), Bitterli 2020 (ReSTIR DI).
//
// At a depth-0 vertex the engine resamples WHICH secondary vertex x2
// the path continues through: K candidate bounce rays are evaluated
// with the proxy target pi_hat = f_r(x1)*G(x1,x2)*L_hat(x2), where
// L_hat(x2) is x2's direct-light estimate (one NEE shadow ray). The
// winner's continuation is scaled by W = wSum/(M*pi_hat) and traced
// normally, so the estimator stays unbiased while good x2's get
// reused temporally and (G1-b, later) spatially.

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
class PathVolumeInfo;

class RestirGI {
public:
	// One reservoir per film pixel (screen-space reuse needs a pixel
	// context, unlike the DI world-space hash grid). Entries are
	// advisory data shared between render threads: a torn read yields
	// a bounded wrong-weight merge, never a crash.
	struct Reservoir {
		float x1[3];		// primary hit point (shift source)
		float x1n[3];		// primary geometric normal
		float x2[3];		// winning secondary vertex
		float x2n[3];		// secondary geometric normal (Jacobian cos)
		float dir[3];		// x1 -> x2 (or the miss direction)
		float lHat[3];		// proxy outgoing radiance at x2 (env on miss)
		float wSum;			// accumulated RIS weight
		float target;		// pi_hat of the stored winner
		u_int m;			// accumulated candidate count
		u_int isMiss;		// 1 = winner was a miss (env direction)
	};

	RestirGI() : filmW(0), filmH(0) { }
	~RestirGI() { }

	void Init(const u_int width, const u_int height);
	void Reset();

	// nullptr when out of the film bounds
	Reservoir *Lookup(const u_int px, const u_int py) {
		return ((px < filmW) && (py < filmH)) ?
				&entries[py * filmW + px] : nullptr;
	}

	// Resample the first-bounce continuation at a depth-0 vertex
	// (G1 estimator - see the design doc). On success returns true and
	// fills outDir with the winning direction, outEval with the
	// f_r*cos*W throughput factor that REPLACES the bsdf.Sample()
	// result, outPdfW with the RIS marginal selection density
	// (pi_hat*M/wSum = 1/W) for MIS bookkeeping, and outEvent for the
	// depth counters. Returns false when no usable winner exists - the
	// caller then takes the regular BSDF continuation.
	bool ResampleFirstBounce(
			luxrays::IntersectionDeviceRef device, SceneConstRef scene,
			const float time, const BSDF &bsdf,
			const PathVolumeInfo &volInfo,
			const luxrays::Point &x1,
			const u_int pixelX, const u_int pixelY, const u_int pass,
			const u_int candidateCount, const bool temporalEnable,
			const bool spatialEnable,
			luxrays::Vector *outDir, luxrays::Spectrum *outEval,
			float *outPdfW, BSDFEvent *outEvent);

private:
	u_int filmW, filmH;
	std::vector<Reservoir> entries;
};

}

#endif	/* _SLG_RESTIRGI_H */
