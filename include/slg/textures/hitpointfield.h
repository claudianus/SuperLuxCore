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

#ifndef _SLG_HITPOINTFIELDTEX_H
#define	_SLG_HITPOINTFIELDTEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// HitPoint texture
//
// Exposes raw HitPoint fields the shading graph couldn't read before: the
// un-shaded geometric normal, back-face flag, incoming ray direction and
// triangle barycentrics. Used to fill the remaining outputs of Cycles'
// Geometry node (True Normal / Backfacing / Incoming / Parametric).
//------------------------------------------------------------------------------

typedef enum {
	HITPOINT_GEOMETRYN,    // float3(hitPoint->geometryN)
	HITPOINT_BACKFACING,   // hitPoint->intoObject ? 1 : 0
	HITPOINT_INCOMING,     // float3(hitPoint->fixedDir) - ray direction
	HITPOINT_PARAMETRIC,   // float3(b1, b2, 0) - triangle barycentrics
	HITPOINT_CHANNEL_COUNT
} HitPointChannel;

class HitPointFieldTexture : public Texture {
public:
	HitPointFieldTexture(const HitPointChannel ch) : channel(ch) { }
	virtual ~HitPointFieldTexture() { }

	virtual TextureType GetType() const { return HITPOINT_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual float Y() const { return .5f; }
	virtual float Filter() const { return 1.f; }

	HitPointChannel GetChannel() const { return channel; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

private:
	const HitPointChannel channel;
};

}

#endif	/* _SLG_HITPOINTFIELDTEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
