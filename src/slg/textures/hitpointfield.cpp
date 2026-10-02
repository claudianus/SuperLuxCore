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
			return Spectrum(s > 0.f ? 1.f : 0.f);
		}
		default:
			return Spectrum(0.f, 0.f, 0.f);
	}
}

PropertiesUPtr HitPointFieldTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	static const char *channelNames[] = {
		"geometrynormal", "backfacing", "incoming", "parametric", "reflection", "radial"
	};

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("hitpoint"));
	props->Set(Property("scene.textures." + name + ".channel")(channelNames[channel]));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
