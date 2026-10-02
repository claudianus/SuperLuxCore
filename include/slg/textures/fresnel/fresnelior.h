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

#ifndef _SLG_FRESNELIORTEX_H
#define	_SLG_FRESNELIORTEX_H

#include "slg/textures/fresnel/fresneltexture.h"

namespace slg {

//------------------------------------------------------------------------------
// Fresnel IOR texture
//
// Unlike the other Fresnel textures - which are *parameter carriers* the
// BSDF sampling code calls with an explicit incidence cosine - this one is
// a plain shading texture: GetFloatValue()/EvalSpectrumValue() evaluate the
// dielectric Fresnel reflectance at the incident angle carried by the
// HitPoint (cosi = dot(-fixedDir, shadeN)). It closes Cycles'
// ShaderNodeFresnel and the LayerWeight "Facing" channel, both of which are
// pure functions of the view angle and were previously Schlick-approximated.
//------------------------------------------------------------------------------

class FresnelIorTexture : public FresnelTexture {
public:
	FresnelIorTexture(const float etaVal) : eta(etaVal) { }
	virtual ~FresnelIorTexture() { }

	virtual TextureType GetType() const { return FRESNELIOR_TEX; }
	virtual float GetFloatValue(const HitPoint &hitPoint) const;
	virtual luxrays::Spectrum EvalSpectrumValue(const HitPoint &hitPoint) const;
	virtual float Y() const;
	virtual float Filter() const;

	virtual luxrays::Spectrum Evaluate(const HitPoint &hitPoint, const float cosi) const;

	float GetEta() const { return eta; }

	virtual luxrays::PropertiesUPtr ToProperties(const ImageMapCache &imgMapCache, const bool useRealFileName) const;

private:
	const float eta;
};

}

#endif	/* _SLG_FRESNELIORTEX_H */
// vim: autoindent noexpandtab tabstop=4 shiftwidth=4
