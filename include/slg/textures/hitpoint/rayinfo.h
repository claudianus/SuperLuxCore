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

#ifndef _SLG_RAYINFOTEX_H
#define	_SLG_RAYINFOTEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// RayInfo texture
//
// Evaluates to information about the ray that generated the current hit
// point (stored in HitPoint by Scene::Intersect()). It is the equivalent of
// the Cycles "Light Path" node and can be used to implement ray-dependent
// shading (for instance a different material color for camera and indirect
// rays).
//
// Hit points evaluated outside of a ray-traced path (light source sampling,
// volume internals, ...) carry the zero defaults: all "is*ray" channels
// return 0 and the depth/length channels return 0.
//
// The channel is selected with the ".channel" string property:
//   "iscameraray"       1 if the hit was reached by a camera ray
//   "isshadowray"       1 if the hit was reached by a shadow ray
//   "isdiffuseray"      1 if the bounce that generated the ray was DIFFUSE
//   "isglossyray"       1 if it was GLOSSY
//   "issingularray"     1 if it was SPECULAR
//   "isreflectionray"   1 if it was a reflection (REFLECT)
//   "istransmissionray" 1 if it was a transmission (TRANSMIT)
//   "isvolumescatterray" 1 if the hit point is a volume scatter event
//   "raylength"         the length of the incoming ray segment
//   "raydepth"          the number of bounces before the ray (0 = camera)
//   "diffusedepth"      the number of DIFFUSE bounces before the ray
//   "glossydepth"       the number of GLOSSY bounces before the ray
//   "speculardepth"     the number of SPECULAR bounces before the ray
//   "transmissiondepth" the number of TRANSMIT bounces before the ray
//   "transparentdepth"  the number of transparent surfaces crossed by the
//                       path before this hit
//
// The bounce event is only meaningful for indirect path continuations:
// camera rays have no generating bounce and shadow rays are spawned for
// light transport queries, so the event-based channels are masked to 0 for
// both.
//------------------------------------------------------------------------------

typedef enum {
	RAYINFO_IS_CAMERA_RAY,
	RAYINFO_IS_SHADOW_RAY,
	RAYINFO_IS_DIFFUSE_RAY,
	RAYINFO_IS_GLOSSY_RAY,
	RAYINFO_IS_SINGULAR_RAY,
	RAYINFO_IS_REFLECTION_RAY,
	RAYINFO_IS_TRANSMISSION_RAY,
	RAYINFO_IS_VOLUME_SCATTER_RAY,
	RAYINFO_RAY_LENGTH,
	RAYINFO_RAY_DEPTH,
	RAYINFO_DIFFUSE_DEPTH,
	RAYINFO_GLOSSY_DEPTH,
	RAYINFO_SPECULAR_DEPTH,
	RAYINFO_TRANSMISSION_DEPTH,
	RAYINFO_TRANSPARENT_DEPTH
} RayInfoChannel;

class RayInfoTexture : public Texture {
public:
	RayInfoTexture(const RayInfoChannel ch) : channel(ch) { }
	virtual ~RayInfoTexture() { }

	virtual TextureType GetType() const { return RAYINFO_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	// The following methods don't make very much sense in this case. I have no
	// information about the color.
	virtual float Y() const { return 1.f; }
	virtual float Filter() const { return 1.f; }

	RayInfoChannel GetChannel() const { return channel; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

private:
	const RayInfoChannel channel;
};

}

#endif	/* _SLG_RAYINFOTEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
