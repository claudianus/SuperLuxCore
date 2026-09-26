/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * http://www.apache.org/licenses/LICENSE-2.0                              *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include <cstddef>

#include "luxrays/usings.h"
#include "slg/volumes/homogenous.h"
#include "slg/volumes/heterogenous.h"
#include "slg/bsdf/bsdf.h"
#include "slg/textures/densitygrid.h"
#include "slg/textures/constfloat.h"
#include "slg/textures/constfloat3.h"
#include "slg/textures/math/abs.h"
#include "slg/textures/math/add.h"
#include "slg/textures/math/clamp.h"
#include "slg/textures/math/mix.h"
#include "slg/textures/math/scale.h"
#include "slg/textures/math/subtract.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// Texture upper-bound estimation (used to build the majorant grid)
//
// For null-collision tracking we need a conservative upper bound (majorant)
// of sigma_t = sigma_a + sigma_s over each grid cell. Density grid textures
// (i.e. VDB data) support an exact bound over a world-space box; constant
// and simple math textures are bounded recursively; everything else
// (procedurals, UV-mapped textures, ...) is estimated by sampling and any
// residual underestimation is corrected at render time by the weighted
// delta-tracking weight.
//------------------------------------------------------------------------------

// Returns true and sets *bound to an upper bound of the texture value over
// the given world-space box when it can be computed analytically.
static bool TextureBoundInBox(const Texture &tex, const BBox &box, float *bound) {
	switch (tex.GetType()) {
		case CONST_FLOAT:
			*bound = static_cast<const ConstFloatTexture &>(tex).GetValue();
			return true;
		case CONST_FLOAT3:
			*bound = static_cast<const ConstFloat3Texture &>(tex).GetColor().Max();
			return true;
		case DENSITYGRID_TEX:
			return static_cast<const DensityGridTexture &>(tex).GetMaxInWorldBBox(box, bound);
		case SCALE_TEX: {
			auto& t = static_cast<const ScaleTexture &>(tex);
			float b1, b2;
			if (!TextureBoundInBox(t.GetTexture1(), box, &b1) ||
					!TextureBoundInBox(t.GetTexture2(), box, &b2))
				return false;
			*bound = b1 * b2;
			return true;
		}
		case ADD_TEX: {
			auto& t = static_cast<const AddTexture &>(tex);
			float b1, b2;
			if (!TextureBoundInBox(t.GetTexture1(), box, &b1) ||
					!TextureBoundInBox(t.GetTexture2(), box, &b2))
				return false;
			*bound = b1 + b2;
			return true;
		}
		case SUBTRACT_TEX: {
			// t1 - t2 <= t1
			auto& t = static_cast<const SubtractTexture &>(tex);
			return TextureBoundInBox(t.GetTexture1(), box, bound);
		}
		case MIX_TEX: {
			// Lerp(amount, t1, t2) <= max(t1, t2) for amount in [0, 1]
			auto& t = static_cast<const MixTexture &>(tex);
			float b1, b2;
			const bool ok1 = TextureBoundInBox(t.GetTexture1(), box, &b1);
			const bool ok2 = TextureBoundInBox(t.GetTexture2(), box, &b2);
			if (!ok1 && !ok2)
				return false;
			*bound = Max(ok1 ? b1 : 0.f, ok2 ? b2 : 0.f);
			return true;
		}
		case ABS_TEX: {
			auto& t = static_cast<const AbsTexture &>(tex);
			return TextureBoundInBox(t.GetTexture(), box, bound);
		}
		case CLAMP_TEX: {
			auto& t = static_cast<const ClampTexture &>(tex);
			float b;
			if (!TextureBoundInBox(t.GetTexture(), box, &b))
				return false;
			*bound = Max(t.GetMinVal(), Min(b, t.GetMaxVal()));
			return true;
		}
		default:
			return false;
	}
}

// Returns true and sets *bound to a lower bound (minorant) of the texture
// value over the box when it can be computed analytically. Used as the
// control extinction of residual ratio tracking: unlike a majorant, a
// sampled estimate can not prove a lower bound, so unknown texture types
// simply return false and the caller uses a zero minorant.
static bool TextureMinBoundInBox(const Texture &tex, const BBox &box, float *bound) {
	switch (tex.GetType()) {
		case CONST_FLOAT:
			*bound = static_cast<const ConstFloatTexture &>(tex).GetValue();
			return true;
		case CONST_FLOAT3:
			*bound = static_cast<const ConstFloat3Texture &>(tex).GetColor().Min();
			return true;
		case DENSITYGRID_TEX:
			return static_cast<const DensityGridTexture &>(tex).GetMinInWorldBBox(box, bound);
		case SCALE_TEX: {
			auto& t = static_cast<const ScaleTexture &>(tex);
			float m1, m2, x1, x2;
			if (!TextureMinBoundInBox(t.GetTexture1(), box, &m1) ||
					!TextureMinBoundInBox(t.GetTexture2(), box, &m2) ||
					!TextureBoundInBox(t.GetTexture1(), box, &x1) ||
					!TextureBoundInBox(t.GetTexture2(), box, &x2))
				return false;
			// min(f*g) is min1*min2 for positive factors; mixed-sign factors
			// get the smallest of the four products
			*bound = Min(Min(m1 * m2, m1 * x2), Min(x1 * m2, x1 * x2));
			return true;
		}
		case ADD_TEX: {
			auto& t = static_cast<const AddTexture &>(tex);
			float b1, b2;
			if (!TextureMinBoundInBox(t.GetTexture1(), box, &b1) ||
					!TextureMinBoundInBox(t.GetTexture2(), box, &b2))
				return false;
			*bound = b1 + b2;
			return true;
		}
		case SUBTRACT_TEX: {
			// t1 - t2 >= min(t1) - max(t2)
			auto& t = static_cast<const SubtractTexture &>(tex);
			float m1, x2;
			if (!TextureMinBoundInBox(t.GetTexture1(), box, &m1) ||
					!TextureBoundInBox(t.GetTexture2(), box, &x2))
				return false;
			*bound = m1 - x2;
			return true;
		}
		case MIX_TEX: {
			// Lerp(amount, t1, t2) >= min(t1, t2) for amount in [0, 1]
			auto& t = static_cast<const MixTexture &>(tex);
			float b1, b2;
			const bool ok1 = TextureMinBoundInBox(t.GetTexture1(), box, &b1);
			const bool ok2 = TextureMinBoundInBox(t.GetTexture2(), box, &b2);
			if (!ok1 && !ok2)
				return false;
			*bound = Min(ok1 ? b1 : INFINITY, ok2 ? b2 : INFINITY);
			return true;
		}
		case CLAMP_TEX: {
			auto& t = static_cast<const ClampTexture &>(tex);
			float b;
			if (!TextureMinBoundInBox(t.GetTexture(), box, &b))
				return false;
			*bound = Max(t.GetMinVal(), Min(b, t.GetMaxVal()));
			return true;
		}
		default:
			return false;
	}
}

static float TextureMinBoundInBoxOrZero(const Texture &tex, const BBox &box) {
	float bound;
	if (TextureMinBoundInBox(tex, box, &bound))
		return Max(bound, 0.f);
	return 0.f;
}

// Estimates the texture bound by sampling a 3x3x3 pattern inside the box.
// The safety margin makes underestimation rare; any residual is corrected
// by the weighted delta-tracking weight at render time.
static float TextureSampledBound(const Texture &tex, const BBox &box, HitPoint &hitPoint) {
	const float margin = 1.25f;
	float m = 0.f;
	for (u_int i = 0; i < 3; ++i) {
		const float x = Lerp(i * .5f, box.pMin.x, box.pMax.x);
		for (u_int j = 0; j < 3; ++j) {
			const float y = Lerp(j * .5f, box.pMin.y, box.pMax.y);
			for (u_int k = 0; k < 3; ++k) {
				hitPoint.p = Point(x, y, Lerp(k * .5f, box.pMin.z, box.pMax.z));
				m = Max(m, tex.GetSpectrumValue(hitPoint).Clamp().Max());
			}
		}
	}
	return m * margin;
}

static float TextureBoundInBoxOrSampled(const Texture &tex, const BBox &box, HitPoint &hitPoint) {
	float bound;
	if (TextureBoundInBox(tex, box, &bound))
		return Max(bound, 0.f);
	return TextureSampledBound(tex, box, hitPoint);
}

//------------------------------------------------------------------------------
// HeterogeneousVolume
//------------------------------------------------------------------------------

HeterogeneousVolume::HeterogeneousVolume(
	TextureConstRef iorTex,
	TextureConstPtr emiTex,
	TextureConstRef a, TextureConstRef s, TextureConstRef g,
	const float ss, const u_int maxStepC,
	const bool multiScat,
	const bool deltaTrack, const u_int majRes, const bool useHG
) :
	Volume(iorTex, emiTex),
	schlickScatter(*this, g, useHG), stepSize(ss), maxStepsCount(maxStepC),
	multiScattering(multiScat),
	deltaTracking(deltaTrack), majorantRes(majRes),
	sigmaA(a),
	sigmaS(s),
	globalMajorant(0.f), globalMinorant(0.f) {
	majorantGridRes[0] = majorantGridRes[1] = majorantGridRes[2] = 0;
}

Spectrum HeterogeneousVolume::SigmaA(const HitPoint &hitPoint) const {
	return GetSigmaA().GetSpectrumValue(hitPoint).Clamp();
}

Spectrum HeterogeneousVolume::SigmaS(const HitPoint &hitPoint) const {
	return GetSigmaS().GetSpectrumValue(hitPoint).Clamp();
}

float HeterogeneousVolume::Scatter(const Ray &ray, const float u,
		const bool scatteredStart, Spectrum *connectionThroughput,
		Spectrum *connectionEmission) const {
	if (deltaTracking && HasMajorantGrid())
		return DeltaTrackScatter(ray, u, scatteredStart, connectionThroughput, connectionEmission);
	else
		return MarchScatter(ray, u, scatteredStart, connectionThroughput, connectionEmission);
}

Spectrum HeterogeneousVolume::TransmittanceEstimate(const Ray &ray, const float u) const {
	if (deltaTracking && HasMajorantGrid())
		return RatioTrackTransmittance(ray, u);
	else
		return MarchTransmittance(ray, u);
}

//------------------------------------------------------------------------------
// Majorant grid construction and DDA traversal
//------------------------------------------------------------------------------

void HeterogeneousVolume::BuildMajorantGrid(const BBox &domain) {
	majorantCells.clear();
	minorantCells.clear();
	globalMajorant = 0.f;
	globalMinorant = 0.f;
	majorantGridRes[0] = majorantGridRes[1] = majorantGridRes[2] = 0;

	if (!domain.IsValid())
		return;

	majorantBBox = domain;
	// Pad the domain so points on the boundary are inside a cell
	const float pad = 1e-3f * Max(1.f, (majorantBBox.pMax - majorantBBox.pMin).Length());
	majorantBBox.Expand(pad);

	const Vector extent = majorantBBox.pMax - majorantBBox.pMin;
	const float maxExtent = Max(extent.x, Max(extent.y, extent.z));
	if (!(maxExtent > 0.f) || !isfinite(maxExtent) || (majorantRes == 0))
		return;

	// Cubic cells: the largest extent gets majorantRes cells
	const float cellSize = maxExtent / majorantRes;
	for (u_int i = 0; i < 3; ++i)
		majorantGridRes[i] = Max(1u, Ceil2UInt(extent[i] / cellSize));

	majorantCellSize = cellSize;
	const u_int rx = majorantGridRes[0], ry = majorantGridRes[1], rz = majorantGridRes[2];
	majorantCells.assign(rx * ry * rz, 0.f);
	minorantCells.assign(rx * ry * rz, 0.f);

	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = Vector(0.f, 0.f, -1.f);
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(0.f, 0.f, 1.f);
	hitPoint.passThroughEvent = 0.5f;

	float gMaxA = 0.f, gMaxS = 0.f;
	for (u_int z = 0; z < rz; ++z) {
		for (u_int y = 0; y < ry; ++y) {
			for (u_int x = 0; x < rx; ++x) {
				const BBox cellBox(
						Point(majorantBBox.pMin.x + x * cellSize,
								majorantBBox.pMin.y + y * cellSize,
								majorantBBox.pMin.z + z * cellSize),
						Point(majorantBBox.pMin.x + Min((float)rx, (float)x + 1.f) * cellSize,
								majorantBBox.pMin.y + Min((float)ry, (float)y + 1.f) * cellSize,
								majorantBBox.pMin.z + Min((float)rz, (float)z + 1.f) * cellSize));
				const float boundA = TextureBoundInBoxOrSampled(sigmaA.get(), cellBox, hitPoint);
				const float boundS = TextureBoundInBoxOrSampled(sigmaS.get(), cellBox, hitPoint);
				majorantCells[(z * ry + y) * rx + x] = boundA + boundS;
				minorantCells[(z * ry + y) * rx + x] =
						TextureMinBoundInBoxOrZero(sigmaA.get(), cellBox) +
						TextureMinBoundInBoxOrZero(sigmaS.get(), cellBox);
				gMaxA = Max(gMaxA, boundA);
				gMaxS = Max(gMaxS, boundS);
			}
		}
	}

	// Majorant for the space outside the grid domain. Textures with exact
	// bounds (e.g. density grids) are bounded over all of space; for the
	// others the cell maximum is used and any residual underestimation is
	// corrected by the weighted delta-tracking weight.
	const BBox infBox(Point(-INFINITY, -INFINITY, -INFINITY), Point(INFINITY, INFINITY, INFINITY));
	float boundA, boundS;
	globalMajorant =
			(TextureBoundInBox(sigmaA.get(), infBox, &boundA) ? Max(boundA, 0.f) : gMaxA) +
			(TextureBoundInBox(sigmaS.get(), infBox, &boundS) ? Max(boundS, 0.f) : gMaxS);
	globalMinorant =
			(TextureMinBoundInBox(sigmaA.get(), infBox, &boundA) ? Max(boundA, 0.f) : 0.f) +
			(TextureMinBoundInBox(sigmaS.get(), infBox, &boundS) ? Max(boundS, 0.f) : 0.f);

	SDL_LOG("Heterogeneous volume '" << GetName() << "' majorant grid: "
			<< rx << "x" << ry << "x" << rz << " cells, globalMajorant = " << globalMajorant);
}

void HeterogeneousVolume::WalkInit(MajorantWalk *w, const Ray &ray, const float tEnd) const {
	w->ray = &ray;
	w->t = ray.mint;
	w->tMax = tEnd;
	w->phase = 0;
	w->gridInit = false;

	float tEnter, tExit;
	w->hasGrid = majorantBBox.IntersectP(ray, &tEnter, &tExit);
	if (w->hasGrid) {
		w->tEnter = Clamp(tEnter, ray.mint, tEnd);
		w->tExit = Clamp(tExit, ray.mint, tEnd);
		w->hasGrid = (w->tExit > w->tEnter);
	}
	if (!w->hasGrid) {
		w->tEnter = tEnd;
		w->tExit = tEnd;
	}
}

bool HeterogeneousVolume::WalkNext(MajorantWalk *w) const {
	while (true) {
		switch (w->phase) {
			case 0: {
				// Segment before the grid domain (global majorant)
				const float t1 = w->hasGrid ? w->tEnter : w->tMax;
				if (w->t < t1) {
					w->t0 = w->t;
					w->t1 = t1;
					w->maj = globalMajorant;
					w->mn = globalMinorant;
					w->t = t1;
					return true;
				}
				w->phase = 1;
				break;
			}
			case 1: {
				if (!w->hasGrid || (w->t >= w->tExit)) {
					w->phase = 2;
					break;
				}
				if (!w->gridInit) {
					// 3D DDA setup at the domain entry point. Cells are
					// cubic (majorantCellSize); the grid covers
					// res[i]*cellSize >= extent[i] along each axis, so it
					// may extend slightly past majorantBBox.
					const Ray &ray = *w->ray;
					const Point p = ray(w->t);
					const float cellSize = majorantCellSize;
					for (u_int i = 0; i < 3; ++i) {
						w->voxel[i] = Clamp(Floor2Int((p[i] - majorantBBox.pMin[i]) / cellSize),
								0, (int)majorantGridRes[i] - 1);
						const float d = ray.d[i];
						if (d > 0.f) {
							w->step[i] = 1;
							w->voxelLimit[i] = majorantGridRes[i];
							w->deltaT[i] = cellSize / d;
							w->nextCrossingT[i] = w->t +
									(majorantBBox.pMin[i] + (w->voxel[i] + 1) * cellSize - p[i]) / d;
						} else if (d < 0.f) {
							w->step[i] = -1;
							w->voxelLimit[i] = -1;
							w->deltaT[i] = -cellSize / d;
							w->nextCrossingT[i] = w->t +
									(majorantBBox.pMin[i] + w->voxel[i] * cellSize - p[i]) / d;
						} else {
							w->step[i] = 0;
							w->voxelLimit[i] = majorantGridRes[i];
							w->deltaT[i] = INFINITY;
							w->nextCrossingT[i] = INFINITY;
						}
					}
					w->gridInit = true;
				}

				// Emit the current cell segment: [t, next voxel boundary]
				const u_int axis =
						(w->nextCrossingT[0] < w->nextCrossingT[1]) ?
						((w->nextCrossingT[0] < w->nextCrossingT[2]) ? 0 : 2) :
						((w->nextCrossingT[1] < w->nextCrossingT[2]) ? 1 : 2);
				w->t0 = w->t;
				w->t1 = Max(w->t, Min(w->nextCrossingT[axis], w->tExit));
				w->maj = CellMajorant(w->voxel[0], w->voxel[1], w->voxel[2]);
				w->mn = CellMinorant(w->voxel[0], w->voxel[1], w->voxel[2]);

				w->t = w->t1;
				w->voxel[axis] += w->step[axis];
				w->nextCrossingT[axis] += w->deltaT[axis];
				if ((w->t >= w->tExit) || (w->voxel[axis] == w->voxelLimit[axis]))
					w->phase = 2;

				return true;
			}
			case 2: {
				// Segment after the grid domain (global majorant)
				if (w->t < w->tMax) {
					w->t0 = w->t;
					w->t1 = w->tMax;
					w->maj = globalMajorant;
					w->mn = globalMinorant;
					w->t = w->tMax;
					return true;
				}
				w->phase = 3;
				return false;
			}
			default:
				return false;
		}
	}
}

//------------------------------------------------------------------------------
// Legacy fixed-step ray marching
//------------------------------------------------------------------------------

float HeterogeneousVolume::MarchScatter(const Ray &ray, const float u,
		const bool scatteredStart, Spectrum *connectionThroughput,
		Spectrum *connectionEmission) const {
	// I need a sequence of pseudo-random numbers starting form a floating point
	// pseudo-random number
	TauswortheRandomGenerator rng(u);

	// Compute the number of steps to evaluate the volume
	const float segmentLength = ray.maxt - ray.mint;

	// Handle the case when segmentLength is infinity or a very large number
	//
	// Note: the old code"Min(maxStepsCount, Ceil2UInt(segmentLength / stepSize))"
	// can overflow for large values of segmentLength so I have to use
	// "Ceil2UInt(Min((float)maxStepsCount, segmentLength / stepSize))"
	const u_int steps = Ceil2UInt(Min((float)maxStepsCount, segmentLength / stepSize));

	const float currentStepSize = Min(segmentLength / steps, maxStepsCount * stepSize);

	// Check if I have to support multi-scattering
	const bool scatterAllowed = (!scatteredStart || multiScattering);

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	for (u_int s = 0; s < steps; ++s) {
		// Compute the scattering over the current step
		const float evaluationPoint = (s + rng.floatValue()) * currentStepSize;

		hitPoint.p = ray(ray.mint + evaluationPoint);

		// Volume segment values
		const Spectrum sigmaA = SigmaA(hitPoint);
		const Spectrum sigmaS = SigmaS(hitPoint);
		const Spectrum emission = Emission(hitPoint);

		// Evaluate the current segment like if it was an homogenous volume
		//
		// This could be optimized by inlining the code and exploiting
		// exp(a) * exp(b) = exp(a + b) in order to evaluate a single exp() at
		// the end instead of one each step.
		// However the code would be far less simple and readable.
		Spectrum segmentTransmittance, segmentEmission;
		const float scatterDistance = HomogeneousVolume::Scatter(rng.floatValue(), scatterAllowed,
				currentStepSize, sigmaA, sigmaS, emission,
				segmentTransmittance, segmentEmission);

		// I need to update first connectionEmission and than connectionThroughput
		*connectionEmission += *connectionThroughput * emission;
		*connectionThroughput *= segmentTransmittance;

		if (scatterDistance >= 0.f)
			return ray.mint + s * currentStepSize + scatterDistance;
	}

	return -1.f;
}

// Jittered ray-marching estimate of the whole-segment transmittance
Spectrum HeterogeneousVolume::MarchTransmittance(const Ray &ray, const float u) const {
	TauswortheRandomGenerator rng(u);

	const float segmentLength = ray.maxt - ray.mint;
	const u_int steps = Ceil2UInt(Min((float)maxStepsCount, segmentLength / stepSize));
	if (steps == 0)
		return Spectrum(1.f);
	const float currentStepSize = Min(segmentLength / steps, maxStepsCount * stepSize);

	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	Spectrum transmittance(1.f);
	for (u_int s = 0; s < steps; ++s) {
		hitPoint.p = ray(ray.mint + (s + rng.floatValue()) * currentStepSize);
		transmittance *= Exp(-(SigmaA(hitPoint) + SigmaS(hitPoint)) * currentStepSize);
		// Early-out: exp() underflows to an exact 0 only after thousands of
		// steps, so test the filter against a small threshold instead
		if (transmittance.Filter() <= 1e-4f)
			return Spectrum(0.f);
	}

	return transmittance;
}

//------------------------------------------------------------------------------
// Null-collision tracking (delta tracking / ratio tracking)
//
// References:
//  - Woodcock et al., "Techniques used in the GEM code for Monte Carlo
//    neutronics calculation", 1965 (delta tracking)
//  - Novak et al., "Residual Ratio Tracking for Estimating Attenuation in
//    Participating Media", SIGGRAPH Asia 2014 (ratio tracking)
//  - Kutz et al., "Spectral and Decomposition Tracking for Rendering
//    Heterogeneous Volumes", SIGGRAPH 2017 (spectral correctness weight)
//  - Miller et al., "A Null-Scattering Path Integral Formulation of Light
//    Transport", SIGGRAPH 2019 (analog vs weighted tracking weights)
//  - Galtier et al., "Integral formulation of null-collision Monte Carlo
//    algorithms", 2013 (weight correction for underestimated majorants)
//
// The filter channel sigma_t,f = Filter(sigma_a + sigma_s) is used as the
// tracking channel; a per-wavelength factor R corrects the estimate so the
// result stays unbiased for colored media (R accumulates (1 - sigma_t/maj)
// at rejected candidates, resp. (maj - sigma_t)/(maj - sigma_t,f), and the
// sigma_t,lambda / sigma_t,f ratio at an accepted collision).
//------------------------------------------------------------------------------

float HeterogeneousVolume::DeltaTrackScatter(const Ray &ray, const float u,
		const bool scatteredStart, Spectrum *connectionThroughput,
		Spectrum *connectionEmission) const {
	TauswortheRandomGenerator rng(u);

	// Check if I have to support multi-scattering
	const bool scatterAllowed = (!scatteredStart || multiScattering);
	const bool hasEmission = HasVolumeEmissionTexture();

	// Point where to evaluate the volume
	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	// Weighted delta-tracking correction (underestimated majorants)
	float w = 1.f;
	// Spectral correctness factor (per-channel transmittance ratio)
	Spectrum R(1.f);

	MajorantWalk walk;
	WalkInit(&walk, ray, ray.maxt);

	// Safety net for pathological cases (e.g. a non-zero global majorant
	// over an unbounded segment)
	u_int candidateCount = 0;
	const u_int maxCandidateCount = 65536;

	while (WalkNext(&walk)) {
		const float maj = walk.maj;
		if (!(maj > 0.f))
			continue;
		const float invMaj = 1.f / maj;
		// Residual decomposition (Novak et al. 2014 applied to collision
		// sampling): sigma_t = sigma_c + sigma_r with the per-cell
		// minorant sigma_c. The minorant stream is a homogeneous
		// exponential process of real collisions (sampled analytically,
		// no texture evaluations); residual candidates arrive at rate
		// maj - sigma_c and are accepted with (sigma_t - sigma_c)/(maj - sigma_c).
		// Cells with sigma_c ~= maj (dense, smooth media) produce almost
		// no residual candidates: the dominant cost of delta tracking
		// vanishes while the estimator stays the exact analog process
		// (the two streams' combined marginal density is still sigma_t*T).
		const float mn = Min(walk.mn, maj);
		const float resRate = maj - mn;
		const float invResRate = (resRate > 0.f) ? 1.f / resRate : INFINITY;

		float t = walk.t0;
		// Next minorant-stream event (real collision, analytic Exp(sigma_c)).
		// Transmittance-only walks fold the minorant analytically instead.
		float tMinor = (scatterAllowed && (mn > 0.f)) ?
				t + (-logf(1.f - rng.floatValue()) / mn) : INFINITY;

		while (true) {
			float tCand = INFINITY;
			if (resRate > 0.f) {
				t += -logf(1.f - rng.floatValue()) * invResRate;
				tCand = t;
			}
			t = Min(tCand, tMinor);
			if (t >= walk.t1)
				break;
			if (++candidateCount > maxCandidateCount) {
				*connectionThroughput *= w * R;
				return -1.f;
			}
			const bool minorEvent = (t == tMinor);

			hitPoint.p = ray(t);
			const Spectrum sigmaT = SigmaA(hitPoint) + SigmaS(hitPoint);
			const float sigmaTf = sigmaT.Filter();

			// Accumulate emission with the Poisson estimator: candidates
			// arrive at rate maj (minorant stream + residual stream),
			// each contributes Le * T_lambda / maj where T_lambda is the
			// running transmittance estimate (w * R)
			if (hasEmission)
				*connectionEmission += (*connectionThroughput) * w * R *
						Emission(hitPoint) * invMaj;

			if (scatterAllowed) {
				if (minorEvent) {
					// Minorant-stream collision: unconditionally real.
					// If the bound overestimated (sigma_t < sigma_c) the
					// stream overproduced collisions; the weight corrects
					// for the local rate ratio (Galtier et al. 2013).
					if (sigmaTf < mn)
						w *= sigmaTf / mn;
					if (sigmaTf > 0.f)
						for (u_int i = 0; i < COLOR_SAMPLES; ++i)
							R.c[i] *= sigmaT.c[i] / sigmaTf;
					*connectionThroughput *= w * R;
					return t;
				}

				const float pAccept = (sigmaTf - mn) * invResRate;
				if ((pAccept >= 1.f) || (rng.floatValue() < pAccept)) {
					// Real collision at t. The BSDF will apply the
					// sigma_s/sigma_t albedo, so the connection throughput
					// gets sigma_t,lambda / sigma_t,f times the correction
					// for the tracking-channel transmittance (R = T/T_f).
					if (pAccept >= 1.f)
						w *= pAccept;
					if (sigmaTf > 0.f)
						for (u_int i = 0; i < COLOR_SAMPLES; ++i)
							R.c[i] *= sigmaT.c[i] / sigmaTf;
					*connectionThroughput *= w * R;
					return t;
				}

				// Null collision: update the spectral correctness factor.
				// R *= (maj - sigma_t,lambda) / (maj - sigma_t,f)
				// (the minorant cancels out of both numerator and
				// denominator; sigmaT,f < maj is guaranteed here because
				// pAccept >= 1 candidates are always accepted above)
				const float invDenom = 1.f / (maj - sigmaTf);
				for (u_int i = 0; i < COLOR_SAMPLES; ++i)
					R.c[i] *= Max(0.f, maj - sigmaT.c[i]) * invDenom;
			} else {
				// No scattering allowed: residual ratio-tracking estimate
				// T = exp(-sigma_c * l) * prod((maj - sigma_t)/(maj - sigma_c))
				for (u_int i = 0; i < COLOR_SAMPLES; ++i)
					R.c[i] *= Max(0.f, (maj - sigmaT.c[i]) / resRate);
				if (R.Filter() <= 0.f) {
					*connectionThroughput *= w * R;
					return -1.f;
				}
			}
		}

		// No-scatter paths fold the minorant stream analytically
		if (!scatterAllowed && (mn > 0.f))
			for (u_int i = 0; i < COLOR_SAMPLES; ++i)
				R.c[i] *= expf(-mn * (walk.t1 - walk.t0));
	}

	// No collision over the whole segment
	*connectionThroughput *= w * R;
	return -1.f;
}

// Residual ratio tracking (Novak et al. 2014) with a per-cell control
// extinction sigma_c = cell minorant: T = prod(exp(-sigma_c * l) *
// prod(1 - (sigma_t - sigma_c) / (maj - sigma_c))). Cells where sigma_t is
// nearly constant (minorant ~= majorant, e.g. the interior of dense media)
// degenerate to a deterministic exponential: zero texture evaluations and
// zero variance. A zero minorant (unbounded textures) falls back to plain
// ratio tracking.
Spectrum HeterogeneousVolume::RatioTrackTransmittance(const Ray &ray, const float u) const {
	TauswortheRandomGenerator rng(u);

	HitPoint hitPoint;
	hitPoint.Init();
	hitPoint.fixedDir = ray.d;
	hitPoint.p = ray.o;
	hitPoint.geometryN = hitPoint.interpolatedN = hitPoint.shadeN = Normal(-ray.d);
	hitPoint.passThroughEvent = u;

	Spectrum T(1.f);
	u_int candidateCount = 0;
	const u_int maxCandidateCount = 65536;

	MajorantWalk walk;
	WalkInit(&walk, ray, ray.maxt);

	while (WalkNext(&walk)) {
		const float maj = walk.maj;
		if (!(maj > 0.f))
			continue;
		const float sigmaC = Min(walk.mn, maj);
		const float residualRate = maj - sigmaC;

		// Deterministic control-extinction part
		const float segLen = walk.t1 - walk.t0;
		if (sigmaC > 0.f) {
			const float t = expf(-sigmaC * segLen);
			for (u_int i = 0; i < COLOR_SAMPLES; ++i)
				T.c[i] *= t;
		}
		if (!(residualRate > 0.f))
			continue;
		const float invRate = 1.f / residualRate;

		float t = walk.t0;
		while (true) {
			t += -logf(1.f - rng.floatValue()) * invRate;
			if (t >= walk.t1)
				break;
			if (++candidateCount > maxCandidateCount)
				return T;

			hitPoint.p = ray(t);
			const Spectrum sigmaT = SigmaA(hitPoint) + SigmaS(hitPoint);
			for (u_int i = 0; i < COLOR_SAMPLES; ++i)
				T.c[i] *= Max(0.f, 1.f - (sigmaT.c[i] - sigmaC) * invRate);

			// Early-out when the transmittance is numerically zero
			if (T.Filter() <= 1e-6f)
				return Spectrum(0.f);
		}
	}

	return T;
}

Spectrum HeterogeneousVolume::Albedo(const HitPoint &hitPoint) const {
	return schlickScatter.Albedo(hitPoint);
}

Spectrum HeterogeneousVolume::Evaluate(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir, BSDFEvent *event,
		float *directPdfW, float *reversePdfW) const {
	return schlickScatter.Evaluate(hitPoint, localLightDir, localEyeDir, event, directPdfW, reversePdfW);
}

Spectrum HeterogeneousVolume::Sample(const HitPoint &hitPoint,
		const Vector &localFixedDir, Vector *localSampledDir,
		const float u0, const float u1, const float passThroughEvent,
		float *pdfW, BSDFEvent *event) const {
	return schlickScatter.Sample(hitPoint, localFixedDir, localSampledDir,
			u0, u1, passThroughEvent, pdfW, event);
}

void HeterogeneousVolume::Pdf(const HitPoint &hitPoint,
		const Vector &localLightDir, const Vector &localEyeDir,
		float *directPdfW, float *reversePdfW) const {
	schlickScatter.Pdf(hitPoint, localLightDir, localEyeDir, directPdfW, reversePdfW);
}

void HeterogeneousVolume::AddReferencedTextures(std::unordered_set<const Texture *>  &referencedTexs) const {
	Volume::AddReferencedTextures(referencedTexs);

	GetSigmaA().AddReferencedTextures(referencedTexs);
	GetSigmaS().AddReferencedTextures(referencedTexs);
	schlickScatter.GetG().AddReferencedTextures(referencedTexs);
}

void HeterogeneousVolume::UpdateTextureReferences(
	TextureConstRef oldTex, TextureRef newTex
) {
	Volume::UpdateTextureReferences(oldTex, newTex);

	updtex(sigmaA, oldTex, newTex);
	updtex(sigmaS, oldTex, newTex);
	if (&schlickScatter.GetG() == &oldTex)
		schlickScatter.SetG(newTex);
}

PropertiesUPtr HeterogeneousVolume::ToProperties() const {
	PropertiesUPtr props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.volumes." + name + ".type")("heterogeneous"));
	props->Set(Property("scene.volumes." + name + ".absorption")(GetSigmaA().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".scattering")(GetSigmaS().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".asymmetry")(schlickScatter.GetG().GetSDLValue()));
	props->Set(Property("scene.volumes." + name + ".multiscattering")(multiScattering));
	props->Set(Property("scene.volumes." + name + ".steps.size")(stepSize));
	props->Set(Property("scene.volumes." + name + ".steps.maxcount")(maxStepsCount));
	props->Set(Property("scene.volumes." + name + ".tracking")(deltaTracking ? "delta" : "march"));
	props->Set(Property("scene.volumes." + name + ".majorantres")(majorantRes));
	props->Set(Property("scene.volumes." + name + ".phase")(schlickScatter.IsHGPhase() ? "hg" : "schlick"));
	props->Set(Volume::ToProperties());

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
