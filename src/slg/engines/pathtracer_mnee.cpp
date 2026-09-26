/***************************************************************************
 * Copyright 1998-2026 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 *     http://www.apache.org/licenses/LICENSE-2.0                          *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software      *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and      *
 * limitations under the License.                                           *
 ***************************************************************************/

// MNEE (Manifold Next Event Estimation): direct light sampling through a
// single delta specular chain x0 -> x1 -> y.
//
// Reference: Hanika, Droske, Fascione 2015 (MNEE) and Zeltner, Hanika, Gross
// 2020 (Specular Manifold Sampling, SS solver). The implementation follows the
// official SMS reference (dev-tools/reference/manifold_ss.cpp) with two
// adaptations:
//   - the constraint Jacobian dH/dX is computed numerically (finite
//     differences with a re-projection trace) instead of analytically, so
//     curvature (dndu/dndv from the mesh differentials) is included without
//     hand-porting the full derivative algebra;
//   - the specular factor is evaluated with LuxCore's own material code
//     (MirrorMaterial::Kr, GlassMaterial::EvalSpecularTransmission static
//     method) so the estimator is exactly consistent with the renderer's
//     BSDF sampling.
// Validated against a virtual-light truth in dev-tools/mnee_design.md
// (numpy port of the analytic pipeline: E[C_mnee]/E[C_virtual] = 1.00000).
//
// Disjointness (unbiasedness) with the plain direct light estimator: the
// plain estimator contributes 0 whenever a delta specular surface blocks the
// shadow ray (Scene::Intersect stops there), and forward BSDF sampling hits a
// positional delta light with probability 0. MNEE fills exactly the paths
// x0 -> x1 (delta specular) -> y (point/spot/mappoint), so no MIS is needed.
// Mesh/area lights are excluded from MNEE (forward BSDF sampling covers them
// through delta speculars) until a MIS weight is added.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "luxrays/usings.h"
#include "luxrays/core/color/spectral.h"
#include "slg/lights/light.h"
#include "slg/usings.h"
#include "slg/cameras/camera.h"
#include "slg/engines/pathtracer.h"
#include "slg/materials/mirror.h"
#include "slg/materials/glass.h"
#include "slg/scene/scene.h"

using namespace std;
using namespace luxrays;
using namespace slg;

static bool LMneeRejEnabled();

//------------------------------------------------------------------------------
// Small linear algebra helpers (2x2) and the specular chain vertex
//------------------------------------------------------------------------------

namespace {

struct MneeVec2 {
	float x, y;
};

struct MneeMat2 {
	// [a11 a12]
	// [a21 a22]
	float a11, a12, a21, a22;
};

inline float MneeDet(const MneeMat2 &m) {
	return m.a11 * m.a22 - m.a12 * m.a21;
}

inline MneeMat2 MneeInverse(const MneeMat2 &m, const float det) {
	return MneeMat2{ m.a22 / det, -m.a12 / det, -m.a21 / det, m.a11 / det };
}

inline MneeMat2 MneeMul(const MneeMat2 &A, const MneeMat2 &B) {
	return MneeMat2{
		A.a11 * B.a11 + A.a12 * B.a21, A.a11 * B.a12 + A.a12 * B.a22,
		A.a21 * B.a11 + A.a22 * B.a21, A.a21 * B.a12 + A.a22 * B.a22
	};
}

// A specular chain vertex on the caustic caster surface (Zeltner
// ManifoldVertex restricted to what the SS solver needs).
struct MneeVertex {
	Point p;
	Vector dpdu, dpdv;
	Normal n, gn, dndu, dndv;
	// Orthonormal tangents (Zeltner make_orthonormal result)
	Vector s, t;
	// Relative IOR (interior/exterior). 1.f == conductor (mirror).
	float eta;
};

// Endpoint of the specular connection. For a point-like emitter (isDir ==
// false) pos is the emitter position and wo = normalize(pos - x1) moves with
// the vertex; for a directional light (isDir == true) dir is the fixed unit
// direction toward the light, wo = dir is constant, there is no finite
// emitter position and the endpoint Jacobian is taken in direction space.
struct MneeEndpoint {
	Point pos;
	Vector dir;
	bool isDir;
};

inline void MneeCoordinateSystem(const Vector &n, Vector &s, Vector &t);

// Orthonormalize the surface parameterization (Zeltner
// ManifoldVertex::make_orthonormal)
inline void MneeOrthonormalize(MneeVertex &v) {
	const float invNorm1 = 1.f / sqrtf(v.dpdu.LengthSquared());
	v.dpdu *= invNorm1;
	v.dndu *= invNorm1;

	const float dp = Dot(v.dpdu, v.dpdv);
	const Vector dpdvTmp = v.dpdv - dp * v.dpdu;
	const Normal dndvTmp = v.dndv - dp * v.dndu;
	const float invNorm2 = 1.f / sqrtf(dpdvTmp.LengthSquared());
	v.dpdv = dpdvTmp * invNorm2;
	v.dndv = dndvTmp * invNorm2;

	v.s = v.dpdu;
	v.t = v.dpdv;
}

inline void MneeInitVertex(MneeVertex &v, const BSDF &bsdf, const float eta) {
	const HitPoint &hitPoint = bsdf.hitPoint;
	v.p = hitPoint.p;
	v.dpdu = hitPoint.dpdu;
	v.dpdv = hitPoint.dpdv;
	v.n = hitPoint.shadeN;
	v.gn = hitPoint.geometryN;
	v.dndu = hitPoint.dndu;
	v.dndv = hitPoint.dndv;
	v.eta = eta;

	// Meshes without UVs have degenerate differentials: fall back to an
	// orthonormal flat parameterization around the shading normal (curvature
	// terms are lost in this case, i.e. non-UV meshes are treated as flat).
	const float dpduLenSq = v.dpdu.LengthSquared();
	const float dpdvLenSq = v.dpdv.LengthSquared();
	if (dpduLenSq < 1e-24f || dpdvLenSq < 1e-24f ||
			Dot(v.dpdu, v.dpdv) * Dot(v.dpdu, v.dpdv) >
			0.9999f * 0.9999f * dpduLenSq * dpdvLenSq) {
		Vector s, t;
		MneeCoordinateSystem(Vector(v.n.x, v.n.y, v.n.z), s, t);
		v.dpdu = s;
		v.dpdv = t;
		v.dndu = Normal();
		v.dndv = Normal();
	}

	MneeOrthonormalize(v);
}

inline void MneeCoordinateSystem(const Vector &n, Vector &s, Vector &t) {
	if (fabsf(n.x) > fabsf(n.y)) {
		const float invNorm = 1.f / sqrtf(n.x * n.x + n.z * n.z);
		s = Vector(-n.z * invNorm, 0.f, n.x * invNorm);
	} else {
		const float invNorm = 1.f / sqrtf(n.y * n.y + n.z * n.z);
		s = Vector(0.f, n.z * invNorm, -n.y * invNorm);
	}
	t = Cross(n, s);
}

} // anonymous namespace

//------------------------------------------------------------------------------
// Half-vector constraint (Zeltner compute_step_halfvector, n_offset = 0)
//
// Residual C = (h.s, h.t) at the specular vertex; the Jacobian dC/dX is
// computed numerically (finite differences with a re-projection trace so the
// normal rotation / curvature is captured). The light-side Jacobian dC/dX2 is
// pure arithmetic on the fake orthonormal light frame (Zeltner point emitter
// handling).
//------------------------------------------------------------------------------

// etaOverride > 0.f forces the half-vector IOR ratio of the constraint
// (0.f = use the physical vertex eta). The geometric term is always
// evaluated with eta = +1 (the area-measure Jacobian of Zeltner's
// geometric_term, validated by the numpy prototype), while the Newton
// constraint may use the physical eta (e.g. -1 for an opposite-side mirror
// reflection).
static bool MneeResidual(const Point &x0p, const MneeEndpoint &ep,
		const MneeVertex &vtx, MneeVec2 &C, const float etaOverride = 0.f) {
	Vector wi = x0p - vtx.p;
	float r01 = wi.Length();
	if (r01 < 1e-3f)
		return false;
	wi /= r01;

	Vector wo;
	if (ep.isDir) {
		// Directional endpoint: wo is the constant light direction.
		wo = ep.dir;
	} else {
		wo = ep.pos - vtx.p;
		const float r12 = wo.Length();
		if (r12 < 1e-3f)
			return false;
		wo /= r12;
	}

	float eta = (etaOverride > 0.f) ? etaOverride : vtx.eta;
	if (Dot(wi, vtx.gn) < 0.f)
		eta = 1.f / eta;
	Vector h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	h *= 1.f / h.Length();

	C.x = Dot(vtx.s, h);
	C.y = Dot(vtx.t, h);
	return true;
}

static bool MneeReproject(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time,
		const Point &pOff, const Normal &gn, const float gap,
		BSDF &reprojectBsdf) {
	// Start slightly above the surface (along the geometric normal) to avoid
	// self-intersection: the perturbed point is only ~O(eps^2) off the
	// (curved) surface, for a flat one it is exactly on it.
	const Point origin = pOff + gap * Vector(gn.x, gn.y, gn.z);
	Ray reprojectRay(origin, Vector(-gn.x, -gn.y, -gn.z), 0.f, .1f, time);
	RayHit reprojectHit;
	Spectrum reprojectThru;
	PathVolumeInfo reprojectVol;
	if (!scene.Intersect(IntersectionDevicePtr(&device),
			INDIRECT_RAY, &reprojectVol, .5f, &reprojectRay,
			&reprojectHit, &reprojectBsdf, &reprojectThru, nullptr,
			nullptr, false))
		return false;
	return true;
}

static bool MneeConstraintWithJacobian(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time,
		const Point &x0p, const MneeEndpoint &ep,
		const MneeVertex &vtx,
		MneeVec2 &C, MneeMat2 &jac, const float etaOverride = 0.f) {
	if (!MneeResidual(x0p, ep, vtx, C, etaOverride))
		return false;

	const float eps = Max(1e-5f, 1e-4f * Distance(x0p, vtx.p));
	const float C0[2] = { C.x, C.y };
	for (int k = 0; k < 2; ++k) {
		// Perturb along the (orthonormal) tangent, re-project onto the same
		// surface so the normal rotation (curvature) is included
		const Point pPert = vtx.p + ((k == 0) ? eps * vtx.dpdu : eps * vtx.dpdv);
		BSDF pertBsdf;
		if (!MneeReproject(device, scene, time, pPert, vtx.gn, .5f * eps, pertBsdf))
			return false;

		MneeVertex vPert;
		MneeInitVertex(vPert, pertBsdf, vtx.eta);

		MneeVec2 CP;
		if (!MneeResidual(x0p, ep, vPert, CP, etaOverride))
			return false;

		if (k == 0) {
			jac.a11 = (CP.x - C0[0]) / eps;
			jac.a21 = (CP.y - C0[1]) / eps;
		} else {
			jac.a12 = (CP.x - C0[0]) / eps;
			jac.a22 = (CP.y - C0[1]) / eps;
		}
	}

	return true;
}

static MneeMat2 MneeLightJacobian(const Point &x0p, const Point &lightPos,
		const MneeVertex &vtx, const float eps, const float etaOverride = 0.f) {
	// Fake light vertex frame: orthonormal s/t built on the light->x1
	// direction (Zeltner emitter_interaction_to_vertex, point emitter branch)
	Vector dLight = vtx.p - lightPos;
	const float r = dLight.Length();
	if (r < 1e-3f)
		return MneeMat2{ 0.f, 0.f, 0.f, 0.f };

	Vector s2, t2;
	MneeCoordinateSystem(dLight, s2, t2);

	const Vector wi = Normalize(x0p - vtx.p);
	float eta = (etaOverride > 0.f) ? etaOverride : vtx.eta;
	if (Dot(wi, vtx.gn) < 0.f)
		eta = 1.f / eta;

	Vector h = wi + eta * Normalize(lightPos - vtx.p);
	if (eta != 1.f)
		h = -h;
	h *= 1.f / h.Length();

	const float C0[2] = { Dot(vtx.s, h), Dot(vtx.t, h) };
	float CP[2][2];
	for (int k = 0; k < 2; ++k) {
		const Point lightPosP = lightPos + ((k == 0) ? eps * s2 : eps * t2);
		const Vector woP = Normalize(lightPosP - vtx.p);
		Vector hP = wi + eta * woP;
		if (eta != 1.f)
			hP = -hP;
		hP *= 1.f / hP.Length();
		CP[k][0] = Dot(vtx.s, hP);
		CP[k][1] = Dot(vtx.t, hP);
	}

	return MneeMat2{
		(CP[0][0] - C0[0]) / eps, (CP[1][0] - C0[0]) / eps,
		(CP[0][1] - C0[1]) / eps, (CP[1][1] - C0[1]) / eps
	};
}

// Analytic constraint Jacobians and geometric term (Zeltner
// geometric_term structure, evaluated with the physical vertex eta; flat and
// curved surfaces via the dndu/dndv curvature terms of the mesh differentials).
// Returns dw0/dx1 * |det(inv(J1) * J2)|. If jacVertex (the constraint
// Jacobian dC/dX used by the Newton step) is not null it receives J1.
static float MneeGeometricTermWithJacobians(const Point &x0p,
		const MneeEndpoint &ep, const MneeVertex &vtx, MneeMat2 *jacVertex,
		float *det1Out = nullptr, float *det2Out = nullptr) {
	Vector wi = x0p - vtx.p;
	const float r01 = wi.Length();
	if (r01 < 1e-3f)
		return 0.f;
	wi /= r01;

	Vector wo;
	float r12 = 0.f;
	if (ep.isDir) {
		// Directional endpoint: wo is the constant light direction, it does
		// not depend on the vertex position (ilo = 0 below).
		wo = ep.dir;
	} else {
		wo = ep.pos - vtx.p;
		r12 = wo.Length();
		if (r12 < 1e-3f)
			return 0.f;
		wo /= r12;
	}

	float eta = vtx.eta;
	if (Dot(wi, vtx.gn) < 0.f)
		eta = 1.f / eta;
	Vector h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	const float ilh = 1.f / h.Length();
	h *= ilh;
	// Vertex-side coupling of wo to x1. For a point endpoint wo =
	// normalize(pos - x1) moves with x1: ilo = eta*ilh/r12. For a directional
	// endpoint wo is constant: ilo = 0, dropping the wo-motion terms from J1.
	const float ilo = ep.isDir ? 0.f : (1.f / r12) * eta * ilh;
	const float ili = (1.f / r01) * ilh;

	const Vector &s = vtx.s;
	const Vector &t = vtx.t;

	// dC/dx1 (vertex side, with curvature terms)
	Vector dhDdu = -vtx.dpdu * (ili + ilo) +
			wi * (Dot(wi, vtx.dpdu) * ili) +
			wo * (Dot(wo, vtx.dpdu) * ilo);
	Vector dhDdv = -vtx.dpdv * (ili + ilo) +
			wi * (Dot(wi, vtx.dpdv) * ili) +
			wo * (Dot(wo, vtx.dpdv) * ilo);
	dhDdu -= h * Dot(dhDdu, h);
	dhDdv -= h * Dot(dhDdv, h);
	if (eta != 1.f) {
		dhDdu = -dhDdu;
		dhDdv = -dhDdv;
	}
	const float dotHN = Dot(h, vtx.n);
	const float dotHDndu = Dot(h, vtx.dndu);
	const float dotHDndv = Dot(h, vtx.dndv);
	const float dotDpduN = Dot(vtx.dpdu, vtx.n);
	const float dotDpdvN = Dot(vtx.dpdv, vtx.n);
	const MneeMat2 j1{
		Dot(dhDdu, s) - Dot(vtx.dpdu, vtx.dndu) * dotHN - dotDpduN * dotHDndu,
		Dot(dhDdv, s) - Dot(vtx.dpdu, vtx.dndv) * dotHN - dotDpduN * dotHDndv,
		Dot(dhDdu, t) - Dot(vtx.dpdv, vtx.dndu) * dotHN - dotDpdvN * dotHDndu,
		Dot(dhDdv, t) - Dot(vtx.dpdv, vtx.dndv) * dotHN - dotDpdvN * dotHDndv
	};

	// dC/dx2: perturb the endpoint on its own measure. For a point light the
	// endpoint is the emitter position and the fake frame is built on the
	// light->vertex direction (-wo); moving the position by eps*s2 changes wo
	// by (s2 - wo*wo.s2)/r12, captured by the ilo = eta*ilh/r12 factor. For a
	// directional light the endpoint IS the direction: rotating wo by eps*s2
	// changes wo by (s2 - wo*wo.s2) directly, so the factor is eta*ilh (the
	// 1/r12 position->direction conversion does not apply). In both cases the
	// frame is perpendicular to wo, i.e. dLight = -wo.
	const Vector dLight = -wo;
	Vector s2, t2;
	MneeCoordinateSystem(dLight, s2, t2);
	const float ilo2 = ep.isDir ? eta * ilh : ilo;
	Vector dhDdu2 = ilo2 * (s2 - wo * Dot(wo, s2));
	Vector dhDdv2 = ilo2 * (t2 - wo * Dot(wo, t2));
	dhDdu2 -= h * Dot(dhDdu2, h);
	dhDdv2 -= h * Dot(dhDdv2, h);
	if (eta != 1.f) {
		dhDdu2 = -dhDdu2;
		dhDdv2 = -dhDdv2;
	}
	const MneeMat2 j2{
		Dot(dhDdu2, s), Dot(dhDdv2, s),
		Dot(dhDdu2, t), Dot(dhDdv2, t)
	};

	const float det1 = MneeDet(j1);
	const float det2 = MneeDet(j2);

	if (jacVertex)
		*jacVertex = j1;
	if (det1Out)
		*det1Out = det1;
	if (det2Out)
		*det2Out = det2;
	if (fabs(det1) < 1e-9f || fabs(det2) < 1e-15f)
		return 0.f;

	const float dx1Dx2 = fabsf(det2 / det1);
	const Vector d01 = x0p - vtx.p;
	const float r01sq = d01.LengthSquared();
	const float dw0Dx1 = fabsf(Dot(d01, vtx.gn)) / (sqrtf(r01sq) * r01sq);
	return dw0Dx1 * dx1Dx2;
}

static float MneeGeometricTerm(const Point &x0p, const MneeEndpoint &ep,
		const MneeVertex &vtx, float *det1Out = nullptr,
		float *det2Out = nullptr) {
	return MneeGeometricTermWithJacobians(x0p, ep, vtx, nullptr,
			det1Out, det2Out);
}

// Term-by-term diagnostic of the assembly. Enabled with LUX_MNEE_DEBUG=1: for
// every accepted contribution it prints the receiver, the solved specular
// vertex and the light position together with each factor of the estimate, so
// an external analytic model (dev-tools/mnee_glass_term_model.py) can be
// diffed against the live code term by term.
static bool MneeDebugEnabled() {
	static const bool enabled = (getenv("LUX_MNEE_DEBUG") != nullptr);
	return enabled;
}
// Instrument (revert): per-REJECTED-attempt dump (x0, light) for the
// Newton-failure energy measurement (does the -5% glass shortfall sit in
// rejected attempts?). Pair with LUX_MNEE_DEBUG accepted lines.
static bool MneeRejXEnabled() {
	static const bool enabled = (getenv("LUX_MNEE_REJX") != nullptr);
	return enabled;
}
#define MNEE_REJX(why) do { \
		if (MneeRejXEnabled()) { \
			printf("MNEE_REJX %s x0=%.9g %.9g %.9g y=%.9g %.9g %.9g\n", why, \
				x0p.x, x0p.y, x0p.z, lightPos.x, lightPos.y, lightPos.z); \
			fflush(stdout); \
		} \
	} while (0)

//------------------------------------------------------------------------------
// MNEE manifold seed cache (path.mnee.seedcache, GPU mneeSeeds port).
//
// Every converged single-vertex solve stores its vertex in a fixed-size
// world-space hash grid keyed by (endpoint id, occluder mesh, quantized
// blocker-hit position). A later attempt blocked by the same occluder
// region warm-starts Newton from the cached vertex instead of the line
// seed, which is what makes solves on a curved caster converge at all (the
// straight-line seed sits far outside the Newton basin there). The seed
// only selects the basin: the solver still verifies the half-vector
// constraint on re-projected surface vertices, so a stale or colliding
// entry costs iterations but never biases the estimator. Eye-side solves
// namespace entries by the light pointer; light-side (LMNEE) solves share
// one camera sentinel - the same split as the GPU table.
//------------------------------------------------------------------------------

#define MNEE_SEED_CACHE_SIZE_CPU (1u << 14)
#define MNEE_SEED_CELL_FRAC_CPU 64.f
#define LMNEE_CAMERA_SEED_ID 0xFFFFFFFEu

static u_int MneeSeedKey(const u_int lightIndex, const u_int meshIndex,
		const Point &p, const float cellSize) {
	const int cx = Floor2Int(p.x / cellSize);
	const int cy = Floor2Int(p.y / cellSize);
	const int cz = Floor2Int(p.z / cellSize);
	u_int h = (u_int)cx * 73856093u ^ (u_int)cy * 19349663u ^
			(u_int)cz * 83492791u;
	h ^= lightIndex * 2654435761u;
	h ^= meshIndex * 40503u;
	h ^= h >> 16;
	h *= 2246822519u;
	h ^= h >> 13;
	return h & (MNEE_SEED_CACHE_SIZE_CPU - 1u);
}

// Returns true and fills *v when a usable seed was found (the flat tangent
// frame fallback like the GPU Mnee_SeedCacheLookup; the first proposal
// re-projects onto the real surface anyway).
static bool MneeSeedLookup(const PathTracer::MneeSeedEntry *cache,
		const u_int key, const u_int lightIndex, const u_int meshIndex,
		const bool mirrorMode, const float eta, MneeVertex *v) {
	const PathTracer::MneeSeedEntry &e = cache[key];
	if (!e.valid.load(std::memory_order_relaxed) ||
			(e.lightIndex.load(std::memory_order_relaxed) != lightIndex) ||
			(e.meshIndex.load(std::memory_order_relaxed) != meshIndex) ||
			(e.mirrorMode.load(std::memory_order_relaxed) !=
				(mirrorMode ? 1u : 0u)))
		return false;

	const Point p(e.vx.load(std::memory_order_relaxed),
			e.vy.load(std::memory_order_relaxed),
			e.vz.load(std::memory_order_relaxed));
	const Normal n(e.nx.load(std::memory_order_relaxed),
			e.ny.load(std::memory_order_relaxed),
			e.nz.load(std::memory_order_relaxed));
	if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z) ||
			!isfinite(n.x) || !isfinite(n.y) || !isfinite(n.z) ||
			(n.LengthSquared() < 1e-12f))
		return false;

	v->p = p;
	v->n = n;
	v->gn = n;
	MneeCoordinateSystem(Vector(n.x, n.y, n.z), v->dpdu, v->dpdv);
	v->dndu = Normal();
	v->dndv = Normal();
	v->eta = eta;
	MneeOrthonormalize(*v);
	return true;
}

static void MneeSeedStore(PathTracer::MneeSeedEntry *cache,
		const u_int key, const Point &p, const Normal &n,
		const u_int lightIndex, const u_int meshIndex, const bool mirrorMode) {
	PathTracer::MneeSeedEntry &e = cache[key];
	e.vx.store(p.x, std::memory_order_relaxed);
	e.vy.store(p.y, std::memory_order_relaxed);
	e.vz.store(p.z, std::memory_order_relaxed);
	e.nx.store(n.x, std::memory_order_relaxed);
	e.ny.store(n.y, std::memory_order_relaxed);
	e.nz.store(n.z, std::memory_order_relaxed);
	e.lightIndex.store(lightIndex, std::memory_order_relaxed);
	e.meshIndex.store(meshIndex, std::memory_order_relaxed);
	e.mirrorMode.store(mirrorMode ? 1u : 0u, std::memory_order_relaxed);
	e.valid.store(1u, std::memory_order_relaxed);
}

// Single-vertex Newton solve of the specular constraint between x0p and the
// endpoint ep (Zeltner newton_solver, n_offset = 0, step_scale = 1). Shared
// by the eye-side estimator (MNEEDirectSampling) and the light-side camera
// connect (LMNEEConnectToEye): the solver is endpoint-agnostic, only the
// seeding and the contribution assembly differ. On success *vtx and
// *finalBsdf hold the solved specular vertex. seedVtx (optional) replaces
// the line/mirror seeding with a cached manifold solution.
static bool MneeSolveSingleVertex(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const Point &x0p, const MneeEndpoint &ep,
		const BSDF &x0Bsdf, const RayHit &shadowRayHit,
		const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
		const float etaVertex, const u_int maxIterations,
		const MneeVertex *seedVtx, MneeVertex *vtx, BSDF *finalBsdf) {
	if (seedVtx)
		*vtx = *seedVtx;
	else
		MneeInitVertex(*vtx, shadowBsdf, etaVertex);
	*finalBsdf = shadowBsdf;

	// The shadow-ray seed lies exactly on the x0->y line, where the
	// generalized half-vector degenerates (h = wi + eta*wo = (1 - eta)*wi).
	// For a mirror (eta == 1) the line seed carries no information at all and
	// the Newton iteration diverges from there. Seed the reflection chain
	// with the first hit of the ray from x0 toward the endpoint mirrored
	// across the tangent plane at the shadow hit: for a flat mirror this is
	// the exact solution, for curved reflectors the best local approximation
	// (Zeltner's "Modified MNEE" idea, with the mirrored endpoint instead of
	// the shape bbox center). For eta != 1 the line seed is informative
	// ((1 - eta) * wi != 0) and is kept. A seed-cache hit skips the mirror
	// seed trace entirely (GPU parity: the cached vertex is already a
	// verified surface point).
	if (!seedVtx && etaVertex == 1.f) {
		const Normal &gn1 = shadowBsdf.hitPoint.geometryN;
		const Point x1Line = shadowBsdf.hitPoint.p;
		Vector dSeed;
		if (ep.isDir) {
			// Reflect the constant endpoint direction across the tangent
			// plane: the virtual emitter is at infinity along the mirrored
			// direction, so the seed direction is the reflection itself.
			dSeed = ep.dir - 2.f * Dot(ep.dir, gn1) *
					Vector(gn1.x, gn1.y, gn1.z);
		} else {
			const Vector x1ToLight = ep.pos - x1Line;
			const float proj = 2.f * Dot(x1ToLight, gn1);
			const Vector mirroredLight = Vector(ep.pos.x, ep.pos.y, ep.pos.z) -
					proj * Vector(gn1.x, gn1.y, gn1.z);
			dSeed = Normalize(mirroredLight - Vector(x0p.x, x0p.y, x0p.z));
		}
		Ray seedRay(x0Bsdf.GetRayOrigin(dSeed), dSeed, 0.f,
				numeric_limits<float>::infinity(), time);
		RayHit seedHit;
		BSDF seedBsdf;
		Spectrum seedThru;
		PathVolumeInfo seedVol = volInfo;
		if (scene.Intersect(IntersectionDevicePtr(&device),
				INDIRECT_RAY, &seedVol, .5f, &seedRay,
				&seedHit, &seedBsdf, &seedThru, nullptr, nullptr, false) &&
				seedHit.meshIndex == shadowRayHit.meshIndex) {
			MneeInitVertex(*vtx, seedBsdf, etaVertex);
		}
	}

	// Small deterministic tangent offset so the initial half-vector never
	// degenerates exactly (e.g. an off-axis endpoint still on the x0->center
	// line). The offset only selects the Newton basin; the solve itself is
	// pulled to the exact constraint solution.
	const float seedShift = Max(1e-4f, 1e-3f * Distance(x0p, vtx->p));
	vtx->p += (vtx->dpdu + vtx->dpdv) * (seedShift / sqrtf(2.f));

	bool solved = false;
	float beta = 1.f;
	MneeVec2 residual{ 0.f, 0.f };
	MneeMat2 jac{ 0.f, 0.f, 0.f, 0.f };
	u_int dbgMeshMiss = 0, dbgResFail = 0, dbgEsc = 0;

	u_int iteration = 0;
	while (iteration < maxIterations) {
		// Constraint residual and analytic Jacobian of the current vertex
		if (!MneeResidual(x0p, ep, *vtx, residual)) {
			break;
		}
		const float g = MneeGeometricTermWithJacobians(x0p, ep, *vtx, &jac);

		if (sqrtf(residual.x * residual.x + residual.y * residual.y) < 3e-4f) {
			solved = true;
			break;
		}

		const float det = MneeDet(jac);
		if (fabs(det) < 1e-9f) {
			break;
		}
		const MneeMat2 invJac = MneeInverse(jac, det);
		MneeVec2 dX{ invJac.a11 * residual.x + invJac.a12 * residual.y,
				invJac.a21 * residual.x + invJac.a22 * residual.y };
		// Clamp the step near singular configurations (det(J1) -> 0 along the
		// mirror axis): the Newton direction is still the descent direction
		// but its magnitude explodes, so cap it to a fraction of the vertex
		// distance and let the line search find the residual decrease.
		const float dXNorm = sqrtf(dX.x * dX.x + dX.y * dX.y);
		const float dXMax = .25f * Distance(x0p, vtx->p);
		if (dXNorm > dXMax) {
			dX.x *= dXMax / dXNorm;
			dX.y *= dXMax / dXNorm;
		}

		// Line search: accept the step only when the residual decreases and
		// the re-projection stays on the same mesh (Zeltner's beta
		// backtracking, extended with a residual decrease check; the
		// half-vector residual is strongly nonlinear for coarse seeds, so
		// this keeps the iteration inside the Newton basin).
		const float resNorm = sqrtf(residual.x * residual.x + residual.y * residual.y);
		bool stepAccepted = false;
		while (beta > 1e-2f) {
			const Point pProp = vtx->p - beta * (vtx->dpdu * dX.x + vtx->dpdv * dX.y);
			const Vector dProp = Normalize(pProp - x0p);

			Ray propRay(x0Bsdf.GetRayOrigin(dProp), dProp, 0.f,
					numeric_limits<float>::infinity(), time);
			RayHit propHit;
			BSDF propBsdf;
			Spectrum propThru;
			PathVolumeInfo propVol = volInfo;
			if (!scene.Intersect(IntersectionDevicePtr(&device),
					INDIRECT_RAY, &propVol, .5f, &propRay,
					&propHit, &propBsdf, &propThru, nullptr, nullptr, false)) {
				// The proposal left every surface (e.g. past the caster's
				// silhouette as seen from x0, through the open camera
				// face): halving beta lands back on the mesh, so treat it
				// like any rejected step instead of aborting the solve.
				++dbgEsc;
				beta *= .5f;
				++iteration;
				continue;
			}

			if (propHit.meshIndex != shadowRayHit.meshIndex) {
				++dbgMeshMiss;
				beta *= .5f;
				++iteration;
				continue;
			}

			MneeVertex vProp;
			MneeInitVertex(vProp, propBsdf, etaVertex);
			MneeVec2 resProp;
			if (!MneeResidual(x0p, ep, vProp, resProp)) {
				++dbgResFail;
				beta *= .5f;
				++iteration;
				continue;
			}
			const float resPropNorm = sqrtf(resProp.x * resProp.x + resProp.y * resProp.y);

			if (resPropNorm < resNorm) {
				beta = Min(1.f, 2.f * beta);
				*vtx = vProp;
				*finalBsdf = propBsdf;
				stepAccepted = true;
				break;
			}

			beta *= .5f;
			++iteration;
		}
		if (!stepAccepted)
			break;
		++iteration;
	}

	if (!solved && LMneeRejEnabled()) {
		printf("LMNEE_DIVE x0=%.4g %.4g %.4g v=%.4g %.4g %.4g it=%u res=%.4g "
				"meshmiss=%u resfail=%u esc=%u\n",
				x0p.x, x0p.y, x0p.z, vtx->p.x, vtx->p.y, vtx->p.z,
				iteration, sqrtf(residual.x * residual.x + residual.y * residual.y),
				dbgMeshMiss, dbgResFail, dbgEsc);
		fflush(stdout);
	}
	return solved;
}

//------------------------------------------------------------------------------
// PathTracer::MNEEDirectSampling
//------------------------------------------------------------------------------


bool PathTracer::MNEEDirectSampling(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene,
		const float time,
		const EyePathInfo &pathInfo,
		const luxrays::Spectrum &pathThroughput,
		const BSDF &bsdf,
		LightSourceConstRef light, const float lightPickPdf, const float risScale,
		const luxrays::Ray &shadowRay, const float directPdfW0,
		const luxrays::RayHit &shadowRayHit,
		const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
		const float u1, const float u2, const float u3, const float u4,
		SampleResult *sampleResult) const {
	// The occluder material defines the specular event
	const MaterialType seedMatType = shadowBsdf.GetMaterialType();
	const Point &x0p = bsdf.hitPoint.p;

	// Endpoint of the specular connection. A point-like emitter has a finite
	// position: the shadow ray maxt has been rewritten by Scene::Intersect to
	// the occluder distance, so recover the light distance from directPdfW (=
	// squared distance to the light, preserved by Illuminate). A directional
	// light has no finite position: the endpoint is the constant shadow-ray
	// direction (its directPdfW is a solid-angle pdf, not a distance).
	const LightSourceType lightType = light.GetType();
	const bool lightIsDir = (lightType == TYPE_DISTANT ||
			lightType == TYPE_SHARPDISTANT);
	MneeEndpoint ep;
	ep.isDir = lightIsDir;
	if (lightIsDir) {
		ep.dir = Normalize(shadowRay.d);
		ep.pos = Point(0.f, 0.f, 0.f);
	} else {
		ep.pos = shadowRay.o + shadowRay.d * sqrtf(directPdfW0);
		ep.dir = Vector(0.f, 0.f, 0.f);
	}
	const Point lightPos = ep.pos;
	observer_ptr<const MirrorMaterial> mirrorMat = nullptr;
	observer_ptr<const GlassMaterial> glassMat = nullptr;
	float etaVertex = 1.f;
	// True when the occluder is dispersive glass: the solve runs at the
	// path hero wavelength and the connect contribution collapses to the
	// hero bin at assembly (Spectral::KeepHeroBins).
	bool dispersiveConnect = false;
	if (seedMatType == MIRROR) {
		mirrorMat = dynamic_observer_cast<const MirrorMaterial>(shadowBsdf.GetMaterial());
		if (!mirrorMat)
			{ return false; }

		// Generalized half-vector IOR ratio for a conductor: +1 when the two
		// endpoints are on the same side of the surface (h = wi + wo, the
		// same-side reflection case), -1 when they are on opposite sides
		// (h = wi - wo).
		//
		// Only the same-side relation is a reflection: the reflected ray
		// leaves the surface with the normal component of its direction
		// flipped, so it reaches the receiver on the same side of the tangent
		// plane the light is on. Endpoints on opposite sides would need the
		// surface to transmit, which a mirror does not do; solving that case
		// injects light where light tracing, BIDIR and deep path tracing all
		// measure exactly zero (dev-tools/sota_p1_mnee_mirror_physics_test.py,
		// "plane" case).
		const Normal &gn1s = shadowBsdf.hitPoint.geometryN;
		const Vector toX0 = x0p - shadowBsdf.hitPoint.p;
		const Vector toY = lightIsDir ? ep.dir :
				(lightPos - shadowBsdf.hitPoint.p);
		etaVertex = (Dot(toX0, gn1s) * Dot(toY, gn1s) > 0.f) ? 1.f : -1.f;
		if (etaVertex != 1.f)
			return false;
	} else if (seedMatType == GLASS) {
		glassMat = dynamic_observer_cast<const GlassMaterial>(shadowBsdf.GetMaterial());
		if (!glassMat)
			{ return false; }

		const float nc = ExtractExteriorIors(shadowBsdf.hitPoint, glassMat->GetExteriorIOR());
		const float nt = ExtractInteriorIors(shadowBsdf.hitPoint, glassMat->GetInteriorIOR());
		if (nt <= 0.f || nc <= 0.f)
			return false;
		const float cauchyB = glassMat->GetCauchyB() ?
				glassMat->GetCauchyB()->GetFloatValue(shadowBsdf.hitPoint) : 0.f;
		if (cauchyB > 0.f) {
			if (!Spectral::Current())
				// No wavelength state on the path: a single IOR ratio
				// cannot represent dispersion, keep skipping.
				return false;
			// Hero-wavelength solve: the manifold constraint is evaluated
			// at the path hero wavelength, the direction-defining
			// wavelength of a dispersive transmission. The connect exists
			// only for that bin, flagged for the hero collapse below.
			dispersiveConnect = true;
			etaVertex = DispersiveIOR(nt, cauchyB) / nc;
		} else
			etaVertex = nt / nc;
	} else
		{ return false; }

	// Count as an attempt (past the material gate)
	static int attemptCheck = 0;
	++attemptCheck;

	//------------------------------------------------------------------------------
	// Newton solve (Zeltner newton_solver, n_offset = 0, step_scale = 1)
	//------------------------------------------------------------------------------

	// Seed cache policy: the cache is a basin-selection accelerator, never
	// the authority on which root is found. For glass (eta != 1) the cold
	// line seed is free (the shadow-ray hit itself) and defines the
	// reference basin selection - seeding Newton from a cached vertex
	// instead pins every nearby attempt to the first-cached basin, which
	// measured ~5% caustic energy loss on the multi-root bumpy-sphere
	// scene. Glass therefore solves cold first and consults the cache only
	// as a failure rescue below. For a mirror (eta == 1) the cold seed
	// needs an extra seed trace - the cache gets first refusal and skips
	// it entirely. Entries are namespaced by light object, mesh and the
	// incident side (meshIndex*2+1 when the connect ray hits the blocker
	// on its -geometryN side, i.e. exiting a dielectric): a vertex solved
	// for the opposite-side context sits in the wrong Newton basin.
	MneeVertex seedVtx;
	const MneeVertex *seedPtr = nullptr;
	const u_int seedLightIndex = (u_int)(uintptr_t)&light;
	const u_int seedMesh = shadowRayHit.meshIndex * 2u +
			(Dot(Normalize(shadowRay.d), shadowBsdf.hitPoint.geometryN) > 0.f ?
			1u : 0u);
	float seedCellSize = 0.f;
	u_int seedKey = 0;
	if (mneeSeedCacheEnable && mneeSeeds) {
		seedCellSize = Max(scene.GetDataSet().GetBSphere().rad /
				MNEE_SEED_CELL_FRAC_CPU, 1e-4f);
		seedKey = MneeSeedKey(seedLightIndex, seedMesh,
				shadowBsdf.hitPoint.p, seedCellSize);
		if ((etaVertex == 1.f) &&
				MneeSeedLookup(mneeSeeds.get(), seedKey, seedLightIndex,
				seedMesh, mirrorMat != nullptr, etaVertex, &seedVtx))
			seedPtr = &seedVtx;
	}

	MneeVertex vtx;
	BSDF finalBsdf = shadowBsdf;
	bool solveOk = MneeSolveSingleVertex(device, scene, time, x0p, ep, bsdf,
			shadowRayHit, shadowBsdf, volInfo, etaVertex, mneeMaxIterations,
			seedPtr, &vtx, &finalBsdf);
	if (!solveOk && (etaVertex != 1.f) && !seedPtr &&
			mneeSeedCacheEnable && mneeSeeds &&
			MneeSeedLookup(mneeSeeds.get(), seedKey, seedLightIndex,
				seedMesh, mirrorMat != nullptr, etaVertex, &seedVtx)) {
		// Failure rescue: retry once from the cached vertex. Deterministic
		// in (x0, light, cache state) - the estimator stays unbiased.
		solveOk = MneeSolveSingleVertex(device, scene, time, x0p, ep, bsdf,
				shadowRayHit, shadowBsdf, volInfo, etaVertex,
				mneeMaxIterations, &seedVtx, &vtx, &finalBsdf);
	}
	if (!solveOk) {
		MNEE_REJX("newton");
		return false;
	}

	//------------------------------------------------------------------------------
	// Post-solve validity check (Zeltner newton_solver tail): the half-vector
	// formulation can converge to a solution of the wrong specular mode
	//------------------------------------------------------------------------------

	const Vector wi = Normalize(x0p - vtx.p);
	const Vector wo = lightIsDir ? ep.dir : Normalize(lightPos - vtx.p);
	const float cosX = Dot(vtx.gn, wi);
	const float cosY = Dot(vtx.gn, wo);
	const bool refraction = (cosX * cosY < 0.f);
	if (mirrorMat) {
		// Mirror: only a same-side reflection is physical (see the etaVertex
		// gate above). Reject solutions with the opposite side relation - the
		// half-vector formulation can converge to such a mode, and accepting
		// it produced light where none exists.
		if (refraction) {
			MNEE_REJX("mode-mirror");
			return false;
		}
	} else if (!refraction) {
		// Glass: only refraction solutions are supported (Zeltner SS handles
		// dielectric transmission; external dielectric reflection is out of
		// scope)
		MNEE_REJX("mode-glass");
		return false;
	}

	//------------------------------------------------------------------------------
	// Specular factor at the solved vertex, with LuxCore's own material code
	// (mirror: Kr; glass: (1 - F) * eta^2 through the renderer's static
	// evaluation, in the eye-path convention: hitPoint.fromLight == false and
	// localFixedDir = wi)
	//------------------------------------------------------------------------------

	Spectrum specFactor;
	BSDFEvent specEvent;
	if (mirrorMat) {
		specFactor = mirrorMat->GetKr()->GetSpectrumValue(finalBsdf.hitPoint).Clamp(0.f, 1.f);
		specEvent = SPECULAR | REFLECT;
	} else {
		const Spectrum kt = glassMat->GetKt()->GetSpectrumValue(finalBsdf.hitPoint).Clamp(0.f, 1.f);
		const float nc = ExtractExteriorIors(finalBsdf.hitPoint, glassMat->GetExteriorIOR());
		const float nt = ExtractInteriorIors(finalBsdf.hitPoint, glassMat->GetInteriorIOR());

		const Vector localFixedDir = finalBsdf.GetFrame().ToLocal(wi);
		Vector localSampledDir;
		// Pass the real Cauchy coefficient: reaching this point with
		// cauchyB > 0 implies spectral transport (the gate above rejects
		// otherwise), and the spectral branch evaluates the transmission
		// at the same hero wavelength the manifold was solved for.
		const float cauchyB = glassMat->GetCauchyB() ?
				glassMat->GetCauchyB()->GetFloatValue(finalBsdf.hitPoint) : 0.f;
		specFactor = GlassMaterial::EvalSpecularTransmission(finalBsdf.hitPoint,
				localFixedDir, 0.f, kt, nc, nt, cauchyB, &localSampledDir);
		specEvent = SPECULAR | TRANSMIT;
	}
	if (specFactor.Black()) {
		MNEE_REJX("spec");
		return false;
	}

	//------------------------------------------------------------------------------
	// Geometric term (analytic): dw0/dx1 * |det(inv(dc1/dx1) * dc1/dx2)| with
	// Zeltner's curvature structure evaluated with the physical constraint
	// eta. Validated numerically:
	//  - same-side reflection (eta = +1): E[G]/virtual-light truth = 1.00000
	//    (numpy prototype, dev-tools/mnee_design.md);
	//  - opposite-side mirror reflection (eta = -1): G = 1/r'^2 with the
	//    virtual-light truth (derive/curved checks in the session notes).
	// No clamping: the true Jacobian exceeds 1 near glancing configurations
	// and clamping would bias the estimate.
	//------------------------------------------------------------------------------
	float mneeDet1 = 0.f, mneeDet2 = 0.f;
	const float geometricTerm = MneeGeometricTerm(x0p, ep, vtx,
			&mneeDet1, &mneeDet2);
	if (geometricTerm <= 0.f || isnan(geometricTerm) || isinf(geometricTerm)) {
		MNEE_REJX("geoterm");
		return false;
	}

	//------------------------------------------------------------------------------
	// Second segment x1 -> y: Illuminate at the specular vertex and check
	// visibility (the chain is valid only if y is directly visible from x1)
	//------------------------------------------------------------------------------

	// Volume state after the specular event at x1
	PathVolumeInfo volSeg2 = volInfo;
	volSeg2.Update(specEvent, finalBsdf);

	Ray shadowRay2;
	float directPdfW2;
	const Spectrum lightRadiance2 = light.Illuminate(scene, finalBsdf, time,
			u1, u2, u3, shadowRay2, directPdfW2);
	if (lightRadiance2.Black()) {
		MNEE_REJX("seg2black");
		return false;
	}
	verify(!isnan(directPdfW2) && !isinf(directPdfW2));

	if (lightIsDir) {
		// Illuminate() resampled a direction inside the emitter lobe; the
		// manifold endpoint is the fixed direction ep.dir that the solve was
		// run for. Rebuild the second-segment ray toward wo and extend it to
		// the scene bounding sphere (a miss = the directional light is
		// reached). The emitted radiance is constant across the delta / cone
		// lobe, so lightRadiance2 stays valid; only the ray direction and
		// length must reflect the solved endpoint.
		const Vector wo2 = ep.dir;
		const Point o2 = finalBsdf.GetRayOrigin(wo2);
		const BSphere &bs = scene.GetDataSet().GetBSphere();
		const Vector toCenter(bs.center.x - o2.x, bs.center.y - o2.y,
				bs.center.z - o2.z);
		const float approach = Dot(toCenter, wo2);
		const float dist = approach + sqrtf(Max(0.f, bs.rad * bs.rad -
				toCenter.LengthSquared() + approach * approach));
		shadowRay2 = Ray(o2, wo2, 0.f, dist, time);
	}

	RayHit occlHit;
	BSDF occlBsdf;
	Spectrum seg2Throughput;
	PathVolumeInfo volSeg2Trace = volSeg2;
	if (scene.Intersect(IntersectionDevicePtr(&device),
			SHADOW_RAY, &volSeg2Trace, u4, &shadowRay2,
			&occlHit, &occlBsdf, &seg2Throughput, nullptr, nullptr, true)) {
		return false;
	}

	//------------------------------------------------------------------------------
	// Contribution assembly
	//------------------------------------------------------------------------------

	// Receiver BSDF toward x1 (includes the cosine, like the plain estimator)
	BSDFEvent receiverEvent;
	float bsdfPdfW;
	const Spectrum bsdfEval0 = bsdf.Evaluate(Normalize(vtx.p - x0p),
			&receiverEvent, &bsdfPdfW);
	if (bsdfEval0.Black()) {
		return false;
	}
	verify(!isnan(bsdfPdfW) && !isinf(bsdfPdfW));

	// Path depth: one vertex for the receiver event, one for the specular
	// event at x1 (bookkeeping only; the visibility flags are used by the
	// MIS weight in the plain estimator, MNEE has no MIS partner)
	PathDepthInfo mneeDepthInfo = pathInfo.depth;
	mneeDepthInfo.IncDepths(receiverEvent);
	mneeDepthInfo.IncDepths(specEvent);

	// MNEE light weight. The second-segment measure conversion depends on the
	// constraint that was solved:
	//
	//  - plain half-vector (vtx eta == 1, i.e. the same-side reflection seed
	//    h = wi + wo): the analytic geometric term converts to the light's
	//    area measure and the r12^2 factor rides here. Validated against an
	//    independent virtual-light scene at 0.013% (dev-tools/
	//    sota_p1_mnee_test.py, flat mirror replacing the mirror with the
	//    mirrored point light).
	//  - generalized half-vector (eta != 1: dielectric transmission with
	//    eta = nt/nc, and the opposite-side mirror law with eta = -1): the
	//    analytic geometric term already carries the full conversion and
	//    directPdfW2 must NOT be multiplied. Validated per pixel against
	//    converged light tracing plus the analytic per-point radiance
	//    (dev-tools/mnee_glass_pixel_compare.py: including r12^2 gives a 6.2x
	//    shape error over one image, excluding it agrees to 1.3x, the same
	//    spread with which light tracing itself matches the analytic
	//    radiance).
	//
	// For a directional endpoint (lightIsDir) the geometric term is already
	// the direction-space Jacobian (point-light limit: lightPos = x0 + wo*R,
	// R -> infinity makes the perpendicular-frame Jacobian and the r12^2
	// factor cancel exactly). The only remaining factor is the emitter's
	// direction sampling pdf: 1 for a delta direction (sharpdistant), the
	// uniform cone pdf for a distant light.
	//
	// risScale keeps ReSTIR RIS unbiased.
	const bool plainHalfVector = (etaVertex == 1.f);
	const Spectrum lightWeight = lightIsDir ?
			(lightRadiance2 * (risScale / (directPdfW2 * lightPickPdf))) :
			(lightRadiance2 * ((plainHalfVector ? directPdfW2 : 1.f) *
			risScale / lightPickPdf));
	Spectrum incomingRadiance = bsdfEval0 * specFactor * geometricTerm *
			lightWeight * seg2Throughput;
	if (dispersiveConnect)
		// The solved constraint holds at the hero wavelength only: carry
		// the wavelength-selection weight and drop the dead bins.
		incomingRadiance = Spectral::KeepHeroBins(incomingRadiance,
				*Spectral::Current());
	verify(!incomingRadiance.IsNaN() && !incomingRadiance.IsInf());

	if (MneeDebugEnabled()) {
		const Vector dbgWi = Normalize(x0p - vtx.p);
		const Vector dbgWo = lightIsDir ? ep.dir : Normalize(lightPos - vtx.p);
		const float dbgR01 = Distance(x0p, vtx.p);
		const float dbgR12 = lightIsDir ? 0.f : Distance(lightPos, vtx.p);
		// Analytic per-point radiance leaving x0 toward the camera for this
		// path (the quantity a light-transport estimator measures):
		//   (kd/pi)*|cos| * (1-F)*eta_t^2 * I / r12^2
		// built from the live factors, so the two can be compared per pixel
		// without knowing the camera projection.
		const float dbgTruth = bsdfEval0.Filter() * specFactor.Filter() *
				lightRadiance2.Filter() / (dbgR12 * dbgR12);
		printf("MNEE_DBG film=%.4g %.4g x0=%.9g %.9g %.9g | x1=%.9g %.9g %.9g | "
				"y=%.9g %.9g %.9g | r01=%.9g r12=%.9g eta=%.9g dotWiGn=%.9g "
				"dotWiShadeN=%.9g cosWiGn=%.9g cosWoGn=%.9g | det1=%.9g "
				"det2=%.9g G=%.9g dw0dx1=%.9g | spec=%.9g bsdf0=%.9g lr=%.9g "
				"dpdf=%.9g pick=%.9g seg2=%.9g in=%.9g truth=%.9g\n",
				sampleResult->filmX, sampleResult->filmY,
				x0p.x, x0p.y, x0p.z, vtx.p.x, vtx.p.y, vtx.p.z,
				lightPos.x, lightPos.y, lightPos.z,
				dbgR01, dbgR12, etaVertex,
				Dot(dbgWi, vtx.gn), Dot(dbgWi, vtx.n),
				fabsf(Dot(dbgWi, vtx.gn)), fabsf(Dot(dbgWo, vtx.gn)),
				mneeDet1, mneeDet2, geometricTerm,
				fabsf(Dot(x0p - vtx.p, vtx.gn)) / (dbgR01 * dbgR01),
				specFactor.Filter(), bsdfEval0.Filter(),
				lightRadiance2.Filter(), directPdfW2, lightPickPdf,
				seg2Throughput.Filter(), incomingRadiance.Filter(), dbgTruth);
		fflush(stdout);
	}

	sampleResult->AddDirectLight(light.GetID(), specEvent, pathThroughput,
			incomingRadiance, 1.f);
	AccumulateLPE(sampleResult, pathInfo,
			LPEVertexEvent(specEvent, false), LPE_SYM_L, pathThroughput * incomingRadiance);

	// Publish the converged vertex as a warm-start seed only after the full
	// connect validated (seg2 visibility + receiver BSDF): a vertex that
	// solves but fails downstream lands its reuse in the same dead basin,
	// and on multi-root casters those polluted seeds systematically
	// out-compete the cold line seed (~5% caustic energy loss measured on
	// the bumpy-sphere seedcache scene). Keyed by the blocker-hit cell,
	// valued by the solved vertex (GPU SolveEnd parity).
	if (mneeSeedCacheEnable && mneeSeeds)
		MneeSeedStore(mneeSeeds.get(), seedKey, vtx.p, vtx.n,
				seedLightIndex, seedMesh, mirrorMat != nullptr);

	return true;
}

//------------------------------------------------------------------------------
// MNEE, multi-specular chain (N >= 2 delta specular vertices)
//
// The single vertex solver above covers eye -> x0 -> x1 -> y. Closed glass
// slabs and glass balls need more: the light reaches the receiver through two
// or more refractions, so the shadow ray from x0 is stopped by a specular
// surface that cannot see the light either (the next face is in the way).
//
// This solver generalizes the chain to N vertices
//
//   eye -> x0 -> x1 -> ... -> xN -> y
//
// with one generalized half-vector constraint per vertex. The numerics are
// ported from the numpy prototype that was verified before the port
// (dev-tools/mnee_ms_proto.py, notes in dev-tools/mnee_design.md section 4b):
//
//   - the constraint at vertex i involves only x_{i-1}, x_i and x_{i+1}, so
//     the finite difference Jacobian is exactly block tridiagonal (verified
//     there: no leakage into the far blocks);
//   - the Newton step solves that system with the block Thomas recursion
//     (verified against the dense solution), and the matrix right hand side
//     variant of the same recursion against the dense determinant (2.6e-14);
//   - the chain geometric term is |det(dx_1/dy)|, i.e. the (1, N) block of the
//     inverse constraint Jacobian times the light's own Jacobian; for N = 1 it
//     reduces to the single vertex term |det(J2) / det(J1)|;
//   - Newton converges with Snell's law satisfied at every vertex (1.3e-09).
//
// The topology is discovered by tracing the straight ray from x0 toward the
// light and collecting the delta specular surfaces it pierces (the reference's
// mnee_init seeding); the solve then bends the vertices onto the refraction
// manifold. The delta mode of a vertex is fixed by its material (a mirror
// reflects, glass transmits) and the constraint IOR ratio follows the side of
// the previous point, exactly as in the single vertex solver.
//
// Disjointness with the plain estimator follows the single vertex argument:
// the shadow ray from x0 is blocked, and the chain cannot be sampled by
// forward BSDF sampling of a positional delta light. Light tracing and BIDIR
// do measure these paths, so they are the unbiased reference (see the slab
// case of dev-tools/sota_p1_mnee_glass_test.py).
//------------------------------------------------------------------------------

// One vertex of the MNEE chain
struct MneeChainVertex {
	MneeVertex v;
	// The material's relative IOR (interior/exterior), 1 for a conductor. The
	// constraint flips it when the previous point lies on the -geometryN side,
	// exactly like MneeResidual does for a single vertex.
	float etaVertex;
	// True for a dispersive-glass vertex (cauchyB > 0, spectral paths only):
	// the constraint ran at the hero wavelength, so the chain contribution
	// collapses to the hero bin.
	bool dispersive;
	Spectrum specFactor;
	BSDFEvent specEvent;
	BSDF bsdf;
	observer_ptr<const MirrorMaterial> mirrorMat;
	observer_ptr<const GlassMaterial> glassMat;
};

static const u_int MNEE_MS_MAX_VERTICES = 4;

// Block tridiagonal constraint Jacobian: the 2x2 blocks of x_{i-1}, x_i, x_{i+1}
struct MneeJacobianBlock {
	MneeMat2 prev, cur, next;
};

// 2x2 helpers on top of the anonymous namespace ones
inline MneeMat2 MneeMatSub(const MneeMat2 &A, const MneeMat2 &B) {
	return MneeMat2{ A.a11 - B.a11, A.a12 - B.a12, A.a21 - B.a21, A.a22 - B.a22 };
}

inline MneeVec2 MneeMatVec(const MneeMat2 &A, const MneeVec2 &v) {
	return MneeVec2{ A.a11 * v.x + A.a12 * v.y, A.a21 * v.x + A.a22 * v.y };
}

// The generalized half-vector constraint at one chain vertex: h = wi + eta * wo
// must be parallel to the surface normal, with eta the material's relative IOR
// flipped when the previous point lies on the -geometryN side (LuxCore's
// exterior IOR sits on the +geometryN side; the same convention as
// MneeResidual). wi points at the previous chain point, wo at the next one.
// For the last vertex of a chain ending at a directional light, wo is the
// constant light direction instead of a point difference (dirNext != nullptr).
// Returns false for degenerate configurations.
static bool MneeChainResidual(const Point &pPrev, const Point &pNext,
		const Vector *dirNext, const MneeVertex &v, const float etaVertex,
		MneeVec2 &C) {
	Vector wi = pPrev - v.p;
	const float r0 = wi.Length();
	if (r0 < 1e-4f)
		return false;
	wi *= 1.f / r0;

	Vector wo;
	if (dirNext) {
		wo = *dirNext;
	} else {
		wo = pNext - v.p;
		const float r1 = wo.Length();
		if (r1 < 1e-4f)
			return false;
		wo *= 1.f / r1;
	}

	float eta = etaVertex;
	if (Dot(wi, v.gn) < 0.f)
		eta = 1.f / eta;

	Vector h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	const float l = h.Length();
	// Collinear segments make the half-vector degenerate (for eta == 1,
	// wi == -wo gives h == 0): the constraint carries no information there.
	if (l < 1e-5f)
		return false;
	h *= 1.f / l;

	C.x = Dot(v.s, h);
	C.y = Dot(v.t, h);
	return true;
}

// Residuals of the whole chain (no scene access)
static bool MneeChainResiduals(const Point &x0p, const MneeEndpoint &ep,
		const MneeChainVertex *chain, const u_int n,
		MneeVec2 *residual, float &maxResidual) {
	Point pts[MNEE_MS_MAX_VERTICES + 2];
	pts[0] = x0p;
	for (u_int i = 0; i < n; ++i)
		pts[i + 1] = chain[i].v.p;
	pts[n + 1] = ep.pos;

	maxResidual = 0.f;
	for (u_int i = 0; i < n; ++i) {
		const Vector *dirNext = (ep.isDir && (i == n - 1)) ? &ep.dir : nullptr;
		if (!MneeChainResidual(pts[i], pts[i + 2], dirNext, chain[i].v,
				chain[i].etaVertex, residual[i]))
			return false;
		maxResidual = Max(maxResidual, sqrtf(residual[i].x * residual[i].x +
				residual[i].y * residual[i].y));
	}

	return true;
}

// Finite difference constraint Jacobian of the whole chain. Perturbing vertex i
// changes the constraints at i-1, i and i+1 only, so the result is exactly
// block tridiagonal. The perturbed vertex is re-projected onto its surface, so
// the normal rotation (curvature) is included, exactly like the single vertex
// solver's MneeConstraintWithJacobian.
static bool MneeChainJacobian(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const Point &x0p, const MneeEndpoint &ep,
		const MneeChainVertex *chain, const u_int n,
		MneeJacobianBlock *blocks, MneeVec2 *residual, float &maxResidual) {
	if (!MneeChainResiduals(x0p, ep, chain, n, residual, maxResidual))
		return false;

	const MneeMat2 zero{ 0.f, 0.f, 0.f, 0.f };
	for (u_int i = 0; i < n; ++i) {
		blocks[i].prev = zero;
		blocks[i].cur = zero;
		blocks[i].next = zero;
	}

	// Note: the finite difference Jacobian is computed even when the residuals
	// are already zero (i.e. the straight line seed is the exact solution, as
	// it is for a flat interface at normal incidence). The geometric term needs
	// the Jacobian regardless, so returning early here would drop exactly those
	// contributions.
	Point pts[MNEE_MS_MAX_VERTICES + 2];
	pts[0] = x0p;
	for (u_int i = 0; i < n; ++i)
		pts[i + 1] = chain[i].v.p;
	pts[n + 1] = ep.pos;

	for (u_int i = 0; i < n; ++i) {
		const float eps = Max(1e-5f, 1e-4f * Distance(pts[i], pts[i + 1]));
		for (u_int k = 0; k < 2; ++k) {
			const Point pPert = chain[i].v.p +
					((k == 0) ? eps * chain[i].v.dpdu : eps * chain[i].v.dpdv);
			BSDF pertBsdf;
			if (!MneeReproject(device, scene, time, pPert, chain[i].v.gn, .5f * eps, pertBsdf))
				return false;

			MneeVertex pertV;
			MneeInitVertex(pertV, pertBsdf, chain[i].etaVertex);

			Point ptsP[MNEE_MS_MAX_VERTICES + 2];
			for (u_int t = 0; t < n + 2; ++t)
				ptsP[t] = pts[t];
			ptsP[i + 1] = pertV.p;

			for (int j = (int)i - 1; j <= (int)i + 1; ++j) {
				if ((j < 0) || (j >= (int)n))
					continue;

				MneeVec2 CP;
				const MneeVertex &vj = (j == (int)i) ? pertV : chain[j].v;
				const Vector *dirNext = (ep.isDir && (j == (int)n - 1)) ?
						&ep.dir : nullptr;
				if (!MneeChainResidual(ptsP[j], ptsP[j + 2], dirNext, vj,
						chain[j].etaVertex, CP))
					return false;

				MneeMat2 *block;
				if (j == (int)i - 1)
					block = &blocks[j].next;
				else if (j == (int)i)
					block = &blocks[i].cur;
				else
					block = &blocks[j].prev;

				const float dCx = (CP.x - residual[j].x) / eps;
				const float dCy = (CP.y - residual[j].y) / eps;
				if (k == 0) {
					block->a11 = dCx;
					block->a21 = dCy;
				} else {
					block->a12 = dCx;
					block->a22 = dCy;
				}
			}
		}
	}

	return true;
}

// Block Thomas decomposition (Kaplanyan 2014 supplement fig. 2, the reference's
// invert_tridiagonal_step). Returns false on a singular diagonal block.
static bool MneeTridiagonalInvert(const MneeJacobianBlock *blocks, const u_int n,
		MneeMat2 *tmp, MneeMat2 *invLambda) {
	if (n == 0)
		return false;

	float det = MneeDet(blocks[0].cur);
	if (fabsf(det) < 1e-12f)
		return false;
	invLambda[0] = MneeInverse(blocks[0].cur, det);
	tmp[0] = blocks[0].prev;

	for (u_int i = 1; i < n; ++i) {
		tmp[i] = MneeMul(blocks[i].prev, invLambda[i - 1]);
		const MneeMat2 m = MneeMatSub(blocks[i].cur,
				MneeMul(tmp[i], blocks[i - 1].next));
		det = MneeDet(m);
		if (fabsf(det) < 1e-12f)
			return false;
		invLambda[i] = MneeInverse(m, det);
	}

	return true;
}

// Newton step of the chain (vector right hand side)
static bool MneeTridiagonalSolve(const MneeJacobianBlock *blocks, const u_int n,
		const MneeVec2 *rhs, MneeVec2 *dx) {
	MneeMat2 tmp[MNEE_MS_MAX_VERTICES], invLambda[MNEE_MS_MAX_VERTICES];
	if (!MneeTridiagonalInvert(blocks, n, tmp, invLambda))
		return false;

	dx[0] = rhs[0];
	for (u_int i = 1; i < n; ++i) {
		const MneeVec2 back = MneeMatVec(tmp[i], dx[i - 1]);
		dx[i] = MneeVec2{ rhs[i].x - back.x, rhs[i].y - back.y };
	}
	dx[n - 1] = MneeMatVec(invLambda[n - 1], dx[n - 1]);
	for (int i = (int)n - 2; i >= 0; --i) {
		const MneeVec2 back = MneeMatVec(blocks[i].next, dx[i + 1]);
		dx[i] = MneeMatVec(invLambda[i], MneeVec2{ dx[i].x - back.x,
				dx[i].y - back.y });
	}

	return true;
}

// The same verified recursion with a matrix right hand side: a unit block at
// the last vertex yields the (1, N) block of the inverse constraint Jacobian,
// which is the chain's response to a perturbation of the light.
static bool MneeTridiagonalSolveMatrixRhs(const MneeJacobianBlock *blocks,
		const u_int n, MneeMat2 &dxFirst) {
	MneeMat2 tmp[MNEE_MS_MAX_VERTICES], invLambda[MNEE_MS_MAX_VERTICES];
	if (!MneeTridiagonalInvert(blocks, n, tmp, invLambda))
		return false;

	const MneeMat2 zero{ 0.f, 0.f, 0.f, 0.f };
	MneeMat2 d[MNEE_MS_MAX_VERTICES];
	for (u_int i = 0; i < n; ++i)
		d[i] = zero;
	d[n - 1] = MneeMat2{ 1.f, 0.f, 0.f, 1.f };

	for (u_int i = 1; i < n; ++i)
		d[i] = MneeMatSub(d[i], MneeMul(tmp[i], d[i - 1]));
	d[n - 1] = MneeMul(invLambda[n - 1], d[n - 1]);
	for (int i = (int)n - 2; i >= 0; --i)
		d[i] = MneeMul(invLambda[i], MneeMatSub(d[i],
				MneeMul(blocks[i].next, d[i + 1])));

	dxFirst = d[0];
	return true;
}

// dC_last/dy: how the last vertex's constraint reacts to perturbing the light
// endpoint, the emitter_interaction_to_vertex analog (same construction as
// MneeLightJacobian). For a point emitter the endpoint moves along a tangent
// frame; for a directional endpoint the direction itself is perturbed (the
// position->direction 1/r conversion does not apply, same as the single
// vertex solver's j2 block).
static MneeMat2 MneeChainLightJacobian(const Point &pPrev, const MneeEndpoint &ep,
		const MneeVertex &v, const float etaVertex, const float eps) {
	const MneeMat2 zero{ 0.f, 0.f, 0.f, 0.f };

	const Vector wo = ep.isDir ? ep.dir : Normalize(ep.pos - v.p);
	const Vector dLight = -wo;
	Vector s2, t2;
	MneeCoordinateSystem(dLight, s2, t2);

	const Vector wi = Normalize(pPrev - v.p);
	float eta = etaVertex;
	if (Dot(wi, v.gn) < 0.f)
		eta = 1.f / eta;

	Vector h = wi + eta * wo;
	if (eta != 1.f)
		h = -h;
	const float l = h.Length();
	if (l < 1e-6f)
		return zero;
	h *= 1.f / l;
	const float C0x = Dot(v.s, h), C0y = Dot(v.t, h);

	float CP[2][2];
	for (u_int k = 0; k < 2; ++k) {
		// Point endpoint: move the position on the tangent frame.
		// Directional endpoint: tilt the direction by eps on the same frame.
		const Vector woP = ep.isDir ?
				Normalize(ep.dir + ((k == 0) ? eps * s2 : eps * t2)) :
				Normalize(ep.pos + ((k == 0) ? eps * s2 : eps * t2) - v.p);
		Vector hP = wi + eta * woP;
		if (eta != 1.f)
			hP = -hP;
		hP *= 1.f / hP.Length();
		CP[k][0] = Dot(v.s, hP);
		CP[k][1] = Dot(v.t, hP);
	}

	return MneeMat2{ (CP[0][0] - C0x) / eps, (CP[1][0] - C0x) / eps,
			(CP[0][1] - C0y) / eps, (CP[1][1] - C0y) / eps };
}

// Initialize a chain vertex from a delta specular hit. Returns false for
// materials the chain solver does not model.
static bool MneeChainVertexInit(MneeChainVertex &cv, const BSDF &bsdf) {
	const MaterialType type = bsdf.GetMaterialType();

	cv.bsdf = bsdf;
	cv.mirrorMat = nullptr;
	cv.glassMat = nullptr;
	cv.specEvent = SPECULAR;
	cv.specFactor = Spectrum(1.f);
	cv.dispersive = false;

	if (type == MIRROR) {
		cv.mirrorMat = dynamic_observer_cast<const MirrorMaterial>(bsdf.GetMaterial());
		if (!cv.mirrorMat)
			return false;
		cv.etaVertex = 1.f;
		cv.specFactor = cv.mirrorMat->GetKr()->GetSpectrumValue(bsdf.hitPoint).Clamp(0.f, 1.f);
		cv.specEvent |= REFLECT;
	} else if (type == GLASS) {
		cv.glassMat = dynamic_observer_cast<const GlassMaterial>(bsdf.GetMaterial());
		if (!cv.glassMat)
			return false;

		const float nc = ExtractExteriorIors(bsdf.hitPoint, cv.glassMat->GetExteriorIOR());
		const float nt = ExtractInteriorIors(bsdf.hitPoint, cv.glassMat->GetInteriorIOR());
		if (nt <= 0.f || nc <= 0.f)
			return false;
		const float cauchyB = cv.glassMat->GetCauchyB() ?
				cv.glassMat->GetCauchyB()->GetFloatValue(bsdf.hitPoint) : 0.f;
		if (cauchyB > 0.f) {
			if (!Spectral::Current())
				// No wavelength state: keep ending the chain here.
				return false;
			// Hero-wavelength constraint IOR (see MNEEDirectSampling).
			cv.dispersive = true;
			cv.etaVertex = DispersiveIOR(nt, cauchyB) / nc;
		} else
			cv.etaVertex = nt / nc;
		cv.specEvent |= TRANSMIT;
	} else
		return false;

	MneeInitVertex(cv.v, bsdf, cv.etaVertex);
	return true;
}

// Continue the discovery walk through a collected vertex: refract at
// dielectrics (etaVertex = interior/exterior), reflect at mirrors and on
// TIR. Walking the physical refraction - instead of marching along the
// straight receiver -> endpoint line - finds interfaces the straight ray
// misses when the solved path deviates far from it (curved glass near the
// silhouette).
static Vector MneeChainWalkDir(const Vector &d, const MneeChainVertex &cv) {
	const Vector gn(cv.v.gn.x, cv.v.gn.y, cv.v.gn.z);
	if (cv.mirrorMat)
		return d - 2.f * Dot(d, gn) * gn;

	// d travels into the vertex; gn is the raw mesh normal. When it faces
	// the incident side the ray enters the denser medium (etaRel = nc/nt),
	// otherwise it exits (etaRel = nt/nc) - the same side test the solver's
	// half-vector residual uses (Dot(wi, gn), wi = -d).
	Vector N = gn;
	float cosI = -Dot(d, N);
	float etaRel;
	if (cosI > 0.f)
		etaRel = 1.f / cv.etaVertex;
	else {
		N = -N;
		cosI = -cosI;
		etaRel = cv.etaVertex;
	}
	const float sin2T = etaRel * etaRel * (1.f - cosI * cosI);
	if (sin2T >= 1.f)
		return d - 2.f * Dot(d, N) * N;     // TIR: continue as a reflection
	const float cosT = sqrtf(1.f - sin2T);
	return etaRel * d + (etaRel * cosI - cosT) * N;
}

// Chain topology: walk the refracted/reflected path from the connect
// blocker and collect the delta specular surfaces it crosses (the first
// vertex is the shadow ray's blocker, which the caller already has).
// seedV0/seedV1 optionally carry a consistent [solved vertex, re-blocker]
// pair from the failed single-vertex solve: both lie on the solved ray's
// path, a far better Newton seed than geometry the straight line finds.
// Returns the number of collected vertices; the solver only handles 2 or
// more (a single vertex is the SS solver's job).
static u_int MneeChainDiscover(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const Point &x0p, const MneeEndpoint &ep,
		const float u4, PathVolumeInfo volInfo,
		const BSDF &firstBsdf, MneeChainVertex *chain, const u_int maxVertices,
		const BSDF *seedV0 = nullptr, const BSDF *seedV1 = nullptr) {
	if (maxVertices < 2)
		return 0;
	if (!MneeChainVertexInit(chain[0], seedV0 ? *seedV0 : firstBsdf))
		return 0;
	u_int n = 1;

	// The direction the path arrives at the last collected vertex: the
	// straight x0 -> endpoint line for a fresh walk (the fixed direction
	// for a directional light), the solved seg2 direction when a warm
	// pair seeded the chain.
	Vector dir = ep.isDir ? ep.dir : Normalize(ep.pos - x0p);
	if (seedV1 && MneeChainVertexInit(chain[1], *seedV1)) {
		chain[1].v.p = seedV1->hitPoint.p;
		dir = ep.isDir ? ep.dir : Normalize(ep.pos - chain[0].v.p);
		n = 2;
	}

	// The walk follows the physical refraction/reflection at each
	// collected interface (the straight line misses interfaces where the
	// solved path deviates far from it).
	dir = MneeChainWalkDir(dir, chain[n - 1]);
	// Whether the walk is currently inside a dielectric body (entered a
	// glass interface and not yet exited). While inside, a matte hit is
	// geometry intruding into the glass (e.g. a box the caster rests on):
	// skip it and keep walking so the chain still collects the exit
	// interface - the solver validates the final path, the discovery only
	// needs the right topology plus plausible positions.
	bool insideGlass;
	if (n == 2) {
		// chain[0] was entered, chain[1] was hit on its far side: the
		// walk inside ends only when chain[1] was an exit (normal faces
		// the walk direction side).
		insideGlass = (chain[0].glassMat != nullptr) &&
				(Dot(Normalize(chain[0].v.p - x0p), chain[0].v.gn) < 0.f);
		if (chain[1].glassMat)
			insideGlass = (Dot(Normalize(chain[1].v.p - chain[0].v.p),
					chain[1].v.gn) < 0.f);
	} else
		insideGlass = (chain[0].glassMat != nullptr) &&
				(Dot(dir, chain[0].v.gn) < 0.f);
	BSDF skipBsdf;              // ray origin when stepping past intruders
	bool skipped = false;
	static const bool discDbg = (getenv("LUX_MNEE_DISC") != nullptr);
	for (u_int guard = 0; (guard < MNEE_MS_MAX_VERTICES * 4) && (n < maxVertices); ++guard) {
		const BSDF &originBsdf = skipped ? skipBsdf : chain[n - 1].bsdf;
		Ray ray(originBsdf.GetRayOrigin(dir), dir, 0.f,
				numeric_limits<float>::infinity(), time);
		RayHit hit;
		BSDF hitBsdf;
		Spectrum through;
		PathVolumeInfo vol = volInfo;
		if (!scene.Intersect(IntersectionDevicePtr(&device), INDIRECT_RAY, &vol, u4,
				&ray, &hit, &hitBsdf, &through, nullptr, nullptr, false)) {
			if (discDbg)
				printf("MNEE_DISC escape n=%u dir=%.4g %.4g %.4g o=%.4g %.4g %.4g\n",
						n, dir.x, dir.y, dir.z, ray.o.x, ray.o.y, ray.o.z);
			break;      // the ray escaped: the last vertex may see the light
		}

		if (!hitBsdf.IsDelta() || !(hitBsdf.GetEventTypes() & SPECULAR)) {
			if (insideGlass) {
				// Opaque intrusion inside the dielectric: step past it
				// along the same direction and keep collecting.
				skipBsdf = hitBsdf;
				skipped = true;
				continue;
			}
			if (discDbg)
				printf("MNEE_DISC nonspec n=%u hit=%.4g %.4g %.4g delta=%d ev=%u mat=%d\n",
						n, hitBsdf.hitPoint.p.x, hitBsdf.hitPoint.p.y,
						hitBsdf.hitPoint.p.z, (int)hitBsdf.IsDelta(),
						(unsigned)hitBsdf.GetEventTypes(),
						(int)hitBsdf.GetMaterialType());
			break;      // a non specular surface ends the chain
		}

		MneeChainVertex cv;
		if (!MneeChainVertexInit(cv, hitBsdf)) {
			if (discDbg)
				printf("MNEE_DISC initfail n=%u mat=%d\n", n,
						(int)hitBsdf.GetMaterialType());
			break;
		}
		const Vector dIn = dir;
		dir = MneeChainWalkDir(dir, cv);
		if (cv.glassMat)
			// Entering when the mesh normal faces the incident side; a
			// TIR bounce keeps the walk inside either way.
			insideGlass = (Dot(dIn, cv.v.gn) < 0.f) || (Dot(dIn, dir) < 0.f);
		chain[n] = cv;
		skipped = false;
		++n;
	}

	return n;
}

// Newton solve of the whole specular chain (block tridiagonal step, line
// search with re-projection onto the shapes). Shared by the eye-side
// estimator (MNEEMultiDirectSampling) and the light-side camera connect
// (LMNEEMultiConnectToEye): the solver is endpoint-agnostic, only the
// contribution assembly differs. *failWhy reports the failure stage for the
// LUX_MNEE_REJ rejection counters.
static bool MneeSolveChain(
		luxrays::IntersectionDeviceRef device, SceneConstRef scene,
		const float time, const Point &x0p, const MneeEndpoint &endpoint,
		MneeChainVertex *chain, const u_int n, const u_int maxIterations,
		const char **failWhy) {
	MneeJacobianBlock blocks[MNEE_MS_MAX_VERTICES];
	MneeVec2 residual[MNEE_MS_MAX_VERTICES];
	float maxResidual = 0.f;
	float beta = 1.f;
	u_int iteration = 0;
	*failWhy = "iterations";

	// Per-iteration trace (LUX_MNEE_ITER): shows whether the Newton stalls at
	// a fixed residual, creeps down, or oscillates. The outer caustic of a
	// curved caster is where the chain solve fails, and the failure mode
	// decides what to change (step size policy vs the walk parameterization
	// itself).
	static const bool iterDebug = (getenv("LUX_MNEE_ITER") != nullptr);

	while (iteration < maxIterations) {
		if (!MneeChainJacobian(device, scene, time, x0p, endpoint, chain, n,
				blocks, residual, maxResidual)) {
			*failWhy = "jacobian";
			return false;
		}
		if (iterDebug) {
			printf("MNEE_MS_IT x0=%.9g %.9g %.9g it=%u res=%.6g beta=%.4g",
					x0p.x, x0p.y, x0p.z, iteration, maxResidual, beta);
			for (u_int k = 0; k < n; ++k)
				printf(" | x%u=%.9g %.9g %.9g C=(%.4g %.4g)", k + 1,
						chain[k].v.p.x, chain[k].v.p.y, chain[k].v.p.z,
						residual[k].x, residual[k].y);
			printf("\n");
		}
		if (maxResidual < 1e-5f)
			return true;

		MneeVec2 dx[MNEE_MS_MAX_VERTICES];
		if (!MneeTridiagonalSolve(blocks, n, residual, dx)) {
			*failWhy = "tridiagonal";
			return false;
		}

		bool stepAccepted = false;
		while (beta > 1e-2f) {
			MneeChainVertex trial[MNEE_MS_MAX_VERTICES];
			bool projected = true;
			for (u_int i = 0; i < n; ++i) {
				const float eps = Max(1e-5f, 1e-4f * Distance(x0p, chain[i].v.p));
				const Point pProp = chain[i].v.p - beta *
						(chain[i].v.dpdu * dx[i].x + chain[i].v.dpdv * dx[i].y);
				BSDF propBsdf;
				if (!MneeReproject(device, scene, time, pProp, chain[i].v.gn,
						.5f * eps, propBsdf) ||
						(propBsdf.GetMaterialType() != chain[i].bsdf.GetMaterialType())) {
					projected = false;
					break;
				}
				// Keep the material data and the constraint mode of the seed:
				// only the surface position and differentials move.
				trial[i] = chain[i];
				MneeInitVertex(trial[i].v, propBsdf, chain[i].etaVertex);
				trial[i].bsdf = propBsdf;
			}

			if (projected) {
				MneeVec2 trialRes[MNEE_MS_MAX_VERTICES];
				float trialMax = 0.f;
				if (MneeChainResiduals(x0p, endpoint, trial, n, trialRes, trialMax) &&
						(trialMax < maxResidual)) {
					for (u_int i = 0; i < n; ++i)
						chain[i] = trial[i];
					beta = Min(1.f, 2.f * beta);
					stepAccepted = true;
					break;
				}
			}

			beta *= .5f;
			++iteration;
		}
		if (!stepAccepted) {
			*failWhy = "no-step";
			return false;
		}
		++iteration;
	}

	return false;
}

bool PathTracer::MNEEMultiDirectSampling(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene,
		const float time,
		const EyePathInfo &pathInfo,
		const luxrays::Spectrum &pathThroughput,
		const BSDF &bsdf,
		LightSourceConstRef light, const float lightPickPdf, const float risScale,
		const luxrays::Ray &shadowRay, const float directPdfW0,
		const luxrays::RayHit &shadowRayHit,
		const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
		const float u1, const float u2, const float u3, const float u4,
		SampleResult *sampleResult) const {
	const Point &x0p = bsdf.hitPoint.p;
	// Manifold endpoint: the shadow ray maxt has been rewritten by
	// Scene::Intersect to the occluder distance, so a point light's position
	// is recovered from directPdfW (= squared distance, preserved by
	// Illuminate). A directional light has no finite position: the endpoint
	// is the constant shadow-ray direction (its directPdfW is a solid-angle
	// pdf, not a distance) - same construction as MNEEDirectSampling.
	const LightSourceType lightType = light.GetType();
	const bool lightIsDir = (lightType == TYPE_DISTANT ||
			lightType == TYPE_SHARPDISTANT);
	MneeEndpoint ep;
	ep.isDir = lightIsDir;
	if (lightIsDir) {
		ep.dir = Normalize(shadowRay.d);
		ep.pos = Point(0.f, 0.f, 0.f);
	} else {
		ep.pos = shadowRay.o + shadowRay.d * sqrtf(directPdfW0);
		ep.dir = Vector(0.f, 0.f, 0.f);
	}

	// Every exit of this function is a sample the estimator does not cover, and
	// coverage (not the per-chain value, which an independent analytic model
	// reproduces to 1-3%) is what limits a curved caster. LUX_MNEE_REJ prints one
	// line per rejected attempt so the reasons can be counted: pair it with
	// LUX_MNEE_DEBUG and attempts = accepted + rejected.
	static const bool rejDebug = (getenv("LUX_MNEE_REJ") != nullptr);
	auto rej = [&](const char *why) {
		if (rejDebug)
			printf("MNEE_MS_REJ %s film=%.4g %.4g\n", why, sampleResult->filmX,
					sampleResult->filmY);
		return false;
	};

	//--------------------------------------------------------------------------
	// Chain topology (straight line seed)
	//--------------------------------------------------------------------------
	MneeChainVertex chain[MNEE_MS_MAX_VERTICES];
	const u_int maxVertices = Min(mneeMaxSpecular, MNEE_MS_MAX_VERTICES);
	const u_int n = MneeChainDiscover(device, scene, time, x0p, ep, u4,
			volInfo, shadowBsdf, chain, maxVertices);
	if (n < 2)
		return rej("chain<2");

	//--------------------------------------------------------------------------
	// Newton solve on the whole chain (block tridiagonal step, line search with
	// re-projection onto the shapes).
	//--------------------------------------------------------------------------
	const char *failWhy = "iterations";
	if (!MneeSolveChain(device, scene, time, x0p, ep, chain, n,
			mneeMaxIterations, &failWhy))
		return rej(failWhy);

	//--------------------------------------------------------------------------
	// Post-solve validity: each vertex must have solved the mode its material
	// implies (a mirror reflects, so the two segments stay on one side of the
	// surface; glass transmits, so they are on opposite sides), and every
	// specular factor must be valid at the solved configuration.
	//--------------------------------------------------------------------------
	Point pts[MNEE_MS_MAX_VERTICES + 2];
	pts[0] = x0p;
	for (u_int i = 0; i < n; ++i)
		pts[i + 1] = chain[i].v.p;
	pts[n + 1] = ep.pos;

	Spectrum specProduct(1.f);
	bool plainHalfVector = true;
	for (u_int i = 0; i < n; ++i) {
		const Vector wi = Normalize(pts[i] - pts[i + 1]);
		// The last vertex's outgoing direction is the constant light
		// direction for a directional endpoint (ep.pos is degenerate there).
		const Vector wo = (ep.isDir && (i == n - 1)) ? ep.dir :
				Normalize(pts[i + 2] - pts[i + 1]);
		const float cosI = Dot(chain[i].v.gn, wi);
		const float cosO = Dot(chain[i].v.gn, wo);

		if (chain[i].etaVertex == 1.f) {
			if (cosI * cosO < 0.f)
				return rej("mirror-side");
			specProduct *= chain[i].specFactor;
		} else {
			if (cosI * cosO > 0.f)
				return rej("dielectric-same-side");

			const Spectrum kt = chain[i].glassMat->GetKt()->
					GetSpectrumValue(chain[i].bsdf.hitPoint).Clamp(0.f, 1.f);
			const float nc = ExtractExteriorIors(chain[i].bsdf.hitPoint,
					chain[i].glassMat->GetExteriorIOR());
			const float nt = ExtractInteriorIors(chain[i].bsdf.hitPoint,
					chain[i].glassMat->GetInteriorIOR());
			const Vector localFixedDir = chain[i].bsdf.GetFrame().ToLocal(wi);
			Vector localSampledDir;
			// Same hero-wavelength evaluation as the single vertex solver:
			// a dispersive vertex is only reachable under spectral transport.
			const float cauchyB = chain[i].glassMat->GetCauchyB() ?
					chain[i].glassMat->GetCauchyB()->GetFloatValue(
						chain[i].bsdf.hitPoint) : 0.f;
			const Spectrum trans = GlassMaterial::EvalSpecularTransmission(
					chain[i].bsdf.hitPoint, localFixedDir, 0.f, kt, nc, nt, cauchyB,
					&localSampledDir);
			if (trans.Black())
				return rej("tir");
			specProduct *= trans;
			plainHalfVector = false;
		}
	}
	if (specProduct.Black())
		return rej("spec-black");

	//--------------------------------------------------------------------------
	// Last segment xN -> y: Illuminate at the last vertex, then check visibility
	//--------------------------------------------------------------------------
	PathVolumeInfo volLast = volInfo;
	for (u_int i = 0; i < n; ++i)
		volLast.Update(chain[i].specEvent, chain[i].bsdf);

	Ray shadowRay2;
	float directPdfW2;
	const Spectrum lightRadiance2 = light.Illuminate(scene, chain[n - 1].bsdf, time,
			u1, u2, u3, shadowRay2, directPdfW2);
	if (lightRadiance2.Black())
		return rej("light-black");
	verify(!isnan(directPdfW2) && !isinf(directPdfW2));

	if (ep.isDir) {
		// Illuminate() resampled a direction inside the emitter lobe; the
		// manifold endpoint is the fixed direction ep.dir that the solve was
		// run for. Rebuild the last-segment ray toward it and extend to the
		// scene bounding sphere (a miss = the light is reached). Same
		// construction as the single vertex solver.
		const Point o2 = chain[n - 1].bsdf.GetRayOrigin(ep.dir);
		const BSphere &bs = scene.GetDataSet().GetBSphere();
		const Vector toCenter(bs.center.x - o2.x, bs.center.y - o2.y,
				bs.center.z - o2.z);
		const float approach = Dot(toCenter, ep.dir);
		const float dist = approach + sqrtf(Max(0.f, bs.rad * bs.rad -
				toCenter.LengthSquared() + approach * approach));
		shadowRay2 = Ray(o2, ep.dir, 0.f, dist, time);
	}

	RayHit occlHit;
	BSDF occlBsdf;
	Spectrum segThroughput;
	PathVolumeInfo volTrace = volLast;
	if (scene.Intersect(IntersectionDevicePtr(&device), SHADOW_RAY, &volTrace, u4,
			&shadowRay2, &occlHit, &occlBsdf, &segThroughput, nullptr, nullptr, true))
		return rej("occluded");

	//--------------------------------------------------------------------------
	// Chain geometric term: dw0_dx1 * |det(dx_1 / dy)| with
	//   dx_1 / dy = (A^-1)_{1,N} * dC_N/dy
	// (A = the block tridiagonal constraint Jacobian; only the last constraint
	// involves the light). For N = 1 this is the single vertex term.
	//--------------------------------------------------------------------------
	MneeJacobianBlock geoBlocks[MNEE_MS_MAX_VERTICES];
	MneeVec2 geoResidual[MNEE_MS_MAX_VERTICES];
	float geoMax = 0.f;
	if (!MneeChainJacobian(device, scene, time, x0p, ep, chain, n,
			geoBlocks, geoResidual, geoMax))
		return rej("geo-jacobian");

	MneeMat2 dxFirst;
	if (!MneeTridiagonalSolveMatrixRhs(geoBlocks, n, dxFirst))
		return rej("geo-tridiagonal");

	const float epsLight = Max(1e-5f, 1e-4f * Distance(x0p, chain[n - 1].v.p));
	const MneeMat2 lightJac = MneeChainLightJacobian(pts[n - 1], ep,
			chain[n - 1].v, chain[n - 1].etaVertex, epsLight);
	const MneeMat2 dxDy = MneeMul(dxFirst, lightJac);

	const Vector d01 = x0p - chain[0].v.p;
	const float r01sq = d01.LengthSquared();
	if (r01sq < 1e-6f)
		return rej("r01sq");
	const float dw0Dx1 = fabsf(Dot(d01, chain[0].v.gn)) / (sqrtf(r01sq) * r01sq);
	const float geometricTerm = dw0Dx1 * fabsf(MneeDet(dxDy));
	if (geometricTerm <= 0.f || isnan(geometricTerm) || isinf(geometricTerm))
		return rej("geo-term");

	//--------------------------------------------------------------------------
	// Contribution. The r12^2 (Illuminate directPdfW) measure factor belongs to
	// the light weight only when every vertex solved the plain half-vector
	// (eta == 1, a pure reflection chain); with any dielectric vertex the
	// analytic geometric term carries the full conversion (see the single
	// vertex solver for the validation of this rule).
	//--------------------------------------------------------------------------
	BSDFEvent receiverEvent;
	float bsdfPdfW;
	const Spectrum bsdfEval0 = bsdf.Evaluate(Normalize(chain[0].v.p - x0p),
			&receiverEvent, &bsdfPdfW);
	if (bsdfEval0.Black())
		return rej("bsdf0-black");

	PathDepthInfo mneeDepthInfo = pathInfo.depth;
	mneeDepthInfo.IncDepths(receiverEvent);
	for (u_int i = 0; i < n; ++i)
		mneeDepthInfo.IncDepths(chain[i].specEvent);

	// For a directional endpoint the chain geometric term is already the
	// direction-space Jacobian (the perpendicular-frame Jacobian and the
	// r12^2 factor cancel in the point-light limit, as in the single vertex
	// solver); the only remaining factor is the emitter's direction pdf,
	// which divides out here.
	const Spectrum lightWeight = ep.isDir ?
			(lightRadiance2 * (risScale / (directPdfW2 * lightPickPdf))) :
			(lightRadiance2 * ((plainHalfVector ? directPdfW2 : 1.f) *
			risScale / lightPickPdf));
	Spectrum incomingRadiance = bsdfEval0 * specProduct * geometricTerm *
			lightWeight * segThroughput;
	{
		bool chainDispersive = false;
		for (u_int i = 0; i < n; ++i)
			chainDispersive |= chain[i].dispersive;
		if (chainDispersive)
			// The solved constraint holds at the hero wavelength only.
			incomingRadiance = Spectral::KeepHeroBins(incomingRadiance,
					*Spectral::Current());
	}
	verify(!incomingRadiance.IsNaN() && !incomingRadiance.IsInf());

	if (MneeDebugEnabled()) {
		const float dbgR12 = ep.isDir ? 1.f :
				Distance(ep.pos, chain[n - 1].v.p);
		// The analytic per-point radiance a light transport estimator measures
		// for this chain: every specular factor times the light's radiance at
		// the last vertex (see the single vertex debug print). A directional
		// endpoint is already in direction space: no r^2 falloff applies.
		const float dbgTruth = bsdfEval0.Filter() * specProduct.Filter() *
				lightRadiance2.Filter() / (dbgR12 * dbgR12);
		printf("MNEE_MS_DBG film=%.4g %.4g n=%u x0=%.9g %.9g %.9g",
				sampleResult->filmX, sampleResult->filmY, n,
				x0p.x, x0p.y, x0p.z);
		for (u_int i = 0; i < n; ++i) {
			// The eta side rule keys off Dot(wi, gn), so the debug output has to
			// show the geometric normal itself: a mesh whose geometric normals
			// point against the surface's outward direction silently inverts
			// every vertex's constraint IOR (see dev-tools/mnee_design.md 4e).
			const Vector dbgWi = Normalize(pts[i] - pts[i + 1]);
			const Vector dbgWo = (ep.isDir && (i == n - 1)) ? ep.dir :
					Normalize(pts[i + 2] - pts[i + 1]);
			printf(" | x%u=%.9g %.9g %.9g gn=%.4g %.4g %.4g eta=%.9g "
					"dWiGn=%.4g cWiGn=%.4g cWoGn=%.4g", i + 1,
					chain[i].v.p.x, chain[i].v.p.y, chain[i].v.p.z,
					chain[i].v.gn.x, chain[i].v.gn.y, chain[i].v.gn.z,
					chain[i].etaVertex, Dot(dbgWi, chain[i].v.gn),
					fabsf(Dot(dbgWi, chain[i].v.gn)),
					fabsf(Dot(dbgWo, chain[i].v.gn)));
		}
		printf(" | y=%.9g %.9g %.9g residual=%.3e spec=%.9g G=%.9g in=%.9g "
				"truth=%.9g\n", ep.pos.x, ep.pos.y, ep.pos.z,
				geoMax, specProduct.Filter(), geometricTerm,
				incomingRadiance.Filter(), dbgTruth);
		fflush(stdout);
	}

	sampleResult->AddDirectLight(light.GetID(), chain[n - 1].specEvent,
			pathThroughput, incomingRadiance, 1.f);
	AccumulateLPE(sampleResult, pathInfo,
			LPEVertexEvent(chain[n - 1].specEvent, false), LPE_SYM_L,
			pathThroughput * incomingRadiance);

	return true;
}

//------------------------------------------------------------------------------
// LMNEE: light-side manifold connect x0 -> specular -> camera lens
//
// ConnectToEye drops the light-path vertex when a delta specular surface
// blocks the camera connect; the GPU kernels instead solve the specular
// manifold (LMnee_Start / LMnee_SolveEnd / LMneeChain_*, see
// pathoclbase_funcs.cl). These are the CPU ports of that driver, reusing the
// eye-side Newton machinery with the endpoint roles swapped: the endpoint is
// the sampled lens point and the emission weight is the camera's emitted
// importance (Camera::GetPDF pdfW) instead of LightSource::Illuminate.
//
// Endpoint measure: the manifold geometric term already carries the last
// segment's 1/d^2 conversion, so the camera weight is pdfW alone for a
// refractive chain and pdfW * dSeg^2 for a pure mirror chain - the same rule
// as directPdfW2 on the eye side (the pinhole lens is a delta, so the r^2
// factor is the squared segment length).
//
// The splat lands where the SOLVED segment projects: the film position is
// re-computed from the last specular vertex toward the lens, not from the
// blocked connect's projection.
//------------------------------------------------------------------------------

// Project the last specular vertex to the film and evaluate the camera
// endpoint weight (the light-side analog of LightSource::Illuminate +
// directPdfW2 on the eye side). Returns false when the vertex projects
// outside the film or behind the camera.
static bool LMneeCameraEndpoint(SceneConstRef scene, const Point &lensPoint,
		const Point &v, const float time, const bool plainHalfVector,
		float *filmX, float *filmY, float *camWeight) {
	const Vector toVtx = v - lensPoint;
	const float dSeg2 = toVtx.Length();
	if (dSeg2 < 1e-3f)
		return false;

	Ray segRay;
	if (scene.GetCamera().GetType() == Camera::ORTHOGRAPHIC) {
		// An orthographic camera has no finite lens position: the solve
		// keeps the sampled point on the camera plane as endpoint (the GPU
		// kernel does the same) and the arriving segment projects along
		// the fixed camera direction.
		segRay = Ray(v, Normalize(scene.GetCamera().GetDir()),
				0.f, dSeg2, time);
	} else
		segRay = Ray(lensPoint, toVtx / dSeg2, 0.f, dSeg2, time);
	if (!scene.GetCamera().GetSamplePosition(&segRay, filmX, filmY))
		return false;

	float pdfW, fluxToRadianceFactor;
	scene.GetCamera().GetPDF(segRay, dSeg2, *filmX, *filmY,
			&pdfW, &fluxToRadianceFactor);
	if (fluxToRadianceFactor <= 0.f)
		return false;

	*camWeight = pdfW * (plainHalfVector ? dSeg2 * dSeg2 : 1.f);
	return true;
}

// Rejection diagnostics (LUX_LMNEE_REJ=1): one line per rejected attempt,
// keyed by stage; pair with the accepted splats to measure coverage.
static bool LMneeRejEnabled() {
	static const bool enabled = (getenv("LUX_LMNEE_REJ") != nullptr);
	return enabled;
}
#define LMNEE_REJ(why) do { \
		if (LMneeRejEnabled()) { \
			printf("LMNEE_REJ %s x0=%.9g %.9g %.9g\n", why, \
					x0p.x, x0p.y, x0p.z); \
			fflush(stdout); \
		} \
	} while (0)

// Splat the solved light-side contribution. isCaustic is fixed: the path
// crossed at least one specular interface on the way to the lens (the GPU
// pending splat uses the same convention).
static void LMneeSplat(FilmConstRef film, const LightSource &light,
		const float filmX, const float filmY, const Spectrum &radiance,
		const BSDF &receiver, std::vector<SampleResult> &sampleResults,
		const char *how = nullptr, const Point *x0dbg = nullptr) {
	if (LMneeRejEnabled() && how) {
		printf("LMNEE_ACC %s film=%.1f %.1f r=%.4g %.4g %.4g x0=%.4g %.4g %.4g\n",
				how, filmX, filmY, radiance.c[0], radiance.c[1], radiance.c[2],
				x0dbg ? x0dbg->x : 0.f, x0dbg ? x0dbg->y : 0.f,
				x0dbg ? x0dbg->z : 0.f);
		fflush(stdout);
	}
	SampleResult &sampleResult =
			PathTracer::AddLightSampleResult(sampleResults, film);
	sampleResult.filmX = filmX;
	sampleResult.filmY = filmY;
	sampleResult.pixelX = Floor2UInt(filmX);
	sampleResult.pixelY = Floor2UInt(filmY);
	sampleResult.isCaustic = true;
	// The splat's visible surface is the solved receiver vertex x0
	sampleResult.cryptoObjectID = receiver.GetCryptoObjectID();
	sampleResult.cryptoMaterialID = receiver.GetCryptoMaterialID();
	sampleResult.radiance[light.GetID()] = radiance;
}

bool PathTracer::LMNEEConnectToEye(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene,
		FilmConstRef film, const float time,
		const LightSource &light, const BSDF &bsdf,
		const luxrays::Spectrum &flux, const LightPathInfo &pathInfo,
		const luxrays::RayHit &shadowRayHit,
		const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
		BSDF &warmV0, BSDF &warmV1, bool &warmOk,
		std::vector<SampleResult> &sampleResults) const {
	warmOk = false;
	const Point &x0p = bsdf.hitPoint.p;
	const Point &lensPoint = pathInfo.lensPoint;
	MneeEndpoint ep;
	ep.isDir = false;	// the sampled lens point is a finite position
	ep.pos = lensPoint;
	ep.dir = Vector(0.f, 0.f, 0.f);

	//--------------------------------------------------------------------------
	// Occluder material gate (same rules as the eye side)
	//--------------------------------------------------------------------------
	const MaterialType seedMatType = shadowBsdf.GetMaterialType();
	observer_ptr<const MirrorMaterial> mirrorMat = nullptr;
	observer_ptr<const GlassMaterial> glassMat = nullptr;
	float etaVertex = 1.f;
	bool dispersiveConnect = false;
	if (seedMatType == MIRROR) {
		mirrorMat = dynamic_observer_cast<const MirrorMaterial>(
				shadowBsdf.GetMaterial());
		if (!mirrorMat)
			{ LMNEE_REJ("mirror-mat"); return false; }

		// Only the same-side relation is a reflection (see
		// MNEEDirectSampling); endpoints on opposite sides would need the
		// mirror to transmit.
		const Normal &gn1s = shadowBsdf.hitPoint.geometryN;
		const Vector toX0 = x0p - shadowBsdf.hitPoint.p;
		const Vector toY = lensPoint - shadowBsdf.hitPoint.p;
		etaVertex = (Dot(toX0, gn1s) * Dot(toY, gn1s) > 0.f) ? 1.f : -1.f;
		if (etaVertex != 1.f)
			{ LMNEE_REJ("mirror-side"); return false; }
	} else if (seedMatType == GLASS) {
		glassMat = dynamic_observer_cast<const GlassMaterial>(
				shadowBsdf.GetMaterial());
		if (!glassMat)
			{ LMNEE_REJ("glass-mat"); return false; }

		const float nc = ExtractExteriorIors(shadowBsdf.hitPoint,
				glassMat->GetExteriorIOR());
		const float nt = ExtractInteriorIors(shadowBsdf.hitPoint,
				glassMat->GetInteriorIOR());
		if (nt <= 0.f || nc <= 0.f)
			{ LMNEE_REJ("ior"); return false; }
		const float cauchyB = glassMat->GetCauchyB() ?
				glassMat->GetCauchyB()->GetFloatValue(shadowBsdf.hitPoint) : 0.f;
		if (cauchyB > 0.f) {
			if (!Spectral::Current())
				{ LMNEE_REJ("disp-nospectral"); return false; }
			dispersiveConnect = true;
			etaVertex = DispersiveIOR(nt, cauchyB) / nc;
		} else
			etaVertex = nt / nc;
	} else
		{ LMNEE_REJ("material"); return false; }

	//--------------------------------------------------------------------------
	// Newton solve x0 -> x1 -> lens
	//--------------------------------------------------------------------------

	// Seed cache policy (eye-side parity): mirror endpoints (eta == 1)
	// consult the cache first - the cold mirrored-lens seed costs a trace
	// the cache skips. For glass the free cold line seed keeps the
	// reference basin selection and the cache is only a failure rescue
	// below (the eye-side bumpy-sphere scene measured ~5% caustic energy
	// loss from cache-first glass seeding). The camera endpoint shares the
	// GPU LMNEE_CAMERA_SEED_ID namespace. Same incident-side namespacing
	// as the eye-side caller: solves from inside a closed dielectric (the
	// connect ray hits the blocker on its -geometryN side) must not
	// warm-start from outside-context seeds.
	MneeVertex seedVtx;
	const MneeVertex *seedPtr = nullptr;
	const u_int seedMesh = shadowRayHit.meshIndex * 2u +
			(Dot(Normalize(shadowBsdf.hitPoint.p - x0p),
			shadowBsdf.hitPoint.geometryN) > 0.f ? 1u : 0u);
	float seedCellSize = 0.f;
	u_int seedKey = 0;
	if (mneeSeedCacheEnable && mneeSeeds) {
		seedCellSize = Max(scene.GetDataSet().GetBSphere().rad /
				MNEE_SEED_CELL_FRAC_CPU, 1e-4f);
		seedKey = MneeSeedKey(LMNEE_CAMERA_SEED_ID, seedMesh,
				shadowBsdf.hitPoint.p, seedCellSize);
		if ((etaVertex == 1.f) &&
				MneeSeedLookup(mneeSeeds.get(), seedKey, LMNEE_CAMERA_SEED_ID,
				seedMesh, mirrorMat != nullptr, etaVertex, &seedVtx))
			seedPtr = &seedVtx;
	}

	MneeVertex vtx;
	BSDF finalBsdf;
	bool solveOk = MneeSolveSingleVertex(device, scene, time, x0p, ep, bsdf,
			shadowRayHit, shadowBsdf, volInfo, etaVertex, mneeMaxIterations,
			seedPtr, &vtx, &finalBsdf);
	if (!solveOk && (etaVertex != 1.f) && !seedPtr &&
			mneeSeedCacheEnable && mneeSeeds &&
			MneeSeedLookup(mneeSeeds.get(), seedKey, LMNEE_CAMERA_SEED_ID,
				seedMesh, mirrorMat != nullptr, etaVertex, &seedVtx)) {
		// Failure rescue: retry once from the cached vertex.
		solveOk = MneeSolveSingleVertex(device, scene, time, x0p, ep, bsdf,
				shadowRayHit, shadowBsdf, volInfo, etaVertex,
				mneeMaxIterations, &seedVtx, &vtx, &finalBsdf);
	}
	if (!solveOk)
		{ LMNEE_REJ(seedPtr ? "newton-s" : "newton"); return false; }

	//--------------------------------------------------------------------------
	// Post-solve validity check (same as the eye side): the half-vector
	// formulation can converge to a solution of the wrong specular mode
	//--------------------------------------------------------------------------
	const Vector wi = Normalize(x0p - vtx.p);
	const Vector wo = Normalize(lensPoint - vtx.p);
	const float cosX = Dot(vtx.gn, wi);
	const float cosY = Dot(vtx.gn, wo);
	const bool refraction = (cosX * cosY < 0.f);
	if (mirrorMat ? refraction : !refraction)
		{ LMNEE_REJ("mode"); return false; }

	//--------------------------------------------------------------------------
	// Specular factor at the solved vertex (same code as the eye side)
	//--------------------------------------------------------------------------
	Spectrum specFactor;
	BSDFEvent specEvent;
	if (mirrorMat) {
		specFactor = mirrorMat->GetKr()->GetSpectrumValue(finalBsdf.hitPoint).
				Clamp(0.f, 1.f);
		specEvent = SPECULAR | REFLECT;
	} else {
		const Spectrum kt = glassMat->GetKt()->GetSpectrumValue(
				finalBsdf.hitPoint).Clamp(0.f, 1.f);
		const float nc = ExtractExteriorIors(finalBsdf.hitPoint,
				glassMat->GetExteriorIOR());
		const float nt = ExtractInteriorIors(finalBsdf.hitPoint,
				glassMat->GetInteriorIOR());
		const Vector localFixedDir = finalBsdf.GetFrame().ToLocal(wi);
		Vector localSampledDir;
		const float cauchyB = glassMat->GetCauchyB() ?
				glassMat->GetCauchyB()->GetFloatValue(finalBsdf.hitPoint) : 0.f;
		specFactor = GlassMaterial::EvalSpecularTransmission(finalBsdf.hitPoint,
				localFixedDir, 0.f, kt, nc, nt, cauchyB, &localSampledDir);
		specEvent = SPECULAR | TRANSMIT;
	}
	if (specFactor.Black())
		{ LMNEE_REJ("spec"); return false; }

	const float geometricTerm = MneeGeometricTerm(x0p, ep, vtx);
	if (geometricTerm <= 0.f || isnan(geometricTerm) || isinf(geometricTerm))
		{ LMNEE_REJ("geo"); return false; }

	//--------------------------------------------------------------------------
	// Camera endpoint: project the arriving segment x1 -> lens to the film
	//--------------------------------------------------------------------------
	const bool plainHalfVector = (etaVertex == 1.f);
	float filmX, filmY, camWeight;
	if (!LMneeCameraEndpoint(scene, lensPoint, vtx.p, time,
			plainHalfVector, &filmX, &filmY, &camWeight))
		{ LMNEE_REJ("camproj"); return false; }

	// Receiver BSDF at x0 toward the solved vertex (includes the cosine)
	BSDFEvent receiverEvent;
	float receiverPdfW;
	const Spectrum bsdfEval0 = bsdf.Evaluate(Normalize(vtx.p - x0p),
			&receiverEvent, &receiverPdfW);
	if (bsdfEval0.Black())
		{ LMNEE_REJ("recv"); return false; }

	//--------------------------------------------------------------------------
	// Last segment x1 -> lens: visibility through the updated volume state
	//--------------------------------------------------------------------------
	PathVolumeInfo volSeg2 = volInfo;
	volSeg2.Update(specEvent, finalBsdf);
	const Vector toLens = lensPoint - vtx.p;
	const float dSeg2 = toLens.Length();
	const Vector segDir = toLens / dSeg2;
	Ray segRay(finalBsdf.GetRayOrigin(segDir), segDir, 0.f,
			dSeg2 * (1.f - 1e-4f), time);
	RayHit segHit;
	BSDF segBsdf;
	Spectrum segThroughput;
	PathVolumeInfo volSegTrace = volSeg2;
	if (scene.Intersect(IntersectionDevicePtr(&device), SHADOW_RAY,
			&volSegTrace, .5f, &segRay, &segHit, &segBsdf, &segThroughput,
			nullptr, nullptr, true)) {
		// Re-blocked: another interface is in the way - the caller falls
		// back to the chain solve (the GPU fromMnee escalation). Hand the
		// converged vertex and the re-blocker to the chain as a consistent
		// seed pair: both sit on the solved ray's path, unlike a straight
		// -line discovery seed.
		if (segBsdf.IsDelta() && (segBsdf.GetEventTypes() & SPECULAR)) {
			warmV0 = finalBsdf;
			warmV1 = segBsdf;
			warmOk = true;
		}
		LMNEE_REJ("seg2block");
		return false;
	}

	Spectrum radiance = flux * bsdfEval0 * specFactor * geometricTerm *
			camWeight * segThroughput;
	if (dispersiveConnect)
		// The solved constraint holds at the hero wavelength only: carry
		// the wavelength-selection weight and drop the dead bins.
		radiance = Spectral::KeepHeroBins(radiance, *Spectral::Current());
	if (radiance.IsNaN() || radiance.IsInf())
		{ LMNEE_REJ("nan"); return false; }

	LMneeSplat(film, light, filmX, filmY, radiance, bsdf, sampleResults, "single", &x0p);

	// Manifold-guided emission: a solved-manifold connect reaches the
	// camera only through specular interfaces - credit the receiver x0
	// into the light's focus ring (GPU pendingSplat.recvP parity)
	LightFocusCredit(scene, light.lightSceneIndex, x0p);

	// Publish the converged vertex as a warm-start seed only after the
	// full connect validated (eye-side parity: solved-but-unusable roots
	// pollute the cache and drain the caustic on multi-root casters).
	if (mneeSeedCacheEnable && mneeSeeds)
		MneeSeedStore(mneeSeeds.get(), seedKey, vtx.p, vtx.n,
				LMNEE_CAMERA_SEED_ID, seedMesh,
				mirrorMat != nullptr);
	return true;
}

bool PathTracer::LMNEEMultiConnectToEye(
		luxrays::IntersectionDeviceRef device,
		SceneConstRef scene,
		FilmConstRef film, const float time,
		const LightSource &light, const BSDF &bsdf,
		const luxrays::Spectrum &flux, const LightPathInfo &pathInfo,
		const BSDF &shadowBsdf, PathVolumeInfo &volInfo,
		const BSDF *warmV0, const BSDF *warmV1,
		std::vector<SampleResult> &sampleResults) const {
	const Point &x0p = bsdf.hitPoint.p;
	const Point &lensPoint = pathInfo.lensPoint;
	// The camera endpoint is always a finite position (the sampled lens point).
	MneeEndpoint ep;
	ep.isDir = false;
	ep.pos = lensPoint;
	ep.dir = Vector(0.f, 0.f, 0.f);

	//--------------------------------------------------------------------------
	// Chain topology: the delta specular surfaces along the refracted walk
	// (the first is the connect blocker itself; a warm pair from the single
	// solve seeds both ends when available)
	//--------------------------------------------------------------------------
	MneeChainVertex chain[MNEE_MS_MAX_VERTICES];
	const u_int maxVertices = Min(mneeMaxSpecular, MNEE_MS_MAX_VERTICES);
	const u_int n = MneeChainDiscover(device, scene, time, x0p, ep,
			.5f, volInfo, shadowBsdf, chain, maxVertices, warmV0, warmV1);
	if (n < 2) {
		if (LMneeRejEnabled()) {
			printf("LMNEE_REJ ms-chain<2 x0=%.9g %.9g %.9g n=%u b0=%s p=%.4g %.4g %.4g\n",
					x0p.x, x0p.y, x0p.z, n,
					shadowBsdf.GetMaterial() ? "mat" : "null",
					shadowBsdf.hitPoint.p.x, shadowBsdf.hitPoint.p.y,
					shadowBsdf.hitPoint.p.z);
			fflush(stdout);
		}
		return false;
	}



	//--------------------------------------------------------------------------
	// Newton solve on the whole chain
	//--------------------------------------------------------------------------
	const char *failWhy = "iterations";
	if (!MneeSolveChain(device, scene, time, x0p, ep, chain, n,
			mneeMaxIterations, &failWhy))
		{ LMNEE_REJ("ms-newton"); return false; }

	//--------------------------------------------------------------------------
	// Post-solve validity + specular factor product (same rules as the eye
	// side: mirrors keep both segments on one side, dielectrics transmit)
	//--------------------------------------------------------------------------
	Point pts[MNEE_MS_MAX_VERTICES + 2];
	pts[0] = x0p;
	for (u_int i = 0; i < n; ++i)
		pts[i + 1] = chain[i].v.p;
	pts[n + 1] = ep.pos;

	Spectrum specProduct(1.f);
	bool plainHalfVector = true;
	for (u_int i = 0; i < n; ++i) {
		const Vector wi = Normalize(pts[i] - pts[i + 1]);
		const Vector wo = Normalize(pts[i + 2] - pts[i + 1]);
		const float cosI = Dot(chain[i].v.gn, wi);
		const float cosO = Dot(chain[i].v.gn, wo);

		if (chain[i].etaVertex == 1.f) {
			if (cosI * cosO < 0.f)
				{ LMNEE_REJ("ms-mirror-side"); return false; }
			specProduct *= chain[i].specFactor;
		} else {
			if (cosI * cosO > 0.f)
				{ LMNEE_REJ("ms-sameside"); return false; }

			const Spectrum kt = chain[i].glassMat->GetKt()->
					GetSpectrumValue(chain[i].bsdf.hitPoint).Clamp(0.f, 1.f);
			const float nc = ExtractExteriorIors(chain[i].bsdf.hitPoint,
					chain[i].glassMat->GetExteriorIOR());
			const float nt = ExtractInteriorIors(chain[i].bsdf.hitPoint,
					chain[i].glassMat->GetInteriorIOR());
			const Vector localFixedDir = chain[i].bsdf.GetFrame().ToLocal(wi);
			Vector localSampledDir;
			const float cauchyB = chain[i].glassMat->GetCauchyB() ?
					chain[i].glassMat->GetCauchyB()->GetFloatValue(
						chain[i].bsdf.hitPoint) : 0.f;
			const Spectrum trans = GlassMaterial::EvalSpecularTransmission(
					chain[i].bsdf.hitPoint, localFixedDir, 0.f, kt, nc, nt,
					cauchyB, &localSampledDir);
			if (trans.Black())
				{ LMNEE_REJ("ms-tir"); return false; }
			specProduct *= trans;
			plainHalfVector = false;
		}
	}
	if (specProduct.Black())
		{ LMNEE_REJ("ms-spec"); return false; }

	//--------------------------------------------------------------------------
	// Chain geometric term: dw0_dx1 * |det(dx_1 / dy)| with the lens point as
	// the moving endpoint (same construction as the eye side)
	//--------------------------------------------------------------------------
	MneeJacobianBlock geoBlocks[MNEE_MS_MAX_VERTICES];
	MneeVec2 geoResidual[MNEE_MS_MAX_VERTICES];
	float geoMax = 0.f;
	if (!MneeChainJacobian(device, scene, time, x0p, ep, chain, n,
			geoBlocks, geoResidual, geoMax))
		{ LMNEE_REJ("ms-geojac"); return false; }

	MneeMat2 dxFirst;
	if (!MneeTridiagonalSolveMatrixRhs(geoBlocks, n, dxFirst))
		{ LMNEE_REJ("ms-geotri"); return false; }

	const float epsLight = Max(1e-5f, 1e-4f * Distance(x0p, chain[n - 1].v.p));
	const MneeMat2 lightJac = MneeChainLightJacobian(pts[n - 1], ep,
			chain[n - 1].v, chain[n - 1].etaVertex, epsLight);
	const MneeMat2 dxDy = MneeMul(dxFirst, lightJac);

	const Vector d01 = x0p - chain[0].v.p;
	const float r01sq = d01.LengthSquared();
	if (r01sq < 1e-6f)
		{ LMNEE_REJ("ms-r01"); return false; }
	const float dw0Dx1 = fabsf(Dot(d01, chain[0].v.gn)) / (sqrtf(r01sq) * r01sq);
	const float geometricTerm = dw0Dx1 * fabsf(MneeDet(dxDy));
	if (geometricTerm <= 0.f || isnan(geometricTerm) || isinf(geometricTerm))
		{ LMNEE_REJ("ms-geo"); return false; }

	//--------------------------------------------------------------------------
	// Camera endpoint: project the LAST vertex to the film (the far
	// interface for a closed dielectric)
	//--------------------------------------------------------------------------
	float filmX, filmY, camWeight;
	if (!LMneeCameraEndpoint(scene, lensPoint, chain[n - 1].v.p, time,
			plainHalfVector, &filmX, &filmY, &camWeight))
		{ LMNEE_REJ("ms-camproj"); return false; }

	// Receiver BSDF at x0 toward the FIRST chain vertex
	BSDFEvent receiverEvent;
	float receiverPdfW;
	const Spectrum bsdfEval0 = bsdf.Evaluate(Normalize(chain[0].v.p - x0p),
			&receiverEvent, &receiverPdfW);
	if (bsdfEval0.Black())
		{ LMNEE_REJ("ms-recv"); return false; }

	//--------------------------------------------------------------------------
	// Last segment xN -> lens: visibility through the chain's exit volume
	//--------------------------------------------------------------------------
	PathVolumeInfo volLast = volInfo;
	for (u_int i = 0; i < n; ++i)
		volLast.Update(chain[i].specEvent, chain[i].bsdf);
	const Vector toLens = lensPoint - chain[n - 1].v.p;
	const float dSeg2 = toLens.Length();
	const Vector segDir = toLens / dSeg2;
	Ray segRay(chain[n - 1].bsdf.GetRayOrigin(segDir), segDir, 0.f,
			dSeg2 * (1.f - 1e-4f), time);
	RayHit segHit;
	BSDF segBsdf;
	Spectrum segThroughput;
	PathVolumeInfo volSegTrace = volLast;
	if (scene.Intersect(IntersectionDevicePtr(&device), SHADOW_RAY,
			&volSegTrace, .5f, &segRay, &segHit, &segBsdf, &segThroughput,
			nullptr, nullptr, true))
		{ LMNEE_REJ("ms-segblock"); return false; }

	Spectrum radiance = flux * bsdfEval0 * specProduct * geometricTerm *
			camWeight * segThroughput;
	{
		bool chainDispersive = false;
		for (u_int i = 0; i < n; ++i)
			chainDispersive |= chain[i].dispersive;
		if (chainDispersive)
			radiance = Spectral::KeepHeroBins(radiance, *Spectral::Current());
	}
	if (radiance.IsNaN() || radiance.IsInf())
		{ LMNEE_REJ("ms-nan"); return false; }

	LMneeSplat(film, light, filmX, filmY, radiance, bsdf, sampleResults, "chain", &x0p);
	LightFocusCredit(scene, light.lightSceneIndex, x0p);
	return true;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
