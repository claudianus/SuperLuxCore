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
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include "slg/textures/hitpointfield.h"
#include "slg/bsdf/hitpoint.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// HitPoint texture - raw HitPoint field reads for the shading graph
//------------------------------------------------------------------------------

float HitPointFieldTexture::GetFloatValue(const HitPoint &hitPoint) const {
	switch (channel) {
		case HITPOINT_BACKFACING:
			// Cycles' Backfacing is the geometric winding sign: the face's
			// front-back classification, NOT ray-entry (intoObject is true on
			// a solid surface's visible face too - the ray crosses inside).
			return (Dot(hitPoint.fixedDir, Vector(hitPoint.geometryN.x, hitPoint.geometryN.y, hitPoint.geometryN.z)) < 0.f) ? 1.f : 0.f;
		default:
			return EvalSpectrumValue(hitPoint).Y();
	}
}

Spectrum HitPointFieldTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	switch (channel) {
		case HITPOINT_GEOMETRYN:
			return Spectrum(hitPoint.geometryN.x, hitPoint.geometryN.y, hitPoint.geometryN.z);
		case HITPOINT_INCOMING:
			return Spectrum(hitPoint.fixedDir.x, hitPoint.fixedDir.y, hitPoint.fixedDir.z);
		case HITPOINT_PARAMETRIC:
			return Spectrum(hitPoint.triangleBariCoord1, hitPoint.triangleBariCoord2, 0.f);
		case HITPOINT_RADIAL: {
			// Polar coordinates of the default UV map: (theta/2pi, r, 0)
			// centered at the UV square's midpoint. The "theta" texture in
			// pathoclbase already uses a 0..1 angular wedge for Voronoi;
			// here the whole disk maps to theta in [0,1) via fract().
			const float du = hitPoint.defaultUV.u - 0.5f;
			const float dv = hitPoint.defaultUV.v - 0.5f;
			const float r = sqrtf(du * du + dv * dv);
			// atan2 in [-pi,pi] -> wrap to [0,1)
			float theta = atan2f(dv, du) * (1.f / (2.f * 3.14159265f));
			theta -= floorf(theta);
			return Spectrum(theta, r, 0.f);
		}
		case HITPOINT_OBJECTSPACE: {
			// 광선 거리의 뺄셈 오차가 정수 평면의 Checker 셀을 뒤집지 않도록
			// 실제 삼각형 평면에 투영한다. 접선 방향 범프 오프셋은 유지한다.
			Point p = hitPoint.p;
			if (hitPoint.mesh && hitPoint.triangleIndex < hitPoint.mesh->GetTotalTriangleCount()) {
				const Triangle &triangle = hitPoint.mesh->GetTriangles()[hitPoint.triangleIndex];
				const Point anchor = hitPoint.mesh->GetVertex(hitPoint.localToWorld, triangle.v[0]);
				const Vector n(hitPoint.geometryN);
				const float n2 = Dot(n, n);
				if (n2 > 0.f)
					p += (Dot(anchor - p, n) / n2) * n;
			}
			const Point pObj = Inverse(hitPoint.localToWorld) * p;
			return Spectrum(pObj.x, pObj.y, pObj.z);
		}
		case HITPOINT_GENERATED: {
			const auto *base = hitPoint.mesh ? hitPoint.mesh->GetAsExtTriangleMesh() : nullptr;
			if (!base) {
				const Point pObj = Inverse(hitPoint.localToWorld) * hitPoint.p;
				return Spectrum(pObj.x, pObj.y, pObj.z);
			}
			// Direct meshes are already baked. Only wrappers need undoing;
			// the cached map includes the base mesh's applied transform.
			const Point p = hitPoint.mesh->GetType() == TYPE_EXT_TRIANGLE ?
					hitPoint.p : Inverse(hitPoint.localToWorld) * hitPoint.p;
			const auto &m = base->GetGeneratedTransform();
			return Spectrum(
					m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3],
					m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
					m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]);
		}
		case HITPOINT_WORLDPOS:
			return Spectrum(hitPoint.p.x, hitPoint.p.y, hitPoint.p.z);
		case HITPOINT_OBJORIGIN:
			return Spectrum(hitPoint.localToWorld.m.m[0][3],
					hitPoint.localToWorld.m.m[1][3],
					hitPoint.localToWorld.m.m[2][3]);
		case HITPOINT_REFLECTION: {
			// r = d - 2(d.n)n where d = -fixedDir is the direction of travel;
			// fixedDir points back to the camera, so -fixedDir is "into" the scene.
			const Vector d(-hitPoint.fixedDir.x, -hitPoint.fixedDir.y, -hitPoint.fixedDir.z);
			const float nd = 2.f * (d.x * hitPoint.shadeN.x + d.y * hitPoint.shadeN.y + d.z * hitPoint.shadeN.z);
			return Spectrum(d.x - nd * hitPoint.shadeN.x,
							d.y - nd * hitPoint.shadeN.y,
							d.z - nd * hitPoint.shadeN.z);
		}
		case HITPOINT_BACKFACING: {
			const float s = Dot(hitPoint.fixedDir, Vector(hitPoint.geometryN.x, hitPoint.geometryN.y, hitPoint.geometryN.z));
			return Spectrum(s < 0.f ? 1.f : 0.f);
		}
		default:
			return Spectrum(0.f, 0.f, 0.f);
	}
}

PropertiesUPtr HitPointFieldTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();
	static const char *channelNames[] = {
		"geometrynormal", "backfacing", "incoming", "parametric", "reflection", "radial", "objectspace", "generated", "worldpos", "objectorigin"
	};

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("hitpoint"));
	props->Set(Property("scene.textures." + name + ".channel")(channelNames[channel]));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
