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

#ifndef _SLG_FACINGTEX_H
#define	_SLG_FACINGTEX_H

#include "slg/textures/texture.h"

namespace slg {

//------------------------------------------------------------------------------
// Facing texture
//
// Cycles' LayerWeight "Facing" output: pow(1 - |cosi|, blend) where
// cosi = dot(-fixedDir, shadeN) is the incident cosine. Not a Fresnel term -
// a view-angle falloff used for rim/backlighting and edge blends.
//------------------------------------------------------------------------------

class FacingTexture : public Texture {
public:
	FacingTexture(const float blendVal) : blend(blendVal) { }
	virtual ~FacingTexture() { }

	virtual TextureType GetType() const { return FACING_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual float Y() const { return .5f; }
	virtual float Filter() const { return 1.f; }

	float GetBlend() const { return blend; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

private:
	const float blend;
};

}

#endif	/* _SLG_FACINGTEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
