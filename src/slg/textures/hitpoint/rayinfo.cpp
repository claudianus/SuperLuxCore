/***************************************************************************
 * Copyright 1998-2020 by authors (see AUTHORS.txt)                        *
 *                                                                         *
 *   This file is part of LuxCoreRender.                                   *
 *                                                                         *
 * Licensed under the Apache License, Version 2.0 (the "License");         *
 * you may not use this file except in compliance with the License.        *
 * You may obtain a copy of the License at                                 *
 *                                                                         *
 * Unless required by applicable law or agreed to in writing, software     *
 * distributed under the License is distributed on an "AS IS" BASIS,       *
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*
 * See the License for the specific language governing permissions and     *
 * limitations under the License.                                          *
 ***************************************************************************/

#include "slg/textures/hitpoint/rayinfo.h"
#include "slg/scene/scene.h"

using namespace std;
using namespace luxrays;
using namespace slg;

//------------------------------------------------------------------------------
// RayInfo texture
//------------------------------------------------------------------------------

float RayInfoTexture::GetFloatValue(const HitPoint &hitPoint) const {
	const u_int rayFlags = hitPoint.rayFlags;

	// The stored bounce event is only meaningful for indirect path
	// continuations: camera rays have no generating bounce and shadow rays
	// are spawned for light transport queries
	const BSDFEvent rayEvent =
			(rayFlags & (CAMERA_RAY | SHADOW_RAY)) ? NONE : hitPoint.rayEvent;

	switch (channel) {
		case RAYINFO_IS_CAMERA_RAY:
			return (rayFlags & CAMERA_RAY) ? 1.f : 0.f;
		case RAYINFO_IS_SHADOW_RAY:
			return (rayFlags & SHADOW_RAY) ? 1.f : 0.f;
		case RAYINFO_IS_DIFFUSE_RAY:
			return (rayEvent & DIFFUSE) ? 1.f : 0.f;
		case RAYINFO_IS_GLOSSY_RAY:
			return (rayEvent & GLOSSY) ? 1.f : 0.f;
		case RAYINFO_IS_SINGULAR_RAY:
			return (rayEvent & SPECULAR) ? 1.f : 0.f;
		case RAYINFO_IS_REFLECTION_RAY:
			return (rayEvent & REFLECT) ? 1.f : 0.f;
		case RAYINFO_IS_TRANSMISSION_RAY:
			return (rayEvent & TRANSMIT) ? 1.f : 0.f;
		case RAYINFO_IS_VOLUME_SCATTER_RAY:
			// Volume scatter events produce a BSDF on a hit point with no
			// mesh; the rayFlags check excludes uninitialized contexts
			return ((hitPoint.mesh == nullptr) && (rayFlags != 0)) ? 1.f : 0.f;
		case RAYINFO_RAY_LENGTH:
			return hitPoint.rayLength;
		case RAYINFO_RAY_DEPTH:
			return hitPoint.rayDepth;
		case RAYINFO_DIFFUSE_DEPTH:
			return hitPoint.rayDiffuseDepth;
		case RAYINFO_GLOSSY_DEPTH:
			return hitPoint.rayGlossyDepth;
		case RAYINFO_SPECULAR_DEPTH:
			return hitPoint.raySpecularDepth;
		case RAYINFO_TRANSMISSION_DEPTH:
			return hitPoint.rayTransmissionDepth;
		case RAYINFO_TRANSPARENT_DEPTH:
			return hitPoint.rayTransparentDepth;
		default:
			throw runtime_error("Unknown channel in RayInfoTexture::GetFloatValue(): " +
					ToString(channel));
	}
}

Spectrum RayInfoTexture::EvalSpectrumValue(const HitPoint &hitPoint) const {
	return Spectrum(GetFloatValue(hitPoint));
}

static string Channel2String(const RayInfoChannel channel) {
	switch (channel) {
		case RAYINFO_IS_CAMERA_RAY: return "iscameraray";
		case RAYINFO_IS_SHADOW_RAY: return "isshadowray";
		case RAYINFO_IS_DIFFUSE_RAY: return "isdiffuseray";
		case RAYINFO_IS_GLOSSY_RAY: return "isglossyray";
		case RAYINFO_IS_SINGULAR_RAY: return "issingularray";
		case RAYINFO_IS_REFLECTION_RAY: return "isreflectionray";
		case RAYINFO_IS_TRANSMISSION_RAY: return "istransmissionray";
		case RAYINFO_IS_VOLUME_SCATTER_RAY: return "isvolumescatterray";
		case RAYINFO_RAY_LENGTH: return "raylength";
		case RAYINFO_RAY_DEPTH: return "raydepth";
		case RAYINFO_DIFFUSE_DEPTH: return "diffusedepth";
		case RAYINFO_GLOSSY_DEPTH: return "glossydepth";
		case RAYINFO_SPECULAR_DEPTH: return "speculardepth";
		case RAYINFO_TRANSMISSION_DEPTH: return "transmissiondepth";
		case RAYINFO_TRANSPARENT_DEPTH: return "transparentdepth";
		default:
			throw runtime_error("Unknown channel in RayInfoTexture::Channel2String(): " +
					ToString(channel));
	}
}

PropertiesUPtr RayInfoTexture::ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const {
	auto props = std::make_unique<Properties>();

	const string name = GetName();
	props->Set(Property("scene.textures." + name + ".type")("rayinfo"));
	props->Set(Property("scene.textures." + name + ".channel")(Channel2String(channel)));

	return props;
}
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
